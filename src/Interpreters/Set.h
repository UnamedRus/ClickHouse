#pragma once

#include <QueryPipeline/SizeLimits.h>
#include <DataTypes/IDataType.h>
#include <Interpreters/SetVariants.h>
#include <Interpreters/SetKeys.h>
#include <Core/PlainRanges.h>
#include <Storages/MergeTree/BoolMask.h>

#include <Common/callOnce.h>
#include <Common/SharedMutex.h>
#include <Common/VectorWithMemoryTracking.h>
#include <Interpreters/castColumn.h>


namespace DB
{

struct Range;
using Ranges = VectorWithMemoryTracking<Range>;

class Context;
class IFunctionBase;
using FunctionBasePtr = std::shared_ptr<const IFunctionBase>;
using Sizes = std::vector<size_t>;

struct ColumnWithTypeAndName;
using ColumnsWithTypeAndName = VectorWithMemoryTracking<ColumnWithTypeAndName>;

class Chunk;

/** Data structure for implementation of IN expression.
  */
class Set
{
public:
    /// 'fill_set_elements': in addition to hash table
    /// (that is useful only for checking that some value is in the set and may not store the original values),
    /// store all set elements in explicit form.
    /// This is needed for subsequent use for index.
    Set(const SizeLimits & limits_, size_t max_elements_to_fill_, bool transform_null_in_);

    bool transformNullIn() const { return transform_null_in; }

    /** Set can be created either from AST or from a stream of data (subquery result).
      */

    /** Create a Set from stream.
      * Call setHeader, then call insertFromBlock for each block.
      */
    void setHeader(const ColumnsWithTypeAndName & header);

    /// Returns false, if some limit was exceeded and no need to insert more data.
    bool insertFromColumns(const Columns & columns);
    bool insertFromBlock(const ColumnsWithTypeAndName & columns);

    void fillSetElements();
    bool insertFromColumns(const Columns & columns, SetKeyColumns & holder);
    void appendSetElements(SetKeyColumns & holder);

    /// Call after all blocks were inserted. To get the information that set is already created.
    void finishInsert() { is_created = true; }

    /// finishInsert and isCreated are thread-safe
    bool isCreated() const { return is_created.load(); }

    /// Whether the set building was stopped early because of size limits with OverflowMode::BREAK.
    bool isTruncated() const { return is_truncated.load(); }

    void checkIsCreated() const;

    void processDateTime64Column(const ColumnWithTypeAndName & column_to_cast, ColumnPtr & result, ColumnPtr & null_map_holder, ConstNullMapPtr & null_map) const;

    /** For columns of 'block', check belonging of corresponding rows to the set.
      * Return UInt8 column with the result.
      */
    ColumnPtr execute(const ColumnsWithTypeAndName & columns, bool negative) const;

    bool hasNull() const;

    bool empty() const;
    size_t getTotalRowCount() const;
    size_t getTotalByteCount() const;

    const DataTypes & getDataTypes() const { return data_types; }
    const DataTypes & getElementsTypes() const { return set_elements_types; }

    bool hasExplicitSetElements() const { return fill_set_elements || (!set_elements.empty() && set_elements.front()->size() == data.getTotalRowCount()); }
    bool hasSetElements() const { return !set_elements.empty(); }
    Columns getSetElements() const;

    /// The elements of a single-column set viewed as sorted, non-overlapping ranges, built once and
    /// shared afterwards. Deriving them costs one `Field` per element plus an O(N log N) sort, which is
    /// substantial for a large set, and every consumer of the same set derives exactly the same value —
    /// notably the two plan builds that automatic parallel replicas performs for one query.
    /// Returns null for a multi-column (tuple) set, which has no single-column range representation.
    std::shared_ptr<const PlainRanges> getPlainRanges() const;

    void checkColumnsNumber(size_t num_key_columns) const;
    bool areTypesEqual(size_t set_type_idx, const DataTypePtr & other_type) const;
    void checkTypesEqual(size_t set_type_idx, const DataTypePtr & other_type) const;

    static DataTypes getElementTypes(DataTypes types, bool transform_null_in);

