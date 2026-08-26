#pragma once

#include "config.h"

#if USE_DATASKETCHES

#include <boost/noncopyable.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <hll.hpp>

#include <Columns/ColumnsNumber.h>
#include <Common/Exception.h>
#include <Common/FieldVisitorConvertToNumber.h>
#include <Common/assert_cast.h>
#include <Core/Field.h>
#include <DataTypes/DataTypesNumber.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int BAD_ARGUMENTS;
}


/** Wraps an Apache DataSketches HLL sketch so that its aggregate function state is
  * byte-compatible with the DataSketches serialized HLL format (as produced by the
  * Java/Python/C++ libraries). This mirrors `ThetaSketchData` and lets the standard
  * `-State`/`-Merge` combinators produce and consume sketches that interoperate with
  * external services.
  *
  * The configuration (`lg_config_k`, `target_hll_type`) is owned by the aggregate
  * function and passed into the methods that need it, so the state itself stores only
  * the sketch and no redundant copy of the configuration.
  */
class HllSketchData : private boost::noncopyable
{
private:
    /// Used for insertions.
    std::unique_ptr<datasketches::hll_sketch> sk_update;
    /// Used for merging.
    std::unique_ptr<datasketches::hll_union> sk_union;

    datasketches::hll_sketch * getSkUpdate(uint8_t lg_config_k, datasketches::target_hll_type tgt_type)
    {
        if (!sk_update)
            sk_update = std::make_unique<datasketches::hll_sketch>(lg_config_k, tgt_type);
        return sk_update.get();
    }

    datasketches::hll_union * getSkUnion(uint8_t lg_config_k)
    {
        if (!sk_union)
        {
            /// `hll_union` requires `lg_max_k` in [7, 21], while a sketch may use `lg_config_k` in
            /// [4, 21], so the union is built with `max(lg_config_k, 7)`. Raising the floor does not
            /// inflate the result: `get_result` downsamples to the smallest `lg_config_k` the union
            /// has actually seen, so a sketch built with `lg_config_k = 4` still yields a result at 4.
            ///
            /// That same rule means the resolution of a merged state is the minimum over its inputs
            /// rather than the `lg_config_k` named by the aggregate function's type. This matters for
            /// a sketch produced elsewhere and deserialized here: nothing checks its `lg_config_k`
            /// against ours, so merging a coarser one permanently lowers the resolution of both the
            /// estimate and the state written back, which still carries this function's type.
            ///
            /// With no input at all the union has no minimum to downsample to and reports its own
            /// `lg_max_k`, so a state that was only ever merged from empty states serializes as
            /// `lg_config_k = 7` even when the type says less.
            sk_union = std::make_unique<datasketches::hll_union>(std::max<uint8_t>(lg_config_k, 7));
        }
        return sk_union.get();
    }

    /// After an insert that happens once the union is already allocated, fold the freshly updated
    /// sketch into the union and drop it, so that subsequent merges observe a consistent state.
    void foldUpdateIntoUnionIfNeeded()
    {
        if (sk_union && sk_update)
        {
            sk_union->update(*sk_update);
            sk_update.reset(nullptr);
        }
    }

public:
    HllSketchData() = default;
    ~HllSketchData() = default;

    template <typename T>
    void insert(T value, uint8_t lg_config_k, datasketches::target_hll_type tgt_type)
    {
        getSkUpdate(lg_config_k, tgt_type)->update(value);
        foldUpdateIntoUnionIfNeeded();
    }

    void insertData(const char * data, size_t size, uint8_t lg_config_k, datasketches::target_hll_type tgt_type)
    {
        getSkUpdate(lg_config_k, tgt_type)->update(static_cast<const void *>(data), size);
        foldUpdateIntoUnionIfNeeded();
    }

    UInt64 size(datasketches::target_hll_type tgt_type) const
    {
        /// `get_estimate` returns a `double`. Truncating it would bias every result downwards by up
        /// to one, turning an estimate of `999.9999` for an exactly-known cardinality into `999`.
        if (sk_union)
            return static_cast<UInt64>(std::llround(sk_union->get_result(tgt_type).get_estimate()));
        if (sk_update)
            return static_cast<UInt64>(std::llround(sk_update->get_estimate()));
        return 0;
    }

    void merge(const HllSketchData & rhs, uint8_t lg_config_k, datasketches::target_hll_type tgt_type)
    {
        datasketches::hll_union * u = getSkUnion(lg_config_k);

        if (sk_update)
        {
            u->update(*sk_update);
            sk_update.reset(nullptr);
        }

        if (rhs.sk_update)
            u->update(*rhs.sk_update);
        else if (rhs.sk_union)
            u->update(rhs.sk_union->get_result(tgt_type));
    }

