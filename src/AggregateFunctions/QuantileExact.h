#pragma once

#include <IO/ReadBuffer.h>
#include <IO/VarInt.h>
#include <IO/WriteBuffer.h>
#include <Common/NaNUtils.h>
#include <Common/PODArray.h>
#include <Common/VectorWithMemoryTracking.h>
#include <base/Decimal_fwd.h>
#include <base/extended_types.h>
#include <base/sort.h>
#include <base/types.h>

#include <algorithm>
#include <bit>
#include <memory>

#define QUANTILE_EXACT_MAX_ARRAY_SIZE 1'000'000'000


namespace DB
{
struct Settings;

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int BAD_ARGUMENTS;
    extern const int TOO_LARGE_ARRAY_SIZE;
}

/// Interpolation delta `b - a` as Float64. Integral Value subtracts in Int128 (the widest
/// integral Value is (U)Int64, so the difference is exact and cannot overflow) before casting;
/// floating Value subtracts in Float64.
template <typename Value>
static inline Float64 quantileExactInterpolationDelta(const Value & a, const Value & b)
{
    if constexpr (is_integer<Value>)
        return static_cast<Float64>(static_cast<Int128>(b) - static_cast<Int128>(a));
    else
        return static_cast<Float64>(b) - static_cast<Float64>(a);
}


namespace QuantileExactDetail
{

/// A request to find the value at position `rank` of the sorted data and to write it into `result[index]`.
struct Request
{
    size_t rank;
    size_t index;
};

using Requests = PODArrayWithStackMemory<Request, 4 * sizeof(Request)>;

inline void makeSortedRequests(const size_t * ranks, size_t num_ranks, Requests & requests)
{
    requests.resize(num_ranks);
    for (size_t i = 0; i < num_ranks; ++i)
        requests[i] = {ranks[i], i};
    std::sort(requests.begin(), requests.end(), [](const Request & a, const Request & b) { return a.rank < b.rank; });
}

/// Finds the values at the requested positions of the sorted data, reordering `data` in place.
/// `requests` must be sorted by rank, every rank must be less than `size`.
template <typename Value>
void selectInPlace(Value * data, size_t size, const Request * requests, size_t num_requests, Value * result)
{
    size_t prev = 0;
    for (size_t i = 0; i < num_requests; ++i)
    {
        size_t rank = requests[i].rank;
        if (i == 0)
        {
            ::nth_element(data, data + rank, data + size);
        }
        else if (rank == prev + 1)
        {
            /// Everything after `prev` is not less than data[prev], so the next value is the minimum of the rest.
            /// It is a common case for interpolating quantiles.
            std::iter_swap(data + rank, std::min_element(data + rank, data + size));
        }
        else if (rank != prev)
        {
            ::nth_element(data + prev + 1, data + rank, data + size);
        }
        result[requests[i].index] = data[rank];
        prev = rank;
    }
}

/** Maps values to unsigned integer keys, so that the order of keys is the same as the order of values.
  * The only exception is that -0.0 is less than +0.0, although these values are equal.
  * It is used for radix selection. Not supported for types wider than 64 bits.
  */
template <typename Value>
struct RadixKey
{
    static constexpr bool supported = false;
    using Key = uint64_t;
    static constexpr size_t bits = 64;
};

/// Int8 is `_BitInt(8)`, which is not `std::is_integral` for some standard libraries.
template <typename Value>
requires ((is_integer<Value> || std::is_same_v<Value, Int8> || is_decimal<Value> || std::is_same_v<Value, Float32> || std::is_same_v<Value, Float64>)
    && sizeof(Value) <= 8)
struct RadixKey<Value>
{
    static constexpr bool supported = true;

    using Key = std::conditional_t<sizeof(Value) == 1, uint8_t,
        std::conditional_t<sizeof(Value) == 2, uint16_t,
        std::conditional_t<sizeof(Value) == 4, uint32_t, uint64_t>>>;

    static_assert(sizeof(Key) == sizeof(Value));

