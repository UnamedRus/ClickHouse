#include <Storages/MergeTree/TextIndexMapGranule.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Common/Exception.h>

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace DB
{

namespace ErrorCodes
{
    extern const int SUPPORT_IS_DISABLED;
}

MapGranuleSlots computeMapGranuleSlots(const MapGranuleEntries & granules)
{
    MapGranuleSlots result;

    /// Global key frequency = number of granules a key appears in.
    std::unordered_map<std::string_view, size_t> key_frequency;
    UInt64 stride = 0;
    for (const auto & granule : granules)
    {
        stride = std::max<UInt64>(stride, granule.size());
        for (const auto & [key, values] : granule)
            ++key_frequency[key];
    }
    result.stride = std::max<UInt64>(stride, 1);

    const UInt64 total_slots = static_cast<UInt64>(granules.size()) * result.stride;
    if (total_slots > std::numeric_limits<UInt32>::max())
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
            "Cannot build granule map text index: granules * stride ({}) exceeds the maximum slot id {}",
            total_slots, std::numeric_limits<UInt32>::max());

    /// token -> collected slot ids (may receive duplicates across values/granules; sorted+uniqued at the end).
    std::unordered_map<std::string, std::vector<UInt32>> postings;

    for (size_t g = 0; g < granules.size(); ++g)
    {
        /// Rank this granule's keys by (global frequency desc, key asc) -> local slot.
        std::vector<size_t> order(granules[g].size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::ranges::sort(order, [&](size_t lhs, size_t rhs)
        {
            const auto & kl = granules[g][lhs].first;
            const auto & kr = granules[g][rhs].first;
            const size_t fl = key_frequency[kl];
            const size_t fr = key_frequency[kr];
            return fl != fr ? fl > fr : kl < kr;
        });

        for (UInt32 slot = 0; slot < order.size(); ++slot)
        {
            const auto & [key, values] = granules[g][order[slot]];
            const UInt32 kid = static_cast<UInt32>(g * result.stride + slot);

            postings[String(1, MAP_KEY_NAMESPACE) + key].push_back(kid);
            for (const auto & value : values)
                postings[String(1, MAP_VALUE_NAMESPACE) + value].push_back(kid);
        }
    }

    result.postings.reserve(postings.size());
    for (auto & [token, ids] : postings)
    {
        std::ranges::sort(ids);
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        result.postings.emplace_back(token, std::move(ids));
    }
    /// Deterministic order for reproducible dictionaries.
    std::ranges::sort(result.postings, [](const auto & a, const auto & b) { return a.first < b.first; });

    return result;
}

}
