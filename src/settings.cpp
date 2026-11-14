#include "settings.hpp"

#include <shlobj.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct Value {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    const Value* field(const char* key) const {
        if (type != OBJ) return nullptr;
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool Parse(Value& out) {
        if (!ParseValue(out)) return false;
        Skip();
        return i_ == s_.size();
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void Skip() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }

    bool Consume(char c) {
        Skip();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    static int Hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static int Hex4(const std::string& s, size_t i) {
        int v = 0;
        for (int k = 0; k < 4; ++k) {
            int h = Hex(s[i + k]);
            if (h < 0) return -1;
            v = (v << 4) | h;
        }
        return v;
    }

    static void AppendUtf8(std::string& out, int cp) {
        if (cp < 0 || cp > 0x10FFFF) cp = 0xFFFD;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseString(std::string& out) {
        if (!Consume('"')) return false;
        out.clear();
        while (i_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= s_.size()) return false;
                char e = s_[i_++];
                switch (e) {
                case '"':
                case '\\':
                case '/': out.push_back(e); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i_ + 4 > s_.size()) return false;
                    int cp = Hex4(s_, i_);
                    if (cp < 0) return false;
                    i_ += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        int lo = Hex4(s_, i_ + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i_ += 6;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
                }
            } else if (c < 0x20) {
                return false;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return false;
    }

    bool ParseNumber(double& out) {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) return false;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        char* end = nullptr;
        out = std::strtod(s_.c_str() + start, &end);
        if (end != s_.c_str() + i_) return false;
        return true;
    }

    bool ParseValue(Value& out);
    bool ParseObject(Value& out);
    bool ParseArray(Value& out);
};

bool Parser::ParseObject(Value& out) {
    if (!Consume('{')) return false;
    out.type = Value::OBJ;
    out.obj.clear();
    if (Consume('}')) return true;
    while (true) {
        std::string key;
        if (!ParseString(key)) return false;
        if (!Consume(':')) return false;
        Value v;
        if (!ParseValue(v)) return false;
        out.obj.emplace_back(std::move(key), std::move(v));
        if (Consume('}')) return true;
        if (!Consume(',')) return false;
    }
}

bool Parser::ParseArray(Value& out) {
    if (!Consume('[')) return false;
    out.type = Value::ARR;
    out.arr.clear();
    if (Consume(']')) return true;
    while (true) {
        Value v;
        if (!ParseValue(v)) return false;
        out.arr.push_back(std::move(v));
        if (Consume(']')) return true;
        if (!Consume(',')) return false;
    }
}

bool Parser::ParseValue(Value& out) {
    Skip();
    if (i_ >= s_.size()) return false;
    char c = s_[i_];
    if (c == '"') {
        out.type = Value::STR;
        return ParseString(out.str);
    }
    if (c == '{') return ParseObject(out);
    if (c == '[') return ParseArray(out);
    if (c == 't' && s_.compare(i_, 4, "true") == 0) {
        i_ += 4;
        out.type = Value::BOOL;
        out.b = true;
        return true;
    }
    if (c == 'f' && s_.compare(i_, 5, "false") == 0) {
        i_ += 5;
        out.type = Value::BOOL;
        out.b = false;
        return true;
    }
    if (c == 'n' && s_.compare(i_, 4, "null") == 0) {
        i_ += 4;
        out.type = Value::NUL;
        return true;
    }
    if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
        out.type = Value::NUM;
        return ParseNumber(out.num);
    }
    return false;
}

