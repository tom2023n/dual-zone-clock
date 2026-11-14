#include "render.hpp"
#include "resource.h"

#include <wincodec.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwrite_3.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

struct RendererImpl {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory5> dw;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    ComPtr<IDWriteFontCollection1> collection;
    ComPtr<IDWriteFontCollection> system;
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> rt;
    std::vector<unsigned char> fontBytes;
    int cacheW = 0;
    int cacheH = 0;
    bool useSystem = false;
    bool loaderRegistered = false;
    std::wstring face = L"Segoe UI";
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_MEDIUM;
};

namespace {

struct Geom {
    float scale = 1.f;
    float shadow = 16.f;
    float radius = 16.f;
    float padX = 20.f;
    float padY = 14.f;
    float colGap = 28.f;
    float gap1 = 2.f;
    float gap2 = 2.f;
    float dot = 7.f;
    float dotGap = 7.f;
    float labelH = 16.f;
    float timeH = 36.f;
    float dateH = 16.f;
    float colW = 80.f;
    float cardW = 200.f;
    float cardH = 80.f;
    float digitW = 16.f;
    float colonW = 8.f;
    float timeBlockW = 80.f;
    float labelTextW[2] = {};
    float weatherTextW[2] = {};
    bool showWeather = false;
    bool hasStrip = false;
    float stripH = 0;
    float stripOffset = 0;
    int winW = 0;
    int winH = 0;
    int digits = 6;
    int colons = 2;
};

std::wstring WidestDigits(const std::wstring& s) {
    std::wstring o = s;
    for (wchar_t& ch : o) {
        if (ch >= L'0' && ch <= L'9') ch = L'8';
    }
    return o;
}

bool MeasureString(IDWriteFactory* dw, IDWriteTextFormat* fmt, const std::wstring& text, float& w, float& h) {
    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 4000.f, 800.f, layout.put());
    if (FAILED(hr)) return false;
    DWRITE_TEXT_METRICS m{};
    hr = layout->GetMetrics(&m);
    if (FAILED(hr)) return false;
    w = m.widthIncludingTrailingWhitespace;
    h = m.height;
    return true;
}

bool MakeFormat(RendererImpl& impl, float px, DWRITE_TEXT_ALIGNMENT align, ComPtr<IDWriteTextFormat>& fmt) {
    IDWriteFontCollection* coll = impl.useSystem ? impl.system.get() : static_cast<IDWriteFontCollection*>(impl.collection.get());
    HRESULT hr = impl.dw->CreateTextFormat(impl.face.c_str(), coll, impl.weight, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, px, L"zh-CN", fmt.put());
    if (FAILED(hr)) {
        hr = impl.dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, px, L"zh-CN", fmt.put());
    }
    if (FAILED(hr)) return false;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return true;
}

