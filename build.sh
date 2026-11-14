#!/bin/bash
# Cross-compile the portable Windows 11 clock from Linux.
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v x86_64-w64-mingw32-g++ >/dev/null || ! command -v x86_64-w64-mingw32-windres >/dev/null; then
  echo "Install MinGW-w64: g++-mingw-w64-x86-64 and binutils-mingw-w64" >&2
  exit 1
fi

mkdir -p build
x86_64-w64-mingw32-windres -O coff -i src/app.rc -o build/app.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
  -municode -mwindows \
  -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 \
  -finput-charset=UTF-8 -fwide-exec-charset=UTF-16LE \
  -static-libgcc -static-libstdc++ \
  src/main.cpp src/settings.cpp src/timezone.cpp src/render.cpp src/weather.cpp src/pomodoro.cpp \
  build/app.res \
  -ld2d1 -ldwrite -lwindowscodecs -lgdi32 -luser32 -lshell32 -lshlwapi \
  -ladvapi32 -lole32 -loleaut32 -lcomctl32 -luuid -lwinhttp -lwinmm \
  -o build/DualZoneClock.exe

echo "Built build/DualZoneClock.exe"
