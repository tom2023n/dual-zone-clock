#include "pomodoro.hpp"
#include "render.hpp"
#include "resource.h"
#include "settings.hpp"
#include "timezone.hpp"
#include "weather.hpp"

#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClass[] = L"DualZoneClockWidget";
constexpr wchar_t kLabelClass[] = L"DualZoneLabelDialog";
constexpr wchar_t kZoneClass[] = L"DualZoneZoneDialog";
constexpr wchar_t kLookClass[] = L"DualZoneLookDialog";
constexpr wchar_t kCityClass[] = L"DualZoneCityDialog";
constexpr wchar_t kNumClass[] = L"DualZoneNumDialog";
constexpr wchar_t kDurClass[] = L"DualZoneDurDialog";
constexpr wchar_t kUntilClass[] = L"DualZoneUntilDialog";

constexpr UINT WM_NUDGE = WM_APP + 1;
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT WM_WEATHER = WM_APP + 3;
constexpr UINT WM_GEOCODE = WM_APP + 4;

constexpr int ID_C1_LABEL = 1001;
constexpr int ID_C1_ZONE = 1002;
constexpr int ID_C2_LABEL = 1003;
constexpr int ID_C2_ZONE = 1004;
constexpr int ID_TOPMOST = 1010;
constexpr int ID_DESKTOP = 1011;
constexpr int ID_SECONDS = 1012;
constexpr int ID_STARTUP = 1013;
constexpr int ID_RESET = 1014;
constexpr int ID_EXIT = 1015;
constexpr int ID_LOOK = 1016;
constexpr int ID_C1_CITY = 1005;
constexpr int ID_C2_CITY = 1006;
constexpr int ID_WEATHER = 1020;
constexpr int ID_UNIT_C = 1021;
constexpr int ID_UNIT_F = 1022;
constexpr int ID_POMO_FOCUS = 1030;
constexpr int ID_POMO_MINUTES = 1031;
constexpr int ID_POMO_UNTIL = 1032;
constexpr int ID_POMO_PAUSE = 1033;
constexpr int ID_POMO_STOP = 1034;
constexpr int ID_POMO_LENGTHS = 1035;
constexpr int IDC_SEARCH = 2010;
constexpr int IDC_H1 = 2020;
constexpr int IDC_H2 = 2021;
constexpr int IDC_H3 = 2022;
constexpr int IDC_H4 = 2023;

constexpr int IDC_EDIT = 2001;
constexpr int IDC_LIST = 2002;

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
Renderer g_renderer;
Settings g_settings;
ClockText g_shown[2];
UINT g_dpi = 96;
int g_w = 0;
int g_h = 0;
bool g_hovered = false;
bool g_dragging = false;
bool g_menuOpen = false;
bool g_dialogOpen = false;
bool g_exiting = false;
WidgetHits g_hits;
bool g_pressInStrip = false;
POINT g_pressClient{};
unsigned g_weatherGen = 0;
int g_weatherLeft = 0;
bool g_weatherBusy = false;
bool g_weatherGotOne = false;
unsigned long long g_weatherStamp = 0;
bool g_inZOrder = false;
POINT g_dragPt{};
int g_dragX = 0;
int g_dragY = 0;
HWINEVENTHOOK g_hook = nullptr;
HICON g_icon = nullptr;
HFONT g_uiFont = nullptr;
UINT g_taskbarCreated = 0;
DWORD g_lastMenuTick = 0;
int g_zTick = 0;

struct MenuItem {
    const wchar_t* text;
    bool popup;
};

struct Theme {
    COLORREF bg;
    COLORREF text;
    COLORREF hi;
    COLORREF check;
    bool dark;
};

int Scale(int px) {
    return MulDiv(px, (int)g_dpi, 96);
}

Theme CurrentTheme() {
    DWORD light = 1;
    DWORD sz = sizeof(light);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegGetValueW(key, nullptr, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &sz);
        RegCloseKey(key);
    }
    Theme t{};
    t.dark = light == 0;
    if (t.dark) {
        t.bg = RGB(32, 32, 36);
        t.text = RGB(240, 240, 242);
        t.hi = RGB(58, 58, 66);
        t.check = RGB(142, 190, 255);
    } else {
        t.bg = RGB(252, 252, 253);
        t.text = RGB(28, 28, 32);
        t.hi = RGB(226, 234, 246);
        t.check = RGB(18, 92, 196);
    }
    return t;
}

HFONT UiFont() {
    if (g_uiFont) return g_uiFont;
    int h = -MulDiv(9, (int)g_dpi, 72);
    g_uiFont = CreateFontW(h, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_renderer.UiFace().c_str());
    return g_uiFont;
}

void DropUiFont() {
    if (g_uiFont) {
        DeleteObject(g_uiFont);
        g_uiFont = nullptr;
    }
}

UINT DpiAtPoint(int x, int y) {
    POINT pt{x, y};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    if (sh) {
        using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        auto fn = reinterpret_cast<Fn>(GetProcAddress(sh, "GetDpiForMonitor"));
        if (fn) fn(mon, 0, &dx, &dy);
        FreeLibrary(sh);
    }
    return dx ? dx : 96;
}

void ClampVisible(int& x, int& y, int w, int h) {
    POINT pt{x + w / 2, y + std::max(1, h) / 2};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    RECT wa = mi.rcWork;
    const int margin = 48;
    if (x > wa.right - margin) x = wa.right - w - 8;
    if (y > wa.bottom - margin) y = wa.bottom - h - 8;
    if (x + w < wa.left + margin) x = wa.left + 8;
    if (y + h < wa.top + margin) y = wa.top + 8;
}

void DefaultPlace(int w, int h, int& x, int& y) {
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    x = wa.right - w - 24;
    y = wa.top + 24;
    ClampVisible(x, y, w, h);
}

bool SetRunAtStartup(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const wchar_t* name = L"DualZoneClock";
    if (!enable) {
        LONG d = RegDeleteValueW(key, name);
        RegCloseKey(key);
        return d == ERROR_SUCCESS || d == ERROR_FILE_NOT_FOUND;
    }
    wchar_t path[1024];
    DWORD n = GetModuleFileNameW(nullptr, path, 1024);
    if (n == 0 || n >= 1023) {
        RegCloseKey(key);
        return false;
    }
    std::wstring cmd = L"\"";
    cmd += path;
    cmd += L"\"";
    LONG s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return s == ERROR_SUCCESS;
}

