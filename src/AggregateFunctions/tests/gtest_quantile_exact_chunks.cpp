#include <AggregateFunctions/QuantileExact.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <base/Decimal.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

using namespace DB;

namespace
{

template <typename T>
bool sameValue(const T & a, const T & b)
{
    /// -0.0 and +0.0 are equal, either of them is a correct result.
    return a == b;
}

/// The reference: sort all values.
template <typename T>
std::vector<T> sortedCopy(const std::vector<T> & values)
{
    std::vector<T> res;
    for (const auto & x : values)
        if (!isNaN(x))
            res.push_back(x);
    std::sort(res.begin(), res.end());
    return res;
}

template <typename T>
std::vector<T> generateValues(size_t size, size_t distinct, std::mt19937_64 & rng)
{
    std::vector<T> res(size);
    for (auto & x : res)
    {
        UInt64 r = distinct ? rng() % distinct : rng();
        if constexpr (std::is_floating_point_v<T>)
            x = static_cast<T>(static_cast<Int64>(r) % 2000001 - 1000000) / static_cast<T>(7);
        else if constexpr (is_decimal<T>)
            x = T(static_cast<typename T::NativeType>(static_cast<Int64>(r)));
        else
            x = static_cast<T>(r);
    }
    return res;
}

template <template <typename> class Quantile, typename T>
size_t referenceIndex(Float64 level, size_t size)
{
    if constexpr (std::is_same_v<Quantile<T>, QuantileExactLow<T>>)
    {
        if (level == 0.5)
            return size % 2 ? size / 2 : size / 2 - 1;
    }
    else if constexpr (std::is_same_v<Quantile<T>, QuantileExactHigh<T>>)
    {
        if (level == 0.5)
            return size / 2;
    }
    return level < 1 ? static_cast<size_t>(level * static_cast<Float64>(size)) : size - 1;
}

/// Fills `num_states` states with the values, merges them into the first one, and checks all the variants.
template <typename T, template <typename> class Quantile>
void checkQuantile(const std::vector<T> & values, size_t num_states, const std::vector<Float64> & levels)
{
    std::vector<Quantile<T>> states(num_states);
    for (size_t i = 0; i < values.size(); ++i)
        states[i % num_states].add(values[i]);
    for (size_t i = 1; i < num_states; ++i)
        states[0].merge(states[i]);

    auto sorted = sortedCopy(values);
    auto & state = states[0];

    for (Float64 level : levels)
    {
        T res = state.get(level);
        if (sorted.empty())
            continue;

        size_t n = referenceIndex<Quantile, T>(level, sorted.size());
        ASSERT_TRUE(sameValue(res, sorted[n])) << "level " << level << ", size " << sorted.size();
    }

    std::vector<size_t> indices(levels.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return levels[a] < levels[b]; });
    std::vector<T> many(levels.size());
    state.getMany(levels.data(), indices.data(), levels.size(), many.data());
    for (size_t i = 0; i < levels.size(); ++i)
        ASSERT_TRUE(sameValue(many[i], sorted.empty() ? T() : state.get(levels[i])));

    /// The states that were merged must stay valid and unchanged.
    for (size_t i = 1; i < num_states; ++i)
    {
        std::vector<T> part;
        for (size_t j = i; j < values.size(); j += num_states)
            part.push_back(values[j]);
        auto part_sorted = sortedCopy(part);
        if (!part_sorted.empty())
            ASSERT_TRUE(sameValue(states[i].get(0.5), part_sorted[referenceIndex<Quantile, T>(0.5, part_sorted.size())]));
    }
}

template <typename T, template <typename> class Quantile>
void checkInterpolated(const std::vector<T> & values, size_t num_states, const std::vector<Float64> & levels, bool exclusive)
{
    std::vector<Quantile<T>> states(num_states);
    for (size_t i = 0; i < values.size(); ++i)
        states[i % num_states].add(values[i]);
    for (size_t i = 1; i < num_states; ++i)
        states[0].merge(states[i]);

    auto sorted = sortedCopy(values);
    auto & state = states[0];

    std::vector<Float64> many(levels.size());
    state.getManyFloat(levels.data(), nullptr, levels.size(), many.data());

    for (size_t i = 0; i < levels.size(); ++i)
    {
        Float64 res = state.getFloat(levels[i]);
        Float64 expected = std::numeric_limits<Float64>::quiet_NaN();
        if (!sorted.empty())
        {
            Float64 h = exclusive ? levels[i] * static_cast<Float64>(sorted.size() + 1) : levels[i] * static_cast<Float64>(sorted.size() - 1) + 1;
            auto n = static_cast<size_t>(h);
            if (n >= sorted.size())
                expected = static_cast<Float64>(sorted.back());
            else if (n < 1)
                expected = static_cast<Float64>(sorted.front());
            else
                expected = static_cast<Float64>(sorted[n - 1]) + (h - static_cast<Float64>(n)) * quantileExactInterpolationDelta(sorted[n - 1], sorted[n]);
        }

        if (std::isnan(expected))
        {
            ASSERT_TRUE(std::isnan(res));
            ASSERT_TRUE(std::isnan(many[i]));
        }
        else
        {
            ASSERT_EQ(res, expected) << "level " << levels[i];
            ASSERT_EQ(many[i], expected) << "level " << levels[i];
        }
    }
}

