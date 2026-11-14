#include "timezone.hpp"

#include <shlwapi.h>

#include <algorithm>
#include <cstring>

namespace {

std::vector<ZoneInfo> g_zones;

std::wstring RegString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, &type, s.data(), &size) != ERROR_SUCCESS) return {};
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

std::wstring ExpandIndirect(const std::wstring& src) {
    if (src.empty() || src[0] != L'@') return {};
    wchar_t out[512];
    out[0] = 0;
    if (SUCCEEDED(SHLoadIndirectString(src.c_str(), out, 512, nullptr)) && out[0]) return out;
    return {};
}

std::wstring ZoneDisplay(const std::wstring& key) {
    std::wstring path = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\" + key;
    HKEY keyh = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &keyh) != ERROR_SUCCESS) return key;
    std::wstring display = ExpandIndirect(RegString(keyh, L"MUI_Display"));
    if (display.empty()) display = RegString(keyh, L"Display");
    RegCloseKey(keyh);
    if (display.empty()) display = key;
    return display;
}

}  // namespace

void LoadZones() {
    g_zones.clear();
    DWORD index = 0;
    for (;;) {
        DYNAMIC_TIME_ZONE_INFORMATION dtzi{};
        DWORD rc = EnumDynamicTimeZoneInformation(index, &dtzi);
        if (rc != ERROR_SUCCESS) break;
        ++index;
        ZoneInfo z;
        z.info = dtzi;
        z.key = dtzi.TimeZoneKeyName;
        if (z.key.empty()) continue;
        z.standardOffsetMin = -(dtzi.Bias + dtzi.StandardBias);
        z.display = ZoneDisplay(z.key);
        g_zones.push_back(std::move(z));
    }
    std::sort(g_zones.begin(), g_zones.end(), [](const ZoneInfo& a, const ZoneInfo& b) {
        if (a.standardOffsetMin != b.standardOffsetMin) return a.standardOffsetMin < b.standardOffsetMin;
        return CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a.display.c_str(), -1, b.display.c_str(), -1) == CSTR_LESS_THAN;
    });
}

const std::vector<ZoneInfo>& Zones() { return g_zones; }

const ZoneInfo* FindZone(const std::wstring& key) {
    for (const ZoneInfo& z : g_zones) {
        if (z.key == key) return &z;
    }
    return nullptr;
}

static const char* g_convertApi = "none";

bool ConvertUtc(const DYNAMIC_TIME_ZONE_INFORMATION& tz, const SYSTEMTIME& utc, SYSTEMTIME& local, int& offsetMin) {
    // Prefer the per-year rules plus SystemTimeToTzSpecificLocalTime. That pair is
    // what the self-test exercises, and it honors daylight saving for that year.
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using YearFn = BOOL(WINAPI*)(USHORT, PDYNAMIC_TIME_ZONE_INFORMATION, LPTIME_ZONE_INFORMATION);
    using ExFn = BOOL(WINAPI*)(const DYNAMIC_TIME_ZONE_INFORMATION*, const SYSTEMTIME*, LPSYSTEMTIME);
    auto yearFn = reinterpret_cast<YearFn>(GetProcAddress(kernel, "GetTimeZoneInformationForYear"));
    auto ex = reinterpret_cast<ExFn>(GetProcAddress(kernel, "SystemTimeToTzSpecificLocalTimeEx"));
    BOOL ok = FALSE;
    if (yearFn) {
        TIME_ZONE_INFORMATION tzi{};
        DYNAMIC_TIME_ZONE_INFORMATION copy = tz;
        if (!yearFn(utc.wYear, &copy, &tzi)) return false;
        ok = SystemTimeToTzSpecificLocalTime(&tzi, &utc, &local);
        g_convertApi = "year";
    } else if (ex) {
        ok = ex(&tz, &utc, &local);
        g_convertApi = "ex";
    } else {
        TIME_ZONE_INFORMATION tzi{};
        static_assert(sizeof(TIME_ZONE_INFORMATION) < sizeof(DYNAMIC_TIME_ZONE_INFORMATION), "tz struct layout");
        memcpy(&tzi, &tz, sizeof(tzi));
        ok = SystemTimeToTzSpecificLocalTime(&tzi, &utc, &local);
        g_convertApi = "current";
    }
    if (!ok) return false;
    FILETIME ftU{}, ftL{};
    if (!SystemTimeToFileTime(&utc, &ftU) || !SystemTimeToFileTime(&local, &ftL)) return false;
    ULARGE_INTEGER u{}, l{};
    u.LowPart = ftU.dwLowDateTime;
    u.HighPart = ftU.dwHighDateTime;
    l.LowPart = ftL.dwLowDateTime;
    l.HighPart = ftL.dwHighDateTime;
    long long diff = (long long)l.QuadPart - (long long)u.QuadPart;
    offsetMin = (int)(diff / 600000000LL);
    return true;
}

std::wstring FormatOffset(int minutesEast) {
    int sign = minutesEast < 0 ? -1 : 1;
    int a = minutesEast < 0 ? -minutesEast : minutesEast;
    int h = a / 60;
    int m = a % 60;
    wchar_t buf[32];
    if (m == 0) Fmt(buf, L"UTC%c%d", sign < 0 ? L'-' : L'+', h);
    else Fmt(buf, L"UTC%c%d:%02d", sign < 0 ? L'-' : L'+', h, m);
    return buf;
}

std::wstring FormatTime(const SYSTEMTIME& local, bool seconds) {
    wchar_t buf[16];
    if (seconds) Fmt(buf, L"%02u:%02u:%02u", local.wHour, local.wMinute, local.wSecond);
    else Fmt(buf, L"%02u:%02u", local.wHour, local.wMinute);
    return buf;
}

std::wstring FormatDateLine(const SYSTEMTIME& local, int offsetMin) {
    static const wchar_t* kWeek[] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
    unsigned dow = local.wDayOfWeek;
    if (dow > 6) dow = 0;
    wchar_t buf[32];
    // Don't pass wide strings through %s: MinGW's swprintf treats %s as char*.
    Fmt(buf, L"%04u年%02u月%02u日 ", local.wYear, local.wMonth, local.wDay);
    return std::wstring(buf) + kWeek[dow] + L" · " + FormatOffset(offsetMin);
}

const char* TimeConvertApi() { return g_convertApi; }

ClockText MakeClockText(const std::wstring& label, const std::wstring& zoneKey, bool seconds) {
    ClockText t;
    t.label = label;
    const ZoneInfo* zone = FindZone(zoneKey);
    if (!zone) {
        t.time = seconds ? L"--:--:--" : L"--:--";
        t.date = L"时区不可用";
        t.ok = false;
        return t;
    }
    SYSTEMTIME utc{}, local{};
    GetSystemTime(&utc);
    int offset = 0;
    if (!ConvertUtc(zone->info, utc, local, offset)) {
        t.time = seconds ? L"--:--:--" : L"--:--";
        t.date = L"无法换算";
        t.ok = false;
        return t;
    }
    t.time = FormatTime(local, seconds);
    t.date = FormatDateLine(local, offset);
    t.ok = true;
    return t;
}