bool ComputeGeom(RendererImpl& impl, const Frame& frame, Geom& g) {
    float user = frame.scale / 100.f;
    if (user < 0.5f) user = 0.5f;
    if (user > 3.f) user = 3.f;
    float dpi = frame.dpi < 48 ? 96.f : (float)frame.dpi;
    g.scale = user * (dpi / 96.f);
    if (g.scale < 0.45f) g.scale = 0.45f;
    const float s = g.scale;
    g.shadow = 16.f * s;
    g.radius = 16.f * s;
    g.padX = 20.f * s;
    g.padY = 14.f * s;
    g.colGap = 32.f * s;
    g.gap1 = 1.f * s;
    g.gap2 = 2.f * s;
    g.dot = 7.f * s;
    g.dotGap = 7.f * s;

    ComPtr<IDWriteTextFormat> labelFmt, timeFmt, dateFmt;
    if (!MakeFormat(impl, 13.f * s, DWRITE_TEXT_ALIGNMENT_LEADING, labelFmt)) return false;
    if (!MakeFormat(impl, 36.f * s, DWRITE_TEXT_ALIGNMENT_CENTER, timeFmt)) return false;
    if (!MakeFormat(impl, 12.f * s, DWRITE_TEXT_ALIGNMENT_CENTER, dateFmt)) return false;

    float labelH = 0, timeH = 0, dateH = 0;
    g.digitW = 1.f;
    g.colonW = 1.f;
    for (wchar_t ch = L'0'; ch <= L'9'; ++ch) {
        float w = 0, h = 0;
        if (!MeasureString(impl.dw.get(), timeFmt.get(), std::wstring(1, ch), w, h)) return false;
        g.digitW = std::max(g.digitW, w);
        timeH = std::max(timeH, h);
    }
    {
        float w = 0, h = 0;
        if (!MeasureString(impl.dw.get(), timeFmt.get(), L"-", w, h)) return false;
        g.digitW = std::max(g.digitW, w);
        timeH = std::max(timeH, h);
        if (!MeasureString(impl.dw.get(), timeFmt.get(), L":", w, h)) return false;
        g.colonW = std::max(1.f, w);
        timeH = std::max(timeH, h);
    }
    g.digitW += 0.6f * s;
    g.colonW += 0.4f * s;

    const std::wstring& sampleTime = frame.time[0].empty() ? L"88:88:88" : frame.time[0];
    g.digits = 0;
    g.colons = 0;
    for (wchar_t ch : sampleTime) {
        if (ch == L':') ++g.colons;
        else ++g.digits;
    }
    if (g.digits == 0) {
        g.digits = 6;
        g.colons = 2;
    }
    g.timeBlockW = g.digitW * (float)g.digits + g.colonW * (float)g.colons;

    g.showWeather = frame.showWeather;
    float colNeed = g.timeBlockW;
    for (int i = 0; i < 2; ++i) {
        float w = 0, h = 0;
        std::wstring label = frame.label[i].empty() ? L" " : frame.label[i];
        if (!MeasureString(impl.dw.get(), labelFmt.get(), label, w, h)) return false;
        g.labelTextW[i] = w;
        labelH = std::max(labelH, h);
        g.weatherTextW[i] = 0;
        if (frame.showWeather) {
            std::wstring sample = frame.weatherText[i].empty() ? L"--" : frame.weatherText[i];
            std::wstring wide = L"-88°C";
            float tw = 0, th = 0, ww = 0, wh = 0;
            if (!MeasureString(impl.dw.get(), labelFmt.get(), sample, tw, th)) return false;
            if (!MeasureString(impl.dw.get(), labelFmt.get(), wide, ww, wh)) return false;
            g.weatherTextW[i] = std::max(tw, ww);
            labelH = std::max(labelH, th);
        }
        std::wstring date = WidestDigits(frame.date[i].empty() ? L"8888年88月88日 周三 · UTC+8" : frame.date[i]);
        float dw = 0, dh = 0;
        if (!MeasureString(impl.dw.get(), dateFmt.get(), date, dw, dh)) return false;
        float aw = 0, ah = 0;
        if (!MeasureString(impl.dw.get(), dateFmt.get(), frame.date[i].empty() ? date : frame.date[i], aw, ah)) return false;
        dateH = std::max(dateH, std::max(dh, ah));
        float weatherExtra = 0;
        if (frame.showWeather) weatherExtra = 5.f * s + 14.f * s + 4.f * s + g.weatherTextW[i];
        float need = std::max(g.dot + g.dotGap + w + weatherExtra, std::max(g.timeBlockW, std::max(dw, aw)));
        colNeed = std::max(colNeed, need);
    }
    g.labelH = labelH + 3.f * s;
    g.timeH = timeH + 2.f * s;
    g.dateH = dateH + 3.f * s;
    g.colW = colNeed;
    g.hasStrip = frame.showTimer && !frame.timerText.empty();
    g.stripH = g.hasStrip ? 24.f * s : 0;
    float stripGap = g.hasStrip ? 8.f * s : 0;
    g.cardW = g.padX * 2.f + g.colW * 2.f + g.colGap;
    g.cardH = g.padY * 2.f + g.labelH + g.gap1 + g.timeH + g.gap2 + g.dateH + stripGap + g.stripH;
    g.stripOffset = g.cardH - g.padY - g.stripH;
    g.winW = (int)std::ceil(g.cardW + g.shadow * 2.f);
    g.winH = (int)std::ceil(g.cardH + g.shadow * 2.f);
    if (g.winW < 32) g.winW = 32;
    if (g.winH < 32) g.winH = 32;
    return true;
}

