#include <Processors/QueryPlan/ReadFromObjectStorageStep.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Core/Settings.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Interpreters/ActionsDAG.h>
#include <Processors/Sources/NullSource.h>
#include <Processors/QueryPlan/Serialization.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/Operators.h>
#include <Storages/ObjectStorage/S3/Configuration.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadata.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeConfiguration.h>
#include <Processors/QueryPlan/QueryPlanStepRegistry.h>
#include <Formats/FormatFactory.h>
#include <Formats/FormatParserSharedResources.h>
#include <IO/ReadBufferFromString.h>
#include <Interpreters/Context.h>
#include <Storages/prepareReadingFromFormat.h>
#include <Storages/VirtualColumnUtils.h>
#include <Parsers/IAST.h>
#include <Parsers/parseQuery.h>
#include <Parsers/ExpressionElementParsers.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/ITableFunction.h>
#include <boost/algorithm/string/predicate.hpp>


namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

namespace Setting
{
    extern const SettingsBool parallelize_output_from_storages;
}


ReadFromObjectStorageStep::ReadFromObjectStorageStep(
    const StorageID & storage_id_,
    ObjectStoragePtr object_storage_,
    StorageObjectStorageConfigurationPtr configuration_,
    const Names & columns_to_read,
    const NamesAndTypesList & virtual_columns_,
    const SelectQueryInfo & query_info_,
    const StorageSnapshotPtr & storage_snapshot_,
    const std::optional<DB::FormatSettings> & format_settings_,
    bool distributed_processing_,
    ReadFromFormatInfo info_,
    bool need_only_count_,
    ContextPtr context_,
    size_t max_block_size_,
    size_t num_streams_)
    : SourceStepWithFilter(std::make_shared<const Block>(info_.source_header), columns_to_read, query_info_, storage_snapshot_, context_)
    , storage_id(storage_id_)
    , object_storage(object_storage_)
    , configuration(configuration_)
    , info(std::move(info_))
    , virtual_columns(virtual_columns_)
    , format_settings(format_settings_)
    , need_only_count(need_only_count_)
    , max_block_size(max_block_size_)
    , num_streams(num_streams_)
    , max_num_streams(num_streams_)
    , distributed_processing(distributed_processing_)
{
}

QueryPlanStepPtr ReadFromObjectStorageStep::clone() const
{
    return std::make_unique<ReadFromObjectStorageStep>(*this);
}

void ReadFromObjectStorageStep::applyFilters(ActionDAGNodes added_filter_nodes)
{
    SourceStepWithFilter::applyFilters(std::move(added_filter_nodes));
    if (!filter_actions_dag)
        return;

    if (boost::iequals(configuration->getFormat(), "Parquet") || boost::iequals(configuration->getFormat(), "ORC"))
        prepareEagerKeyConditionSets(
            filter_actions_dag,
            storage_snapshot, info.source_header,
            query_info.prewhere_info, query_info.row_level_filter, getContext());

    // It is important to build the inplace sets for the filter here, before reading data from object storage.
    // If we delay building these sets until later in the pipeline, the filter can be applied after the data
    // has already been read, potentially in parallel across many streams. This can significantly reduce the
    // effectiveness of an Iceberg partition pruning, as unnecessary data may be read. Additionally, building ordered sets
    // at this stage enables the KeyCondition class to apply more efficient optimizations than for unordered sets.
    /// Idempotent — sets already built above are skipped via !future_set->get() check.
    VirtualColumnUtils::buildSetsForDAGExcludingGlobalIn(*filter_actions_dag, getContext());
}

void ReadFromObjectStorageStep::updatePrewhereInfo(const PrewhereInfoPtr & prewhere_info_value)
{
    info = updateFormatPrewhereInfo(info, query_info.row_level_filter, prewhere_info_value);
    query_info.prewhere_info = prewhere_info_value;
    output_header = std::make_shared<const Block>(info.source_header);
}

