#pragma once

#include <Storages/MergeTree/MergeTreeIndices.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeProjectionsIndexesTask.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/TextIndexPositionData.h>
#include <Storages/MergeTree/MergedPartOffsets.h>
#include <Storages/MergeTree/TextIndexSegment.h>
#include <Core/SortCursor.h>
#include <Processors/ISimpleTransform.h>

namespace DB
{

/// Transform that builds text indexes and periodically flushes their segments
/// into temporary storage, when amount of accumulated data reaches some threshold.
/// Used for materialization of text indexes.
class BuildTextIndexTransform final : public ISimpleTransform
{
public:
    BuildTextIndexTransform(
        SharedHeader header,
        String index_file_prefix_,
        std::vector<MergeTreeIndexPtr> indexes_,
        MutableDataPartStoragePtr temporary_storage_,
        MergeTreeWriterSettings writer_settings_,
        CompressionCodecPtr default_codec_,
        String marks_file_extension_);

    String getName() const override { return "BuildTextIndexTransform"; }

    IProcessor::Status prepare() override;
    void transform(Chunk & chunk) override;

    void aggregate(const Block & block);
    void finalize();

    /// Returns all segments created by this transform for the given index and part.
    std::vector<TextIndexSegment> getSegments(const String & index_name, size_t part_idx) const;
    const std::vector<MergeTreeIndexPtr> & getIndexes() const { return indexes; }
    bool hasIndex(const String & index_name) const { return index_position_by_name.contains(index_name); }

private:
    /// Resets current index granule and flush a segment
    /// of the text index to the temporary storage.
    void writeTemporarySegment(size_t i);

    String index_file_prefix;
    std::vector<MergeTreeIndexPtr> indexes;
    std::unordered_map<String, size_t> index_position_by_name;
    MergeTreeIndexAggregators aggregators;
    MutableDataPartStoragePtr temporary_storage;
    MergeTreeWriterSettings writer_settings;
    CompressionCodecPtr default_codec;
    String marks_file_extension;

    /// Number of rows in blocks processed by the transform.
    size_t num_processed_rows = 0;
    /// Number of flushed segments for each index.
    std::vector<size_t> segment_numbers;
};

/// Task that merges text indexes from data parts,
/// or temporary segments of text indexes.
/// Task can recalcute row numbers in the source
/// posting to row numbers in the resulting part.
/// The mapping from old part offsets to the new part offsets is built
/// during the merge of data parts and can be optionally passed to this task.
/// Currently merges all segments in one stage
/// TODO: Implement multi-stage merge to reduce the memory usage.
class MergeTextIndexesTask : public MergeProjectionsIndexesTask
{
public:
    MergeTextIndexesTask(
        std::vector<TextIndexSegment> segments,
        MergeTreeMutableDataPartPtr new_data_part_,
        size_t num_rows_,
        MergeTreeIndexPtr index_ptr_,
        std::shared_ptr<MergedPartOffsets> merged_part_offsets_,
        const MergeTreeReaderSettings & reader_settings_,
        const MergeTreeWriterSettings & writer_settings_,
        bool is_final_);

    ~MergeTextIndexesTask() noexcept override;

    bool executeStep() override;
    void cancel() noexcept override;

    MutableDataPartsVector extractTemporaryParts() override { return {}; }
    void addToChecksums(MergeTreeDataPartChecksums & checksums) override;

private:
    void finalize();
    void cancelImpl() noexcept;
    Block getHeader() const;
    void initializeQueue();

    /// Returns true if the given cursor points to a new token.
    bool isNewToken(const SortCursor & cursor) const;
    /// Reads the next dictionary block for the given source index.
    void readDictionaryBlock(size_t source_num);
    /// Reads the next posting lists for the next token in the given source index.
    std::vector<PostingListPtr> readPostingLists(size_t source_num);
    /// Adjusts row numbers in the postings list according to merged part offsets.
    PostingListPtr adjustPartOffsets(size_t source_num, PostingListPtr posting_list);
    /// map_element mode: remaps element ids (not row ids) across the merge. Fixed stride makes it
    /// arithmetic: new_eid = merged_part_offsets[part, e/S_old] * S_new + (e % S_old).
    PostingListPtr adjustMapElementPostings(size_t source_num, const PostingList & posting_list) const;