void DrawWeatherIcon(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, int code, float cx, float cy, float d) {
    if (code < 0 || d < 2.f) return;
    auto cloud = [&](float x, float y, float s, float alpha) {
        brush->SetColor(D2D1::ColorF(0.86f, 0.89f, 0.94f, alpha));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x - s * 0.22f, y + s * 0.08f), s * 0.34f, s * 0.24f), brush);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + s * 0.22f, y + s * 0.1f), s * 0.28f, s * 0.2f), brush);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y - s * 0.12f), s * 0.24f, s * 0.22f), brush);
    };
    auto sun = [&](float x, float y, float r) {
        brush->SetColor(D2D1::ColorF(1.f, 0.82f, 0.38f, 1.f));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush);
        brush->SetColor(D2D1::ColorF(1.f, 0.86f, 0.45f, 0.9f));
        for (int i = 0; i < 4; ++i) {
            float a = (float)i * 0.785398f;
            float dx = std::cos(a);
            float dy = std::sin(a);
            rt->DrawLine(D2D1::Point2F(x + dx * r * 1.45f, y + dy * r * 1.45f), D2D1::Point2F(x + dx * r * 2.15f, y + dy * r * 2.15f), brush, std::max(1.f, r * 0.28f));
        }
    };
    bool snow = code == 71 || code == 73 || code == 75 || code == 77 || code == 85 || code == 86;
    bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
    bool storm = code == 95 || code == 96 || code == 99;
    bool fog = code == 45 || code == 48;
    if (code <= 1) {
        sun(cx, cy, d * 0.22f);
    } else if (code == 2) {
        sun(cx - d * 0.12f, cy - d * 0.08f, d * 0.16f);
        cloud(cx + d * 0.08f, cy + d * 0.08f, d * 0.7f, 1.f);
    } else if (fog) {
        brush->SetColor(D2D1::ColorF(0.78f, 0.82f, 0.88f, 0.9f));
        for (int i = 0; i < 3; ++i) {
            float y = cy - d * 0.28f + i * d * 0.28f;
            rt->DrawLine(D2D1::Point2F(cx - d * 0.42f, y), D2D1::Point2F(cx + d * 0.42f, y), brush, std::max(1.2f, d * 0.12f));
        }
    } else {
        cloud(cx, cy - (rain || snow || storm ? d * 0.12f : 0), d, 1.f);
        if (rain || storm) {
            brush->SetColor(D2D1::ColorF(0.55f, 0.75f, 1.f, 0.95f));
            rt->DrawLine(D2D1::Point2F(cx - d * 0.18f, cy + d * 0.22f), D2D1::Point2F(cx - d * 0.28f, cy + d * 0.48f), brush, std::max(1.f, d * 0.1f));
            rt->DrawLine(D2D1::Point2F(cx + d * 0.12f, cy + d * 0.22f), D2D1::Point2F(cx + d * 0.02f, cy + d * 0.48f), brush, std::max(1.f, d * 0.1f));
        }
        if (snow) {
            brush->SetColor(D2D1::ColorF(0.9f, 0.95f, 1.f, 1.f));
            rt->DrawLine(D2D1::Point2F(cx, cy + d * 0.12f), D2D1::Point2F(cx, cy + d * 0.48f), brush, std::max(1.f, d * 0.08f));
            rt->DrawLine(D2D1::Point2F(cx - d * 0.16f, cy + d * 0.3f), D2D1::Point2F(cx + d * 0.16f, cy + d * 0.3f), brush, std::max(1.f, d * 0.08f));
        }
        if (storm) {
            brush->SetColor(D2D1::ColorF(1.f, 0.86f, 0.35f, 1.f));
            rt->DrawLine(D2D1::Point2F(cx + d * 0.08f, cy + d * 0.05f), D2D1::Point2F(cx - d * 0.02f, cy + d * 0.28f), brush, std::max(1.2f, d * 0.12f));
            rt->DrawLine(D2D1::Point2F(cx - d * 0.02f, cy + d * 0.28f), D2D1::Point2F(cx + d * 0.16f, cy + d * 0.28f), brush, std::max(1.2f, d * 0.12f));
            rt->DrawLine(D2D1::Point2F(cx + d * 0.16f, cy + d * 0.28f), D2D1::Point2F(cx + d * 0.02f, cy + d * 0.5f), brush, std::max(1.2f, d * 0.12f));
        }
    }
}