void AppendJsonString(std::string& out, const std::string& utf8) {
    out.push_back('"');
    for (unsigned char c : utf8) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

int ClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Older builds stored size as a preset index: 0 small, 1 standard, 2 large.
int ScaleFromPreset(int preset) {
    if (preset <= 0) return 86;
    if (preset == 1) return 100;
    if (preset == 2) return 126;
    return ClampInt(preset, kScaleMin, kScaleMax);
}

bool Apply(const Value& root, Settings& s) {
    if (root.type != Value::OBJ) return false;
    if (const Value* v = root.field("alwaysOnTop")) {
        if (v->type == Value::BOOL) s.alwaysOnTop = v->b;
    }
    if (const Value* v = root.field("opacity")) {
        if (v->type == Value::NUM) s.opacity = ClampInt((int)std::lround(v->num), kOpacityMin, kOpacityMax);
    }
    bool sawScale = false;
    if (const Value* v = root.field("scale")) {
        if (v->type == Value::NUM) {
            s.scale = ClampInt((int)std::lround(v->num), kScaleMin, kScaleMax);
            sawScale = true;
        }
    }
    if (!sawScale) {
        if (const Value* v = root.field("size")) {
            if (v->type == Value::NUM) s.scale = ScaleFromPreset((int)std::lround(v->num));
        }
    }
    if (const Value* v = root.field("showSeconds")) {
        if (v->type == Value::BOOL) s.showSeconds = v->b;
    }
    if (const Value* v = root.field("startWithWindows")) {
        if (v->type == Value::BOOL) s.startWithWindows = v->b;
    }
    if (const Value* v = root.field("showWeather")) {
        if (v->type == Value::BOOL) s.showWeather = v->b;
    }
    if (const Value* v = root.field("fahrenheit")) {
        if (v->type == Value::BOOL) s.fahrenheit = v->b;
    }
    if (const Value* v = root.field("focusMin")) {
        if (v->type == Value::NUM) s.focusMin = ClampInt((int)std::lround(v->num), 1, 180);
    }
    if (const Value* v = root.field("shortBreakMin")) {
        if (v->type == Value::NUM) s.shortBreakMin = ClampInt((int)std::lround(v->num), 1, 90);
    }
    if (const Value* v = root.field("longBreakMin")) {
        if (v->type == Value::NUM) s.longBreakMin = ClampInt((int)std::lround(v->num), 1, 90);
    }
    if (const Value* v = root.field("roundsBeforeLong")) {
        if (v->type == Value::NUM) s.roundsBeforeLong = ClampInt((int)std::lround(v->num), 1, 12);
    }
    bool sawX = false, sawY = false;
    if (const Value* v = root.field("x")) {
        if (v->type == Value::NUM) {
            s.x = (int)std::lround(v->num);
            sawX = true;
        }
    }
    if (const Value* v = root.field("y")) {
        if (v->type == Value::NUM) {
            s.y = (int)std::lround(v->num);
            sawY = true;
        }
    }
    s.hasPosition = sawX && sawY;
    if (const Value* clocks = root.field("clocks")) {
        if (clocks->type == Value::ARR) {
            for (int i = 0; i < 2 && i < (int)clocks->arr.size(); ++i) {
                const Value& c = clocks->arr[i];
                if (const Value* lab = c.field("label")) {
                    if (lab->type == Value::STR) {
                        std::wstring w = Utf8ToWide(lab->str);
                        Trim(w);
                        if (!w.empty()) {
                            if (w.size() > 24) w.resize(24);
                            s.clock[i].label = std::move(w);
                        }
                    }
                }
                if (const Value* tz = c.field("timeZone")) {
                    if (tz->type == Value::STR) {
                        std::wstring w = Utf8ToWide(tz->str);
                        Trim(w);
                        if (!w.empty() && w.size() < 128) s.clock[i].timeZone = std::move(w);
                    }
                }
                if (const Value* lat = c.field("latitude")) {
                    if (lat->type == Value::NUM && lat->num >= -90 && lat->num <= 90) s.clock[i].latitude = lat->num;
                }
                if (const Value* lon = c.field("longitude")) {
                    if (lon->type == Value::NUM && lon->num >= -180 && lon->num <= 180) s.clock[i].longitude = lon->num;
                }
                const Value* temp = c.field("lastTempC");
                const Value* wcode = c.field("lastCode");
                if (temp && wcode && temp->type == Value::NUM && wcode->type == Value::NUM) {
                    s.clock[i].hasWeather = true;
                    s.clock[i].lastTempC = temp->num;
                    s.clock[i].lastCode = (int)std::lround(wcode->num);
                }
            }
        }
    }
    return true;
}

std::wstring ExeDir() {
    wchar_t buf[1024];
    DWORD n = GetModuleFileNameW(nullptr, buf, 1024);
    if (n == 0 || n >= 1024) return L".";
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return p.substr(0, slash);
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    BOOL ok = TRUE;
    if (size.QuadPart > 0) ok = ReadFile(h, out.data(), (DWORD)size.QuadPart, &got, nullptr);
    CloseHandle(h);
    if (!ok || got != size.QuadPart) return false;
    out.resize(got);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool WriteAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr);
    CloseHandle(h);
    if (!ok || wrote != data.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace

Settings DefaultSettings() {
    Settings s;
    s.clock[0].label = L"都柏林";
    s.clock[0].timeZone = L"GMT Standard Time";
    s.clock[0].latitude = 53.35;
    s.clock[0].longitude = -6.26;
    s.clock[1].label = L"北京";
    s.clock[1].timeZone = L"China Standard Time";
    s.clock[1].latitude = 39.90;
    s.clock[1].longitude = 116.40;
    s.opacity = 85;
    s.scale = 100;
    s.showSeconds = true;
    s.alwaysOnTop = false;
    s.startWithWindows = false;
    s.showWeather = true;
    s.fahrenheit = false;
    s.focusMin = 25;
    s.shortBreakMin = 5;
    s.longBreakMin = 15;
    s.roundsBeforeLong = 4;
    s.hasPosition = false;
    return s;
}

std::string FormatNum(double v, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

std::string SettingsToJson(const Settings& s) {
    std::string o;
    o += "{\n";
    o += std::string("  \"alwaysOnTop\": ") + (s.alwaysOnTop ? "true" : "false") + ",\n";
    o += "  \"opacity\": " + std::to_string(s.opacity) + ",\n";
    o += "  \"scale\": " + std::to_string(s.scale) + ",\n";
    o += std::string("  \"showSeconds\": ") + (s.showSeconds ? "true" : "false") + ",\n";
    o += std::string("  \"startWithWindows\": ") + (s.startWithWindows ? "true" : "false") + ",\n";
    o += std::string("  \"showWeather\": ") + (s.showWeather ? "true" : "false") + ",\n";
    o += std::string("  \"fahrenheit\": ") + (s.fahrenheit ? "true" : "false") + ",\n";
    o += "  \"focusMin\": " + std::to_string(s.focusMin) + ",\n";
    o += "  \"shortBreakMin\": " + std::to_string(s.shortBreakMin) + ",\n";
    o += "  \"longBreakMin\": " + std::to_string(s.longBreakMin) + ",\n";
    o += "  \"roundsBeforeLong\": " + std::to_string(s.roundsBeforeLong) + ",\n";
    if (s.hasPosition) {
        o += "  \"x\": " + std::to_string(s.x) + ",\n";
        o += "  \"y\": " + std::to_string(s.y) + ",\n";
    }
    o += "  \"clocks\": [\n";
    for (int i = 0; i < 2; ++i) {
        o += "    {\"label\": ";
        AppendJsonString(o, WideToUtf8(s.clock[i].label));
        o += ", \"timeZone\": ";
        AppendJsonString(o, WideToUtf8(s.clock[i].timeZone));
        o += ", \"latitude\": " + FormatNum(s.clock[i].latitude, 4);
        o += ", \"longitude\": " + FormatNum(s.clock[i].longitude, 4);
        if (s.clock[i].hasWeather) {
            o += ", \"lastTempC\": " + FormatNum(s.clock[i].lastTempC, 1);
            o += ", \"lastCode\": " + std::to_string(s.clock[i].lastCode);
        }
        o += "}";
        if (i == 0) o += ",";
        o += "\n";
    }
    o += "  ]\n}\n";
    return o;
}

bool SettingsFromJson(const std::string& json, Settings& s) {
    Parser parser(json);
    Value root;
    if (!parser.Parse(root)) return false;
    Settings out = DefaultSettings();
    if (!Apply(root, out)) return false;
    s = std::move(out);
    return true;
}

std::wstring SettingsPath() {
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring dir = std::wstring(buf) + L"\\DualZoneClock";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\settings.json";
    }
    return ExeDir() + L"\\settings.json";
}

bool LoadSettings(Settings& s) {
    std::string raw;
    if (!ReadFileUtf8(SettingsPath(), raw)) return false;
    return SettingsFromJson(raw, s);
}

bool SaveSettings(const Settings& s) {
    return WriteAtomic(SettingsPath(), SettingsToJson(s));
}