void RemoveTray() {
    if (!g_hwnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdateTip() {
    if (!g_hwnd) return;
    auto hm = [](const std::wstring& t) {
        if (t.size() >= 5) return t.substr(0, 5);
        return t;
    };
    std::wstring tip = L"双时区时钟";
    for (int i = 0; i < 2; ++i) {
        tip += L"\n" + g_shown[i].label + L" " + hm(g_shown[i].time);
        if (g_settings.showWeather && g_settings.clock[i].hasWeather) {
            tip += L" · " + std::wstring(WeatherDescription(g_settings.clock[i].lastCode)) + L" " + FormatTemperature(g_settings.clock[i].lastTempC, g_settings.fahrenheit);
        }
    }
    if (TimerActive()) tip += L"\n" + TimerCaption();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    CopyW(nid.szTip, sizeof(nid.szTip) / sizeof(wchar_t), tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AddTray() {
    if (!g_hwnd) return;
    if (!g_icon) {
        g_icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    CopyW(nid.szTip, sizeof(nid.szTip) / sizeof(wchar_t), L"双时区时钟");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    UpdateTip();
}

void ApplyZOrder() {
    if (!g_hwnd || g_inZOrder || g_exiting) return;
    g_inZOrder = true;
    LONG ex = GetWindowLongW(g_hwnd, GWL_EXSTYLE);
    if (g_settings.alwaysOnTop) ex |= WS_EX_TOPMOST;
    else ex &= ~WS_EX_TOPMOST;
    SetWindowLongW(g_hwnd, GWL_EXSTYLE, ex);
    HWND after = HWND_TOPMOST;
    if (!g_settings.alwaysOnTop) {
        HWND prog = FindWindowW(L"Progman", nullptr);
        after = prog ? prog : HWND_NOTOPMOST;
    }
    SetWindowPos(g_hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    g_inZOrder = false;
}

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (!g_hwnd || g_settings.alwaysOnTop || g_inZOrder || g_dialogOpen || g_menuOpen || g_dragging) return;
    if (hwnd == g_hwnd) return;
    ApplyZOrder();
}

Frame MakeFrame() {
    Frame f;
    f.showWeather = g_settings.showWeather;
    for (int i = 0; i < 2; ++i) {
        f.label[i] = g_shown[i].label;
        f.time[i] = g_shown[i].time;
        f.date[i] = g_shown[i].date;
        if (g_settings.showWeather) {
            f.weatherCode[i] = g_settings.clock[i].hasWeather ? g_settings.clock[i].lastCode : -1;
            f.weatherText[i] = g_settings.clock[i].hasWeather ? FormatTemperature(g_settings.clock[i].lastTempC, g_settings.fahrenheit) : L"--";
        }
    }
    f.showTimer = TimerActive();
    f.timerText = TimerCaption();
    f.timerStyle = TimerStyle();
    f.opacity = g_settings.opacity;
    f.scale = g_settings.scale;
    f.dpi = g_dpi;
    f.hovered = g_hovered;
    return f;
}

void Redraw(bool anchorCenter) {
    if (!g_hwnd) return;
    Frame frame = MakeFrame();
    int w = 0, h = 0;
    if (!g_renderer.Measure(frame, w, h)) return;
    int nx = g_settings.x;
    int ny = g_settings.y;
    if (!g_settings.hasPosition) {
        DefaultPlace(w, h, nx, ny);
        g_settings.hasPosition = true;
    } else if (anchorCenter && g_w > 0 && (w != g_w || h != g_h)) {
        nx += (g_w - w) / 2;
        ny += (g_h - h) / 2;
    }
    ClampVisible(nx, ny, w, h);
    g_settings.x = nx;
    g_settings.y = ny;
    g_w = w;
    g_h = h;
    g_renderer.Draw(g_hwnd, nx, ny, frame, &g_hits);
}

void Refresh(bool force, bool anchorCenter) {
    ClockText next[2];
    for (int i = 0; i < 2; ++i) {
        next[i] = MakeClockText(g_settings.clock[i].label, g_settings.clock[i].timeZone, g_settings.showSeconds);
    }
    bool changed = force;
    bool minute = force;
    for (int i = 0; i < 2; ++i) {
        if (next[i].time != g_shown[i].time || next[i].date != g_shown[i].date || next[i].label != g_shown[i].label) changed = true;
        std::wstring a = next[i].time.size() >= 5 ? next[i].time.substr(0, 5) : next[i].time;
        std::wstring b = g_shown[i].time.size() >= 5 ? g_shown[i].time.substr(0, 5) : g_shown[i].time;
        if (a != b || next[i].label != g_shown[i].label) minute = true;
    }
    g_shown[0] = next[0];
    g_shown[1] = next[1];
    if (changed) Redraw(anchorCenter);
    if (minute) UpdateTip();
}

void Nudge() {
    if (!g_hwnd) return;
    ShowWindow(g_hwnd, SW_SHOW);
    RECT r{};
    GetWindowRect(g_hwnd, &r);
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
        g_settings.hasPosition = false;
        Refresh(true, false);
        SaveSettings(g_settings);
    }
}

void DrawCheck(HDC dc, int x, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x, y + 5, nullptr);
    LineTo(dc, x + 4, y + 9);
    LineTo(dc, x + 11, y + 1);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawChevron(HDC dc, int x, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x, y, nullptr);
    LineTo(dc, x + 5, y + 5);
    LineTo(dc, x, y + 10);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void OnMeasureItem(MEASUREITEMSTRUCT* mi) {
    if (!mi || mi->CtlType != ODT_MENU || !mi->itemData) return;
    auto* item = reinterpret_cast<MenuItem*>(mi->itemData);
    HDC dc = GetDC(g_hwnd);
    HFONT font = UiFont();
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, item->text, (int)wcslen(item->text), &sz);
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    mi->itemWidth = (UINT)(sz.cx + Scale(item->popup ? 64 : 52));
    mi->itemHeight = (UINT)Scale(32);
}

void OnDrawItem(DRAWITEMSTRUCT* di) {
    if (!di || di->CtlType != ODT_MENU || !di->itemData) return;
    auto* item = reinterpret_cast<MenuItem*>(di->itemData);
    Theme theme = CurrentTheme();
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    HBRUSH bg = CreateSolidBrush(theme.bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    bool selected = (di->itemState & ODS_SELECTED) != 0;
    if (selected) {
        RECT hi = rc;
        InflateRect(&hi, -Scale(4), -Scale(3));
        HBRUSH br = CreateSolidBrush(theme.hi);
        HPEN pen = (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ oldBr = SelectObject(dc, br);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, hi.left, hi.top, hi.right, hi.bottom, Scale(8), Scale(8));
        SelectObject(dc, oldBr);
        SelectObject(dc, oldPen);
        DeleteObject(br);
    }
    if (di->itemState & ODS_CHECKED) {
        DrawCheck(dc, rc.left + Scale(12), rc.top + (rc.bottom - rc.top - Scale(10)) / 2, theme.check);
    }
    if (item->popup) {
        DrawChevron(dc, rc.right - Scale(16), rc.top + (rc.bottom - rc.top - Scale(10)) / 2, theme.text);
    }
    HFONT font = UiFont();
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    bool gray = (di->itemState & ODS_GRAYED) != 0;
    SetTextColor(dc, gray ? (theme.dark ? RGB(120, 120, 128) : RGB(150, 150, 156)) : theme.text);
    RECT tr = rc;
    tr.left += Scale(30);
    tr.right -= Scale(item->popup ? 22 : 12);
    DrawTextW(dc, item->text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

HMENU BuildMenu(HBRUSH* background) {
    static MenuItem clock1{L"时钟 1", true};
    static MenuItem clock2{L"时钟 2", true};
    static MenuItem cLabel{L"修改名称…", false};
    static MenuItem cZone{L"选择时区…", false};
    static MenuItem cCity{L"选择城市…", false};
    static MenuItem top{L"始终置顶", false};
    static MenuItem desk{L"留在桌面", false};
    static MenuItem seconds{L"显示秒", false};
    static MenuItem startup{L"开机启动", false};
    static MenuItem reset{L"重置位置", false};
    static MenuItem quit{L"退出", false};
    static MenuItem look{L"大小与透明度…", false};
    static MenuItem weather{L"显示天气", false};
    static MenuItem unitMenu{L"温度单位", true};
    static MenuItem unitC{L"摄氏 °C", false};
    static MenuItem unitF{L"华氏 °F", false};
    static MenuItem pomoMenu{L"番茄钟", true};
    static MenuItem pomoFocus{L"开始专注", false};
    static MenuItem pomoMinutes{L"自定义倒计时…", false};
    static MenuItem pomoUntil{L"倒计时到指定时间…", false};
    static wchar_t pauseText[16] = L"暂停";
    static MenuItem pomoPause{pauseText, false};
    static MenuItem pomoStop{L"停止", false};
    static MenuItem pomoLengths{L"时长设置…", false};
    CopyW(pauseText, 16, TimerActive() && Timer().paused ? L"继续" : L"暂停");

    HMENU menu = CreatePopupMenu();
    HMENU m1 = CreatePopupMenu();
    HMENU m2 = CreatePopupMenu();
    AppendMenuW(m1, MF_OWNERDRAW, ID_C1_LABEL, reinterpret_cast<LPCWSTR>(&cLabel));
    AppendMenuW(m1, MF_OWNERDRAW, ID_C1_ZONE, reinterpret_cast<LPCWSTR>(&cZone));
    AppendMenuW(m1, MF_OWNERDRAW, ID_C1_CITY, reinterpret_cast<LPCWSTR>(&cCity));
    AppendMenuW(m2, MF_OWNERDRAW, ID_C2_LABEL, reinterpret_cast<LPCWSTR>(&cLabel));
    AppendMenuW(m2, MF_OWNERDRAW, ID_C2_ZONE, reinterpret_cast<LPCWSTR>(&cZone));
    AppendMenuW(m2, MF_OWNERDRAW, ID_C2_CITY, reinterpret_cast<LPCWSTR>(&cCity));
    AppendMenuW(menu, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)m1, reinterpret_cast<LPCWSTR>(&clock1));
    AppendMenuW(menu, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)m2, reinterpret_cast<LPCWSTR>(&clock2));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | (g_settings.alwaysOnTop ? MF_CHECKED : 0), ID_TOPMOST, reinterpret_cast<LPCWSTR>(&top));
    AppendMenuW(menu, MF_OWNERDRAW | (!g_settings.alwaysOnTop ? MF_CHECKED : 0), ID_DESKTOP, reinterpret_cast<LPCWSTR>(&desk));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW, ID_LOOK, reinterpret_cast<LPCWSTR>(&look));
    AppendMenuW(menu, MF_OWNERDRAW | (g_settings.showWeather ? MF_CHECKED : 0), ID_WEATHER, reinterpret_cast<LPCWSTR>(&weather));
    HMENU units = CreatePopupMenu();
    AppendMenuW(units, MF_OWNERDRAW | (!g_settings.fahrenheit ? MF_CHECKED : 0), ID_UNIT_C, reinterpret_cast<LPCWSTR>(&unitC));
    AppendMenuW(units, MF_OWNERDRAW | (g_settings.fahrenheit ? MF_CHECKED : 0), ID_UNIT_F, reinterpret_cast<LPCWSTR>(&unitF));
    AppendMenuW(menu, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)units, reinterpret_cast<LPCWSTR>(&unitMenu));
    AppendMenuW(menu, MF_OWNERDRAW | (g_settings.showSeconds ? MF_CHECKED : 0), ID_SECONDS, reinterpret_cast<LPCWSTR>(&seconds));
    HMENU pomo = CreatePopupMenu();
    AppendMenuW(pomo, MF_OWNERDRAW, ID_POMO_FOCUS, reinterpret_cast<LPCWSTR>(&pomoFocus));
    AppendMenuW(pomo, MF_OWNERDRAW, ID_POMO_MINUTES, reinterpret_cast<LPCWSTR>(&pomoMinutes));
    AppendMenuW(pomo, MF_OWNERDRAW, ID_POMO_UNTIL, reinterpret_cast<LPCWSTR>(&pomoUntil));
    AppendMenuW(pomo, MF_OWNERDRAW | (TimerActive() ? 0 : MF_GRAYED), ID_POMO_PAUSE, reinterpret_cast<LPCWSTR>(&pomoPause));
    AppendMenuW(pomo, MF_OWNERDRAW | (TimerActive() ? 0 : MF_GRAYED), ID_POMO_STOP, reinterpret_cast<LPCWSTR>(&pomoStop));
    AppendMenuW(pomo, MF_OWNERDRAW, ID_POMO_LENGTHS, reinterpret_cast<LPCWSTR>(&pomoLengths));
    AppendMenuW(menu, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)pomo, reinterpret_cast<LPCWSTR>(&pomoMenu));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | (g_settings.startWithWindows ? MF_CHECKED : 0), ID_STARTUP, reinterpret_cast<LPCWSTR>(&startup));
    AppendMenuW(menu, MF_OWNERDRAW, ID_RESET, reinterpret_cast<LPCWSTR>(&reset));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW, ID_EXIT, reinterpret_cast<LPCWSTR>(&quit));

    Theme theme = CurrentTheme();
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    info.hbrBack = CreateSolidBrush(theme.bg);
    if (background) *background = info.hbrBack;
    SetMenuInfo(menu, &info);
    return menu;
}

