#include "weather.hpp"

#include <winhttp.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

struct Inet {
    HINTERNET h = nullptr;
    ~Inet() {
        if (h) WinHttpCloseHandle(h);
    }
};

bool ExtractNumberAt(const std::string& json, const char* key, double& out, bool last) {
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = last ? json.rfind(pattern) : json.find(pattern);
    if (p == std::string::npos) return false;
    p = json.find(':', p + pattern.size());
    if (p == std::string::npos) return false;
    const char* begin = json.c_str() + p + 1;
    char* end = nullptr;
    double v = std::strtod(begin, &end);
    if (end == begin) return false;
    out = v;
    return true;
}

bool ExtractString(const std::string& json, const char* key, std::string& out) {
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return false;
    p = json.find(':', p + pattern.size());
    if (p == std::string::npos) return false;
    p = json.find('"', p + 1);
    if (p == std::string::npos) return false;
    ++p;
    out.clear();
    while (p < json.size()) {
        unsigned char c = (unsigned char)json[p++];
        if (c == '"') return true;
        if (c == '\\' && p < json.size()) {
            char e = json[p++];
            if (e == 'u' && p + 4 <= json.size()) {
                int cp = 0;
                bool ok = true;
                for (int k = 0; k < 4; ++k) {
                    char h = json[p++];
                    int d = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (d < 0) {
                        ok = false;
                        break;
                    }
                    cp = (cp << 4) | d;
                }
                if (!ok) return false;
                wchar_t wch = (wchar_t)cp;
                out += WideToUtf8(std::wstring(1, wch));
            } else if (e == '"' || e == '\\' || e == '/') {
                out.push_back(e);
            } else if (e == 'n') out.push_back('\n');
            else out.push_back(e);
        } else {
            out.push_back((char)c);
        }
    }
    return false;
}

std::string EncodeQuery(const std::wstring& text) {
    std::string utf = WideToUtf8(text);
    std::string out;
    for (unsigned char c : utf) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

bool HttpGet(const wchar_t* host, const std::wstring& path, std::string& body) {
    body.clear();
    Inet session;
    session.h = WinHttpOpen(L"DualZoneClock/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        session.h = WinHttpOpen(L"DualZoneClock/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) return false;
    WinHttpSetTimeouts(session.h, 4000, 4000, 8000, 8000);
    Inet connect;
    connect.h = WinHttpConnect(session.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect.h) return false;
    Inet request;
    request.h = WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.h) return false;
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) return false;
    if (!WinHttpReceiveResponse(request.h, nullptr)) return false;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    if (status < 200 || status >= 300) return false;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) return false;
        if (avail == 0) break;
        if (body.size() + avail > 512 * 1024) return false;
        size_t at = body.size();
        body.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, body.data() + at, avail, &read)) return false;
        body.resize(at + read);
        if (read == 0) break;
    }
    return !body.empty();
}

}  // namespace

const wchar_t* WeatherDescription(int code) {
    switch (code) {
    case 0: return L"晴";
    case 1: return L"大部晴朗";
    case 2: return L"多云";
    case 3: return L"阴";
    case 45: return L"雾";
    case 48: return L"雾凇";
    case 51: return L"小毛毛雨";
    case 53: return L"毛毛雨";
    case 55: return L"大毛毛雨";
    case 56: return L"冻毛毛雨";
    case 57: return L"强冻毛毛雨";
    case 61: return L"小雨";
    case 63: return L"中雨";
    case 65: return L"大雨";
    case 66: return L"冻雨";
    case 67: return L"强冻雨";
    case 71: return L"小雪";
    case 73: return L"中雪";
    case 75: return L"大雪";
    case 77: return L"雪粒";
    case 80: return L"小阵雨";
    case 81: return L"阵雨";
    case 82: return L"强阵雨";
    case 85: return L"小阵雪";
    case 86: return L"阵雪";
    case 95: return L"雷暴";
    case 96: return L"雷暴伴小冰雹";
    case 99: return L"雷暴伴冰雹";
    default: return L"未知天气";
    }
}

std::wstring FormatTemperature(double tempC, bool fahrenheit) {
    double shown = fahrenheit ? tempC * 9.0 / 5.0 + 32.0 : tempC;
    int n = (int)std::lround(shown);
    wchar_t buf[16];
    Fmt(buf, L"%d°%c", n, fahrenheit ? L'F' : L'C');
    return buf;
}

bool ExtractNumber(const std::string& json, const char* key, double& out) {
    return ExtractNumberAt(json, key, out, false);
}

bool ParseForecastBody(const std::string& body, double& tempC, int& code) {
    // current_units lists the same keys as strings; the numeric reading is the last one.
    double temp = 0;
    double raw = 0;
    if (!ExtractNumberAt(body, "temperature_2m", temp, true)) return false;
    if (!ExtractNumberAt(body, "weather_code", raw, true)) return false;
    tempC = temp;
    code = (int)std::lround(raw);
    return true;
}

bool ParseGeocodingBody(const std::string& body, std::vector<GeoPlace>& out) {
    out.clear();
    size_t results = body.find("\"results\"");
    if (results == std::string::npos) return true;
    size_t i = body.find('[', results);
    if (i == std::string::npos) return true;
    int depth = 0;
    size_t objStart = std::string::npos;
    for (; i < body.size(); ++i) {
        char c = body[i];
        if (c == '{') {
            if (depth == 0) objStart = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && objStart != std::string::npos) {
                std::string obj = body.substr(objStart, i - objStart + 1);
                double lat = 0;
                double lon = 0;
                std::string name;
                std::string country;
                std::string admin;
                if (ExtractString(obj, "name", name) && ExtractNumber(obj, "latitude", lat) && ExtractNumber(obj, "longitude", lon)) {
                    ExtractString(obj, "country", country);
                    ExtractString(obj, "admin1", admin);
                    GeoPlace place;
                    place.name = Utf8ToWide(name);
                    place.latitude = lat;
                    place.longitude = lon;
                    std::wstring detail = Utf8ToWide(admin);
                    std::wstring countryW = Utf8ToWide(country);
                    if (!detail.empty() && !countryW.empty() && detail != countryW) detail += L" · " + countryW;
                    else if (detail.empty()) detail = countryW;
                    place.detail = detail;
                    if (!place.name.empty() && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) out.push_back(place);
                }
                objStart = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
        if (out.size() >= 12) break;
    }
    return true;
}

bool FetchForecast(double latitude, double longitude, double& tempC, int& code) {
    char narrow[256];
    std::snprintf(narrow, sizeof(narrow), "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code", latitude, longitude);
    std::string body;
    if (!HttpGet(L"api.open-meteo.com", Utf8ToWide(narrow), body)) return false;
    return ParseForecastBody(body, tempC, code);
}

bool FetchCities(const std::wstring& query, std::vector<GeoPlace>& out) {
    std::wstring path = L"/v1/search?name=" + Utf8ToWide(EncodeQuery(query)) + L"&count=8&language=zh&format=json";
    std::string body;
    if (!HttpGet(L"geocoding-api.open-meteo.com", path, body)) return false;
    return ParseGeocodingBody(body, out);
}