void DrawShadowed(ID2D1RenderTarget* rt, IDWriteTextLayout* layout, float x, float y, ID2D1SolidColorBrush* brush, D2D1_COLOR_F color) {
    brush->SetColor(D2D1::ColorF(0.f, 0.f, 0.f, 0.50f));
    rt->DrawTextLayout(D2D1::Point2F(x, y + 1.f), layout, brush);
    brush->SetColor(color);
    rt->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
}

bool EnsureTarget(RendererImpl& impl, int w, int h) {
    if (impl.rt && impl.cacheW == w && impl.cacheH == h) return true;
    impl.rt.reset();
    impl.bmp.reset();
    HRESULT hr = impl.wic->CreateBitmap((UINT)w, (UINT)h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, impl.bmp.put());
    if (FAILED(hr)) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    hr = impl.d2d->CreateWicBitmapRenderTarget(impl.bmp.get(), props, impl.rt.put());
    if (FAILED(hr)) return false;
    impl.cacheW = w;
    impl.cacheH = h;
    return true;
}

bool Paint(RendererImpl& impl, const Frame& frame, const Geom& g) {
    if (!EnsureTarget(impl, g.winW, g.winH)) return false;
    ID2D1RenderTarget* rt = impl.rt.get();
    rt->BeginDraw();
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float s = g.scale;
    D2D1_RECT_F card = D2D1::RectF(g.shadow, g.shadow, g.shadow + g.cardW, g.shadow + g.cardH);

    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush.put());
    if (!brush) {
        rt->EndDraw();
        return false;
    }
    for (int i = 8; i >= 1; --i) {
        float e = (float)i * 1.2f * s;
        float a = 0.05f * (float)(9 - i) / 8.f;
        D2D1_RECT_F r = D2D1::RectF(card.left - e, card.top - e * 0.35f, card.right + e, card.bottom + e * 1.15f);
        brush->SetColor(D2D1::ColorF(0, 0, 0, a));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, g.radius + e, g.radius + e), brush.get());
    }

    float bgA = frame.opacity / 100.f;
    if (bgA < 0.05f) bgA = 0.05f;
    if (bgA > 1.f) bgA = 1.f;
    float lift = frame.hovered ? 0.025f : 0.f;
    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.f;
    stops[0].color = D2D1::ColorF(0.155f + lift, 0.155f + lift, 0.175f + lift, bgA);
    stops[1].position = 1.f;
    stops[1].color = D2D1::ColorF(0.07f, 0.07f, 0.08f, bgA);
    ComPtr<ID2D1GradientStopCollection> collection;
    HRESULT hr = rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, collection.put());
    if (FAILED(hr)) {
        rt->EndDraw();
        return false;
    }
    ComPtr<ID2D1LinearGradientBrush> grad;
    hr = rt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(card.left, card.top), D2D1::Point2F(card.left, card.bottom)),
        collection.get(), grad.put());
    if (FAILED(hr)) {
        rt->EndDraw();
        return false;
    }
    rt->FillRoundedRectangle(D2D1::RoundedRect(card, g.radius, g.radius), grad.get());

    float stroke = std::max(1.f, s);
    D2D1_RECT_F inner = D2D1::RectF(card.left + stroke * 0.5f, card.top + stroke * 0.5f, card.right - stroke * 0.5f, card.bottom - stroke * 0.5f);
    brush->SetColor(D2D1::ColorF(1, 1, 1, frame.hovered ? 0.30f : 0.16f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(inner, std::max(1.f, g.radius - stroke), std::max(1.f, g.radius - stroke)), brush.get(), stroke);

    float dx = std::floor(card.left + g.padX + g.colW + g.colGap * 0.5f) + 0.5f;
    float dividerBottom = g.hasStrip ? card.top + g.stripOffset - 6.f * s : card.bottom - g.padY - 2.f * s;
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.14f));
    rt->DrawLine(D2D1::Point2F(dx, card.top + g.padY + 2.f * s), D2D1::Point2F(dx, dividerBottom), brush.get(), std::max(1.f, s * 0.8f));

    ComPtr<IDWriteTextFormat> labelFmt, timeFmt, dateFmt;
    if (!MakeFormat(impl, 13.f * s, DWRITE_TEXT_ALIGNMENT_LEADING, labelFmt) ||
        !MakeFormat(impl, 36.f * s, DWRITE_TEXT_ALIGNMENT_CENTER, timeFmt) ||
        !MakeFormat(impl, 12.f * s, DWRITE_TEXT_ALIGNMENT_CENTER, dateFmt)) {
        rt->EndDraw();
        return false;
    }

    const D2D1_COLOR_F labelColor[2] = {
        D2D1::ColorF(142.f / 255.f, 190.f / 255.f, 1.f, 1.f),
        D2D1::ColorF(1.f, 197.f / 255.f, 107.f / 255.f, 1.f),
    };
    const D2D1_COLOR_F timeColor = D2D1::ColorF(0.97f, 0.975f, 0.985f, 1.f);
    const D2D1_COLOR_F dateColor = D2D1::ColorF(0.67f, 0.70f, 0.75f, 1.f);

    float colX[2] = {card.left + g.padX, card.left + g.padX + g.colW + g.colGap};
    float labelY = std::round(card.top + g.padY);
    float timeY = std::round(labelY + g.labelH + g.gap1);
    float dateY = std::round(timeY + g.timeH + g.gap2);

    for (int i = 0; i < 2; ++i) {
        std::wstring label = frame.label[i].empty() ? L" " : frame.label[i];
        float weatherExtra = 0;
        if (g.showWeather) weatherExtra = 5.f * s + 14.f * s + 4.f * s + g.weatherTextW[i];
        float group = g.dot + g.dotGap + g.labelTextW[i] + weatherExtra;
        float x = colX[i] + (g.colW - group) * 0.5f;
        brush->SetColor(labelColor[i]);
        float cy = labelY + g.labelH * 0.5f;
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + g.dot * 0.5f, cy), g.dot * 0.5f, g.dot * 0.5f), brush.get());
        ComPtr<IDWriteTextLayout> labelLayout;
        impl.dw->CreateTextLayout(label.c_str(), (UINT32)label.size(), labelFmt.get(), std::max(1.f, g.labelTextW[i] + 2.f), g.labelH, labelLayout.put());
        if (labelLayout) DrawShadowed(rt, labelLayout.get(), x + g.dot + g.dotGap, labelY, brush.get(), labelColor[i]);
        if (g.showWeather) {
            float ix = x + g.dot + g.dotGap + g.labelTextW[i] + 6.f * s;
            float iy = labelY + g.labelH * 0.5f;
            DrawWeatherIcon(rt, brush.get(), frame.weatherCode[i], ix + 7.f * s, iy, 13.f * s);
            std::wstring wt = frame.weatherText[i].empty() ? L"--" : frame.weatherText[i];
            ComPtr<IDWriteTextLayout> weatherLayout;
            impl.dw->CreateTextLayout(wt.c_str(), (UINT32)wt.size(), labelFmt.get(), std::max(1.f, g.weatherTextW[i] + 2.f), g.labelH, weatherLayout.put());
            if (weatherLayout) DrawShadowed(rt, weatherLayout.get(), ix + 14.f * s + 3.f * s, labelY, brush.get(), dateColor);
        }

        std::wstring time = frame.time[i].empty() ? L"--:--" : frame.time[i];
        int digits = 0, colons = 0;
        for (wchar_t ch : time) {
            if (ch == L':') ++colons;
            else ++digits;
        }
        float block = g.digitW * (float)digits + g.colonW * (float)colons;
        float tx = std::round(colX[i] + (g.colW - block) * 0.5f);
        for (wchar_t ch : time) {
            float cell = ch == L':' ? g.colonW : g.digitW;
            wchar_t one[2] = {ch, 0};
            ComPtr<IDWriteTextLayout> glyph;
            impl.dw->CreateTextLayout(one, 1, timeFmt.get(), std::max(1.f, cell), g.timeH, glyph.put());
            if (glyph) DrawShadowed(rt, glyph.get(), tx, timeY, brush.get(), timeColor);
            tx += cell;
        }

        std::wstring date = frame.date[i].empty() ? L" " : frame.date[i];
        ComPtr<IDWriteTextLayout> dateLayout;
        impl.dw->CreateTextLayout(date.c_str(), (UINT32)date.size(), dateFmt.get(), g.colW, g.dateH, dateLayout.put());
        if (dateLayout) DrawShadowed(rt, dateLayout.get(), colX[i], dateY, brush.get(), dateColor);
    }

    if (g.hasStrip) {
        float stripTop = card.top + g.stripOffset;
        brush->SetColor(D2D1::ColorF(1, 1, 1, 0.12f));
        rt->DrawLine(D2D1::Point2F(card.left + g.padX, stripTop - 4.f * s), D2D1::Point2F(card.right - g.padX, stripTop - 4.f * s), brush.get(), std::max(1.f, s * 0.7f));
        D2D1_COLOR_F timerColor = D2D1::ColorF(1.f, 0.78f, 0.42f, 1.f);
        if (frame.timerStyle == 1) timerColor = D2D1::ColorF(0.55f, 0.86f, 0.72f, 1.f);
        if (frame.timerStyle == 2) timerColor = D2D1::ColorF(0.62f, 0.78f, 1.f, 1.f);
        ComPtr<IDWriteTextFormat> timerFmt;
        if (MakeFormat(impl, 13.f * s, DWRITE_TEXT_ALIGNMENT_CENTER, timerFmt)) {
            ComPtr<IDWriteTextLayout> timerLayout;
            impl.dw->CreateTextLayout(frame.timerText.c_str(), (UINT32)frame.timerText.size(), timerFmt.get(), g.cardW - g.padX * 2.f, g.stripH, timerLayout.put());
            if (timerLayout) DrawShadowed(rt, timerLayout.get(), card.left + g.padX, stripTop, brush.get(), timerColor);
        }
    }

    hr = rt->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        impl.rt.reset();
        impl.bmp.reset();
        impl.cacheW = 0;
        impl.cacheH = 0;
        return false;
    }
    return SUCCEEDED(hr);
}