void ShowMenu(HWND hwnd, POINT pt) {
    DWORD now = GetTickCount();
    if (now - g_lastMenuTick < 250) return;
    g_lastMenuTick = now;
    g_menuOpen = true;
    HBRUSH brush = nullptr;
    HMENU menu = BuildMenu(&brush);

    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_NOACTIVATE);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
    DestroyMenu(menu);
    if (brush) DeleteObject(brush);
    g_menuOpen = false;
    // Swallow the WM_CONTEXTMENU that follows a right-click after the menu closes.
    g_lastMenuTick = GetTickCount();
    Refresh(true, false);
}

struct LabelState {
    std::wstring prompt;
    std::wstring value;
    bool accepted = false;
    HWND edit = nullptr;
    HFONT font = nullptr;
};

struct ZoneState {
    std::wstring prompt;
    std::wstring currentKey;
    std::wstring selectedKey;
    std::wstring filter;
    bool accepted = false;
    HWND edit = nullptr;
    HWND list = nullptr;
    HFONT font = nullptr;
};

void ApplyChildFont(HWND parent, HFONT font) {
    EnumChildWindows(parent, [](HWND child, LPARAM lp) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
    }, (LPARAM)font);
}

void PlaceDialog(HWND owner, int clientW, int clientH, DWORD style, DWORD ex, int& x, int& y, int& w, int& h) {
    RECT rc{0, 0, clientW, clientH};
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    RECT wr{};
    GetWindowRect(owner, &wr);
    x = (wr.left + wr.right) / 2 - w / 2;
    y = (wr.top + wr.bottom) / 2 - h / 2;
    POINT pt{x + w / 2, y + h / 2};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        RECT wa = mi.rcWork;
        if (w >= wa.right - wa.left) x = wa.left;
        else {
            if (x < wa.left + 8) x = wa.left + 8;
            if (x + w > wa.right - 8) x = wa.right - w - 8;
        }
        if (h >= wa.bottom - wa.top) y = wa.top;
        else {
            if (y < wa.top + 8) y = wa.top + 8;
            if (y + h > wa.bottom - 8) y = wa.bottom - h - 8;
        }
    }
}

void UseDarkTitle(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    auto fn = reinterpret_cast<HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD)>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (fn && CurrentTheme().dark) {
        BOOL dark = TRUE;
        fn(hwnd, 20, &dark, sizeof(dark));
    }
    FreeLibrary(dwm);
}

bool ContainsFold(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto fold = [](wchar_t c) {
        if (c >= L'A' && c <= L'Z') return (wchar_t)(c - L'A' + L'a');
        return c;
    };
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (fold(hay[i + j]) != fold(needle[j])) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

void RefillZones(ZoneState* st) {
    int keep = -1;
    int sel = (int)SendMessageW(st->list, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) keep = (int)SendMessageW(st->list, LB_GETITEMDATA, sel, 0);
    SendMessageW(st->list, LB_RESETCONTENT, 0, 0);
    const auto& zones = Zones();
    int select = -1;
    int shown = 0;
    for (int i = 0; i < (int)zones.size(); ++i) {
        if (!ContainsFold(zones[i].display, st->filter) && !ContainsFold(zones[i].key, st->filter)) continue;
        int idx = (int)SendMessageW(st->list, LB_ADDSTRING, 0, (LPARAM)zones[i].display.c_str());
        SendMessageW(st->list, LB_SETITEMDATA, idx, i);
        if (keep == i || (keep < 0 && zones[i].key == st->currentKey)) select = shown;
        ++shown;
    }
    if (select >= 0) {
        SendMessageW(st->list, LB_SETCURSEL, select, 0);
        SendMessageW(st->list, LB_SETTOPINDEX, select > 3 ? select - 3 : 0, 0);
    } else if (shown > 0) {
        SendMessageW(st->list, LB_SETCURSEL, 0, 0);
    }
}

LRESULT CALLBACK LabelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<LabelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<LabelState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        CreateWindowExW(0, L"STATIC", st->prompt.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
                        Scale(16), Scale(14), rc.right - Scale(32), Scale(24), hwnd, nullptr, g_inst, nullptr);
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->value.c_str(),
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                    Scale(16), Scale(46), rc.right - Scale(32), Scale(28), hwnd, (HMENU)(INT_PTR)IDC_EDIT, g_inst, nullptr);
        SendMessageW(st->edit, EM_SETLIMITTEXT, 24, 0);
        int bw = Scale(88), bh = Scale(30);
        int y = rc.bottom - Scale(16) - bh;
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        rc.right - Scale(16) - bw, y, bw, bh, hwnd, (HMENU)IDCANCEL, g_inst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        rc.right - Scale(24) - bw * 2, y, bw, bh, hwnd, (HMENU)IDOK, g_inst, nullptr);
        st->font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               g_renderer.UiFace().c_str());
        ApplyChildFont(hwnd, st->font);
        return 0;
    }
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[64];
            GetWindowTextW(st->edit, buf, 64);
            std::wstring v = buf;
            Trim(v);
            if (v.empty()) {
                MessageBeep(MB_ICONWARNING);
                return 0;
            }
            if (v.size() > 24) v.resize(24);
            st->value = std::move(v);
            st->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (st && st->font) {
            DeleteObject(st->font);
            st->font = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ZoneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ZoneState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<ZoneState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        CreateWindowExW(0, L"STATIC", st->prompt.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
                        Scale(16), Scale(12), rc.right - Scale(32), Scale(24), hwnd, nullptr, g_inst, nullptr);
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                   Scale(16), Scale(40), rc.right - Scale(32), Scale(28), hwnd, (HMENU)(INT_PTR)IDC_EDIT, g_inst, nullptr);
        SendMessageW(st->edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"搜索时区");
        int listTop = Scale(78);
        int listH = rc.bottom - listTop - Scale(58);
        st->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                                   Scale(16), listTop, rc.right - Scale(32), listH, hwnd, (HMENU)(INT_PTR)IDC_LIST, g_inst, nullptr);
        SendMessageW(st->list, LB_SETHORIZONTALEXTENT, Scale(720), 0);
        int bw = Scale(88), bh = Scale(30);
        int y = rc.bottom - Scale(16) - bh;
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        rc.right - Scale(16) - bw, y, bw, bh, hwnd, (HMENU)IDCANCEL, g_inst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        rc.right - Scale(24) - bw * 2, y, bw, bh, hwnd, (HMENU)IDOK, g_inst, nullptr);
        st->font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               g_renderer.UiFace().c_str());
        ApplyChildFont(hwnd, st->font);
        RefillZones(st);
        return 0;
    }
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == IDC_EDIT && HIWORD(wp) == EN_CHANGE) {
            wchar_t buf[128];
            GetWindowTextW(st->edit, buf, 128);
            st->filter = buf;
            RefillZones(st);
            return 0;
        }
        if (LOWORD(wp) == IDC_LIST && HIWORD(wp) == LBN_DBLCLK) {
            SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
            return 0;
        }
        if (LOWORD(wp) == IDOK) {
            int sel = (int)SendMessageW(st->list, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) {
                MessageBeep(MB_ICONWARNING);
                return 0;
            }
            int index = (int)SendMessageW(st->list, LB_GETITEMDATA, sel, 0);
            if (index < 0 || index >= (int)Zones().size()) return 0;
            st->selectedKey = Zones()[index].key;
            st->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (st && st->font) {
            DeleteObject(st->font);
            st->font = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PumpModal(HWND dlg) {
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg)) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (r < 0) break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            wchar_t cls[32];
            GetClassNameW(msg.hwnd, cls, 32);
            SendMessageW(dlg, WM_COMMAND, IDOK, 0);
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hwnd, TRUE);
}

