#include <Processors/Formats/Impl/Parquet/Prefetcher.h>

#include <Formats/FormatParserSharedResources.h>
#include <IO/copyData.h>
#include <IO/SeekableReadBuffer.h>
#include <IO/WithFileSize.h>
#include <IO/WriteBufferFromVector.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>

#include <shared_mutex>
#include <algorithm>
#include <limits>

namespace DB::ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
}

namespace ProfileEvents
{
    extern const Event ParquetFetchWaitTimeMicroseconds;
    extern const Event ParquetPrefetcherReadRandomRead;
    extern const Event ParquetPrefetcherReadSeekAndRead;
    extern const Event ParquetPrefetcherReadEntireFile;
    extern const Event ParquetPrefetcherServedFromRetainedTail;
    extern const Event ParquetPrefetcherPartAlignedTasks;
    extern const Event ParquetPrefetcherAlignmentSkippedSmall;
}

namespace DB::Parquet
{

void Prefetcher::init(ReadBuffer * reader_, const ReadOptions & options, FormatParserSharedResourcesPtr parser_shared_resources_)
{
    min_bytes_for_seek = options.min_bytes_for_seek;
    bytes_per_read_task = options.bytes_per_read_task;
    multipart_part_offsets = options.multipart_part_offsets;
    read_alignment_stride = options.read_alignment_stride;
    read_alignment_min_bytes = options.read_alignment_min_bytes;
    parser_shared_resources = parser_shared_resources_;
    determineReadModeAndFileSize(reader_, options);
    range_sets.resize(1);
    read_start_time = std::chrono::steady_clock::now();
}

double Prefetcher::averageThroughputBytesPerSec() const
{
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - read_start_time).count();
    size_t bytes = total_bytes_read.load(std::memory_order_relaxed);
    /// Too little read / too little time elapsed to estimate: report "unknown" so back-pressure
    /// fails open (does not throttle).
    if (seconds < 0.05 || bytes < (1u << 20))
        return 0;
    return static_cast<double>(bytes) / seconds;
}

Prefetcher::~Prefetcher()
{
    shutdown->shutdown();

    /// Assert that all PrefetchHandle-s were destroyed.
    chassert(std::all_of(requests.begin(), requests.end(), [](const RequestState & req)
    {
        return req.state.load(std::memory_order_relaxed) == RequestState::State::Cancelled;
    }));
}

void Prefetcher::determineReadModeAndFileSize(ReadBuffer * reader_, const ReadOptions & options)
{
    if (options.seekable_read)
    {
        bool has_file_size = isBufferWithFileSize(*reader_);
        auto * seekable = dynamic_cast<SeekableReadBuffer *>(reader_);
        if (has_file_size && seekable)
        {
            if (seekable->supportsReadAt())
            {
                reader = seekable;
                read_mode = ReadMode::RandomRead;
            }
            else if (seekable->checkIfActuallySeekable())
            {
                reader = seekable;
                read_mode = ReadMode::SeekAndRead;
            }

            if (reader)
                file_size = getFileSizeFromReadBuffer(*seekable);
        }
    }

    if (!reader)
    {
        /// Avoid loading the whole file if it's clearly not a parquet file.
        constexpr std::string_view expected_prefix = "PAR1";
        if (!reader_->eof() && reader_->available() >= expected_prefix.size() &&
            memcmp(reader_->position(), expected_prefix.data(), expected_prefix.size()) != 0)
        {
            throw Exception(ErrorCodes::INCORRECT_DATA, "Not a Parquet file (wrong magic bytes at the start)");
        }

        WriteBufferFromVector<PaddedPODArray<char>> out(entire_file);
        copyData(*reader_, out);
        out.finalize();

        read_mode = ReadMode::EntireFileIsInMemory;
        file_size = entire_file.size();
    }
}