    /// Limitations on the maximum size of the set
    const SizeLimits limits;

    /// If true, insert NULL values to set.
    const bool transform_null_in;

    const size_t max_elements_to_fill;

private:
    size_t keys_size = 0;
    Sizes key_sizes;

    SetVariants data;

    /** How IN works with Nullable types.
      *
      * For simplicity reasons, all NULL values and any tuples with at least one NULL element are ignored in the Set.
      * And for left hand side values, that are NULLs or contain any NULLs, we return 0 (means that element is not in Set).
      *
      * If we want more standard compliant behaviour, we must return NULL
      *  if lhs is NULL and set is not empty or if lhs is not in set, but set contains at least one NULL.
      * It is more complicated with tuples.
      * For example,
      *      (1, NULL, 2) IN ((1, NULL, 3)) must return 0,
      *  but (1, NULL, 2) IN ((1, 1111, 2)) must return NULL.
      *
      * We have not implemented such sophisticated behaviour.
      */

    /** The data types from which the set was created.
      * When checking for belonging to a set, the types of columns to be checked must match with them.
      */
    DataTypes data_types;

    /// Types for set_elements.
    DataTypes set_elements_types;

    LoggerPtr log;

    /// Do we need to additionally store all elements of the set in explicit form for subsequent use for index.
    bool fill_set_elements = false;

    /// Check if set contains all the data.
    std::atomic<bool> is_created = false;

    /// Whether the set was truncated due to overflow with OverflowMode::BREAK.
    std::atomic<bool> is_truncated = false;

    /// If in the left part columns contains the same types as the elements of the set.
    void executeOrdinary(
        const ColumnRawPtrs & key_columns,
        ColumnUInt8::Container & vec_res,
        bool negative,
        const PaddedPODArray<UInt8> * null_map) const;

    /// Collected elements of `Set`.
    /// It is necessary for the index to work on the primary key in the IN statement.
    MutableColumns set_elements;

    mutable std::shared_ptr<const PlainRanges> plain_ranges;
    mutable OnceFlag plain_ranges_once;

    /** Protects work with the set in the functions `insertFromBlock` and `execute`.
      * These functions can be called simultaneously from different threads only when using StorageSet,
      */
    mutable SharedMutex rwlock;

    /// A cache for cast functions (if any) to avoid rebuilding cast functions
    /// for every call to `execute`
    mutable std::unique_ptr<InternalCastFunctionCache> cast_cache;

    template <typename Method>
    void insertFromBlockImpl(
        Method & method,
        const ColumnRawPtrs & key_columns,
        size_t rows,
        SetVariants & variants,
        ConstNullMapPtr null_map,
        ColumnUInt8::Container * out_filter);

    template <typename Method, bool has_null_map, bool build_filter>
    void insertFromBlockImplCase(
        Method & method,
        const ColumnRawPtrs & key_columns,
        size_t rows,
        SetVariants & variants,
        ConstNullMapPtr null_map,
        ColumnUInt8::Container * out_filter);

    template <typename Method>
    void executeImpl(
        Method & method,
        const ColumnRawPtrs & key_columns,
        ColumnUInt8::Container & vec_res,
        bool negative,
        size_t rows,
        ConstNullMapPtr null_map) const;

    template <typename Method, bool has_null_map>
    void executeImplCase(
        Method & method,
        const ColumnRawPtrs & key_columns,
        ColumnUInt8::Container & vec_res,
        bool negative,
        size_t rows,
        ConstNullMapPtr null_map) const;
};

using SetPtr = std::shared_ptr<Set>;
using ConstSetPtr = std::shared_ptr<const Set>;
using Sets = std::vector<SetPtr>;


/// Shared by `MergeTreeSetIndex` and `MergeTreeKeyRangeSet`: both search lexicographically
/// sorted key entries against a granule's key ranges, and differ only in what an entry is.
namespace SetIndexDetail
{

/** Class that represents single value with possible infinities.
  * Single field is stored in column for more optimal inplace comparisons with other regular columns.
  * Extracting fields from columns and further their comparison is suboptimal and requires extra copying.
  */
struct FieldValue
{
    explicit FieldValue(MutableColumnPtr && column_, bool block_memory_tracker_ = false)
        : column(std::move(column_)), block_memory_tracker(block_memory_tracker_) {}
    void update(const Field & x);