const std::vector<Float64> test_levels = {0.5, 0, 1, 0.001, 0.1, 0.25, 0.3, 0.499999, 0.75, 0.9, 0.999, 0.5};
const std::vector<Float64> test_levels_exclusive = {0.5, 0.0000001, 0.001, 0.1, 0.25, 0.3, 0.75, 0.9, 0.999, 0.9999999};

template <typename T>
void checkAllVariants(size_t size, size_t distinct, size_t num_states, UInt64 seed)
{
    std::mt19937_64 rng(seed);
    auto values = generateValues<T>(size, distinct, rng);
    checkQuantile<T, QuantileExact>(values, num_states, test_levels);
    checkQuantile<T, QuantileExactLow>(values, num_states, test_levels);
    checkQuantile<T, QuantileExactHigh>(values, num_states, test_levels);
    if constexpr (!is_decimal<T> && !is_over_big_int<T>)
    {
        checkInterpolated<T, QuantileExactExclusive>(values, num_states, test_levels_exclusive, true);
        checkInterpolated<T, QuantileExactInclusive>(values, num_states, test_levels, false);
    }
}

template <typename T>
void checkSizes()
{
    for (size_t size : {0uz, 1uz, 2uz, 3uz, 100uz})
        for (size_t num_states : {1uz, 3uz})
            for (size_t distinct : {0uz, 1uz, 2uz, 100uz})
                checkAllVariants<T>(size, distinct, num_states, size * 31 + num_states * 7 + distinct);

    /// Around the threshold of switching to chunks.
    constexpr size_t chunk_size = QuantileExactDetail::QuantileExactChunks<T>::chunk_size;
    for (size_t size : {chunk_size - 1, chunk_size, chunk_size + 1, 2 * chunk_size + 17})
        for (size_t distinct : {0uz, 2uz})
            checkAllVariants<T>(size, distinct, 3, size * 31 + distinct);
}

}

TEST(QuantileExact, Float64) { checkSizes<Float64>(); }
TEST(QuantileExact, Float32) { checkSizes<Float32>(); }
TEST(QuantileExact, UInt8) { checkSizes<UInt8>(); }
TEST(QuantileExact, Int16) { checkSizes<Int16>(); }
TEST(QuantileExact, Int32) { checkSizes<Int32>(); }
TEST(QuantileExact, UInt64) { checkSizes<UInt64>(); }
TEST(QuantileExact, Int64) { checkSizes<Int64>(); }
TEST(QuantileExact, Decimal32) { checkSizes<Decimal32>(); }
TEST(QuantileExact, Decimal64) { checkSizes<Decimal64>(); }
TEST(QuantileExact, DateTime64) { checkSizes<DateTime64>(); }
TEST(QuantileExact, Int128) { checkSizes<Int128>(); }
TEST(QuantileExact, Int256) { checkSizes<Int256>(); }

/// Exercise the radix selection with the default thresholds: more than `max_bucket_size` values.
TEST(QuantileExact, LargeFloat64)
{
    constexpr size_t size = QuantileExactDetail::QuantileExactChunks<Float64>::max_bucket_size * 5 / 4;
    std::mt19937_64 rng(42);
    auto values = generateValues<Float64>(size, 0, rng);
    checkQuantile<Float64, QuantileExact>(values, 4, test_levels);
    checkInterpolated<Float64, QuantileExactExclusive>(values, 4, test_levels_exclusive, true);
    values = generateValues<Float64>(size, 3, rng);
    checkQuantile<Float64, QuantileExact>(values, 4, test_levels);
}