void Prefetcher::readSync(char * to, size_t n, size_t offset)
{
    if (offset > file_size || n > file_size - offset)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "File read out of bounds: offset {}, length {}, file size {}", offset, n, file_size);

    size_t nread = 0;
    switch (read_mode)
    {
        case ReadMode::RandomRead:
            nread = reader->readBigAt(to, n, offset, /*progress_callback*/ nullptr);
            ProfileEvents::increment(ProfileEvents::ParquetPrefetcherReadRandomRead);
            break;
        case ReadMode::SeekAndRead:
        {
            std::lock_guard lock(read_mutex);
            // Seeking to a position above a previous setReadUntilPosition() confuses some of the
            // ReadBuffer implementations.
            reader->setReadUntilEnd();
            reader->seek(offset, SEEK_SET);
            reader->setReadUntilPosition(offset + n);
            nread = reader->readBig(to, n);
            ProfileEvents::increment(ProfileEvents::ParquetPrefetcherReadSeekAndRead);
            break;
        }
        case ReadMode::EntireFileIsInMemory:
            memcpy(to, entire_file.data() + offset, n);
            nread = n;
            ProfileEvents::increment(ProfileEvents::ParquetPrefetcherReadEntireFile);
            break;
    }
    if (nread != n)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Unexpected eof: offset {}, length {}, bytes read {}, expected file size {}", offset, n, nread, file_size);
}

void Prefetcher::retainTail(const char * data, size_t length, size_t file_offset)
{
    /// Nothing to save when the whole file is already in memory (getRangeData copies from
    /// entire_file, no read is issued). Only the read-issuing modes benefit.
    if (read_mode == ReadMode::EntireFileIsInMemory || length == 0)
        return;
    chassert(!ranges_finalized.load(std::memory_order_relaxed));
    chassert(file_offset + length <= file_size);
    retained_tail.assign(data, data + length);
    retained_tail_start = file_offset;
    retained_tail_end = file_offset + length;
}

PrefetchHandle Prefetcher::registerRange(size_t offset, size_t length, bool likely_to_be_used)
{
    chassert(!ranges_finalized.load(std::memory_order_relaxed));
    if (offset > file_size || length > file_size - offset)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Range out of bounds: offset {}, length {}, file size {}", offset, length, file_size);
    RequestState & req = requests.emplace_back();
    req.length = length;
    req.allow_incidental_read.store(likely_to_be_used || length < min_bytes_for_seek, std::memory_order_relaxed);
    range_sets[0].ranges.push_back(RangeState {.request = &req, .start = offset, .end = offset + length});
    return PrefetchHandle(&req);
}

void Prefetcher::finalizeRanges()
{
    bool already_finalized = ranges_finalized.exchange(true);  /// NOLINT(clang-analyzer-deadcode.DeadStores)
    chassert(!already_finalized);
    auto & ranges = range_sets[0].ranges;
    std::sort(ranges.begin(), ranges.end(), [](const RangeState & a, const RangeState & b)
        {
            return a.start < b.start;
        });
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        RequestState * req = ranges[i].request;
        const auto s = req->state.load(std::memory_order_relaxed);
        if (s == RequestState::State::HasRange)
            req->range_idx = i;
        else
            chassert(s == RequestState::State::Cancelled);
    }
}

void Prefetcher::startPrefetch(const std::vector<PrefetchHandle *> & requests_to_start, MemoryUsageDiff * diff)
{
    chassert(ranges_finalized.load(std::memory_order_relaxed));

    /// Allow the requested ranges can be coalesced with each other even if they're longer than
    /// min_bytes_for_seek.
    for (const PrefetchHandle * handle : requests_to_start)
        if (*handle)
            handle->request->allow_incidental_read.store(true, std::memory_order_relaxed);

    for (PrefetchHandle * handle : requests_to_start)
    {
        if (!*handle)
            continue;
        RequestState * req = handle->request;
        chassert(req);
        pickRangesAndCreateTaskIfNotExists(req, *handle, /*splitting=*/ false, 0, 0, std::unique_lock(mutex));
        chassert(req->state.load(std::memory_order_relaxed) == RequestState::State::HasTask);
        const Task * task = req->task;

        if (!handle->memory)
        {
            size_t memory_usage = static_cast<size_t>(static_cast<double>(req->length) * task->memory_amplification);
            handle->memory = MemoryUsageToken(memory_usage, diff);
        }
    }
}

