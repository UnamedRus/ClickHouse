#pragma once

#include "config.h"

#if USE_DATASKETCHES

#include <boost/noncopyable.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <hll.hpp>

#include <Common/Exception.h>
#include <Core/Field.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}


/** An Apache DataSketches HLL sketch as an aggregate function state, serialized in the
  * DataSketches HLL format so that `-State`/`-Merge` interoperate with external services.
  * Mirrors `ThetaSketchData`.
  *
  * `lg_config_k` and the target type are owned by the aggregate function and passed in, so the
  * state stores the sketch alone.
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
            /// `hll_union` takes `lg_max_k` in [7, 21] while a sketch may use [4, 21], hence the
            /// floor of 7. It does not inflate the result: `get_result` downsamples to the smallest
            /// `lg_config_k` the union has seen. So the resolution of a merged state is the minimum
            /// over its inputs, not the `lg_config_k` of the type, and merging a coarser sketch from
            /// elsewhere lowers both the estimate and the state written back. Merged from empty
            /// states only there is no minimum, and it serializes as `lg_config_k = 7`.
            sk_union = std::make_unique<datasketches::hll_union>(std::max<uint8_t>(lg_config_k, 7));
        }
        return sk_union.get();
    }

    /// Fold a sketch updated after the union was allocated into it, so merges see one state.
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
        /// Round rather than truncate: `get_estimate` returns a `double`, and `999.9999` for an
        /// exactly-known cardinality must not become `999`.
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
            /// Memory pressure, not corrupted data.
            throw;
        }
        catch (const std::exception & e)
        {
            /// `datasketches` reports malformed input as `std::invalid_argument` / `std::out_of_range`.
            /// Not being `DB::Exception`, those escape `SerializationAggregateFunction`'s
            /// `catch (...)` and abort as a logical error, so translate them here.
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


}

#endif
