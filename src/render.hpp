#pragma once

#include "common.hpp"

// One frame of the two-clock widget. Sizes are chosen inside the renderer.
struct WidgetHits {
    bool hasStrip = false;
    RECT strip{};
};

struct Frame {
    std::wstring label[2];
    std::wstring time[2];
    std::wstring date[2];
    std::wstring weatherText[2];
    int weatherCode[2] = {-1, -1};
    bool showWeather = false;
    bool showTimer = false;
    std::wstring timerText;
    int timerStyle = 0;
    int opacity = 85;
    int scale = 100;
    UINT dpi = 96;
    bool hovered = false;
};

struct RendererImpl;

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Init();
    void Shutdown();
    bool Measure(const Frame& frame, int& w, int& h);
    bool Draw(HWND hwnd, int x, int y, const Frame& frame, WidgetHits* hits);
    const std::wstring& UiFace() const { return face_; }

private:
    RendererImpl* impl_ = nullptr;
    std::wstring face_ = L"Segoe UI";
};
