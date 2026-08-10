#pragma once
#include <Storages/ObjectStorage/DataLakes/Iceberg/PersistentTableComponents.h>
#include "config.h"

#if USE_AVRO

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <Core/Types.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Interpreters/Context_fwd.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFileIterator.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Snapshot.h>

#include <Common/ConcurrentBoundedQueue.h>
#include <Common/ThreadPool_fwd.h>

#include <optional>
#include <base/defines.h>

#include <Core/BackgroundSchedulePool.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergDataObjectInfo.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadataFilesCache.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergTableStateSnapshot.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/ManifestFilesPruning.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/PositionDeleteTransform.h>

namespace DB
{

namespace Iceberg
{

class SingleThreadIcebergKeysIterator
{
public:
    SingleThreadIcebergKeysIterator(
        ObjectStoragePtr object_storage_,
        ContextPtr local_context_,
        Iceberg::ManifestFileContentType manifest_file_content_type_,
        const ActionsDAG * filter_dag_,
        TableStateSnapshotPtr table_snapshot_,
        IcebergDataSnapshotPtr data_snapshot_,
        PersistentTableComponents persistent_components);

    std::optional<DB::Iceberg::ProcessedManifestFileEntryPtr> next();

    /// Advance to the next manifest file of the matching content type and return an iterator over
    /// its entries (whose `next()` is safe to call from several threads at once), or nullptr when
    /// no manifest files are left. Not thread-safe itself: the caller serializes the advance (it
    /// mutates `manifest_file_index` and fetches the manifest file), then hands the returned
    /// iterator to the parallel workers.
    Iceberg::ManifestIteratorPtr nextManifestFile();

private:
    ObjectStoragePtr object_storage;
    std::shared_ptr<const ActionsDAG> filter_dag;
    ContextPtr local_context;
    Iceberg::TableStateSnapshotPtr table_snapshot;
    Iceberg::IcebergDataSnapshotPtr data_snapshot;
    PersistentTableComponents persistent_components;
    LoggerPtr log;

    size_t manifest_file_index = 0;
    Iceberg::ManifestIteratorPtr current_manifest_file_iterator;

    const Iceberg::ManifestFileContentType manifest_file_content_type;
};

}

class IcebergIterator : public IObjectIterator
{
public:
    explicit IcebergIterator(
        ObjectStoragePtr object_storage_,
        ContextPtr local_context_,
        const ActionsDAG * filter_dag_,
        IDataLakeMetadata::FileProgressCallback callback_,
        Iceberg::TableStateSnapshotPtr table_snapshot_,
        Iceberg::IcebergDataSnapshotPtr data_snapshot_,
        Iceberg::PersistentTableComponents persistent_components);

    ObjectInfoPtr next(size_t) override;

    size_t estimatedKeysCount() override;
    ~IcebergIterator() override;

private:
    LoggerPtr logger;
    std::shared_ptr<ActionsDAG> filter_dag;
    ObjectStoragePtr object_storage;
    const Iceberg::TableStateSnapshotPtr table_state_snapshot;
    Iceberg::PersistentTableComponents persistent_components;
    Iceberg::SingleThreadIcebergKeysIterator data_files_iterator;
    Iceberg::SingleThreadIcebergKeysIterator deletes_iterator;
    ConcurrentBoundedQueue<Iceberg::ProcessedManifestFileEntryPtr> blocking_queue;
    /// Legacy single-threaded producer (iceberg_metadata_processing_threads == 1).
    std::unique_ptr<ThreadFromGlobalPool> producer_task;
    /// Parallel producers (iceberg_metadata_processing_threads > 1). They share the manifest-file
    /// cursor of `data_files_iterator` through `manifest_advance_mutex` and drain each manifest
    /// file's entries concurrently (ManifestFileIterator::next() is thread-safe).
    std::vector<ThreadFromGlobalPool> producer_threads;
    std::mutex manifest_advance_mutex;
    Iceberg::ManifestIteratorPtr current_producer_manifest TSA_GUARDED_BY(manifest_advance_mutex);
    bool manifest_source_exhausted TSA_GUARDED_BY(manifest_advance_mutex) = false;
    /// Number of parallel producer threads still running; the last one to finish closes the queue.
    std::atomic<size_t> producers_remaining{0};
    IDataLakeMetadata::FileProgressCallback callback;
    std::vector<Iceberg::ProcessedManifestFileEntryPtr> position_deletes_files;
    std::vector<Iceberg::ProcessedManifestFileEntryPtr> equality_deletes_files;
    std::exception_ptr exception;
    std::mutex exception_mutex;
};
}


#endif
