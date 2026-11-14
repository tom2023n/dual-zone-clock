#include "pomodoro.hpp"

namespace {
TimerState g_timer;
}

TimerState& Timer() { return g_timer; }

bool TimerActive() { return g_timer.kind != TimerKind::None; }

void TimerStart(TimerKind kind, unsigned long long durationMs) {
    if (durationMs < 1000) durationMs = 1000;
    g_timer.kind = kind;
    g_timer.paused = false;
    g_timer.remainingMs = durationMs;
    g_timer.deadline = GetTickCount64() + durationMs;
}

void TimerPause() {
    if (!TimerActive() || g_timer.paused) return;
    unsigned long long now = GetTickCount64();
    g_timer.remainingMs = now < g_timer.deadline ? g_timer.deadline - now : 0;
    g_timer.paused = true;
}

void TimerResume() {
    if (!TimerActive() || !g_timer.paused) return;
    g_timer.paused = false;
    g_timer.deadline = GetTickCount64() + g_timer.remainingMs;
}

void TimerStop() {
    g_timer = TimerState{};
}

bool TimerTick(unsigned long long now) {
    if (!TimerActive() || g_timer.paused) return false;
    if (now < g_timer.deadline) {
        g_timer.remainingMs = g_timer.deadline - now;
        return false;
    }
    g_timer.remainingMs = 0;
    return true;
}

std::wstring TimerCaption() {
    if (!TimerActive()) return L"";
    unsigned long long ms = g_timer.remainingMs;
    if (!g_timer.paused) {
        unsigned long long now = GetTickCount64();
        ms = now < g_timer.deadline ? g_timer.deadline - now : 0;
    }
    unsigned total = (unsigned)((ms + 999ull) / 1000ull);
    unsigned h = total / 3600;
    unsigned m = (total % 3600) / 60;
    unsigned s = total % 60;
    wchar_t num[16];
    if (h > 0) Fmt(num, L"%u:%02u:%02u", h, m, s);
    else Fmt(num, L"%02u:%02u", m, s);
    const wchar_t* name = L"倒计时";
    if (g_timer.kind == TimerKind::Focus) name = L"专注";
    else if (g_timer.kind == TimerKind::ShortBreak) name = L"短休息";
    else if (g_timer.kind == TimerKind::LongBreak) name = L"长休息";
    std::wstring text = g_timer.paused ? L"已暂停  " : L"";
    text += name;
    text += L"  ";
    text += num;
    return text;
}

int TimerStyle() {
    if (g_timer.kind == TimerKind::Focus) return 0;
    if (g_timer.kind == TimerKind::Countdown) return 2;
    return 1;
}