bool Present(RendererImpl& impl, HWND hwnd, int x, int y, const Geom& g) {
    WICRect rect{0, 0, g.winW, g.winH};
    ComPtr<IWICBitmapLock> lock;
    HRESULT hr = impl.bmp->Lock(&rect, WICBitmapLockRead, lock.put());
    if (FAILED(hr)) return false;
    UINT stride = 0;
    UINT size = 0;
    BYTE* src = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&size, &src);
    if (!src || stride == 0) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g.winW;
    bi.bmiHeader.biHeight = -g.winH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!mem || !dib || !bits) {
        if (dib) DeleteObject(dib);
        if (mem) DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    const int row = g.winW * 4;
    for (int yy = 0; yy < g.winH; ++yy) {
        memcpy(static_cast<unsigned char*>(bits) + yy * row, src + yy * stride, (size_t)row);
    }
    POINT dst{x, y};
    SIZE sz{g.winW, g.winH};
    POINT srcPt{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    BOOL ok = UpdateLayeredWindow(hwnd, screen, &dst, &sz, mem, &srcPt, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok == TRUE;
}

bool ReadLocalized(IDWriteLocalizedStrings* names, std::wstring& out) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
    UINT32 len = 0;
    if (FAILED(names->GetStringLength(index, &len))) return false;
    out.assign(len + 1, L'\0');
    if (FAILED(names->GetString(index, out.data(), len + 1))) return false;
    out.resize(len);
    return !out.empty();
}

bool AdoptFamily(RendererImpl& impl) {
    IDWriteFontCollection* coll = impl.useSystem ? impl.system.get() : static_cast<IDWriteFontCollection*>(impl.collection.get());
    if (!coll || coll->GetFontFamilyCount() == 0) return false;
    ComPtr<IDWriteFontFamily> fam;
    if (FAILED(coll->GetFontFamily(0, fam.put())) || !fam) return false;
    if (!impl.useSystem) {
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(fam->GetFamilyNames(names.put())) || !ReadLocalized(names.get(), impl.face)) return false;
        ComPtr<IDWriteFont> font;
        if (SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.put())) && font) {
            impl.weight = font->GetWeight();
        }
        return true;
    }
    return false;
}

