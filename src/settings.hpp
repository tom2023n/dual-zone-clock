#pragma once

#include "common.hpp"

struct ClockPref {
    std::wstring label;
    std::wstring timeZone;
    double latitude = 0;
    double longitude = 0;
    bool hasWeather = false;
    double lastTempC = 0;
    int lastCode = -1;
};

// Opacity is a percent. Scale is a percent of the standard clock size.
constexpr int kOpacityMin = 20;
constexpr int kOpacityMax = 100;
constexpr int kScaleMin = 50;
constexpr int kScaleMax = 300;

struct Settings {
    int x = 0;
    int y = 0;
    bool hasPosition = false;
    bool alwaysOnTop = false;
    int opacity = 85;
    int scale = 100;
    bool showSeconds = true;
    bool startWithWindows = false;
    bool showWeather = true;
    bool fahrenheit = false;
    int focusMin = 25;
    int shortBreakMin = 5;
    int longBreakMin = 15;
    int roundsBeforeLong = 4;
    ClockPref clock[2];
};

Settings DefaultSettings();
std::string SettingsToJson(const Settings& s);
bool SettingsFromJson(const std::string& json, Settings& s);
std::wstring SettingsPath();
bool LoadSettings(Settings& s);
bool SaveSettings(const Settings& s);