    bool isNormal() const { return !value.isPositiveInfinity() && !value.isNegativeInfinity(); }
    bool isPositiveInfinity() const { return value.isPositiveInfinity(); }
    bool isNegativeInfinity() const { return value.isNegativeInfinity(); }

    Field value; // Null, -Inf, +Inf

    // If value is Null, uses the actual value in column
    MutableColumnPtr column;

    /// True when the column belongs to the thread-local buffer of getFieldValueRangesBuffer,
    /// which outlives the query; its reallocations must not be charged to the current query.
    bool block_memory_tracker = false;
};

struct FieldValueRange
{
    FieldValue left;
    FieldValue right;
    bool left_included = false;
    bool right_included = false;

    explicit FieldValueRange(const IColumn & prototype, bool block_memory_tracker = false)
        : left(prototype.cloneEmpty(), block_memory_tracker), right(prototype.cloneEmpty(), block_memory_tracker) {}
};

using FieldValueRanges = std::vector<FieldValueRange>;

int compareValue(const IColumn & lhs, const FieldValue & rhs, size_t row);

/// Whether the entry at `row` lies below the key range's left corner, and above its right corner.
/// Factored out so a search over ordered entries and a check of a single entry - the running
/// maximum of upper corners - use one definition of each comparison.
bool isBelowLeftCorner(const Columns & corners, size_t row, const FieldValueRanges & ranges, size_t tuple_size);
bool isAboveRightCorner(const Columns & corners, size_t row, const FieldValueRanges & ranges, size_t tuple_size);

/// First entry of `corners`, which must be non-decreasing, lying above the range's right corner.
size_t lexUpperBound(const Columns & corners, const FieldValueRanges & ranges, size_t tuple_size, size_t set_size);

/// The two corner searches over lexicographically sorted entries. `begin_corners` and
/// `end_corners` are the column arrays each search compares against; for a set of points they
/// are the same array, which is why the caller passes it twice rather than the searches
/// assuming it.
std::pair<size_t, size_t> lexCornerSearch(
    const Columns & begin_corners,
    const Columns & end_corners,
    const FieldValueRanges & ranges,
    size_t tuple_size,
    size_t set_size);

bool isAtMostOneElementRange(const FieldValueRanges & ranges, size_t tuple_size);

}

/// Class for checkInRange function.
class MergeTreeSetIndex
{
public:
    /** Mapping for tuple positions from Set::set_elements to
      * position of pk index and functions chain applied to this column.
      */
    struct KeyTuplePositionMapping
    {
        size_t tuple_index{};
        size_t key_index{};
        std::vector<FunctionBasePtr> functions;
    };

    MergeTreeSetIndex(const Columns & set_elements, std::vector<KeyTuplePositionMapping> && indexes_mapping_);

    size_t size() const { return ordered_set.at(0)->size(); }

    bool hasMonotonicFunctionsChain() const;

    BoolMask checkInRange(const Ranges & key_ranges, const DataTypes & data_types, bool single_point = false) const;

    /// Optimized overload. Instead of all/prefix of key columns, any subsequence of key column information (in order) can be given.
    /// `key_col_to_sparse_pos` maps key index to position in `sparse_hyperrectangle`, or -1 if not tracked.
    /// If some key column >= `key_col_to_sparse_pos`.size(), it is considered as not tracked.
    /// See KeyCondition::checkInRange for explanation of relevant parameters.
    BoolMask checkInRange(const std::vector<int> & key_col_to_sparse_pos, const Ranges & sparse_key_ranges, const DataTypes & sparse_data_types, bool single_point = false) const;

    const Columns & getOrderedSet() const { return ordered_set; }

    const std::vector<KeyTuplePositionMapping> & getIndexesMapping() const { return indexes_mapping; }

private:
    using FieldValue = SetIndexDetail::FieldValue;
    using FieldValueRange = SetIndexDetail::FieldValueRange;
    using FieldValueRanges = SetIndexDetail::FieldValueRanges;

