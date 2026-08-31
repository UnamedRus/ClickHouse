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


}

#endif