    /// map_element FINAL merge: instead of the streaming per-token flush, accumulate all merged
    /// (row-remapped) postings, re-assign element slots freq-positionally against the merged
    /// frequencies (compaction), then serialize. Runs in a single step.
    bool executeRerankStep();
    /// Re-assigns slots in the accumulated postings by merged key frequency; rewrites (only) the
    /// element ids that actually change (movers). `tokens_and_postings` is dict-sorted.
    void rerankMapElement(std::vector<std::pair<String, PostingList>> & tokens_and_postings) const;

    void flushPostingList();
    void flushDictionaryBlock();

    std::vector<TextIndexSegment> segments;
    MergeTreeMutableDataPartPtr new_data_part;
    size_t num_rows;
    MergeTreeIndexPtr index_ptr;
    MergeTreeIndexTextParams params;

    /// If not null, posting list values must be recalculated using merged offsets.
    std::shared_ptr<MergedPartOffsets> merged_part_offsets;
    MergeTreeWriterSettings writer_settings;
    size_t step_time_ms;

    std::vector<MergeTreeIndexInputStreams> input_streams;
    std::vector<std::unique_ptr<MergeTreeIndexReaderStream>> input_streams_holders;

    MergeTreeIndexOutputStreams output_streams;
    std::vector<std::unique_ptr<MergeTreeIndexWriterStream>> output_streams_holders;

    SortCursorImpls cursors;
    std::vector<DictionaryBlock> inputs;
    SortingQueue<SortCursor> queue;

    /// Tokens accumulated for the current dictionary block.
    MutableColumnPtr output_tokens;
    /// Tokens infos accumulated for the current dictionary block.
    std::vector<TokenPostingsInfo> output_infos;
    /// Postings accumulated for the current token.
    PostingList output_postings;
    /// Positions accumulated for the current token (phrase query support).
    PODArray<RoaringishEntry> output_positions;
    /// Sparse index accumulated for the task. Flushed only once in the end of the task.
    MutableColumnPtr sparse_index_tokens;
    MutableColumnPtr sparse_index_offsets;

    /// Deserializer for the merged output part, using the destination codec resolved from the index definition.
    PostingsSerialization postings_serialization;
    /// Per-source deserializers, each using the codec read from that source part's own header.
    std::vector<PostingsSerialization> source_postings_serializations;

    /// map_element mode: fixed per-row stride `S` read from each source part's header, and the
    /// merged part's stride (= max of source strides). eid = row*S + slot.
    std::vector<UInt64> source_map_stride;
    UInt64 merged_map_stride = 1;
    /// map_element_granule mode: the granule index is rebuilt from the merged Map column into a
    /// single source segment, so its key stride `R` and row-window `W` are read from that segment's
    /// header and preserved verbatim in the merged header. kid = chunk*R + slot; chunk = abs_row / W.
    UInt64 merged_map_key_stride = 0;
    UInt64 merged_map_chunk_window = 0;
    /// map_element FINAL merge: re-assign slots freq-positionally (compaction) instead of the
    /// streaming slot-preserving remap.
    bool rerank = false;

    bool is_initialized = false;
};

using MergeTextIndexesTaskPtr = std::unique_ptr<MergeTextIndexesTask>;

MutableDataPartStoragePtr createTemporaryTextIndexStorage(const DiskPtr & disk, const String & part_relative_path);

std::unique_ptr<MergeTreeReaderStream> makeTextIndexInputStream(
    DataPartStoragePtr data_part_storage,
    const String & stream_name,
    const String & extension,
    const MergeTreeReaderSettings & reader_settings);

}