    static constexpr size_t bits = sizeof(Key) * 8;
    static constexpr Key sign_bit = static_cast<Key>(Key(1) << (bits - 1));

    static Key toKey(const Value & x)
    {
        if constexpr (std::is_floating_point_v<Value>)
        {
            /// Negative values are reversed, positive values go after them.
            Key u = std::bit_cast<Key>(x);
            return (u & sign_bit) ? static_cast<Key>(~u) : static_cast<Key>(u | sign_bit);
        }
        else if constexpr (is_decimal<Value>)
            return static_cast<Key>(std::bit_cast<Key>(x.value) ^ sign_bit);
        else if constexpr (static_cast<Value>(-1) < static_cast<Value>(0))
            return static_cast<Key>(std::bit_cast<Key>(x) ^ sign_bit);
        else
            return std::bit_cast<Key>(x);
    }

    static Value fromKey(Key key)
    {
        if constexpr (std::is_floating_point_v<Value>)
            return std::bit_cast<Value>((key & sign_bit) ? static_cast<Key>(key ^ sign_bit) : static_cast<Key>(~key));
        else if constexpr (is_decimal<Value>)
            return Value(std::bit_cast<typename Value::NativeType>(static_cast<Key>(key ^ sign_bit)));
        else if constexpr (static_cast<Value>(-1) < static_cast<Value>(0))
            return std::bit_cast<Value>(static_cast<Key>(key ^ sign_bit));
        else
            return std::bit_cast<Value>(key);
    }
};


/** Storage of the values for `quantileExact` functions.
  *
  * Small number of values is kept in one array (`tail`) with inline memory for a few values, as it was always done.
  * When the array reaches `chunk_size` values, its memory becomes an immutable chunk (without copying),
  * and the next values go to the new array. So, large states consist of fixed-size chunks and the tail.
  *
  * It avoids reallocations of large arrays, which copy all the data and require 2x memory at the moment of growth,
  * and allows merging without copying: the full chunks of the merged state (rhs) are shared,
  * and only its tail is copied. The full chunks are never modified, and the tail is never shared,
  * so it is safe to add values to the merged state later, or to destroy the states in any order.
  *
  * Quantiles of the states without full chunks are calculated with `nth_element` in place, as before.
  * Otherwise radix selection is used, which does not modify the data:
  * several passes over the data build histograms of the next 16 bits of the order-preserving keys,
  * narrowing down the range of keys until the number of candidate values is small enough
  * to be copied and processed by `nth_element`.
  * For types without the radix key all the values are copied into a contiguous array.
  *
  * `chunk_bytes` is a compromise between the overhead per chunk and the memory wasted in the tail. Must be a power of two.
  * `max_bucket_bytes` is the maximum size of values to copy into a temporary array for `nth_element`
  * (or 1/16 of all values if it is larger).
  */
template <typename Value, size_t chunk_bytes = 256 * 1024, size_t max_bucket_bytes = 32 * 1024 * 1024>
class QuantileExactChunks
{
public:
    static_assert(std::has_single_bit(chunk_bytes));
    static constexpr size_t chunk_size = std::max<size_t>(1, chunk_bytes / sizeof(Value));
    static constexpr size_t max_bucket_size = std::max<size_t>(1, max_bucket_bytes / sizeof(Value));

    /// The memory will be allocated to several elements at once, so that the state occupies 64 bytes.
    static constexpr size_t bytes_in_arena = 64 - sizeof(PODArray<Value>) - sizeof(void *);
    using Array = PODArrayWithStackMemory<Value, bytes_in_arena>;
    using ChunkPtr = std::shared_ptr<const Array>;
    using Chunks = VectorWithMemoryTracking<ChunkPtr>;

    size_t size() const { return (chunks ? chunks->size() * chunk_size : 0) + tail.size(); }

    void push(const Value & x)
    {
        tail.push_back(x);
        if (unlikely(tail.size() == chunk_size))
            flushTail();
    }

