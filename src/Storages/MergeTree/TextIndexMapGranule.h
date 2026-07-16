#pragma once

#include <base/types.h>
#include <vector>
#include <utility>

namespace DB
{

using MapGranuleEntries = std::vector<std::vector<std::pair<String, std::vector<String>>>>;

struct MapGranuleSlots
{
    UInt64 stride = 0;
    std::vector<std::pair<String, std::vector<UInt32>>> postings;
};

MapGranuleSlots computeMapGranuleSlots(const MapGranuleEntries & granules);

inline UInt64 granuleOfSlot(UInt32 kid, UInt64 stride)
{
    return stride ? kid / stride : 0;
}

}