TEST(QuantileExact, SpecialFloats)
{
    std::vector<Float64> values;
    constexpr size_t chunk_size = QuantileExactDetail::QuantileExactChunks<Float64>::chunk_size;
    for (size_t i = 0; i < 3 * chunk_size; ++i)
    {
        switch (i % 7)
        {
            case 0: values.push_back(-0.0); break;
            case 1: values.push_back(0.0); break;
            case 2: values.push_back(std::numeric_limits<Float64>::infinity()); break;
            case 3: values.push_back(-std::numeric_limits<Float64>::infinity()); break;
            case 4: values.push_back(std::numeric_limits<Float64>::quiet_NaN()); break;
            case 5: values.push_back(std::numeric_limits<Float64>::denorm_min()); break;
            default: values.push_back(-static_cast<Float64>(i)); break;
        }
    }
    checkQuantile<Float64, QuantileExact>(values, 3, test_levels);
    checkQuantile<Float64, QuantileExactLow>(values, 2, test_levels);
    checkQuantile<Float64, QuantileExactHigh>(values, 1, test_levels);
    checkInterpolated<Float64, QuantileExactInclusive>(values, 2, test_levels, false);
}

TEST(QuantileExact, ExtremeIntegers)
{
    std::vector<Int64> values;
    for (size_t i = 0; i < 100000; ++i)
        values.push_back(i % 3 == 0 ? std::numeric_limits<Int64>::min() : (i % 3 == 1 ? std::numeric_limits<Int64>::max() : static_cast<Int64>(i) - 50000));
    checkQuantile<Int64, QuantileExact>(values, 3, test_levels);
    checkInterpolated<Int64, QuantileExactExclusive>(values, 3, test_levels_exclusive, true);
}

/// The serialized state must be the same as in the old implementation: the number of values and the values in insertion order.
TEST(QuantileExact, Serialization)
{
    constexpr size_t chunk_size = QuantileExactDetail::QuantileExactChunks<Int32>::chunk_size;
    for (size_t size : {0uz, 5uz, chunk_size - 1, chunk_size, 2 * chunk_size + 3})
    {
        std::mt19937_64 rng(size);
        auto values = generateValues<Int32>(size, 0, rng);

        QuantileExact<Int32> state;
        for (auto x : values)
            state.add(x);

        WriteBufferFromOwnString expected;
        writeVarUInt(values.size(), expected);
        expected.write(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(Int32));

        WriteBufferFromOwnString out;
        state.serialize(out);
        ASSERT_EQ(out.str(), expected.str());

        ReadBufferFromString in(out.str());
        QuantileExact<Int32> restored;
        restored.deserialize(in);

        WriteBufferFromOwnString out2;
        restored.serialize(out2);
        ASSERT_EQ(out2.str(), expected.str());

        if (size)
            ASSERT_EQ(restored.get(0.3), state.get(0.3));
    }
}

/// After merge the states share chunks. Adding values to any of them or destroying them must not affect the others.
TEST(QuantileExact, SharedChunks)
{
    constexpr size_t chunk_size = QuantileExactDetail::QuantileExactChunks<UInt64>::chunk_size;
    std::vector<UInt64> a_values;
    std::vector<UInt64> b_values;

    auto a = std::make_unique<QuantileExact<UInt64>>();
    auto b = std::make_unique<QuantileExact<UInt64>>();
    for (size_t i = 0; i < 3 * chunk_size + 5; ++i)
    {
        a->add(i * 2);
        a_values.push_back(i * 2);
    }
    for (size_t i = 0; i < 2 * chunk_size + 7; ++i)
    {
        b->add(i * 3 + 1);
        b_values.push_back(i * 3 + 1);
    }

    b->merge(*a);
    b_values.insert(b_values.end(), a_values.begin(), a_values.end());

    /// Add to both after merge.
    for (size_t i = 0; i < 2 * chunk_size; ++i)
    {
        a->add(1000000000 + i);
        a_values.push_back(1000000000 + i);
        b->add(2000000000 + i);
        b_values.push_back(2000000000 + i);
    }

    auto c = std::make_unique<QuantileExact<UInt64>>();
    c->merge(*b);
    c->merge(*a);
    std::vector<UInt64> c_values = b_values;
    c_values.insert(c_values.end(), a_values.begin(), a_values.end());

    auto check = [](QuantileExact<UInt64> & state, const std::vector<UInt64> & values)
    {
        auto sorted = sortedCopy(values);
        for (Float64 level : test_levels)
        {
            size_t n = level < 1 ? static_cast<size_t>(level * static_cast<Float64>(sorted.size())) : sorted.size() - 1;
            ASSERT_EQ(state.get(level), sorted[n]);
        }
    };

    check(*a, a_values);
    check(*b, b_values);
    check(*c, c_values);

    a.reset();
    check(*b, b_values);
    check(*c, c_values);

    b.reset();
    check(*c, c_values);
}