    void append(const Value * from, size_t count)
    {
        while (count)
        {
            size_t to_copy = std::min(count, chunk_size - tail.size());
            tail.insert(from, from + to_copy);
            if (tail.size() == chunk_size)
                flushTail();
            from += to_copy;
            count -= to_copy;
        }
    }

    /// Shares the full chunks of `rhs` and copies the values from its tail.
    void merge(const QuantileExactChunks & rhs)
    {
        if (rhs.chunks && !rhs.chunks->empty())
        {
            if (!chunks)
                chunks = std::make_unique<Chunks>();
            chunks->insert(chunks->end(), rhs.chunks->begin(), rhs.chunks->end());
        }
        append(rhs.tail.data(), rhs.tail.size());
    }

    /// Calls `func(const Value * data, size_t size)` for the full chunks and the tail.
    template <typename Func>
    void forEachChunk(Func && func) const
    {
        if (chunks)
            for (const auto & chunk : *chunks)
                func(chunk->data(), chunk_size);
        if (!tail.empty())
            func(tail.data(), tail.size());
    }

    /// The format is the number of values and the values.
    void write(WriteBuffer & buf) const
    {
        writeVarUInt(size(), buf);
        forEachChunk([&](const Value * data, size_t count) { buf.write(reinterpret_cast<const char *>(data), count * sizeof(Value)); });
    }

    void read(ReadBuffer & buf)
    {
        size_t count = 0;
        readVarUInt(count, buf);
        if (unlikely(count > QUANTILE_EXACT_MAX_ARRAY_SIZE))
            throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE,
                            "Too large array size (maximum: {})", QUANTILE_EXACT_MAX_ARRAY_SIZE);

        chunks.reset();
        tail.clear();
        while (count)
        {
            size_t to_read = std::min(count, chunk_size);
            tail.resize(to_read);
            buf.readStrict(reinterpret_cast<char *>(tail.data()), to_read * sizeof(Value));
            if (tail.size() == chunk_size)
                flushTail();
            count -= to_read;
        }
    }

    /// Writes into `result[i]` the value at position `ranks[i]` in the sorted data. Every rank must be less than `size()`.
    /// Can reorder the values in the tail, but does not modify the full chunks.
    void select(const size_t * ranks, size_t num_ranks, Value * result)
    {
        if (!chunks)
        {
            /// The most common case: a single value or a small state.
            if (num_ranks == 1)
            {
                ::nth_element(tail.begin(), tail.begin() + ranks[0], tail.end());
                result[0] = tail[ranks[0]];
                return;
            }

            Requests requests;
            makeSortedRequests(ranks, num_ranks, requests);
            selectInPlace(tail.data(), tail.size(), requests.data(), requests.size(), result);
            return;
        }

        Requests requests;
        makeSortedRequests(ranks, num_ranks, requests);

        if constexpr (RadixKey<Value>::supported)
        {
            /// Allow to copy up to 1/16 of the values to reduce the number of passes over large data.
            size_t max_copy_size = std::max(max_bucket_size, size() / 16);
            if (size() > max_copy_size)
            {
                radixSelect(0, RadixKey<Value>::bits, max_copy_size, requests.data(), requests.size(), result);
                return;
            }
        }

        PODArray<Value> copy;
        copy.reserve_exact(size());
        forEachChunk([&](const Value * data, size_t count) { copy.insert(data, data + count); });
        selectInPlace(copy.data(), copy.size(), requests.data(), requests.size(), result);
    }

private:
    /// Contains less than `chunk_size` values. Owned by this object, never shared.
    Array tail;
    /// Each chunk contains exactly `chunk_size` values. Immutable, can be shared with other states.
    /// Allocated on demand, so that small states are small.
    std::unique_ptr<Chunks> chunks;

    /// Moves the memory of the full tail into a new chunk.
    void flushTail()
    {
        if (!chunks)
            chunks = std::make_unique<Chunks>();
        if (chunks->size() == chunks->capacity())
            chunks->reserve(std::max<size_t>(16, chunks->size() * 2));
        auto chunk = std::make_shared<Array>();
        /// The full tail uses the heap memory of `chunk_bytes`, which is taken by the chunk without copying.
        chunk->swap(tail);
        chunks->push_back(std::move(chunk));
        tail.reserve(chunk_size);
    }

