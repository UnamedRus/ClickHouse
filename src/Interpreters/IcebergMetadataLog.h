#pragma once

#include <functional>

#include <Core/SettingsEnums.h>
#include <Interpreters/SystemLog.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergPath.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFilesPruning.h>

namespace DB
{

struct IcebergMetadataLogElement
{
    time_t current_time{};
    String query_id;
    IcebergMetadataLogLevel content_type = IcebergMetadataLogLevel::None;
    String table_path;
    String file_path;
    String metadata_content;
    std::optional<UInt64> row_in_file;
    std::optional<Iceberg::PruningReturnStatus> pruning_status;

    static std::string name() { return "IcebergMetadataLog"; }

    static ColumnsDescription getColumnsDescription();
    static NamesAndAliases getNamesAndAliases() { return {}; }
    void appendToBlock(MutableColumns & columns) const;
};

/// `get_row` is evaluated lazily - only if `row_log_level` is actually enabled by the
/// `iceberg_metadata_log_level` setting. Serializing the metadata row (e.g. a manifest entry) is
/// expensive and, on wide tables with many manifest entries, was dominating query planning when
/// done eagerly for every entry with logging off. Pass a closure that builds the string on demand.
void insertRowToLogTable(
    const ContextPtr & local_context,
    std::function<String()> get_row,
    IcebergMetadataLogLevel row_log_level,
    const String & table_path,
    const Iceberg::IcebergPathFromMetadata & file_path,
    std::optional<UInt64> row_in_file,
    std::optional<Iceberg::PruningReturnStatus> pruning_status);

class IcebergMetadataLog : public SystemLog<IcebergMetadataLogElement>
{
    using SystemLog<IcebergMetadataLogElement>::SystemLog;
};

}
