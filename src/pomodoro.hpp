#pragma once

#include "common.hpp"

enum class TimerKind { None, Focus, ShortBreak, LongBreak, Countdown };

struct TimerState {
    TimerKind kind = TimerKind::None;
    bool paused = false;
    unsigned long long remainingMs = 0;
    unsigned long long deadline = 0;
    int focusDone = 0;
};

TimerState& Timer();
bool TimerActive();
void TimerStart(TimerKind kind, unsigned long long durationMs);
void TimerPause();
void TimerResume();
void TimerStop();
// Returns true when the running timer reaches zero.
bool TimerTick(unsigned long long now);
std::wstring TimerCaption();
int TimerStyle();
