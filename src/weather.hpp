#pragma once

#include "common.hpp"

#include <string>
#include <vector>

struct GeoPlace {
    std::wstring name;
    std::wstring detail;
    double latitude = 0;
    double longitude = 0;
};

const wchar_t* WeatherDescription(int code);
std::wstring FormatTemperature(double tempC, bool fahrenheit);

bool ParseForecastBody(const std::string& body, double& tempC, int& code);
bool ParseGeocodingBody(const std::string& body, std::vector<GeoPlace>& out);

// Network calls. Safe to run off the UI thread. Return false on any failure.
bool FetchForecast(double latitude, double longitude, double& tempC, int& code);
bool FetchCities(const std::wstring& query, std::vector<GeoPlace>& out);