std::vector<PrefetchHandle> Prefetcher::splitRange(
    PrefetchHandle request, const std::vector<std::pair</*global_offset*/ size_t, /*length*/ size_t>> & subranges, bool likely_to_be_used)
{
    chassert(ranges_finalized.load(std::memory_order_relaxed));
    chassert(std::is_sorted(subranges.begin(), subranges.end()));
    chassert(!subranges.empty());
    chassert(!request.memory); // prefetch not requested

    RequestState * parent_req = request.request;
    std::vector<PrefetchHandle> out_handles;

    {
        std::unique_lock lock(mutex);

        /// Allocate RequestState-s.
        out_handles.reserve(subranges.size());
        for (size_t i = 0; i < subranges.size(); ++i)
            out_handles.push_back(PrefetchHandle(&requests.emplace_back()));

        if (parent_req->state.load(std::memory_order_relaxed) == RequestState::State::HasRange)
        {
            auto & ranges = range_sets[parent_req->range_set_idx].ranges;
            const auto & range = ranges.at(parent_req->range_idx);

            size_t subrange_start = UINT64_MAX;
            size_t subrange_end = 0;
            for (const auto & [start, length] : subranges)
            {
                if (start < range.start || length > range.end - start)
                    throw Exception(ErrorCodes::INCORRECT_DATA, "Subrange out of bounds: [{}, {}) not in [{}, {})", start, start + length, range.start, range.end);
                subrange_start = std::min(subrange_start, start);
                subrange_end = std::max(subrange_end, start + length);
            }

            /// If the request is already short, don't split it, and try to coalesce with other ranges.
            if (range.length() < min_bytes_for_seek)
            {
                pickRangesAndCreateTaskIfNotExists(parent_req, request, /*splitting=*/ true, subrange_start, subrange_end, std::move(lock));
            }
            else
            {
                /// Normal case: actually split the range.
                ///
                /// We put the split ranges into a new universe instead of inserting into the middle
                /// of the existing RangeSet. This allows us to use a sorted array instead of a slow
                /// tree (e.g. std::map), but introduces a limitation: ranges produced by a split can only
                /// be coalesced among each other, not with other ranges (non-split ranges or ranges from
                /// other splits). (I just guessed that this would be a better tradeoff, didn't benchmark it.)
                size_t new_range_set_idx = range_sets.size();
                auto & new_ranges = range_sets.emplace_back().ranges;
                new_ranges.reserve(subranges.size());
                for (size_t i = 0; i < subranges.size(); ++i)
                {
                    const auto [start, length] = subranges[i];
                    RequestState * req = out_handles[i].request;
                    req->state.store(RequestState::State::HasRange, std::memory_order_relaxed);
                    req->allow_incidental_read.store(likely_to_be_used || length < min_bytes_for_seek);
                    req->range_set_idx = new_range_set_idx;
                    req->range_idx = i;
                    req->length = length;

                    RangeState & r = new_ranges.emplace_back();
                    r.start = start;
                    r.end = start + length;
                    r.request = req;
                }

                request.reset(/*diff=*/ nullptr);
                return out_handles;
            }
        }
    } // unlock mutex

    chassert(parent_req->state.load(std::memory_order_relaxed) == RequestState::State::HasTask);
    Task * task = parent_req->task;
    task->refcount.fetch_add(subranges.size());

    for (size_t i = 0; i < subranges.size(); ++i)
    {
        RequestState * req = out_handles[i].request;
        req->state.store(RequestState::State::HasTask, std::memory_order_relaxed);
        req->task = task;
        req->length = subranges[i].second;
        req->task_offset = subranges[i].first - task->offset;
    }

    request.reset(/*diff=*/ nullptr);
    return out_handles;
}