    using Key = typename RadixKey<Value>::Key;
    static constexpr size_t radix_bits = std::min<size_t>(16, RadixKey<Value>::bits);

    /// Calls `func(Key key, const Value & value)` for every value with the key that has the same higher (`bits` - `shift`) bits as `prefix`.
    template <typename Func>
    void forEachKeyWithPrefix(Key prefix, size_t shift, Func && func) const
    {
        if (shift == RadixKey<Value>::bits)
        {
            forEachChunk([&](const Value * data, size_t count)
            {
                for (size_t i = 0; i < count; ++i)
                    func(RadixKey<Value>::toKey(data[i]), data[i]);
            });
        }
        else
        {
            forEachChunk([&](const Value * data, size_t count)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    Key key = RadixKey<Value>::toKey(data[i]);
                    if ((key >> shift) == (prefix >> shift))
                        func(key, data[i]);
                }
            });
        }
    }

    /// Builds the histogram of the highest `bits` - `new_shift` bits of the keys of all values.
    /// It is the most expensive pass, because every value increments a counter. Several histograms with 32-bit counters are used
    /// to avoid stalls on the increments of the same counter, which happen when many values have the same higher bits.
    void buildFirstHistogram(size_t new_shift, size_t digit_mask, size_t * histogram) const
    {
        static constexpr size_t num_histograms = 4;
        /// No counter can overflow before flush.
        static constexpr size_t flush_interval = 1ULL << 31;
        static_assert(flush_interval + chunk_size <= std::numeric_limits<UInt32>::max());

        const size_t num_buckets = digit_mask + 1;
        VectorWithMemoryTracking<UInt32> partial(num_histograms * num_buckets);
        size_t processed = 0;

        auto flush = [&]
        {
            for (size_t i = 0; i < num_histograms; ++i)
                for (size_t bucket = 0; bucket < num_buckets; ++bucket)
                    histogram[bucket] += partial[i * num_buckets + bucket];
            std::fill(partial.begin(), partial.end(), 0);
            processed = 0;
        };

        forEachChunk([&](const Value * data, size_t count)
        {
            size_t i = 0;
            for (; i + num_histograms <= count; i += num_histograms)
                for (size_t j = 0; j < num_histograms; ++j)
                    ++partial[j * num_buckets + ((RadixKey<Value>::toKey(data[i + j]) >> new_shift) & digit_mask)];
            for (; i < count; ++i)
                ++partial[(RadixKey<Value>::toKey(data[i]) >> new_shift) & digit_mask];

            processed += count;
            if (processed >= flush_interval)
                flush();
        });

        flush();
    }

    /// Processes the requests for the values with keys that have the same higher (`bits` - `shift`) bits as `prefix`.
    /// Ranks of the requests are relative to the set of these values.
    void radixSelect(Key prefix, size_t shift, size_t max_copy_size, Request * requests, size_t num_requests, Value * result) const
    {
        const size_t digit_bits = std::min(radix_bits, shift);
        const size_t new_shift = shift - digit_bits;
        const size_t num_buckets = 1ULL << digit_bits;
        const size_t digit_mask = num_buckets - 1;

        VectorWithMemoryTracking<size_t> histogram(num_buckets);
        if (shift == RadixKey<Value>::bits)
            buildFirstHistogram(new_shift, digit_mask, histogram.data());
        else
            forEachKeyWithPrefix(prefix, shift, [&](Key key, const Value &) { ++histogram[(key >> new_shift) & digit_mask]; });

        /// Find the bucket for every request and make the ranks relative to the bucket.
        VectorWithMemoryTracking<size_t> request_buckets(num_requests);
        {
            size_t bucket = 0;
            size_t values_before_bucket = 0;
            for (size_t i = 0; i < num_requests; ++i)
            {
                while (requests[i].rank >= values_before_bucket + histogram[bucket])
                {
                    values_before_bucket += histogram[bucket];
                    ++bucket;
                }
                request_buckets[i] = bucket;
                requests[i].rank -= values_before_bucket;
            }
        }

        /// Groups of requests with the same bucket, which are small enough to be processed by copying the values.
        struct Group
        {
            size_t bucket;
            size_t begin;
            size_t end;
        };
        VectorWithMemoryTracking<Group> groups_to_copy;

        for (size_t begin = 0; begin < num_requests;)
        {
            size_t bucket = request_buckets[begin];
            size_t end = begin + 1;
            while (end < num_requests && request_buckets[end] == bucket)
                ++end;

            Key bucket_prefix = static_cast<Key>(prefix | (static_cast<Key>(bucket) << new_shift));
            if (new_shift == 0)
            {
                /// All the values in the bucket are the same.
                Value value = RadixKey<Value>::fromKey(bucket_prefix);
                for (size_t i = begin; i < end; ++i)
                    result[requests[i].index] = value;
            }
            else if (histogram[bucket] > max_copy_size)
            {
                radixSelect(bucket_prefix, new_shift, max_copy_size, requests + begin, end - begin, result);
            }
            else
            {
                groups_to_copy.push_back({bucket, begin, end});
            }

            begin = end;
        }

        /// Copy the values of several buckets in one pass, but no more than `max_copy_size` values in total.
        for (size_t first_group = 0; first_group < groups_to_copy.size();)
        {
            size_t last_group = first_group;
            size_t values_to_copy = 0;
            while (last_group < groups_to_copy.size()
                && (last_group == first_group || values_to_copy + histogram[groups_to_copy[last_group].bucket] <= max_copy_size))
            {
                values_to_copy += histogram[groups_to_copy[last_group].bucket];
                ++last_group;
            }

            /// Bucket number -> position in `buckets` + 1, zero for buckets that are not copied.
            VectorWithMemoryTracking<UInt32> bucket_to_position(num_buckets);
            VectorWithMemoryTracking<PODArray<Value>> buckets(last_group - first_group);
            for (size_t i = first_group; i < last_group; ++i)
            {
                bucket_to_position[groups_to_copy[i].bucket] = static_cast<UInt32>(i - first_group + 1);
                buckets[i - first_group].reserve_exact(histogram[groups_to_copy[i].bucket]);
            }

            forEachKeyWithPrefix(prefix, shift, [&](Key key, const Value & value)
            {
                if (UInt32 position = bucket_to_position[(key >> new_shift) & digit_mask])
                    buckets[position - 1].push_back(value);
            });

            for (size_t i = first_group; i < last_group; ++i)
            {
                auto & bucket = buckets[i - first_group];
                const auto & group = groups_to_copy[i];
                selectInPlace(bucket.data(), bucket.size(), requests + group.begin, group.end - group.begin, result);
            }

            first_group = last_group;
        }
    }
};

}