    /// Everything after the key ranges have been resolved: the binary searches and the
    /// at-most-one-element shortcut. The two `checkInRange` overloads differ only in how they
    /// resolve those ranges, so this is the whole of what they share.
    BoolMask finishCheckInRange(const FieldValueRanges & ranges, size_t tuple_size) const;

    /// Buffer reused across checkInRange calls (which run once per mark during index analysis)
    /// to avoid per-call column allocations. For fixed-width key types it is a per-thread cache
    /// (invalidated by the next call on the same thread) whose retained size is small and bounded.
    /// Variable-width key types (e.g. String) could pin arbitrarily large reserved capacity in
    /// thread-local storage after the query ends, so for them the given query-scoped scratch
    /// buffer is filled and returned instead.
    FieldValueRanges & getFieldValueRangesBuffer(FieldValueRanges & scratch) const;

    // If all arguments in tuple are key columns, we can optimize NOT IN when there is only one element.
    bool has_all_keys;
    Columns ordered_set;
    std::vector<KeyTuplePositionMapping> indexes_mapping;
    const UInt64 instance_id;
    /// Whether all key columns have fixed-width values, making the ranges buffer cacheable.
    bool cache_ranges = false;
};

/** A disjunction of key constraints: entry `j` constrains key column `i` to the closed interval
  * [lower[i][j], upper[i][j]]. An equality is lower == upper.
  *
  * Where `MergeTreeSetIndex` answers "is any stored key tuple inside this granule's key range",
  * this answers "does any stored interval of key tuples meet it". It exists because a disjunction
  * of `n` key constraints has no compact form today: `KeyCondition` can express one through
  * `Range`, but `n` of them expand into an OR of RPN atoms.
  *
  * Entries must be ordered by their lower corner, and every entry must be non-empty. They need not
  * be disjoint: a running maximum over the upper corners answers the granule test without the upper
  * corners being ordered, which is what lets an entry constrain more than one column by a range.
  *
  * Whether the entries happen to be disjoint is recorded, because it is what makes `contains`
  * answerable by a single binary search. An entry whose columns before the last are all equalities
  * is a contiguous interval of key tuples; entries built that way can be coalesced into disjoint
  * ones, and anything more general cannot.
  *
  * Intervals are closed, which avoids carrying per-entry, per-column inclusion flags.
  */
class MergeTreeKeyRangeSet
{
public:
    using KeyTuplePositionMapping = MergeTreeSetIndex::KeyTuplePositionMapping;

    MergeTreeKeyRangeSet(Columns lower_, Columns upper_, std::vector<KeyTuplePositionMapping> && indexes_mapping_);

    size_t size() const { return lower.at(0)->size(); }

    /// `can_be_false` is always true: an entry can say a granule may match, never that all of it does.
    ///
    /// Exact when every entry is an interval of key tuples, which is what equality on a sort-key
    /// prefix produces. For an entry that constrains a non-final column by a range, the test uses
    /// the entry's lexicographic span, which contains the entry - so the answer stays sound and
    /// only loses selectivity.
    BoolMask checkInRange(const Ranges & key_ranges, const DataTypes & data_types, bool single_point = false) const;

    /// Sparse overload, mirroring `MergeTreeSetIndex`: `key_col_to_sparse_pos` maps a key column to
    /// its position in the given ranges, or -1 when nothing is known about it, in which case it is
    /// treated as unconstrained. This is the form `KeyCondition::checkInHyperrectangle` uses.
    BoolMask checkInRange(
        const std::vector<int> & key_col_to_sparse_pos,
        const Ranges & sparse_key_ranges,
        const DataTypes & sparse_data_types,
        bool single_point = false) const;

    /// Whether the entries are pairwise non-overlapping. Only then can `contains` be answered by a
    /// single binary search, so a caller that needs the row-level test must check this first.
    bool isDisjoint() const { return disjoint; }

