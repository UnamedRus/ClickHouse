#pragma once

#include "config.h"

#if USE_DATASKETCHES

#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnVector.h>
#include <Columns/ColumnsNumber.h>
#include <Common/assert_cast.h>
#include <Core/Field.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDate32.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>

#include <type_traits>

namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

/** Generic aggregate function over an Apache DataSketches sketch.
  *
  * All DataSketches sketches share the same state lifecycle: build a sketch from a configuration,
  * feed it hashable data, merge sketches through a union, and serialize/deserialize to the
  * DataSketches-native binary format. This template captures that shared lifecycle plus the
  * column-value extraction that is common to every sketch. Everything sketch-specific
  * (configuration/parameters, the concrete state type, how a datum is added, how states merge,
  * the result type, and how the result is extracted) is delegated to a small `Policy`.
  *
  * A `Policy` must provide:
  *   - `using Data = ...;`                                       the per-state wrapper type
  *   - `static String getName();`
  *   - `static DataTypePtr getResultType();`
  *   - `static Policy parseParameters(const String & name, const Array & params);`
  *   - `template <typename V> void add(Data &, V value) const;`  add an integer/float datum
  *   - `void addData(Data &, const char * data, size_t size) const;`  add a raw-bytes datum
  *   - `void merge(Data &, const Data & rhs) const;`
  *   - `void serialize(const Data &, WriteBuffer &) const;`
  *   - `void deserialize(Data &, ReadBuffer &) const;`
  *   - `void insertResultInto(const Data &, IColumn & to) const;`
  *   - `static constexpr bool states_compatible_across_parameters;`  whether states built with
  *     different parameters share one binary representation
  *
  * This works for cardinality sketches (HLL, CPC, Theta) whose result is a `UInt64` estimate, and
  * equally for quantile sketches whose result type and extraction differ - only the policy changes.
  */
template <typename Policy, typename T>
class AggregateFunctionApacheDataSketches final
    : public IAggregateFunctionDataHelper<typename Policy::Data, AggregateFunctionApacheDataSketches<Policy, T>>
{
private:
    using Data = typename Policy::Data;
    Policy policy;

public:
    AggregateFunctionApacheDataSketches(Policy policy_, const DataTypes & argument_types_, const Array & params_)
        : IAggregateFunctionDataHelper<Data, AggregateFunctionApacheDataSketches<Policy, T>>(
              argument_types_, params_, Policy::getResultType())
        , policy(std::move(policy_))
    {
    }

    String getName() const override { return Policy::getName(); }

    bool allocatesMemoryInArena() const override { return false; }

    /// A policy may declare that its parameters configure the sketch without changing the layout of
    /// `Data` - they are held by the policy, which lives in the aggregate function rather than in the
    /// state - and that a serialized sketch records its own configuration. States of two
    /// parameterizations are then interchangeable, so `-Merge` can read one under the other and
    /// `CAST` can relabel a column between the two types without touching its data.
    ///
    /// Interchangeable is not the same as equivalent. For `uniqApacheHLL` a union takes the
    /// resolution of its coarsest input, so merging states of different `lg_k` is lossy, and
    /// relabelling alone rescales nothing: it leaves the sketch as it was.
    bool haveSameStateRepresentationImpl(const IAggregateFunction & rhs) const override
    {
        if constexpr (Policy::states_compatible_across_parameters)
            return getName() == rhs.getName() && this->haveEqualArgumentTypes(rhs);
        else
            return IAggregateFunction::haveSameStateRepresentationImpl(rhs);
    }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        auto & data = this->data(place);
        const auto & column = *columns[0];

        if constexpr (std::is_same_v<T, String>)
        {
            const auto value = column.getDataAt(row_num);
            policy.addData(data, value.data(), value.size());
        }
        else
        {
            const auto value = assert_cast<const ColumnVector<T> &>(column).getData()[row_num];
            /// Normalize to a canonical width so that sketches interoperate with external producers:
            /// integers as their 8-byte representation, floats as IEEE-754 double.
            if constexpr (std::is_floating_point_v<T>)
                policy.add(data, static_cast<double>(value));
            else if constexpr (std::is_signed_v<T>)
                policy.add(data, static_cast<Int64>(value));
            else
                policy.add(data, static_cast<UInt64>(value));
        }
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        policy.merge(this->data(place), this->data(rhs));
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        policy.serialize(this->data(place), buf);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena *) const override
    {
        policy.deserialize(this->data(place), buf);
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        policy.insertResultInto(this->data(place), to);
    }
};


/// Shared factory: parses the policy's parameters, validates the single argument, and dispatches on
/// its type. Every DataSketches aggregate function is registered with an instantiation of this.
template <typename Policy>
AggregateFunctionPtr createAggregateFunctionApacheDataSketches(
    const std::string & name, const DataTypes & argument_types, const Array & params, const Settings *)
{
    Policy policy = Policy::parseParameters(name, params);

    if (argument_types.size() != 1)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Aggregate function {} requires exactly one argument.", name);

    const IDataType & argument_type = *argument_types[0];
    WhichDataType which(argument_type);

    auto make = [&]<typename T>() -> AggregateFunctionPtr
    {
        return std::make_shared<AggregateFunctionApacheDataSketches<Policy, T>>(policy, argument_types, params);
    };

    if (which.isUInt8()) return make.template operator()<UInt8>();
    if (which.isUInt16()) return make.template operator()<UInt16>();
    if (which.isUInt32()) return make.template operator()<UInt32>();
    if (which.isUInt64()) return make.template operator()<UInt64>();
    if (which.isInt8()) return make.template operator()<Int8>();
    if (which.isInt16()) return make.template operator()<Int16>();
    if (which.isInt32()) return make.template operator()<Int32>();
    if (which.isInt64()) return make.template operator()<Int64>();
    if (which.isFloat32()) return make.template operator()<Float32>();
    if (which.isFloat64()) return make.template operator()<Float64>();
    if (which.isDate()) return make.template operator()<DataTypeDate::FieldType>();
    if (which.isDate32()) return make.template operator()<DataTypeDate32::FieldType>();
    if (which.isDateTime()) return make.template operator()<DataTypeDateTime::FieldType>();
    if (which.isStringOrFixedString()) return make.template operator()<String>();

    throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
        "Illegal type {} of argument for aggregate function {}.", argument_type.getName(), name);
}

}

#endif