void ReadFromObjectStorageStep::initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &)
{
    createIterator();

    Pipes pipes;
    auto context = getContext();
    size_t estimated_keys_count = iterator_wrapper->estimatedKeysCount();

    if (estimated_keys_count > 1)
        num_streams = std::min(num_streams, estimated_keys_count);
    else
    {
        /// The amount of keys (zero) was probably underestimated.
        /// We will keep one stream for this particular case.
        num_streams = 1;
    }

    // here create for node -> query -> level thread pool
    auto parser_shared_resources = std::make_shared<FormatParserSharedResources>(context->getSettingsRef(), num_streams);

    auto format_filter_info = std::make_shared<FormatFilterInfo>(
        filter_actions_dag,
        context,
        configuration->getColumnMapperForCurrentSchema(storage_snapshot->metadata, context),
        query_info.row_level_filter,
        query_info.prewhere_info);

    for (size_t i = 0; i < num_streams; ++i)
    {
        auto source = std::make_shared<StorageObjectStorageSource>(
            storage_id,
            getName(),
            object_storage,
            configuration,
            storage_snapshot,
            info,
            format_settings,
            context,
            max_block_size,
            iterator_wrapper,
            parser_shared_resources,
            format_filter_info,
            need_only_count);

        pipes.emplace_back(std::move(source));
    }
    auto pipe = Pipe::unitePipes(std::move(pipes));
    if (pipe.empty())
        pipe = Pipe(std::make_shared<NullSource>(std::make_shared<const Block>(info.source_header)));

    size_t output_ports = pipe.numOutputPorts();
    const bool parallelize_output = context->getSettingsRef()[Setting::parallelize_output_from_storages];
    if (parallelize_output
        && FormatFactory::instance().checkParallelizeOutputAfterReading(configuration->getFormat(), context)
        && output_ports > 0 && output_ports < max_num_streams)
        pipe.resize(max_num_streams);

    for (const auto & processor : pipe.getProcessors())
        processors.emplace_back(processor);

    pipeline.init(std::move(pipe));
}

void ReadFromObjectStorageStep::createIterator()
{
    if (iterator_wrapper)
        return;

    const ActionsDAG::Node * predicate = nullptr;
    if (filter_actions_dag)
        predicate = filter_actions_dag->getOutputs().at(0);

    auto context = getContext();

    iterator_wrapper = StorageObjectStorageSource::createFileIterator(
        configuration, configuration->getQuerySettings(context), object_storage, storage_snapshot->metadata, distributed_processing,
        context, predicate, filter_actions_dag.get(), virtual_columns, info.hive_partition_columns_to_read_from_file_path, nullptr, context->getFileProgressCallback(),
        /*ignore_archive_globs=*/ false, /*skip_object_metadata=*/ false, /*with_tags=*/ info.requested_virtual_columns.contains("_tags"));
}

static InputOrderInfoPtr convertSortingKeyToInputOrder(const KeyDescription & key_description)
{
    SortDescription sort_description_for_merging;
    for (size_t i = 0; i < key_description.column_names.size(); ++i)
        sort_description_for_merging.push_back(
            SortColumnDescription(key_description.column_names[i], (!key_description.reverse_flags.empty() && key_description.reverse_flags[i]) ? -1 : 1));
    return std::make_shared<const InputOrderInfo>(sort_description_for_merging, sort_description_for_merging.size(), 1, 0);
}

bool ReadFromObjectStorageStep::requestReadingInOrder() const
{
    return configuration->isDataSortedBySortingKey(storage_snapshot->metadata, getContext());
}

InputOrderInfoPtr ReadFromObjectStorageStep::getDataOrder() const
{
    return convertSortingKeyToInputOrder(getStorageMetadata()->getSortingKey());
}

void ReadFromObjectStorageStep::setDistributedRead(size_t bucket_count)
{
    distributed_read_bucket_count = bucket_count;
}

Strings ReadFromObjectStorageStep::getShardsForDistributedRead() const
{
    /// TODO(distributed_plan): return the shard list the coordinator's task iterator distributes to.
    return {};
}

void ReadFromObjectStorageStep::serialize(Serialization & ctx) const
{
    /// Serialize the object-storage source config faithfully by reusing the table-function args
    /// (endpoint / credentials / format / schema / iceberg metadata pointer) that
    /// object_storage_cluster already ships to workers. The worker reconstructs the reader from these.
    writeStringBinary(configuration->getEngineName(), ctx.out);

    auto args = configuration->createArgsWithAccessData();
    writeStringBinary(args->formatWithSecretsOneLine(), ctx.out);

    auto column_names = getOutputHeader()->getNames();
    writeVarUInt(column_names.size(), ctx.out);
    for (const auto & column : column_names)
        writeStringBinary(column, ctx.out);

    UInt8 flags = 0;
    if (need_only_count)
        flags |= 1;
    if (filter_actions_dag)
        flags |= 2;
    writeIntBinary(flags, ctx.out);

    writeVarUInt(max_block_size, ctx.out);
    writeVarUInt(num_streams, ctx.out);
    writeVarUInt(distributed_read_bucket_count, ctx.out);

    if (filter_actions_dag)
        filter_actions_dag->serialize(ctx.out, ctx.registry);
}