bool EditLabel(const std::wstring& current, std::wstring& out) {
    LabelState st;
    st.prompt = L"为「" + current + L"」设置名称";
    st.value = current;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;
    int x, y, w, h;
    PlaceDialog(g_hwnd, Scale(380), Scale(148), style, ex, x, y, w, h);
    HWND dlg = CreateWindowExW(ex, kLabelClass, L"修改名称", style, x, y, w, h, nullptr, nullptr, g_inst, &st);
    if (!dlg) return false;
    UseDarkTitle(dlg);
    g_dialogOpen = true;
    PumpModal(dlg);
    g_dialogOpen = false;
    if (!st.accepted) return false;
    out = st.value;
    return true;
}

bool PickZone(const std::wstring& label, const std::wstring& currentKey, std::wstring& outKey) {
    if (Zones().empty()) LoadZones();
    ZoneState st;
    st.prompt = L"为「" + label + L"」选择时区";
    st.currentKey = currentKey;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;
    int x, y, w, h;
    PlaceDialog(g_hwnd, Scale(640), Scale(520), style, ex, x, y, w, h);
    HWND dlg = CreateWindowExW(ex, kZoneClass, L"选择时区", style, x, y, w, h, nullptr, nullptr, g_inst, &st);
    if (!dlg) return false;
    UseDarkTitle(dlg);
    g_dialogOpen = true;
    PumpModal(dlg);
    g_dialogOpen = false;
    if (!st.accepted) return false;
    outKey = st.selectedKey;
    return true;
}

void ChangeLabel(int index) {
    std::wstring out;
    if (!EditLabel(g_settings.clock[index].label, out)) {
        ApplyZOrder();
        return;
    }
    g_settings.clock[index].label = out;
    SaveSettings(g_settings);
    Refresh(true, true);
    ApplyZOrder();
}

void ChangeZone(int index) {
    std::wstring key;
    if (!PickZone(g_settings.clock[index].label, g_settings.clock[index].timeZone, key)) {
        ApplyZOrder();
        return;
    }
    g_settings.clock[index].timeZone = key;
    SaveSettings(g_settings);
    Refresh(true, true);
    ApplyZOrder();
}

struct LookState {
    HWND scaleBar = nullptr;
    HWND opacityBar = nullptr;
    HWND scaleValue = nullptr;
    HWND opacityValue = nullptr;
    HFONT font = nullptr;
    bool dirty = false;
};

void SetPercentText(HWND label, int value) {
    wchar_t buf[16];
    Fmt(buf, L"%d%%", value);
    SetWindowTextW(label, buf);
}

void ApplyLookValue(LookState* st, HWND bar) {
    int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
    if (bar == st->scaleBar) {
        if (pos < kScaleMin) pos = kScaleMin;
        if (pos > kScaleMax) pos = kScaleMax;
        if (pos == g_settings.scale) return;
        g_settings.scale = pos;
        SetPercentText(st->scaleValue, pos);
        Refresh(true, true);
        st->dirty = true;
    } else if (bar == st->opacityBar) {
        if (pos < kOpacityMin) pos = kOpacityMin;
        if (pos > kOpacityMax) pos = kOpacityMax;
        if (pos == g_settings.opacity) return;
        g_settings.opacity = pos;
        SetPercentText(st->opacityValue, pos);
        Refresh(true, false);
        st->dirty = true;
    }
}

LRESULT CALLBACK LookProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<LookState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<LookState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int pad = Scale(18);
        int labelH = Scale(22);
        int barH = Scale(34);
        int valueW = Scale(72);
        int y = pad;
        CreateWindowExW(0, L"STATIC", L"大小", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
                        pad, y, Scale(160), labelH, hwnd, nullptr, g_inst, nullptr);
        st->scaleValue = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_NOPREFIX | SS_CENTERIMAGE,
                                          rc.right - pad - valueW, y, valueW, labelH, hwnd, nullptr, g_inst, nullptr);
        y += labelH + Scale(2);
        st->scaleBar = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                        pad, y, rc.right - pad * 2, barH, hwnd, nullptr, g_inst, nullptr);
        SendMessageW(st->scaleBar, TBM_SETRANGEMIN, FALSE, kScaleMin);
        SendMessageW(st->scaleBar, TBM_SETRANGEMAX, FALSE, kScaleMax);
        SendMessageW(st->scaleBar, TBM_SETLINESIZE, 0, 1);
        SendMessageW(st->scaleBar, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(st->scaleBar, TBM_SETPOS, TRUE, g_settings.scale);
        SetPercentText(st->scaleValue, g_settings.scale);

        y += barH + Scale(16);
        CreateWindowExW(0, L"STATIC", L"透明度", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE,
                        pad, y, Scale(160), labelH, hwnd, nullptr, g_inst, nullptr);
        st->opacityValue = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_NOPREFIX | SS_CENTERIMAGE,
                                            rc.right - pad - valueW, y, valueW, labelH, hwnd, nullptr, g_inst, nullptr);
        y += labelH + Scale(2);
        st->opacityBar = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                          pad, y, rc.right - pad * 2, barH, hwnd, nullptr, g_inst, nullptr);
        SendMessageW(st->opacityBar, TBM_SETRANGEMIN, FALSE, kOpacityMin);
        SendMessageW(st->opacityBar, TBM_SETRANGEMAX, FALSE, kOpacityMax);
        SendMessageW(st->opacityBar, TBM_SETLINESIZE, 0, 1);
        SendMessageW(st->opacityBar, TBM_SETPAGESIZE, 0, 5);
        SendMessageW(st->opacityBar, TBM_SETPOS, TRUE, g_settings.opacity);
        SetPercentText(st->opacityValue, g_settings.opacity);

        int btnW = Scale(88);
        int btnH = Scale(30);
        CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        rc.right - pad - btnW, rc.bottom - pad - btnH, btnW, btnH, hwnd, (HMENU)IDOK, g_inst, nullptr);
        st->font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               g_renderer.UiFace().c_str());
        ApplyChildFont(hwnd, st->font);
        return 0;
    }
    case WM_HSCROLL:
        if (st && lp) {
            ApplyLookValue(st, (HWND)lp);
            if (LOWORD(wp) == TB_ENDTRACK && st->dirty) SaveSettings(g_settings);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (st && st->dirty) SaveSettings(g_settings);
        if (st && st->font) {
            DeleteObject(st->font);
            st->font = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EditLook() {
    LookState st;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;
    int x, y, w, h;
    PlaceDialog(g_hwnd, Scale(440), Scale(208), style, ex, x, y, w, h);
    HWND dlg = CreateWindowExW(ex, kLookClass, L"大小与透明度", style, x, y, w, h, nullptr, nullptr, g_inst, &st);
    if (!dlg) return;
    UseDarkTitle(dlg);
    g_dialogOpen = true;
    PumpModal(dlg);
    g_dialogOpen = false;
    ApplyZOrder();
}

void ExitApp() {
    g_exiting = true;
    SaveSettings(g_settings);
    if (g_hwnd) DestroyWindow(g_hwnd);
}

struct WeatherMsg {
    int index = 0;
    unsigned generation = 0;
    bool ok = false;
    double tempC = 0;
    int code = -1;
};

struct WeatherJob {
    HWND hwnd = nullptr;
    unsigned generation = 0;
    double lat[2] = {};
    double lon[2] = {};
};

struct GeoMsg {
    unsigned generation = 0;
    bool ok = false;
    std::vector<GeoPlace>* places = nullptr;
};

struct GeoJob {
    HWND dlg = nullptr;
    unsigned generation = 0;
    std::wstring query;
};

DWORD WINAPI WeatherThread(LPVOID param) {
    std::unique_ptr<WeatherJob> job(static_cast<WeatherJob*>(param));
    for (int i = 0; i < 2; ++i) {
        auto* msg = new WeatherMsg();
        msg->index = i;
        msg->generation = job->generation;
        msg->ok = FetchForecast(job->lat[i], job->lon[i], msg->tempC, msg->code);
        if (!PostMessageW(job->hwnd, WM_WEATHER, 0, (LPARAM)msg)) delete msg;
    }
    return 0;
}

void RequestWeather() {
    if (!g_hwnd || !g_settings.showWeather || g_weatherBusy) return;
    g_weatherBusy = true;
    g_weatherLeft = 2;
    g_weatherGotOne = false;
    g_weatherStamp = GetTickCount64();
    auto* job = new WeatherJob();
    job->hwnd = g_hwnd;
    job->generation = ++g_weatherGen;
    for (int i = 0; i < 2; ++i) {
        job->lat[i] = g_settings.clock[i].latitude;
        job->lon[i] = g_settings.clock[i].longitude;
    }
    HANDLE thread = CreateThread(nullptr, 0, WeatherThread, job, 0, nullptr);
    if (!thread) {
        delete job;
        g_weatherBusy = false;
        g_weatherLeft = 0;
        return;
    }
    CloseHandle(thread);
}

void OnWeather(WeatherMsg* msg) {
    if (!msg) return;
    if (msg->generation != g_weatherGen) {
        delete msg;
        return;
    }
    bool changed = false;
    if (msg->ok && msg->index >= 0 && msg->index < 2) {
        auto& clock = g_settings.clock[msg->index];
        clock.hasWeather = true;
        clock.lastTempC = msg->tempC;
        clock.lastCode = msg->code;
        changed = true;
        g_weatherGotOne = true;
    }
    if (--g_weatherLeft <= 0) {
        g_weatherBusy = false;
        if (!g_weatherGotOne) g_weatherStamp = GetTickCount64() - 18ull * 60ull * 1000ull;
        if (g_weatherGotOne) SaveSettings(g_settings);
    }
    delete msg;
    if (changed) Refresh(true, false);
}

void NotifyDone(const wchar_t* title, const wchar_t* body) {
    PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
    if (!g_hwnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 10000;
    CopyW(nid.szInfoTitle, sizeof(nid.szInfoTitle) / sizeof(wchar_t), title);
    CopyW(nid.szInfo, sizeof(nid.szInfo) / sizeof(wchar_t), body);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AdvanceTimer() {
    TimerKind kind = Timer().kind;
    if (kind == TimerKind::Focus) {
        Timer().focusDone++;
        bool longBreak = g_settings.roundsBeforeLong > 0 && Timer().focusDone % g_settings.roundsBeforeLong == 0;
        NotifyDone(L"专注结束", longBreak ? L"该进行长休息了。" : L"休息一下。");
        TimerStart(longBreak ? TimerKind::LongBreak : TimerKind::ShortBreak, (unsigned long long)(longBreak ? g_settings.longBreakMin : g_settings.shortBreakMin) * 60ull * 1000ull);
    } else if (kind == TimerKind::ShortBreak || kind == TimerKind::LongBreak) {
        NotifyDone(L"休息结束", L"开始下一轮专注。");
        TimerStart(TimerKind::Focus, (unsigned long long)g_settings.focusMin * 60ull * 1000ull);
    } else {
        NotifyDone(L"倒计时结束", L"时间到。");
        TimerStop();
    }
    Refresh(true, false);
}

struct NumState {
    std::wstring prompt;
    std::wstring value;
    int lo = 1;
    int hi = 180;
    int result = 0;
    bool accepted = false;
    HWND edit = nullptr;
    HFONT font = nullptr;
};

LRESULT CALLBACK NumProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<NumState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<NumState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        CreateWindowExW(0, L"STATIC", st->prompt.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                        Scale(16), Scale(16), rc.right - Scale(32), Scale(40), hwnd, nullptr, g_inst, nullptr);
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->value.c_str(), WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                                   Scale(16), Scale(62), rc.right - Scale(32), Scale(28), hwnd, (HMENU)(INT_PTR)IDC_EDIT, g_inst, nullptr);
        SendMessageW(st->edit, EM_SETLIMITTEXT, 4, 0);
        int bw = Scale(88), bh = Scale(30), y = rc.bottom - Scale(16) - bh;
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.right - Scale(16) - bw, y, bw, bh, hwnd, (HMENU)IDCANCEL, g_inst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, rc.right - Scale(24) - bw * 2, y, bw, bh, hwnd, (HMENU)IDOK, g_inst, nullptr);
        st->font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_renderer.UiFace().c_str());
        ApplyChildFont(hwnd, st->font);
        return 0;
    }
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[16];
            GetWindowTextW(st->edit, buf, 16);
            int n = _wtoi(buf);
            if (n < st->lo || n > st->hi) {
                MessageBeep(MB_ICONWARNING);
                return 0;
            }
            st->result = n;
            st->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (st && st->font) DeleteObject(st->font);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool AskInt(const wchar_t* title, const wchar_t* prompt, int current, int lo, int hi, int& out) {
    NumState st;
    st.prompt = prompt;
    wchar_t buf[16];
    Fmt(buf, L"%d", current);
    st.value = buf;
    st.lo = lo;
    st.hi = hi;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;
    int x, y, w, h;
    PlaceDialog(g_hwnd, Scale(360), Scale(160), style, ex, x, y, w, h);
    HWND dlg = CreateWindowExW(ex, kNumClass, title, style, x, y, w, h, nullptr, nullptr, g_inst, &st);
    if (!dlg) return false;
    UseDarkTitle(dlg);
    g_dialogOpen = true;
    PumpModal(dlg);
    g_dialogOpen = false;
    ApplyZOrder();
    if (!st.accepted) return false;
    out = st.result;
    return true;
}

