#pragma once

#include "config.h"

#if USE_DATASKETCHES

#include <AggregateFunctions/HllSketchData.h>
#include <AggregateFunctions/Helpers.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnsNumber.h>
#include <Common/assert_cast.h>
#include <Core/Field.h>
#include <Core/UUID.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDate32.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeIPv4andIPv6.h>
#include <DataTypes/DataTypeUUID.h>
#include <DataTypes/DataTypesNumber.h>

#include <type_traits>

namespace DB
{

/** `uniqApacheHLL` over a single column of a type the sketch can hash directly.
  *
  * The value takes one of the three shapes the DataSketches API accepts - an 8-byte integer, an
  * IEEE-754 double, or raw bytes - so that an external producer reaches the same sketch.
  *
  * `lg_config_k` and the target type live here rather than in the state, which is what lets states
  * of different parameterisations share one binary representation.
  */
template <typename T>
class AggregateFunctionUniqApacheHLL final : public IAggregateFunctionDataHelper<HllSketchData, AggregateFunctionUniqApacheHLL<T>>
{
    using Base = IAggregateFunctionDataHelper<HllSketchData, AggregateFunctionUniqApacheHLL<T>>;

    uint8_t lg_config_k;
    datasketches::target_hll_type target_type;

public:
    AggregateFunctionUniqApacheHLL(
        uint8_t lg_config_k_,
        datasketches::target_hll_type target_type_,
        const DataTypes & argument_types_,
        const Array & params_)
        : Base(argument_types_, params_, std::make_shared<DataTypeUInt64>())
        , lg_config_k(lg_config_k_)
        , target_type(target_type_)
    {
    }

    String getName() const override { return "uniqApacheHLL"; }

    bool allocatesMemoryInArena() const override { return false; }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        auto & data = this->data(place);

        if constexpr (std::is_same_v<T, String>)
        {
            const auto value = columns[0]->getDataAt(row_num);
            data.insertData(value.data(), value.size(), lg_config_k, target_type);
        }
        else
        {
            const auto & value = assert_cast<const ColumnVectorOrDecimal<T> &>(*columns[0]).getData()[row_num];

            if constexpr (std::is_same_v<T, UUID>)
            {
                /// ClickHouse holds a UUID as two 64-bit halves in host order, so its bytes in
                /// memory are not the canonical 16 an external producer works from.
                const UInt64 halves[2] = {
                    std::byteswap(UUIDHelpers::getHighBytes(value)),
                    std::byteswap(UUIDHelpers::getLowBytes(value)),
                };
                data.insertData(reinterpret_cast<const char *>(halves), sizeof(halves), lg_config_k, target_type);
            }
            else if constexpr (std::is_same_v<T, IPv6>)
                /// Already held in network order, which is the canonical form.
                data.insertData(reinterpret_cast<const char *>(&value), sizeof(value), lg_config_k, target_type);
            else if constexpr (is_decimal<T>)
                /// `DateTime64(3)` holds the epoch milliseconds a caller elsewhere passes to
                /// `update(long)`. The scale belongs to the type, so both sides must agree on it.
                data.insert(static_cast<Int64>(value.value), lg_config_k, target_type);
            else if constexpr (std::is_same_v<T, IPv4>)
                data.insert(static_cast<UInt64>(value.toUnderType()), lg_config_k, target_type);
            else if constexpr (std::is_same_v<T, BFloat16> || std::is_floating_point_v<T>)
                data.insert(static_cast<Float64>(value), lg_config_k, target_type);
            else if constexpr (std::is_signed_v<T>)
                data.insert(static_cast<Int64>(value), lg_config_k, target_type);
            else
                data.insert(static_cast<UInt64>(value), lg_config_k, target_type);
        }
    }
    /// A serialized sketch describes its own configuration, so states of different parameterisations
    /// are interchangeable. Merging across them takes the resolution of the coarsest input.
    bool haveSameStateRepresentationImpl(const IAggregateFunction & rhs) const override
    {
        return getName() == rhs.getName() && this->haveEqualArgumentTypes(rhs);
    }

    void mergeImpl(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
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


/// `uniqApacheHLL([lg_k, [type]])(x)`, with `lg_k` in [4, 21] (default 12) and `type` one of
/// 'HLL_4', 'HLL_6', 'HLL_8' (default 'HLL_4').
AggregateFunctionPtr createAggregateFunctionUniqApacheHLL(
    const std::string & name, const DataTypes & argument_types, const Array & params, const Settings *);

}

#endif