void Prefetcher::pickRangesAndCreateTaskIfNotExists(RequestState * initial_req, const PrefetchHandle &, bool splitting, size_t start_offset, size_t end_offset, std::unique_lock<std::mutex> lock)
{
    chassert(lock.owns_lock());

    /// Re-check state after locking mutex.
    switch (initial_req->state.load(std::memory_order_acquire))
    {
        case RequestState::State::Cancelled: // impossible, we hold a PrefetchHandle
            chassert(false);
            break;
        case RequestState::State::HasRange:
            break;
        case RequestState::State::HasTask:
            /// Another thread created a task while we were locking the mutex.
            return;
    }
    size_t range_set_idx = initial_req->range_set_idx;
    size_t range_idx = initial_req->range_idx;
    auto & ranges = range_sets.at(range_set_idx).ranges;
    chassert(ranges.at(range_idx).request == initial_req);
    if (!splitting)
    {
        start_offset = ranges[range_idx].start;
        end_offset = ranges[range_idx].end;
    }

    /// If this range is fully contained in the retained footer tail, serve it directly from that
    /// in-memory copy - no read, no coalescing. Build a one-off Task already in the Done state whose
    /// cached_region points into `retained_tail`; getRangeData's zero-copy path then returns the
    /// span. This eliminates the redundant Column/Offset Index read (their bytes were already
    /// fetched for the footer). `retained_tail` outlives all handles (it is a Prefetcher member), so
    /// the region needs no keep-alive handle.
    if (retained_tail_end > retained_tail_start
        && start_offset >= retained_tail_start && end_offset <= retained_tail_end)
    {
        Task & task = tasks.emplace_back();
        task.offset = start_offset;
        task.length = end_offset - start_offset;
        task.memory_amplification = 1;
        task.refcount.store(1);
        task.state.store(Task::State::Done);
        task.cached_region = Task::CachedReadRegion{
            .handle = {},
            .data = retained_tail.data() + (start_offset - retained_tail_start),
            .size = end_offset - start_offset,
            .file_offset = start_offset};

        initial_req->task = &task;
        initial_req->task_offset = 0;
        RequestState::State s = RequestState::State::HasRange;
        bool ok = initial_req->state.compare_exchange_strong(s, RequestState::State::HasTask);
        chassert(ok); // we hold a PrefetchHandle, so it cannot be Cancelled here
        ProfileEvents::increment(ProfileEvents::ParquetPrefetcherServedFromRetainedTail);
        return; // lock released by unique_lock destructor; task is already Done, nothing to schedule
    }

    /// Try to extend the task's range in both directions to cover more request ranges, as long
    /// as gaps between them are shorter than min_bytes_for_seek.

    size_t start_idx = range_idx;
    size_t end_idx = range_idx + 1;
    size_t total_length_of_covered_ranges = end_offset - start_offset;

    /// EXPERIMENTAL read alignment: keep this coalesced task within a single boundary window
    /// [part_lo, part_hi) - the window containing the initial requested range's start - so one read
    /// never straddles a boundary (an AWS best practice for multipart-uploaded objects). Boundaries
    /// come from the real per-file multipart layout (multipart_part_offsets) when known, else from a
    /// fixed grid (read_alignment_stride). This only limits *coalescing* across a boundary; a single
    /// requested range larger than a window is left as is (would require splitRange).
    size_t part_lo = 0;
    size_t part_hi = std::numeric_limits<size_t>::max();
    bool alignment_active = false;
    if (!multipart_part_offsets.empty())
    {
        auto it = std::upper_bound(multipart_part_offsets.begin(), multipart_part_offsets.end(), start_offset);
        part_hi = (it == multipart_part_offsets.end()) ? file_size : *it;
        part_lo = (it == multipart_part_offsets.begin()) ? 0 : *std::prev(it);
        alignment_active = true;
    }
    else if (read_alignment_stride > 0)
    {
        part_lo = start_offset / read_alignment_stride * read_alignment_stride;
        part_hi = part_lo + read_alignment_stride;
        alignment_active = true;
    }

    /// Anti-fragmentation: if the aligned segment ahead of the initial range would be smaller than
    /// read_alignment_min_bytes, cutting here would emit a tiny read - allow the straddle instead.
    if (alignment_active && read_alignment_min_bytes > 0 && part_hi - start_offset < read_alignment_min_bytes)
    {
        alignment_active = false;
        ProfileEvents::increment(ProfileEvents::ParquetPrefetcherAlignmentSkippedSmall);
    }
    bool part_boundary_constrained = false;

    /// Go left.
    size_t initial_offset = start_offset;
    for (size_t idx = range_idx; idx > 0; --idx)
    {
        const RangeState & r = ranges[idx - 1];
        if (r.end + min_bytes_for_seek <= start_offset || // short gap
            r.start + bytes_per_read_task <= initial_offset || // task not too big
            !r.request->allow_incidental_read.load(std::memory_order_relaxed)) // range wants to be coalesced
            break;

        if (alignment_active && r.start < part_lo) // would cross below this boundary window
        {
            part_boundary_constrained = true;
            break;
        }

        const auto s = r.request->state.load(std::memory_order_relaxed);
        if (s == RequestState::State::HasRange)
        {
            /// Include this range in the task.
            start_idx = idx - 1;
            total_length_of_covered_ranges += r.length();
            start_offset = std::min(start_offset, r.start);
            /// A range found to the left may extend past the current end (e.g. when ranges
            /// share the same start offset but have different lengths, and the sort placed
            /// the longer range first). We must extend end_offset to cover it.
            end_offset = std::max(end_offset, r.end);
        }
        else if (s != RequestState::State::Cancelled)
        {
            /// Range already has a task. No need to scan further, the other task already did that.
            chassert(s == RequestState::State::HasTask);
            break;
        }
        else
        {
            /// Keep going past a cancelled range, but don't update start_idx/start_offset until we
            /// hit a non-cancelled range.
        }
    }

    /// Go right.
    initial_offset = end_offset;
    for (size_t idx = range_idx + 1; idx < ranges.size(); ++idx)
    {
        const RangeState & r = ranges[end_idx];
        if (end_offset + min_bytes_for_seek <= r.start ||
            initial_offset + bytes_per_read_task <= r.end ||
            !r.request->allow_incidental_read.load(std::memory_order_relaxed))
            break;

        if (alignment_active && r.end > part_hi) // would cross above this boundary window
        {
            part_boundary_constrained = true;
            break;
        }

        const auto s = r.request->state.load(std::memory_order_relaxed);
        if (s == RequestState::State::HasRange)
        {
            end_idx = idx + 1;
            total_length_of_covered_ranges += r.length();
            end_offset = std::max(end_offset, r.end);
            /// (This currently doesn't do anything because ranges are sorted by `start`, but why not.)
            start_offset = std::min(start_offset, r.start);
        }
        else if (s != RequestState::State::Cancelled)
        {
            chassert(s == RequestState::State::HasTask);
            break;
        }
    }

    if (part_boundary_constrained)
        ProfileEvents::increment(ProfileEvents::ParquetPrefetcherPartAlignedTasks);

    /// Create task.
    Task & task = tasks.emplace_back();
    task.offset = start_offset;
    task.length = end_offset - task.offset;
    task.memory_amplification = 1. * static_cast<double>(task.length) / static_cast<double>(total_length_of_covered_ranges);
    size_t initial_refcount = end_idx - start_idx + 1;
    task.refcount.store(initial_refcount);

    size_t actual_refcount = 0;
    for (size_t idx = start_idx; idx < end_idx; ++idx)
    {
        const RangeState & range = ranges[idx];
        RequestState * req = range.request;
        req->task = &task;
        req->task_offset = range.start - task.offset;

        RequestState::State s = RequestState::State::HasRange;
        if (req->state.compare_exchange_strong(s, RequestState::State::HasTask))
            actual_refcount += 1;
        else
            chassert(s == RequestState::State::Cancelled);
    }

    chassert(actual_refcount > 0);
    decreaseTaskRefcount(&task, initial_refcount - actual_refcount);

    lock.unlock();

    scheduleTask(&task);
}