bool LoadSystemFont(RendererImpl& impl) {
    if (!impl.system) {
        IDWriteFactory* base = impl.dw.get();
        if (FAILED(base->GetSystemFontCollection(impl.system.put()))) return false;
    }
    const wchar_t* names[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI"};
    for (const wchar_t* name : names) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(impl.system->FindFamilyName(name, &index, &exists)) && exists) {
            impl.face = name;
            impl.weight = DWRITE_FONT_WEIGHT_NORMAL;
            impl.useSystem = true;
            return true;
        }
    }
    impl.face = L"Segoe UI";
    impl.weight = DWRITE_FONT_WEIGHT_NORMAL;
    impl.useSystem = true;
    return true;
}

bool LoadEmbeddedFont(RendererImpl& impl) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_FONT), RT_RCDATA);
    if (!res) return false;
    HGLOBAL glob = LoadResource(nullptr, res);
    void* data = glob ? LockResource(glob) : nullptr;
    DWORD size = SizeofResource(nullptr, res);
    if (!data || size < 64) return false;
    impl.fontBytes.assign(static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);

    DWORD nfonts = 0;
    AddFontMemResourceEx(impl.fontBytes.data(), (DWORD)impl.fontBytes.size(), nullptr, &nfonts);

    if (FAILED(impl.dw->CreateInMemoryFontFileLoader(impl.loader.put()))) return false;
    if (FAILED(impl.dw->RegisterFontFileLoader(impl.loader.get()))) return false;
    impl.loaderRegistered = true;
    ComPtr<IDWriteFontFile> file;
    if (FAILED(impl.loader->CreateInMemoryFontFileReference(impl.dw.get(), impl.fontBytes.data(), (UINT32)impl.fontBytes.size(), nullptr, file.put()))) {
        return false;
    }
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(impl.dw->CreateFontSetBuilder(builder.put()))) return false;
    if (FAILED(builder->AddFontFile(file.get()))) return false;
    ComPtr<IDWriteFontSet> set;
    if (FAILED(builder->CreateFontSet(set.put()))) return false;
    if (FAILED(impl.dw->CreateFontCollectionFromFontSet(set.get(), impl.collection.put()))) return false;
    impl.useSystem = false;
    return AdoptFamily(impl);
}

}  // namespace