std::unique_ptr<IQueryPlanStep> ReadFromObjectStorageStep::deserialize(Deserialization & ctx)
{
    String engine_name;
    readStringBinary(engine_name, ctx.in);
    String args_str;
    readStringBinary(args_str, ctx.in);

    Names column_names;
    size_t num_columns = 0;
    readVarUInt(num_columns, ctx.in);
    column_names.reserve(num_columns);
    for (size_t i = 0; i < num_columns; ++i)
    {
        String column;
        readStringBinary(column, ctx.in);
        column_names.push_back(std::move(column));
    }

    UInt8 flags = 0;
    readIntBinary(flags, ctx.in);
    const bool step_need_only_count = flags & 1;
    const bool has_filter = flags & 2;

    size_t step_max_block_size = 0;
    size_t step_num_streams = 0;
    size_t bucket_count = 0;
    readVarUInt(step_max_block_size, ctx.in);
    readVarUInt(step_num_streams, ctx.in);
    readVarUInt(bucket_count, ctx.in);

    std::optional<ActionsDAG> filter_dag;
    if (has_filter)
        filter_dag = ActionsDAG::deserialize(ctx.in, ctx.registry, ctx.context);

    /// Reconstruct the StorageObjectStorage from the serialized table-function form
    /// (engine + createArgsWithAccessData args) — the same faithful config object_storage_cluster
    /// ships to workers.
    const String func_sql = engine_name + "(" + args_str + ")";
    ParserFunction parser(/*allow_function_parameters_=*/ false);
    ASTPtr func_ast = parseQuery(parser, func_sql.data(), func_sql.data() + func_sql.size(),
        "object storage table function", /*max_query_size=*/ 0, /*max_parser_depth=*/ 0, /*max_parser_backtracks=*/ 0);

    auto table_function = TableFunctionFactory::instance().get(func_ast, ctx.context);
    auto columns_desc = table_function->getActualTableStructureWithAccess(ctx.context, /*is_insert_query=*/ false);
    StoragePtr storage = table_function->execute(func_ast, ctx.context, table_function->getName(), std::move(columns_desc));

    auto * object_storage_table = dynamic_cast<StorageObjectStorage *>(storage.get());
    if (!object_storage_table)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Deserialized storage for step {} is not a StorageObjectStorage", STEP_NAME);

    auto object_storage = object_storage_table->getObjectStorage();
    auto configuration = object_storage_table->getObjectStorageConfiguration();
    auto metadata = object_storage_table->getInMemoryMetadataPtr(ctx.context, /*bypass_metadata_cache=*/ false);
    auto storage_snapshot = object_storage_table->getStorageSnapshot(metadata, ctx.context);

    SelectQueryInfo query_info;
    auto virtual_columns = metadata->virtuals.getSampleBlock(
        VirtualsKind::All, VirtualsMaterializationPlace::Reader).getNamesAndTypesList();

    auto read_from_format_info = configuration->prepareReadingFromFormat(
        object_storage, column_names, storage_snapshot,
        object_storage_table->supportsSubsetOfColumns(ctx.context),
        /*supports_tuple_elements=*/ true, ctx.context, PrepareReadingFromFormatHiveParams{});

    auto step = std::make_unique<ReadFromObjectStorageStep>(
        object_storage_table->getStorageID(),
        object_storage,
        configuration,
        column_names,
        virtual_columns,
        query_info,
        storage_snapshot,
        std::nullopt,
        /*distributed_processing=*/ true,
        std::move(read_from_format_info),
        step_need_only_count,
        ctx.context,
        step_max_block_size,
        step_num_streams);

    if (filter_dag)
        step->applyFilters(ActionDAGNodes{.nodes = filter_dag->getOutputs()});
    if (bucket_count)
        step->setDistributedRead(bucket_count);

    ctx.storage_holders.push_back(storage);
    return step;
}

void registerReadFromObjectStorageStep(QueryPlanStepRegistry & registry);
void registerReadFromObjectStorageStep(QueryPlanStepRegistry & registry)
{
    registry.registerStep(ReadFromObjectStorageStep::STEP_NAME, ReadFromObjectStorageStep::deserialize);
}

}