template <typename Value, typename Derived>
struct QuantileExactBase
{
    QuantileExactDetail::QuantileExactChunks<Value> values;

    void add(const Value & x)
    {
        /// We must skip NaNs as they are not compatible with comparison sorting.
        if (!isNaN(x))
            values.push(x);
    }

    template <typename Weight>
    void add(const Value &, const Weight &)
    {
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method add with weight is not implemented for QuantileExact");
    }

    void merge(const QuantileExactBase & rhs) { values.merge(rhs.values); }

    void serialize(WriteBuffer & buf) const { values.write(buf); }

    void deserialize(ReadBuffer & buf) { values.read(buf); }

    Value get(Float64 level)
    {
        auto derived = static_cast<Derived*>(this);
        return derived->getImpl(level);
    }

    void getMany(const Float64 * levels, const size_t * indices, size_t size, Value * result)
    {
        auto derived = static_cast<Derived*>(this);
        return derived->getManyImpl(levels, indices, size, result);
    }

protected:
    size_t size() const { return values.size(); }

    /// Returns the value at position `rank` in the sorted data. The rank must be less than `size()`.
    Value select(size_t rank)
    {
        Value result{};
        values.select(&rank, 1, &result);
        return result;
    }

    /// Writes into `result[i]` the value at position `ranks[i]` in the sorted data. Every rank must be less than `size()`.
    void select(const size_t * ranks, size_t num_ranks, Value * result) { values.select(ranks, num_ranks, result); }
};

