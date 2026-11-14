#pragma once

#include "common.hpp"

#include <vector>

struct ZoneInfo {
    std::wstring key;
    std::wstring display;
    int standardOffsetMin = 0;
    DYNAMIC_TIME_ZONE_INFORMATION info{};
};

struct ClockText {
    std::wstring label;
    std::wstring time;
    std::wstring date;
    bool ok = false;
};

void LoadZones();
const std::vector<ZoneInfo>& Zones();
const ZoneInfo* FindZone(const std::wstring& key);

bool ConvertUtc(const DYNAMIC_TIME_ZONE_INFORMATION& tz, const SYSTEMTIME& utc, SYSTEMTIME& local, int& offsetMin);
const char* TimeConvertApi();

std::wstring FormatOffset(int minutesEast);
std::wstring FormatTime(const SYSTEMTIME& local, bool seconds);
std::wstring FormatDateLine(const SYSTEMTIME& local, int offsetMin);
ClockText MakeClockText(const std::wstring& label, const std::wstring& zoneKey, bool seconds);