/// Small chunks and buckets to exercise multiple levels of the radix selection.
template <typename T, size_t chunk_bytes, size_t max_bucket_bytes>
void checkChunks(size_t size, size_t distinct, UInt64 seed)
{
    using Chunks = QuantileExactDetail::QuantileExactChunks<T, chunk_bytes, max_bucket_bytes>;
    std::mt19937_64 rng(seed);
    auto values = generateValues<T>(size, distinct, rng);

    Chunks lhs;
    Chunks rhs;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i % 3 == 0)
            lhs.append(&values[i], 1);
        else
            rhs.push(values[i]);
    }
    lhs.merge(rhs);
    ASSERT_EQ(lhs.size(), values.size());

    auto sorted = sortedCopy(values);
    std::vector<size_t> ranks;
    for (size_t i = 0; i < 20 && !sorted.empty(); ++i)
        ranks.push_back(rng() % sorted.size());
    if (!sorted.empty())
    {
        ranks.push_back(0);
        ranks.push_back(sorted.size() - 1);
        ranks.push_back(ranks[0]);
        ranks.push_back(sorted.size() / 2);
        ranks.push_back(std::min(sorted.size() / 2 + 1, sorted.size() - 1));
    }

    std::vector<T> result(ranks.size());
    lhs.select(ranks.data(), ranks.size(), result.data());
    for (size_t i = 0; i < ranks.size(); ++i)
        ASSERT_TRUE(sameValue(result[i], sorted[ranks[i]])) << "rank " << ranks[i] << " of " << sorted.size();

    for (size_t rank : ranks)
    {
        T one;
        lhs.select(&rank, 1, &one);
        ASSERT_TRUE(sameValue(one, sorted[rank]));
    }
}

TEST(QuantileExactChunks, RadixSelect)
{
    for (size_t size : {1uz, 2uz, 17uz, 1000uz, 50000uz})
    {
        for (size_t distinct : {0uz, 1uz, 2uz, 5uz, 1000uz})
        {
            checkChunks<Float64, 64, 64>(size, distinct, size + distinct);
            checkChunks<Float64, 1024, 4096>(size, distinct, size + distinct);
            checkChunks<Float32, 64, 64>(size, distinct, size + distinct);
            checkChunks<UInt8, 16, 16>(size, distinct, size + distinct);
            checkChunks<Int8, 16, 16>(size, distinct, size + distinct);
            checkChunks<UInt16, 64, 64>(size, distinct, size + distinct);
            checkChunks<Int16, 64, 64>(size, distinct, size + distinct);
            checkChunks<UInt32, 64, 64>(size, distinct, size + distinct);
            checkChunks<Int32, 64, 64>(size, distinct, size + distinct);
            checkChunks<UInt64, 64, 64>(size, distinct, size + distinct);
            checkChunks<Int64, 64, 64>(size, distinct, size + distinct);
            checkChunks<Decimal32, 64, 64>(size, distinct, size + distinct);
            checkChunks<Decimal64, 64, 64>(size, distinct, size + distinct);
            checkChunks<DateTime64, 64, 64>(size, distinct, size + distinct);
            checkChunks<Int128, 64, 64>(size, distinct, size + distinct);
            checkChunks<UInt256, 64, 64>(size, distinct, size + distinct);
        }
    }
}

TEST(QuantileExactChunks, RadixKeyOrder)
{
    using Key = QuantileExactDetail::RadixKey<Float64>;
    std::vector<Float64> values = {-std::numeric_limits<Float64>::infinity(), std::numeric_limits<Float64>::lowest(), -1.5, -1, -std::numeric_limits<Float64>::denorm_min(), -0.0, 0.0, std::numeric_limits<Float64>::denorm_min(), 1, 1.5, std::numeric_limits<Float64>::max(), std::numeric_limits<Float64>::infinity()};
    for (size_t i = 0; i < values.size(); ++i)
    {
        ASSERT_EQ(std::bit_cast<UInt64>(Key::fromKey(Key::toKey(values[i]))), std::bit_cast<UInt64>(values[i]));
        if (i)
            ASSERT_LT(Key::toKey(values[i - 1]), Key::toKey(values[i]));
    }

    using IntKey = QuantileExactDetail::RadixKey<Int8>;
    for (int i = -128; i < 127; ++i)
    {
        ASSERT_LT(IntKey::toKey(static_cast<Int8>(i)), IntKey::toKey(static_cast<Int8>(i + 1)));
        ASSERT_EQ(IntKey::fromKey(IntKey::toKey(static_cast<Int8>(i))), static_cast<Int8>(i));
    }
}

/// High-cardinality GROUP BY relies on small states.
TEST(QuantileExact, StateSize)
{
    ASSERT_EQ(sizeof(QuantileExact<UInt8>), 64);
    ASSERT_EQ(sizeof(QuantileExact<Int32>), 64);
    ASSERT_EQ(sizeof(QuantileExact<Float64>), 64);
}
