#pragma once

#include "common.hpp"

struct ClockPref {
    std::wstring label;
    std::wstring timeZone;
};

struct Settings {
    int x = 0;
    int y = 0;
    bool hasPosition = false;
    bool alwaysOnTop = false;
    int opacity = 85;
    int size = 1;
    bool showSeconds = true;
    bool startWithWindows = false;
    ClockPref clock[2];
};

Settings DefaultSettings();
std::string SettingsToJson(const Settings& s);
bool SettingsFromJson(const std::string& json, Settings& s);
std::wstring SettingsPath();
bool LoadSettings(Settings& s);
bool SaveSettings(const Settings& s);