    /// Whether the key tuple at `row` of `key_columns` lies in any entry. This is the row-level
    /// question, as against `checkInRange`'s granule-level one, and it is exact - but only over
    /// disjoint entries, where at most one can contain a given tuple and a binary search finds it.
    /// Raises if the entries are not disjoint rather than answering approximately: a caller reaching
    /// here without checking `isDisjoint` has a bug, and a wrong row-level answer is a wrong result.
    ///
    /// `key_columns` must hold one column per tuple position, in the same order as the entries.
    bool contains(const Columns & key_columns, size_t row) const;

    const std::vector<KeyTuplePositionMapping> & getIndexesMapping() const { return indexes_mapping; }

private:
    SetIndexDetail::FieldValueRanges makeValueRanges() const;
    BoolMask finish(const SetIndexDetail::FieldValueRanges & ranges) const;

    Columns lower;
    Columns upper;
    /// Running lexicographic maximum of `upper` over entries `0..j`. Lets the granule test ask
    /// "does any entry up to here reach the range's left corner" in constant time, which is what
    /// removes the need for `upper` itself to be ordered.
    Columns prefix_max_upper;
    bool disjoint = false;
    std::vector<KeyTuplePositionMapping> indexes_mapping;
};

using MergeTreeKeyRangeSetPtr = std::shared_ptr<const MergeTreeKeyRangeSet>;

/// What `buildKeyRangeSet` produced. The two outcomes are not interchangeable: one says the key is
/// constrained to the entries, the other says it is constrained to nothing at all. Returning a
/// single null pointer for both invites a caller to read "no usable set, prune nothing" where the
/// truth is "no row can match", which is the difference between reading everything and reading
/// nothing.
enum class KeyRangeSetOutcome : uint8_t
{
    /// `set` is non-null and holds at least one entry.
    Built,
    /// No entry survived, so the key is constrained to the empty set and no row can match. A caller
    /// pruning may drop every part; a caller rewriting a predicate must produce a false one.
    MatchesNothing,
};

struct KeyRangeSetBuildResult
{
    KeyRangeSetOutcome outcome = KeyRangeSetOutcome::MatchesNothing;
    /// Non-null exactly when `outcome` is `Built`.
    MergeTreeKeyRangeSetPtr set;
};

/** Builds a key range set from per-position corner columns.
  *
  * Takes one lower and one upper corner column per key position. A column constrained by equality
  * has the same column in both; a column left unconstrained uses the whole range. Nothing about
  * the layout is positional, so any number of positions may carry a range.
  *
  * Does the work `MergeTreeKeyRangeSet`'s preconditions require and it deliberately does not do
  * itself: drops rows that cannot match, and orders entries by their lower corner. Entries that are
  * intervals of key tuples - every column before the last an equality - are also merged where they
  * overlap, which makes them disjoint; more general entries are left as they are, because merging
  * two boxes does not give a box.
  *
  * A row is dropped when its range is empty, and when any of its bounds is NULL - `k BETWEEN NULL
  * AND 5` is NULL rather than true, so such a row satisfies nothing and contributes no entry.
  */
KeyRangeSetBuildResult buildKeyRangeSet(
    const Columns & lower,
    const Columns & upper,
    std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> indexes_mapping);

namespace SetIndexDetail
{

/// Resolve a granule's key ranges into one value range per tuple position, applying that position's
/// monotonic function chain. False means a position could not be resolved and the caller must
/// answer `{true, true}`.
bool resolveKeyRanges(
    const std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> & indexes_mapping,
    const Ranges & key_ranges,
    const DataTypes & data_types,
    bool single_point,
    FieldValueRanges & ranges);

/// As above, but for the sparse form, where a key column may be absent from the given ranges and is
/// then unconstrained. The two differ in that one bails when a position cannot be located and the
/// other carries on, which is why they are separate rather than one function with a flag.
bool resolveSparseKeyRanges(
    const std::vector<MergeTreeSetIndex::KeyTuplePositionMapping> & indexes_mapping,
    const std::vector<int> & key_col_to_sparse_pos,
    const Ranges & sparse_key_ranges,
    const DataTypes & sparse_data_types,
    bool single_point,
    FieldValueRanges & ranges);

}

}
