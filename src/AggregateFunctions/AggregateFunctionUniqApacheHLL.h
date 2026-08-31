#pragma once

#include "config.h"

#if USE_DATASKETCHES

#include <AggregateFunctions/HllSketchData.h>
#include <AggregateFunctions/Helpers.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <AggregateFunctions/UniqVariadicHash.h>
#include <Columns/ColumnsNumber.h>
#include <Common/assert_cast.h>
#include <Core/Field.h>
#include <Core/UUID.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDate32.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeIPv4andIPv6.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeUUID.h>
#include <DataTypes/DataTypesNumber.h>

#include <type_traits>

namespace DB
{

namespace ErrorCodes
{
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int BAD_ARGUMENTS;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

/** Everything `uniqApacheHLL` does apart from turning a row into a sketch update.
  *
  * `lg_config_k` and the target type belong to the function rather than to the state: `HllSketchData`
  * is two pointers whatever they are, and a serialized sketch records its own `lg_config_k`. Keeping
  * them here is what lets states of different parameterisations share one binary representation.
  */
template <typename Derived>
class AggregateFunctionUniqApacheHLLBase : public IAggregateFunctionDataHelper<HllSketchData, Derived>
{
protected:
    uint8_t lg_config_k;
    datasketches::target_hll_type target_type;

public:
    AggregateFunctionUniqApacheHLLBase(
        uint8_t lg_config_k_,
        datasketches::target_hll_type target_type_,
        const DataTypes & argument_types_,
        const Array & params_)
        : IAggregateFunctionDataHelper<HllSketchData, Derived>(argument_types_, params_, std::make_shared<DataTypeUInt64>())
        , lg_config_k(lg_config_k_)
        , target_type(target_type_)
    {
    }

    String getName() const override { return "uniqApacheHLL"; }

    bool allocatesMemoryInArena() const override { return false; }

    /// The parameters configure the sketch without changing the layout of `Data`, and a serialized
    /// sketch describes its own configuration, so a state built with one parameterisation can be used
    /// by a function declared with another. Merging across them is lossy but well defined: a union
    /// takes the resolution of its coarsest input.
    bool haveSameStateRepresentationImpl(const IAggregateFunction & rhs) const override
    {
        return getName() == rhs.getName() && this->haveEqualArgumentTypes(rhs);
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        this->data(place).merge(this->data(rhs), lg_config_k, target_type);
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        this->data(place).write(buf, target_type);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena *) const override
    {
        this->data(place).read(buf, lg_config_k);
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        assert_cast<ColumnUInt64 &>(to).getData().push_back(this->data(place).size(target_type));
    }
};


/** `uniqApacheHLL` over a single column of a type the sketch can hash directly.
  *
  * The value is normalised to one of the three shapes the DataSketches API accepts, so that a sketch
  * built here matches one built by an external producer over the same values: integers as their
  * 8-byte representation, floating-point values as an IEEE-754 double, and everything else as its
  * raw bytes.
  */
template <typename T>
class AggregateFunctionUniqApacheHLL final : public AggregateFunctionUniqApacheHLLBase<AggregateFunctionUniqApacheHLL<T>>
{
    using Base = AggregateFunctionUniqApacheHLLBase<AggregateFunctionUniqApacheHLL<T>>;

public:
    using Base::Base;

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        auto & data = this->data(place);

        if constexpr (std::is_same_v<T, String>)
        {
            const auto value = columns[0]->getDataAt(row_num);
            data.insertData(value.data(), value.size(), this->lg_config_k, this->target_type);
        }
        else
        {
            const auto & value = assert_cast<const ColumnVector<T> &>(*columns[0]).getData()[row_num];

            if constexpr (std::is_same_v<T, UUID>)
            {
                /// ClickHouse holds a UUID as two 64-bit halves in host order, so its bytes in memory
                /// are not the ones the textual form describes. Hash the canonical 16 bytes instead,
                /// which is what a producer outside ClickHouse has to work from.
                const UInt64 halves[2] = {
                    std::byteswap(UUIDHelpers::getHighBytes(value)),
                    std::byteswap(UUIDHelpers::getLowBytes(value)),
                };
                data.insertData(reinterpret_cast<const char *>(halves), sizeof(halves), this->lg_config_k, this->target_type);
            }
            else if constexpr (std::is_same_v<T, IPv6>)
                /// Already held in network order, which is the canonical form.
                data.insertData(reinterpret_cast<const char *>(&value), sizeof(value), this->lg_config_k, this->target_type);
            else if constexpr (is_over_big_int<T>)
                /// No byte order is agreed on across implementations for these; use ClickHouse's own.
                data.insertData(reinterpret_cast<const char *>(&value), sizeof(value), this->lg_config_k, this->target_type);
            else if constexpr (std::is_same_v<T, IPv4>)
                data.insert(static_cast<UInt64>(value.toUnderType()), this->lg_config_k, this->target_type);
            else if constexpr (std::is_same_v<T, BFloat16> || std::is_floating_point_v<T>)
                data.insert(static_cast<Float64>(value), this->lg_config_k, this->target_type);
            else if constexpr (std::is_signed_v<T>)
                data.insert(static_cast<Int64>(value), this->lg_config_k, this->target_type);
            else
                data.insert(static_cast<UInt64>(value), this->lg_config_k, this->target_type);
        }
    }
};


/** `uniqApacheHLL` over several columns, or over one whose type the sketch cannot hash directly
  * (a tuple, a decimal, `DateTime64`, ...).
  *
  * The arguments are hashed to a single `UInt64` by ClickHouse first, exactly as `uniq` does, and only
  * that hash reaches the sketch. Such a sketch is therefore not interoperable with an external
  * producer, which has no way to reproduce the hash.
  */
template <bool is_exact, bool argument_is_tuple>
class AggregateFunctionUniqApacheHLLVariadic final
    : public AggregateFunctionUniqApacheHLLBase<AggregateFunctionUniqApacheHLLVariadic<is_exact, argument_is_tuple>>
{
    using Base = AggregateFunctionUniqApacheHLLBase<AggregateFunctionUniqApacheHLLVariadic<is_exact, argument_is_tuple>>;

    size_t num_args = 0;

public:
    AggregateFunctionUniqApacheHLLVariadic(
        uint8_t lg_config_k_,
        datasketches::target_hll_type target_type_,
        const DataTypes & argument_types_,
        const Array & params_)
        : Base(lg_config_k_, target_type_, argument_types_, params_)
    {
        if constexpr (argument_is_tuple)
            num_args = typeid_cast<const DataTypeTuple &>(*argument_types_[0]).getElements().size();
        else
            num_args = argument_types_.size();
    }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        auto & data = this->data(place);

        /// The exact hash is 128 bits wide, which is more than `update(long)` takes.
        const auto hash = UniqVariadicHash<is_exact, argument_is_tuple>::apply(num_args, columns, row_num);
        if constexpr (sizeof(hash) > sizeof(UInt64))
            data.insertData(reinterpret_cast<const char *>(&hash), sizeof(hash), this->lg_config_k, this->target_type);
        else
            data.insert(static_cast<UInt64>(hash), this->lg_config_k, this->target_type);
    }
};


/// `uniqApacheHLL([lg_k, [type]])(x)`, with `lg_k` in [4, 21] (default 12) and `type` one of
/// 'HLL_4', 'HLL_6', 'HLL_8' (default 'HLL_4').
AggregateFunctionPtr createAggregateFunctionUniqApacheHLL(
    const std::string & name, const DataTypes & argument_types, const Array & params, const Settings *);

}

#endif
