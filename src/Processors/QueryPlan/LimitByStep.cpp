#include <Processors/QueryPlan/LimitByStep.h>
#include <Processors/Transforms/LimitByTransform.h>
#include <Processors/IProcessor.h>
#include <Processors/ResizeProcessor.h>
#include <Processors/Transforms/ScatterByPartitionTransform.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <IO/Operators.h>
#include <Common/JSONBuilder.h>


namespace DB
{

static ITransformingStep::Traits getTraits()
{
    return ITransformingStep::Traits
    {
        {
            .returns_single_stream = true,
            .preserves_number_of_streams = false,
            .preserves_sorting = true,
        },
        {
            .preserves_number_of_rows = false,
        }
    };
}

LimitByStep::LimitByStep(
    const DataStream & input_stream_,
    size_t group_length_, size_t group_offset_, const Names & columns_)
    : ITransformingStep(input_stream_, input_stream_.header, getTraits())
    , group_length(group_length_)
    , group_offset(group_offset_)
    , columns(columns_)
{
}

void LimitByStep::scatterByPartitionIfNeeded(QueryPipelineBuilder& pipeline)
{
    size_t threads = pipeline.getNumThreads();
    size_t streams = pipeline.getNumStreams();
    if (threads > 1)
    {
        Block stream_header = pipeline.getHeader();

        ColumnNumbers key_columns;
        key_columns.reserve(columns.size());
        for (auto & column_name : columns)
        {
            key_columns.push_back(stream_header.getPositionByName(column_name));
        }

        pipeline.transform([&](OutputPortRawPtrs ports)
        {
            Processors processors;
            for (auto * port : ports)
            {
                auto scatter = std::make_shared<ScatterByPartitionTransform>(stream_header, threads, key_columns);
                connect(*port, scatter->getInputs().front());
                processors.push_back(scatter);
            }
            return processors;
        });

        if (streams > 1)
        {
            pipeline.transform([&](OutputPortRawPtrs ports)
            {
                Processors processors;
                for (size_t i = 0; i < threads; ++i)
                {
                    size_t output_it = i;
                    auto resize = std::make_shared<ResizeProcessor>(stream_header, streams, 1);
                    auto & inputs = resize->getInputs();

                    for (auto input_it = inputs.begin(); input_it != inputs.end(); output_it += threads, ++input_it)
                        connect(*ports[output_it], *input_it);
                    processors.push_back(resize);
                }
                return processors;
            });
        }
    }
    else
    {
        pipeline.resize(1);
    }
}

void LimitByStep::transformPipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &)
{

    scatterByPartitionIfNeeded(pipeline);

    pipeline.addSimpleTransform([&](const Block & header, QueryPipelineBuilder::StreamType stream_type) -> ProcessorPtr
    {
        if (stream_type != QueryPipelineBuilder::StreamType::Main)
            return nullptr;

        return std::make_shared<LimitByTransform>(header, group_length, group_offset, columns);
    });
}

void LimitByStep::describeActions(FormatSettings & settings) const
{
    String prefix(settings.offset, ' ');

    settings.out << prefix << "Columns: ";

    if (columns.empty())
        settings.out << "none\n";
    else
    {
        bool first = true;
        for (const auto & column : columns)
        {
            if (!first)
                settings.out << ", ";
            first = false;

            settings.out << column;
        }
        settings.out << '\n';
    }

    settings.out << prefix << "Length " << group_length << '\n';
    settings.out << prefix << "Offset " << group_offset << '\n';
}

void LimitByStep::describeActions(JSONBuilder::JSONMap & map) const
{
    auto columns_array = std::make_unique<JSONBuilder::JSONArray>();
    for (const auto & column : columns)
        columns_array->add(column);

    map.add("Columns", std::move(columns_array));
    map.add("Length", group_length);
    map.add("Offset", group_offset);
}

}