    /// You can only call this for an empty object.
    void read(ReadBuffer & in, uint8_t lg_config_k)
    {
        datasketches::hll_sketch::vector_bytes bytes;
        readVectorBinary(bytes, in);
        if (bytes.empty())
            return;

        try
        {
            auto sk = datasketches::hll_sketch::deserialize(bytes.data(), bytes.size());
            getSkUnion(lg_config_k)->update(std::move(sk));
        }
        catch (const DB::Exception &)
        {
            throw;
        }
        catch (const std::bad_alloc &)
        {
            /// Memory pressure on `hll_sketch::deserialize`, `getSkUnion`, or `hll_union::update` is
            /// not data corruption; let it propagate.
            throw;
        }
        catch (const std::exception & e)
        {
            /// `datasketches` throws `std::invalid_argument` / `std::out_of_range` on malformed input.
            /// These are not `DB::Exception`, so without translation they escape
            /// `SerializationAggregateFunction`'s `catch (...)` block, reach the top level as
            /// `LOGICAL_ERROR`, and abort the process via `abortOnFailedAssertion`. Translate to
            /// `CORRUPTED_DATA` so bad input is rejected cleanly.
            throw Exception(ErrorCodes::CORRUPTED_DATA, "Cannot deserialize HLL sketch state: {}", e.what());
        }
    }

    void write(WriteBuffer & out, datasketches::target_hll_type tgt_type) const
    {
        if (sk_update)
        {
            auto bytes = sk_update->serialize_compact();
            writeVectorBinary(bytes, out);
        }
        else if (sk_union)
        {
            auto bytes = sk_union->get_result(tgt_type).serialize_compact();
            writeVectorBinary(bytes, out);
        }
        else
        {
            datasketches::hll_sketch::vector_bytes bytes;
            writeVectorBinary(bytes, out);
        }
    }
};


/** Policy for `AggregateFunctionApacheDataSketches` implementing `uniqApacheHLL`: an approximate
  * distinct count backed by an Apache DataSketches HLL sketch, whose serialized state is
  * byte-compatible with the DataSketches HLL format.
  *
  * Parameters: `uniqApacheHLL([lg_k, [type]])(x)`, with `lg_k` in [4, 21] (default 12) and `type`
  * one of 'HLL_4', 'HLL_6', 'HLL_8' (default 'HLL_4').
  */
struct HllSketchPolicy
{
    using Data = HllSketchData;

    uint8_t lg_config_k = 12;
    datasketches::target_hll_type target_type = datasketches::HLL_4;

    static String getName() { return "uniqApacheHLL"; }

    static DataTypePtr getResultType() { return std::make_shared<DataTypeUInt64>(); }

    static HllSketchPolicy parseParameters(const String & name, const Array & params)
    {
        HllSketchPolicy policy;

        if (params.size() > 2)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Aggregate function {} accepts at most two parameters (lg_k, type).", name);

        if (params.size() >= 1)
        {
            const UInt64 lg_k_param = applyVisitor(FieldVisitorConvertToNumber<UInt64>(), params[0]);
            if (lg_k_param < 4 || lg_k_param > 21)
                throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND,
                    "Parameter lg_k for aggregate function {} is out of range: [4, 21].", name);
            policy.lg_config_k = static_cast<uint8_t>(lg_k_param);
        }

        if (params.size() == 2)
        {
            const String type_param = params[1].safeGet<String>();
            if (type_param == "HLL_4")
                policy.target_type = datasketches::HLL_4;
            else if (type_param == "HLL_6")
                policy.target_type = datasketches::HLL_6;
            else if (type_param == "HLL_8")
                policy.target_type = datasketches::HLL_8;
            else
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "Parameter type for aggregate function {} must be one of 'HLL_4', 'HLL_6', 'HLL_8'.", name);
        }

        return policy;
    }

    template <typename V>
    void add(Data & data, V value) const
    {
        data.insert(value, lg_config_k, target_type);
    }

    void addData(Data & data, const char * data_ptr, size_t size) const
    {
        data.insertData(data_ptr, size, lg_config_k, target_type);
    }

    void merge(Data & data, const Data & rhs) const
    {
        data.merge(rhs, lg_config_k, target_type);
    }

    void serialize(const Data & data, WriteBuffer & buf) const
    {
        data.write(buf, target_type);
    }

    void deserialize(Data & data, ReadBuffer & buf) const
    {
        data.read(buf, lg_config_k);
    }

    void insertResultInto(const Data & data, IColumn & to) const
    {
        assert_cast<ColumnUInt64 &>(to).getData().push_back(data.size(target_type));
    }
};

}

#endif