Renderer::~Renderer() { Shutdown(); }

void Renderer::Shutdown() {
    if (!impl_) return;
    impl_->rt.reset();
    impl_->bmp.reset();
    impl_->collection.reset();
    impl_->system.reset();
    if (impl_->loaderRegistered && impl_->dw && impl_->loader) {
        impl_->dw->UnregisterFontFileLoader(impl_->loader.get());
        impl_->loaderRegistered = false;
    }
    impl_->loader.reset();
    impl_->dw.reset();
    impl_->d2d.reset();
    impl_->wic.reset();
    delete impl_;
    impl_ = nullptr;
}

bool Renderer::Init() {
    Shutdown();
    impl_ = new RendererImpl();
    D2D1_FACTORY_OPTIONS opt{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opt, (void**)impl_->d2d.put());
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), (IUnknown**)impl_->dw.put());
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)impl_->wic.put());
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }
    if (!LoadEmbeddedFont(*impl_)) {
        if (!LoadSystemFont(*impl_)) {
            Shutdown();
            return false;
        }
    }
    face_ = impl_->face;
    return true;
}

bool Renderer::Measure(const Frame& frame, int& w, int& h) {
    if (!impl_) return false;
    Geom g;
    if (!ComputeGeom(*impl_, frame, g)) return false;
    w = g.winW;
    h = g.winH;
    return true;
}

bool Renderer::Draw(HWND hwnd, int x, int y, const Frame& frame, WidgetHits* hits) {
    if (!impl_ || !hwnd) return false;
    Geom g;
    if (!ComputeGeom(*impl_, frame, g)) return false;
    if (hits) {
        hits->hasStrip = false;
        hits->strip = RECT{};
        if (g.hasStrip) {
            float left = g.shadow;
            float top = g.shadow + g.stripOffset;
            hits->hasStrip = true;
            hits->strip.left = (LONG)std::floor(left);
            hits->strip.top = (LONG)std::floor(top);
            hits->strip.right = (LONG)std::ceil(left + g.cardW);
            hits->strip.bottom = (LONG)std::ceil(g.shadow + g.cardH);
        }
    }
    if (!Paint(*impl_, frame, g)) {
        if (!Paint(*impl_, frame, g)) return false;
    }
    return Present(*impl_, hwnd, x, y, g);
}