/** Calculates quantile by collecting all values into array
  *  and applying n-th element (introselect) algorithm for the resulting array.
  * Large number of values is collected into chunks and radix selection is used instead, see `QuantileExactChunks`.
  *
  * It uses O(N) memory and it is very inefficient in case of high amount of identical values.
  * But it is very CPU efficient for not large datasets.
  */
template <typename Value>
struct QuantileExact : QuantileExactBase<Value, QuantileExact<Value>>
{
    using Base = QuantileExactBase<Value, QuantileExact<Value>>;
    using Base::size;
    using Base::select;

    static size_t getIndex(Float64 level, size_t size)
    {
        return level < 1 ? static_cast<size_t>(level * static_cast<Float64>(size)) : (size - 1);
    }

    // Get the value of the `level` quantile. The level must be between 0 and 1.
    Value getImpl(Float64 level)
    {
        if (size())
            return select(getIndex(level, size()));

        return std::numeric_limits<Value>::quiet_NaN();
    }

    /// Get the `size` values of `levels` quantiles. Write `size` results starting with `result` address.
    /// indices - an array of index levels such that the corresponding elements will go in ascending order.
    void getManyImpl(const Float64 * levels, const size_t * /* indices */, size_t num_levels, Value * result)
    {
        if (size())
        {
            PODArrayWithStackMemory<size_t, 4 * sizeof(size_t)> ranks(num_levels);
            for (size_t i = 0; i < num_levels; ++i)
                ranks[i] = getIndex(levels[i], size());
            select(ranks.data(), num_levels, result);
        }
        else
        {
            for (size_t i = 0; i < num_levels; ++i)
                result[i] = Value();
        }
    }
};

/// Common part of QuantileExactExclusive and QuantileExactInclusive: interpolation between two adjacent values.
template <typename Value, typename Derived>
struct QuantileExactInterpolated : public QuantileExact<Value>
{
    using QuantileExact<Value>::size;
    using QuantileExact<Value>::select;

    /// Get the value of the `level` quantile.
    Float64 getFloat(Float64 level)
    {
        Float64 result = 0;
        getManyFloat(&level, nullptr, 1, &result);
        return result;
    }

    void getManyFloat(const Float64 * levels, const size_t * /* indices */, size_t num_levels, Float64 * result)
    {
        size_t count = size();
        if (!count)
        {
            for (size_t i = 0; i < num_levels; ++i)
                result[i] = std::numeric_limits<Float64>::quiet_NaN();
            return;
        }

        /// For every level we need two adjacent values: at positions n - 1 and n.
        /// If n is out of bounds, the maximum or the minimum is the result.
        PODArrayWithStackMemory<size_t, 2 * sizeof(size_t)> ranks(2 * num_levels);
        PODArrayWithStackMemory<Float64, 2 * sizeof(Float64)> hs(num_levels);
        for (size_t i = 0; i < num_levels; ++i)
        {
            Float64 h = Derived::getPosition(levels[i], count);
            hs[i] = h;
            auto n = static_cast<size_t>(h);

            if (n >= count)
                ranks[2 * i] = ranks[2 * i + 1] = count - 1;
            else if (n < 1)
                ranks[2 * i] = ranks[2 * i + 1] = 0;
            else
            {
                ranks[2 * i] = n - 1;
                ranks[2 * i + 1] = n;
            }
        }

        PODArrayWithStackMemory<Value, 2 * sizeof(Value)> values(2 * num_levels);
        select(ranks.data(), 2 * num_levels, values.data());

        for (size_t i = 0; i < num_levels; ++i)
        {
            if (ranks[2 * i] == ranks[2 * i + 1])
                result[i] = static_cast<Float64>(values[2 * i]);
            else
                result[i] = static_cast<Float64>(values[2 * i])
                    + (hs[i] - static_cast<Float64>(ranks[2 * i + 1])) * quantileExactInterpolationDelta(values[2 * i], values[2 * i + 1]);
        }
    }
};