void Prefetcher::decreaseTaskRefcount(Task * task, size_t amount)
{
    size_t c = task->refcount.fetch_sub(amount, std::memory_order_acq_rel);
    chassert(c >= amount);
    if (c != amount)
        return;

    if (task->state.exchange(Task::State::Deallocated) != Task::State::Running)
    {
        task->buf = {};
        task->cached_region.reset();
    }
}

void Prefetcher::scheduleTask(Task * task)
{
    if (parser_shared_resources && !parser_shared_resources->io_runner.isDisabled())
        parser_shared_resources->io_runner([this, task, _shutdown = shutdown]
            {
                std::shared_lock shutdown_lock(*_shutdown, std::try_to_lock);
                if (!shutdown_lock.owns_lock())
                    return;
                runTask(task);
            });
}

std::span<const char> Prefetcher::getRangeData(const PrefetchHandle & request)
{
    const RequestState * req = request.request;
    chassert(req->state == RequestState::State::HasTask);
    Task * task = req->task;
    Task::State s = task->state.load(std::memory_order_acquire);
    if (s == Task::State::Scheduled || s == Task::State::Running)
    {
        Stopwatch wait_time;

        if (s == Task::State::Scheduled)
        {
            s = runTask(task);
            chassert(s != Task::State::Scheduled);
        }

        if (s == Task::State::Running) // (not `else`, the runTask above may return Running)
        {
            task->completion.wait();
            s = task->state.load();
        }

        ProfileEvents::increment(ProfileEvents::ParquetFetchWaitTimeMicroseconds, wait_time.elapsedMicroseconds());
    }
    if (s == Task::State::Exception)
        rethrowException(task);
    chassert(s == Task::State::Done);

    if (task->cached_region.has_value())
    {
        /// Zero-copy path: serve data directly from cache cells.
        size_t req_file_offset = task->offset + req->task_offset;

        const auto & r = task->cached_region.value();
        chassert(r.file_offset <= req_file_offset);
        chassert(r.file_offset + r.size >= req_file_offset + req->length);

        size_t offset_in_region = req_file_offset - r.file_offset;

        return std::span(r.data + offset_in_region, req->length);
    }

    chassert(req->task_offset + req->length <= task->buf.size());
    return std::span(task->buf.data() + req->task_offset, req->length);
}