struct DurState {
    HWND e[4] = {};
    HFONT font = nullptr;
    bool accepted = false;
    int value[4] = {};
};

LRESULT CALLBACK DurProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<DurState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<DurState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        const wchar_t* labels[] = {L"专注（分钟）", L"短休息（分钟）", L"长休息（分钟）", L"几轮后长休息"};
        int vals[] = {g_settings.focusMin, g_settings.shortBreakMin, g_settings.longBreakMin, g_settings.roundsBeforeLong};
        RECT rc{};
        GetClientRect(hwnd, &rc);
        for (int i = 0; i < 4; ++i) {
            int y = Scale(14) + i * Scale(36);
            CreateWindowExW(0, L"STATIC", labels[i], WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, Scale(16), y, Scale(150), Scale(24), hwnd, nullptr, g_inst, nullptr);
            wchar_t buf[16];
            Fmt(buf, L"%d", vals[i]);
            st->e[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buf, WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP, rc.right - Scale(100), y - Scale(2), Scale(80), Scale(26), hwnd, (HMENU)(INT_PTR)(IDC_H1 + i), g_inst, nullptr);
            SendMessageW(st->e[i], EM_SETLIMITTEXT, 3, 0);
        }
        int bw = Scale(88), bh = Scale(30), y = rc.bottom - Scale(16) - bh;
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.right - Scale(16) - bw, y, bw, bh, hwnd, (HMENU)IDCANCEL, g_inst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, rc.right - Scale(24) - bw * 2, y, bw, bh, hwnd, (HMENU)IDOK, g_inst, nullptr);
        st->font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_renderer.UiFace().c_str());
        ApplyChildFont(hwnd, st->font);
        return 0;
    }
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == IDOK) {
            int lim[4][2] = {{1, 180}, {1, 90}, {1, 90}, {1, 12}};
            for (int i = 0; i < 4; ++i) {
                wchar_t buf[16];
                GetWindowTextW(st->e[i], buf, 16);
                int n = _wtoi(buf);
                if (n < lim[i][0] || n > lim[i][1]) {
                    MessageBeep(MB_ICONWARNING);
                    return 0;
                }
                st->value[i] = n;
            }
            st->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (st && st->font) DeleteObject(st->font);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EditDurations() {
    DurState st;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;
    int x, y, w, h;
    PlaceDialog(g_hwnd, Scale(340), Scale(230), style, ex, x, y, w, h);
    HWND dlg = CreateWindowExW(ex, kDurClass, L"番茄钟时长", style, x, y, w, h, nullptr, nullptr, g_inst, &st);
    if (!dlg) return;
    UseDarkTitle(dlg);
    g_dialogOpen = true;
    PumpModal(dlg);
    g_dialogOpen = false;
    ApplyZOrder();
    if (!st.accepted) return;
    g_settings.focusMin = st.value[0];
    g_settings.shortBreakMin = st.value[1];
    g_settings.longBreakMin = st.value[2];
    g_settings.roundsBeforeLong = st.value[3];
    SaveSettings(g_settings);
}

struct UntilState {
    HWND hour = nullptr;
    HWND minute = nullptr;
    HFONT font = nullptr;
    bool accepted = false;
    int h = 0;
    int m = 0;
};

LRESULT CALLBACK UntilProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<UntilState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<UntilState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        SYSTEMTIME now{};
        GetLocalTime(&now);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        CreateWindowExW(0, L"STATIC", L"结束时间（24 小时制）。如果已过，则算到明天。", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, Scale(16), Scale(14), rc.right - Scale(32), Scale(36), hwnd, nullptr, g_inst, nullptr);
        wchar_t hb[8], mb[8];
        Fmt(hb, L"%02u", now.wHour);
        Fmt(mb, L"%02u", now.wMinute);
        st->hour = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", hb, WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP, Scale(16), Scale(58), Scale(70), Scale(28), hwnd, (HMENU)(INT_PTR)IDC_H1, g_inst, nullptr);
        CreateWindowExW(0, L"STATIC", L":", WS_CHILD | WS_VISIBLE | SS_CENTER, Scale(90), Scale(60), Scale(16), Scale(24), hwnd, nullptr, g_inst, nullptr);
        st->minute = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", mb, WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP, Scale(110), Scale(58), Scale(70), Scale(28), hwnd, (HMENU)(INT_PTR)IDC_H2, g_inst, nullptr);
        SendMessageW(st->hour, EM_SETLIMITTEXT, 2, 0);
        SendMessageW(st->minute, EM_SETLIMITTEXT, 2, 0);
        int bw = Scale(88), bh = Scale(30), y = rc.bottom - Scale(16) - bh;
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.right - Scale(16) - bw, y, bw, bh, hwnd, (HMENU)IDCANCEL, g_inst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, rc.right - Scale(24) - bw * 2, y, bw, bh, hwnd, (HMENU)IDOK, g_inst, nullptr);
        st->font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_renderer.UiFace().c_str());
        ApplyChildFont(hwnd, st->font);
        return 0;
    }
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == IDOK) {
            wchar_t hb[8], mb[8];
            GetWindowTextW(st->hour, hb, 8);
            GetWindowTextW(st->minute, mb, 8);
            int h = _wtoi(hb);
            int m = _wtoi(mb);
            if (h < 0 || h > 23 || m < 0 || m > 59) {
                MessageBeep(MB_ICONWARNING);
                return 0;
            }
            st->h = h;
            st->m = m;
            st->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (st && st->font) DeleteObject(st->font);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void StartUntil() {
    UntilState st;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;
    int x, y, w, h;
    PlaceDialog(g_hwnd, Scale(360), Scale(160), style, ex, x, y, w, h);
    HWND dlg = CreateWindowExW(ex, kUntilClass, L"倒计时到指定时间", style, x, y, w, h, nullptr, nullptr, g_inst, &st);
    if (!dlg) return;
    UseDarkTitle(dlg);
    g_dialogOpen = true;
    PumpModal(dlg);
    g_dialogOpen = false;
    ApplyZOrder();
    if (!st.accepted) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    int nowMin = now.wHour * 60 + now.wMinute;
    int target = st.h * 60 + st.m;
    int diff = target - nowMin;
    if (diff <= 0) diff += 24 * 60;
    unsigned long long ms = (unsigned long long)diff * 60ull * 1000ull;
    if (now.wSecond > 0 && ms > now.wSecond * 1000ull) ms -= now.wSecond * 1000ull;
    TimerStart(TimerKind::Countdown, ms);
    Refresh(true, false);
}

DWORD WINAPI GeoThread(LPVOID param) {
    std::unique_ptr<GeoJob> job(static_cast<GeoJob*>(param));
    auto* msg = new GeoMsg();
    msg->generation = job->generation;
    msg->places = new std::vector<GeoPlace>();
    msg->ok = FetchCities(job->query, *msg->places);
    if (!PostMessageW(job->dlg, WM_GEOCODE, 0, (LPARAM)msg)) {
        delete msg->places;
        delete msg;
    }
    return 0;
}

struct CityState {
    HWND edit = nullptr;
    HWND list = nullptr;
    HWND search = nullptr;
    HFONT font = nullptr;
    std::vector<GeoPlace> places;
    unsigned generation = 0;
    bool accepted = false;
    double lat = 0;
    double lon = 0;
};

void FillCityList(CityState* st, const wchar_t* placeholder) {
    SendMessageW(st->list, LB_RESETCONTENT, 0, 0);
    if (placeholder) {
        SendMessageW(st->list, LB_ADDSTRING, 0, (LPARAM)placeholder);
        return;
    }
    for (int i = 0; i < (int)st->places.size(); ++i) {
        std::wstring line = st->places[i].name;
        if (!st->places[i].detail.empty()) line += L" · " + st->places[i].detail;
        int idx = (int)SendMessageW(st->list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
        SendMessageW(st->list, LB_SETITEMDATA, idx, i);
    }
    if (st->places.empty()) SendMessageW(st->list, LB_ADDSTRING, 0, (LPARAM)L"没有结果");
    else SendMessageW(st->list, LB_SETCURSEL, 0, 0);
}

LRESULT CALLBACK CityProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<CityState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<CityState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        CreateWindowExW(0, L"STATIC", L"搜索城市，结果来自 Open-Meteo。", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, Scale(16), Scale(12), rc.right - Scale(32), Scale(22), hwnd, nullptr, g_inst, nullptr);
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, Scale(16), Scale(40), rc.right - Scale(120), Scale(28), hwnd, (HMENU)(INT_PTR)IDC_EDIT, g_inst, nullptr);
        st->search = CreateWindowExW(0, L"BUTTON", L"搜索", WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.right - Scale(96), Scale(40), Scale(80), Scale(28), hwnd, (HMENU)(INT_PTR)IDC_SEARCH, g_inst, nullptr);
        st->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, Scale(16), Scale(80), rc.right - Scale(32), rc.bottom - Scale(136), hwnd, (HMENU)(INT_PTR)IDC_LIST, g_inst, nullptr);
        int bw = Scale(88), bh = Scale(30), y = rc.bottom - Scale(16) - bh;
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.right - Scale(16) - bw, y, bw, bh, hwnd, (HMENU)IDCANCEL, g_inst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, rc.right - Scale(24) - bw * 2, y, bw, bh, hwnd, (HMENU)IDOK, g_inst, nullptr);
        st->font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_renderer.UiFace().c_str());
        ApplyChildFont(hwnd, st->font);
        return 0;
    }
    case WM_GEOCODE: {
        auto* result = reinterpret_cast<GeoMsg*>(lp);
        if (st && result && result->generation == st->generation) {
            EnableWindow(st->search, TRUE);
            st->places = result->ok && result->places ? *result->places : std::vector<GeoPlace>{};
            FillCityList(st, result->ok ? nullptr : L"搜索失败");
        }
        if (result) {
            delete result->places;
            delete result;
        }
        return 0;
    }
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == IDC_SEARCH) {
            wchar_t buf[64];
            GetWindowTextW(st->edit, buf, 64);
            std::wstring q = buf;
            Trim(q);
            if (q.empty()) {
                MessageBeep(MB_ICONWARNING);
                return 0;
            }
            auto* job = new GeoJob();
            job->dlg = hwnd;
            job->generation = ++st->generation;
            job->query = q;
            EnableWindow(st->search, FALSE);
            st->places.clear();
            FillCityList(st, L"正在搜索…");
            HANDLE thread = CreateThread(nullptr, 0, GeoThread, job, 0, nullptr);
            if (!thread) {
                delete job;
                EnableWindow(st->search, TRUE);
                FillCityList(st, L"搜索失败");
            } else CloseHandle(thread);
            return 0;
        }
        if (LOWORD(wp) == IDC_LIST && HIWORD(wp) == LBN_DBLCLK) {
            SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
            return 0;
        }
        if (LOWORD(wp) == IDOK) {
            int sel = (int)SendMessageW(st->list, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR || st->places.empty()) {
                MessageBeep(MB_ICONWARNING);
                return 0;
            }
            int index = (int)SendMessageW(st->list, LB_GETITEMDATA, sel, 0);
            if (index < 0 || index >= (int)st->places.size()) return 0;
            st->lat = st->places[index].latitude;
            st->lon = st->places[index].longitude;
            st->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (st) st->generation++;
        if (st && st->font) DeleteObject(st->font);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EditCity(int index) {
    CityState st;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;
    int x, y, w, h;
    PlaceDialog(g_hwnd, Scale(460), Scale(420), style, ex, x, y, w, h);
    std::wstring title = L"选择城市 · " + g_settings.clock[index].label;
    HWND dlg = CreateWindowExW(ex, kCityClass, title.c_str(), style, x, y, w, h, nullptr, nullptr, g_inst, &st);
    if (!dlg) return;
    UseDarkTitle(dlg);
    g_dialogOpen = true;
    PumpModal(dlg);
    g_dialogOpen = false;
    ApplyZOrder();
    if (!st.accepted) return;
    g_settings.clock[index].latitude = st.lat;
    g_settings.clock[index].longitude = st.lon;
    g_settings.clock[index].hasWeather = false;
    SaveSettings(g_settings);
    Refresh(true, false);
    g_weatherBusy = false;
    RequestWeather();
}

void OnCommand(int id) {
    if (id == ID_C1_LABEL) ChangeLabel(0);
    else if (id == ID_C1_ZONE) ChangeZone(0);
    else if (id == ID_C1_CITY) EditCity(0);
    else if (id == ID_C2_LABEL) ChangeLabel(1);
    else if (id == ID_C2_ZONE) ChangeZone(1);
    else if (id == ID_C2_CITY) EditCity(1);
    else if (id == ID_TOPMOST) {
        g_settings.alwaysOnTop = true;
        ApplyZOrder();
        SaveSettings(g_settings);
    } else if (id == ID_DESKTOP) {
        g_settings.alwaysOnTop = false;
        ApplyZOrder();
        SaveSettings(g_settings);
    } else if (id == ID_LOOK) {
        EditLook();
    } else if (id == ID_SECONDS) {
        g_settings.showSeconds = !g_settings.showSeconds;
        SaveSettings(g_settings);
        Refresh(true, true);
    } else if (id == ID_WEATHER) {
        g_settings.showWeather = !g_settings.showWeather;
        SaveSettings(g_settings);
        Refresh(true, false);
        if (g_settings.showWeather) RequestWeather();
    } else if (id == ID_UNIT_C || id == ID_UNIT_F) {
        g_settings.fahrenheit = id == ID_UNIT_F;
        SaveSettings(g_settings);
        Refresh(true, false);
    } else if (id == ID_POMO_FOCUS) {
        TimerStart(TimerKind::Focus, (unsigned long long)g_settings.focusMin * 60ull * 1000ull);
        Refresh(true, false);
    } else if (id == ID_POMO_MINUTES) {
        int minutes = 10;
        if (AskInt(L"自定义倒计时", L"倒计时分钟数（1–1440）", minutes, 1, 1440, minutes)) {
            TimerStart(TimerKind::Countdown, (unsigned long long)minutes * 60ull * 1000ull);
            Refresh(true, false);
        }
    } else if (id == ID_POMO_UNTIL) {
        StartUntil();
    } else if (id == ID_POMO_PAUSE) {
        if (TimerActive()) {
            if (Timer().paused) TimerResume();
            else TimerPause();
            Refresh(true, false);
        }
    } else if (id == ID_POMO_STOP) {
        TimerStop();
        Refresh(true, false);
    } else if (id == ID_POMO_LENGTHS) {
        EditDurations();
    } else if (id == ID_STARTUP) {
        bool next = !g_settings.startWithWindows;
        if (!SetRunAtStartup(next)) {
            MessageBoxW(g_hwnd, L"无法写入开机启动项。", L"双时区时钟", MB_ICONWARNING | MB_OK);
            return;
        }
        g_settings.startWithWindows = next;
        SaveSettings(g_settings);
    } else if (id == ID_RESET) {
        g_settings.hasPosition = false;
        Refresh(true, false);
        SaveSettings(g_settings);
        ApplyZOrder();
    } else if (id == ID_EXIT) {
        ExitApp();
    }
}

LRESULT CALLBACK WidgetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) {
        AddTray();
        ApplyZOrder();
        return 0;
    }
    switch (msg) {
    case WM_MEASUREITEM:
        OnMeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp));
        return TRUE;
    case WM_DRAWITEM:
        OnDrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;
    case WM_LBUTTONDOWN: {
        POINT local{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_pressClient = local;
        g_pressInStrip = g_hits.hasStrip && PtInRect(&g_hits.strip, local);
        g_dragging = true;
        SetCapture(hwnd);
        GetCursorPos(&g_dragPt);
        g_dragX = g_settings.x;
        g_dragY = g_settings.y;
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            g_settings.x = g_dragX + pt.x - g_dragPt.x;
            g_settings.y = g_dragY + pt.y - g_dragPt.y;
            SetWindowPos(hwnd, nullptr, g_settings.x, g_settings.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (!g_hovered) {
            g_hovered = true;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            Refresh(true, false);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            POINT local{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int dx = local.x - g_pressClient.x;
            int dy = local.y - g_pressClient.y;
            if (g_pressInStrip && dx * dx + dy * dy < 25 && TimerActive()) {
                g_settings.x = g_dragX;
                g_settings.y = g_dragY;
                SetWindowPos(hwnd, nullptr, g_settings.x, g_settings.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (Timer().paused) TimerResume();
                else TimerPause();
                Refresh(true, false);
            } else {
                ClampVisible(g_settings.x, g_settings.y, g_w, g_h);
                SetWindowPos(hwnd, nullptr, g_settings.x, g_settings.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                SaveSettings(g_settings);
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_dragging = false;
        return 0;
    case WM_MOUSELEAVE:
        g_hovered = false;
        Refresh(true, false);
        return 0;
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &pt);
        ShowMenu(hwnd, pt);
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT pt;
        if ((int)lp == -1) GetCursorPos(&pt);
        else {
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
        }
        ShowMenu(hwnd, pt);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGING:
        if (!g_exiting) {
            auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
            if (pos->flags & SWP_HIDEWINDOW) pos->flags &= ~SWP_HIDEWINDOW;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) return 0;
        if ((wp & 0xFFF0) == SC_CLOSE) {
            ExitApp();
            return 0;
        }
        break;
    case WM_DPICHANGED:
        g_dpi = HIWORD(wp);
        DropUiFont();
        Refresh(true, true);
        return 0;
    case WM_DISPLAYCHANGE:
        Refresh(true, false);
        SaveSettings(g_settings);
        return 0;
    case WM_WEATHER:
        OnWeather(reinterpret_cast<WeatherMsg*>(lp));
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            static std::wstring lastTimer;
            if (TimerTick(GetTickCount64())) AdvanceTimer();
            std::wstring cap = TimerCaption();
            if (cap != lastTimer) {
                lastTimer = cap;
                Refresh(true, false);
            } else {
                Refresh(false, false);
            }
            if (g_settings.showWeather && !g_weatherBusy) {
                unsigned long long now = GetTickCount64();
                if (g_weatherStamp == 0 || now - g_weatherStamp > 20ull * 60ull * 1000ull) RequestWeather();
            }
            if (!g_settings.alwaysOnTop && !g_dragging && !g_menuOpen && !g_dialogOpen) {
                if (++g_zTick >= 30) {
                    g_zTick = 0;
                    ApplyZOrder();
                }
            }
        }
        return 0;
    case WM_NUDGE:
        Nudge();
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
            Nudge();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT pt;
            GetCursorPos(&pt);
            ShowMenu(hwnd, pt);
            break;
        }
        default:
            break;
        }
        return 0;
    case WM_ENDSESSION:
        SaveSettings(g_settings);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        g_exiting = true;
        KillTimer(hwnd, 1);
        if (g_hook) {
            UnhookWinEvent(g_hook);
            g_hook = nullptr;
        }
        RemoveTray();
        if (g_icon) {
            DestroyIcon(g_icon);
            g_icon = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterClasses() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WidgetProc;
    wc.hInstance = g_inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!RegisterClassExW(&wc)) return false;

    WNDCLASSEXW dlg{};
    dlg.cbSize = sizeof(dlg);
    dlg.hInstance = g_inst;
    dlg.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dlg.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    dlg.lpfnWndProc = LabelProc;
    dlg.lpszClassName = kLabelClass;
    if (!RegisterClassExW(&dlg)) return false;
    dlg.lpfnWndProc = ZoneProc;
    dlg.lpszClassName = kZoneClass;
    if (!RegisterClassExW(&dlg)) return false;
    dlg.lpfnWndProc = LookProc;
    dlg.lpszClassName = kLookClass;
    if (!RegisterClassExW(&dlg)) return false;
    dlg.lpfnWndProc = NumProc;
    dlg.lpszClassName = kNumClass;
    if (!RegisterClassExW(&dlg)) return false;
    dlg.lpfnWndProc = DurProc;
    dlg.lpszClassName = kDurClass;
    if (!RegisterClassExW(&dlg)) return false;
    dlg.lpfnWndProc = UntilProc;
    dlg.lpszClassName = kUntilClass;
    if (!RegisterClassExW(&dlg)) return false;
    dlg.lpfnWndProc = CityProc;
    dlg.lpszClassName = kCityClass;
    return RegisterClassExW(&dlg) != 0;
}

bool WriteUtf8File(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == data.size();
}

int RunSelfTest(const wchar_t* outPath) {
    std::string report;
    int fails = 0;
    auto line = [&](const std::string& s) { report += s; report += "\n"; };
    auto expect = [&](bool ok, const char* name) {
        line(std::string(name) + (ok ? "=ok" : "=fail"));
        if (!ok) ++fails;
    };

    line("label_utf8=" + WideToUtf8(L"都柏林"));
    expect(WideToUtf8(L"都柏林") == "\xE9\x83\xBD\xE6\x9F\x8F\xE6\x9E\x97", "label_bytes");

    Settings s = DefaultSettings();
    s.hasPosition = true;
    s.x = -40;
    s.y = 80;
    s.opacity = 37;
    s.scale = 180;
    s.alwaysOnTop = true;
    s.showSeconds = false;
    s.startWithWindows = true;
    s.clock[0].label = L"都\"柏\\林";
    s.clock[1].label = L"北京";
    std::string json = SettingsToJson(s);
    Settings back;
    bool parsed = SettingsFromJson(json, back);
    expect(parsed, "json_parse");
    expect(parsed && back.clock[0].label == s.clock[0].label, "json_label_escape");
    expect(parsed && back.clock[1].timeZone == L"China Standard Time", "json_zone");
    expect(parsed && back.x == -40 && back.y == 80 && back.hasPosition, "json_pos");
    s.showWeather = false;
    s.fahrenheit = true;
    s.focusMin = 30;
    s.shortBreakMin = 4;
    s.longBreakMin = 20;
    s.roundsBeforeLong = 3;
    s.clock[0].latitude = 53.35;
    s.clock[0].longitude = -6.26;
    s.clock[0].hasWeather = true;
    s.clock[0].lastTempC = 10.3;
    s.clock[0].lastCode = 3;
    json = SettingsToJson(s);
    parsed = SettingsFromJson(json, back);
    expect(parsed && back.opacity == 37 && back.scale == 180 && back.alwaysOnTop && !back.showSeconds && back.startWithWindows, "json_flags");
    expect(parsed && !back.showWeather && back.fahrenheit && back.focusMin == 30 && back.shortBreakMin == 4 && back.longBreakMin == 20 && back.roundsBeforeLong == 3, "json_timer_weather");
    expect(parsed && back.clock[0].hasWeather && back.clock[0].lastCode == 3 && back.clock[0].latitude > 53.0 && back.clock[0].latitude < 53.5, "json_place");
    double sampleTemp = 0;
    int sampleCode = 0;
    expect(ParseForecastBody("{\"current_units\":{\"temperature_2m\":\"C\",\"weather_code\":\"wmo code\"},\"current\":{\"temperature_2m\":10.3,\"weather_code\":3}}", sampleTemp, sampleCode) && sampleTemp > 10.0 && sampleTemp < 10.5 && sampleCode == 3, "parse_forecast");
    expect(std::wstring(WeatherDescription(3)) == L"阴" && std::wstring(WeatherDescription(1)) == L"大部晴朗", "weather_desc");
    expect(FormatTemperature(10.3, false) == L"10°C" && FormatTemperature(0.0, true) == L"32°F", "temp_units");
    std::vector<GeoPlace> places;
    expect(ParseGeocodingBody("{\"results\":[{\"id\":1,\"name\":\"上海\",\"latitude\":31.22222,\"longitude\":121.45806,\"country\":\"中国\",\"admin1\":\"上海市\"}]}", places) && places.size() == 1 && places[0].name == L"上海" && places[0].latitude > 31.0, "parse_geo");
    TimerStart(TimerKind::Focus, 125000);
    expect(TimerActive() && TimerCaption().find(L"专注") != std::wstring::npos && TimerCaption().find(L"02:05") != std::wstring::npos, "timer_caption");
    TimerPause();
    expect(Timer().paused && TimerCaption().find(L"已暂停") != std::wstring::npos, "timer_pause");
    TimerStop();
    expect(!TimerActive(), "timer_stop");

    Settings migrated;
    expect(SettingsFromJson("{\"size\":0,\"opacity\":40}", migrated) && migrated.scale == 86 && migrated.opacity == 40, "migrate_small");
    expect(SettingsFromJson("{\"size\":1,\"opacity\":85}", migrated) && migrated.scale == 100 && migrated.opacity == 85, "migrate_medium");
    expect(SettingsFromJson("{\"size\":2,\"opacity\":100}", migrated) && migrated.scale == 126 && migrated.opacity == 100, "migrate_large");
    expect(SettingsFromJson("{\"scale\":175,\"size\":0,\"opacity\":23}", migrated) && migrated.scale == 175 && migrated.opacity == 23, "scale_wins");
    expect(SettingsFromJson("{\"scale\":10,\"opacity\":5}", migrated) && migrated.scale == kScaleMin && migrated.opacity == kOpacityMin, "clamp_low");
    expect(SettingsFromJson("{\"scale\":400,\"opacity\":140}", migrated) && migrated.scale == kScaleMax && migrated.opacity == kOpacityMax, "clamp_high");

    expect(FormatOffset(0) == L"UTC+0", "offset_zero");
    expect(FormatOffset(480) == L"UTC+8", "offset_china");
    expect(FormatOffset(60) == L"UTC+1", "offset_ist");
    expect(FormatOffset(-210) == L"UTC-3:30", "offset_neg");
    expect(FormatOffset(330) == L"UTC+5:30", "offset_half");

    SYSTEMTIME date{};
    date.wYear = 2026;
    date.wMonth = 11;
    date.wDay = 14;
    date.wDayOfWeek = 6;
    expect(FormatDateLine(date, 0) == L"2026年11月14日 周六 · UTC+0", "format_date");
    date.wHour = 7;
    date.wMinute = 8;
    date.wSecond = 9;
    expect(FormatTime(date, true) == L"07:08:09", "format_time");
    expect(FormatTime(date, false) == L"07:08", "format_time_nosec");

    LoadZones();
        line("zones=" + std::to_string(Zones().size()));
        const ZoneInfo* dublin = FindZone(L"GMT Standard Time");
    const ZoneInfo* beijing = FindZone(L"China Standard Time");
    if (!dublin || !beijing) {
        line("tz=skipped");
    } else {
        auto check = [&](const ZoneInfo* zone, int year, int month, int day, int expectHour, int expectMin, int expectOff, const char* name) {
            SYSTEMTIME utc{};
            utc.wYear = (WORD)year;
            utc.wMonth = (WORD)month;
            utc.wDay = (WORD)day;
            utc.wHour = 12;
            SYSTEMTIME local{};
            int off = 0;
            bool ok = ConvertUtc(zone->info, utc, local, off);
            wchar_t buf[64];
            Fmt(buf, L"%02u:%02u offset=%d", local.wHour, local.wMinute, off);
            line(std::string(name) + "=" + WideToUtf8(buf));
            if (!ok || local.wHour != expectHour || local.wMinute != expectMin || off != expectOff) ++fails;
        };
        check(dublin, 2026, 1, 15, 12, 0, 0, "dublin_jan");
        check(dublin, 2026, 7, 15, 13, 0, 60, "dublin_jul");
        check(beijing, 2026, 1, 15, 20, 0, 480, "beijing_jan");
        check(beijing, 2026, 7, 15, 20, 0, 480, "beijing_jul");
        line(std::string("tz_api=") + TimeConvertApi());
    }
    line(fails ? "result=fail" : "result=ok");
    if (!WriteUtf8File(outPath, report)) return 2;
    return fails ? 1 : 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && wcscmp(argv[1], L"--self-test") == 0) {
        const wchar_t* out = argc >= 3 ? argv[2] : L"selftest.txt";
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        int code = RunSelfTest(out);
        if (SUCCEEDED(com)) CoUninitialize();
        LocalFree(argv);
        return code;
    }
    if (argv) LocalFree(argv);

    HMODULE user = GetModuleHandleW(L"user32.dll");
    auto setCtx = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(GetProcAddress(user, "SetProcessDpiAwarenessContext"));
    if (setCtx) setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DualZoneClock.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kClass, nullptr);
        if (existing) PostMessageW(existing, WM_NUDGE, 0, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    g_settings = DefaultSettings();
    LoadSettings(g_settings);
    LoadZones();
    SetRunAtStartup(g_settings.startWithWindows);

    if (!g_renderer.Init()) {
        MessageBoxW(nullptr, L"无法初始化图形组件。", L"双时区时钟", MB_ICONERROR | MB_OK);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    if (!RegisterClasses()) {
        MessageBoxW(nullptr, L"无法创建窗口。", L"双时区时钟", MB_ICONERROR | MB_OK);
        g_renderer.Shutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }

    for (int i = 0; i < 2; ++i) {
        g_shown[i] = MakeClockText(g_settings.clock[i].label, g_settings.clock[i].timeZone, g_settings.showSeconds);
    }
    if (g_settings.hasPosition) g_dpi = DpiAtPoint(g_settings.x + 20, g_settings.y + 20);
    else {
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        g_dpi = DpiAtPoint(wa.right - 40, wa.top + 40);
    }
    Frame frame = MakeFrame();
    int w = 0, h = 0;
    if (!g_renderer.Measure(frame, w, h)) {
        MessageBoxW(nullptr, L"无法排版时钟。", L"双时区时钟", MB_ICONERROR | MB_OK);
        g_renderer.Shutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    if (!g_settings.hasPosition) {
        DefaultPlace(w, h, g_settings.x, g_settings.y);
        g_settings.hasPosition = true;
    }
    ClampVisible(g_settings.x, g_settings.y, w, h);
    g_w = w;
    g_h = h;

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (g_settings.alwaysOnTop) ex |= WS_EX_TOPMOST;
    g_hwnd = CreateWindowExW(ex, kClass, L"双时区时钟", WS_POPUP, g_settings.x, g_settings.y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        MessageBoxW(nullptr, L"无法创建窗口。", L"双时区时钟", MB_ICONERROR | MB_OK);
        g_renderer.Shutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    UINT dpiNow = GetDpiForWindow(g_hwnd);
    if (dpiNow >= 48) g_dpi = dpiNow;

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTray();
    Refresh(true, false);
    ApplyZOrder();
    SaveSettings(g_settings);
    g_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(g_hwnd, 1, 100, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DropUiFont();
    g_renderer.Shutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