/// QuantileExactExclusive is equivalent to Excel PERCENTILE.EXC, R-6, SAS-4, SciPy-(0,0)
template <typename Value>
struct QuantileExactExclusive : public QuantileExactInterpolated<Value, QuantileExactExclusive<Value>>
{
    /// The level must be between 0 and 1 excluding bounds.
    static Float64 getPosition(Float64 level, size_t size)
    {
        if (level == 0. || level == 1.)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "QuantileExactExclusive cannot interpolate for the percentiles 1 and 0");

        return level * static_cast<Float64>(size + 1);
    }
};

/// QuantileExactInclusive is equivalent to Excel PERCENTILE and PERCENTILE.INC, R-7, SciPy-(1,1)
template <typename Value>
struct QuantileExactInclusive : public QuantileExactInterpolated<Value, QuantileExactInclusive<Value>>
{
    /// The level must be between 0 and 1 including bounds.
    static Float64 getPosition(Float64 level, size_t size)
    {
        return level * static_cast<Float64>(size - 1) + 1;
    }
};

/// Common part of QuantileExactLow and QuantileExactHigh.
template <typename Value, typename Derived>
struct QuantileExactLowHigh : public QuantileExactBase<Value, Derived>
{
    using Base = QuantileExactBase<Value, Derived>;
    using Base::size;
    using Base::select;

    Value getImpl(Float64 level)
    {
        if (size())
            return select(Derived::getIndex(level, size()));
        return std::numeric_limits<Value>::quiet_NaN();
    }

    void getManyImpl(const Float64 * levels, const size_t * /* indices */, size_t num_levels, Value * result)
    {
        if (size())
        {
            PODArrayWithStackMemory<size_t, 4 * sizeof(size_t)> ranks(num_levels);
            for (size_t i = 0; i < num_levels; ++i)
                ranks[i] = Derived::getIndex(levels[i], size());
            select(ranks.data(), num_levels, result);
        }
        else
        {
            for (size_t i = 0; i < num_levels; ++i)
                result[i] = Value();
        }
    }
};

// QuantileExactLow returns the low median of given data.
// Implementation is as per "medium_low" function from python:
// https://docs.python.org/3/library/statistics.html#statistics.median_low
template <typename Value>
struct QuantileExactLow : public QuantileExactLowHigh<Value, QuantileExactLow<Value>>
{
    static size_t getIndex(Float64 level, size_t size)
    {
        // if level is 0.5 then compute the "low" median of the sorted array
        // by the method of rounding.
        if (level == 0.5)
            return size % 2 == 1 ? size / 2 : size / 2 - 1;

        // else quantile is the nth index of the sorted array obtained by multiplying
        // level and size of array. Example if level = 0.1 and size of array is 10,
        // then return array[1].
        return level < 1 ? static_cast<size_t>(level * static_cast<Float64>(size)) : (size - 1);
    }
};

// QuantileExactHigh returns the high median of given data.
// Implementation is as per "medium_high function from python:
// https://docs.python.org/3/library/statistics.html#statistics.median_high
template <typename Value>
struct QuantileExactHigh : public QuantileExactLowHigh<Value, QuantileExactHigh<Value>>
{
    static size_t getIndex(Float64 level, size_t size)
    {
        // if level is 0.5 then compute the "high" median of the sorted array
        // by the method of rounding.
        if (level == 0.5)
            return size / 2;

        // else quantile is the nth index of the sorted array obtained by multiplying
        // level and size of array. Example if level = 0.1 and size of array is 10.
        return level < 1 ? static_cast<size_t>(level * static_cast<Float64>(size)) : (size - 1);
    }
};

}