Prefetcher::Task::State Prefetcher::runTask(Task * task)
{
    auto s = Task::State::Scheduled;
    if (!task->state.compare_exchange_strong(s, Task::State::Running))
        return s;
    auto final_state = Task::State::Done;
    try
    {
        /// When the reader supports zero-copy cached reads, get retained cache cells
        /// instead of allocating a buffer and copying data into it.
        if (read_mode == ReadMode::RandomRead && reader->supportsReadAtRetainCells() && task->length > 0)
        {
            auto cached_regions = reader->readBigAtRetainCells(task->length, task->offset);
            chassert(!cached_regions.empty());

            if (cached_regions.size() == 1)
            {
                /// We got lucky and the Task's range is all in one cache cell. Zero-copy it.
                auto & cr = cached_regions[0];
                task->cached_region = Task::CachedReadRegion{
                    .handle = std::move(cr.handle),
                    .data = cr.data,
                    .size = cr.size,
                    .file_offset = cr.file_offset,
                };
            }
            else
            {
                /// If the data spans multiple cache blocks, pre-assemble it into task->buf now
                /// (on the single-threaded producer side) to avoid a data race in getRangeData,
                /// where multiple consumer threads could try to lazily populate task->buf concurrently.
                if (cached_regions.size() > 1)
                {
                    task->buf.resize(task->length);
                    size_t copied = 0;
                    for (const auto & region : cached_regions)
                    {
                        memcpy(task->buf.data() + copied, region.data, region.size);
                        copied += region.size;
                    }
                    chassert(copied == task->length);
                }
            }

            ProfileEvents::increment(ProfileEvents::ParquetPrefetcherReadRandomRead);
        }
        else
        {
            task->buf.resize(task->length);
            readSync(task->buf.data(), task->length, task->offset);
        }
        total_bytes_read.fetch_add(task->length, std::memory_order_relaxed);
    }
    catch (...)
    {
        final_state = Task::State::Exception;
        std::lock_guard lock(exception_mutex);
        task->exception = std::current_exception();
    }

    s = Task::State::Running;
    if (task->state.compare_exchange_strong(s, final_state))
    {
        s = final_state;
    }
    else
    {
        chassert(s == Task::State::Deallocated);
        task->buf = {};
        task->cached_region.reset();
    }

    task->completion.notify();

    return s;
}

void Prefetcher::rethrowException(Task * task)
{
    std::lock_guard lock(exception_mutex);
    /// Each waiter gets a private copy so callers can safely mutate it (addMessage())
    std::rethrow_exception(copyMutableException(task->exception));
}

PrefetchHandle::PrefetchHandle(RequestState * request_) : request(request_) {}

PrefetchHandle::PrefetchHandle(PrefetchHandle && rhs) noexcept
{
    *this = std::move(rhs);
}

PrefetchHandle & PrefetchHandle::operator=(PrefetchHandle && rhs) noexcept
{
    // Shouldn't assign to nonempty handles because deallocation wouldn't be recorded in MemoryUsageDiff.
    chassert(!memory);

    reset(nullptr);
    request = std::exchange(rhs.request, nullptr);
    return *this;
}

PrefetchHandle::~PrefetchHandle()
{
    reset(nullptr);
}

void PrefetchHandle::reset(MemoryUsageDiff * diff)
{
    if (!request)
        return;

    if (diff)
        memory.reset(diff);

    if (request->state.exchange(RequestState::State::Cancelled) == RequestState::State::HasTask)
        Prefetcher::decreaseTaskRefcount(request->task, 1);

    request = nullptr;
}

}
