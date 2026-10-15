// DdcProbe: tiny live graph of DDC/CI read reliability for VCP 0x60, meant for poking at
// the monitor (OSD, input switches, cable wiggles) and watching how the DDC channel reacts.
// Reuses the real app's DdcWorker so it measures exactly what DPSwitcher sees.
//
//   Space   pause / resume polling
//   + / -   poll interval (50..2000 ms)
//   M       drop a marker (e.g. "opened OSD now") -- drawn as a yellow line on both graphs
//   S       save everything so far as CSV next to the app's log
//   C       clear
//   H       toggle keeping the PHYSICAL_MONITOR handle open between reads (vs. reopening it
//           per read) -- drop a marker first to compare fail rate / latency before and after

#include "pch.h"
#include "AppMessages.h"
#include "Ddc.h"
#include "util/Log.h"
#include <deque>
#include <vector>
#include <map>
#include <string>
#include <fstream>
#include <shlobj.h>

namespace {
    using Clock = std::chrono::steady_clock;

    struct Sample { Clock::time_point t; bool ok; HRESULT hr; DWORD value; long long latencyMs; };

    constexpr UINT_PTR kPollTimer = 1;
    constexpr size_t kMaxSamples = 20000;
    constexpr int kGraphSeconds = 120;

    HWND g_hwnd = nullptr;
    DdcWorker g_ddc;
    HMONITOR g_monitor = nullptr;
    UINT g_intervalMs = 150;
    bool g_paused = false;
    bool g_pending = false;
    UINT64 g_nextId = 1;
    Clock::time_point g_postedAt{};
    std::deque<Sample> g_samples;
    std::vector<Clock::time_point> g_markers;
    Clock::time_point g_start = Clock::now();

    HDC g_memDc = nullptr;
    HBITMAP g_memBmp = nullptr;
    int g_memW = 0, g_memH = 0;
    HFONT g_font = nullptr, g_fontSmall = nullptr;
    int g_lineH = 24, g_lineHSmall = 18;   // derived from font metrics at the current DPI
    double g_scale = 1.0;                  // dpi / 96, applied to every layout constant

    int S(int px) { return static_cast<int>(px * g_scale + 0.5); }

    long long MsBetween(Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    }

    void RebuildFonts(UINT dpi) {
        g_scale = dpi / 96.0;
        if (g_font) DeleteObject(g_font);
        if (g_fontSmall) DeleteObject(g_fontSmall);
        g_font = CreateFontW(-MulDiv(11, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_fontSmall = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        HDC dc = GetDC(nullptr);
        TEXTMETRICW tm{};
        SelectObject(dc, g_font);      GetTextMetricsW(dc, &tm); g_lineH = tm.tmHeight + S(6);
        SelectObject(dc, g_fontSmall); GetTextMetricsW(dc, &tm); g_lineHSmall = tm.tmHeight + S(4);
        ReleaseDC(nullptr, dc);
    }

    void PostRead() {
        if (g_paused || g_pending) return;
        g_pending = true;
        g_postedAt = Clock::now();
        g_ddc.Post(DdcRequest{ DdcRequest::Read, g_monitor, 0x60, 0, g_nextId++ });
    }

    void ApplyInterval() {
        SetTimer(g_hwnd, kPollTimer, g_intervalMs, nullptr);
    }

    struct Rate { int total = 0, fails = 0; double Pct() const { return total ? 100.0 * fails / total : 0.0; } };

    Rate RateOverLast(int seconds) {
        Rate r;
        auto cutoff = Clock::now() - std::chrono::seconds(seconds);
        for (auto it = g_samples.rbegin(); it != g_samples.rend() && it->t >= cutoff; ++it) {
            r.total++;
            if (!it->ok) r.fails++;
        }
        return r;
    }

    void EnsureBackBuffer(HDC dc, int w, int h) {
        if (g_memDc && w == g_memW && h == g_memH) return;
        if (g_memBmp) DeleteObject(g_memBmp);
        if (g_memDc) DeleteDC(g_memDc);
        g_memDc = CreateCompatibleDC(dc);
        g_memBmp = CreateCompatibleBitmap(dc, std::max(w, 1), std::max(h, 1));
        SelectObject(g_memDc, g_memBmp);
        g_memW = w; g_memH = h;
    }

    void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
    }

    void TextAt(HDC dc, int x, int y, COLORREF c, const std::wstring& s, HFONT font) {
        SelectObject(dc, font);
        SetTextColor(dc, c);
        TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
    }

    void DrawVLine(HDC dc, int x, int y0, int y1, COLORREF c, int width) {
        HPEN pen = CreatePen(PS_SOLID, width, c);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, x, y0, nullptr);
        LineTo(dc, x, y1);
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    void Paint(HDC target, const RECT& client) {
        const int w = client.right, h = client.bottom;
        EnsureBackBuffer(target, w, h);
        HDC dc = g_memDc;
        SetBkMode(dc, TRANSPARENT);

        FillRectColor(dc, client, RGB(18, 18, 22));

        const auto now = Clock::now();
        const Rate r10 = RateOverLast(10), r60 = RateOverLast(60), rAll = RateOverLast(1 << 20);
        const int margin = S(12);

        // ---- header text -------------------------------------------------------------
        int y = S(10);
        wchar_t buf[512];
        swprintf_s(buf, L"DDC probe  VCP 0x60   interval %u ms   handle: %s   %s", g_intervalMs,
            g_ddc.CacheHandles() ? L"cached" : L"per-read", g_paused ? L"PAUSED" : L"running");
        TextAt(dc, margin, y, RGB(230, 230, 235), buf, g_font); y += g_lineH;

        swprintf_s(buf, L"fail rate   10s: %5.1f%% (%d/%d)    60s: %5.1f%% (%d/%d)    session: %5.1f%% (%d/%d)",
            r10.Pct(), r10.fails, r10.total, r60.Pct(), r60.fails, r60.total, rAll.Pct(), rAll.fails, rAll.total);
        TextAt(dc, margin, y, RGB(230, 230, 235), buf, g_font); y += g_lineH;

        if (!g_samples.empty()) {
            const Sample& last = g_samples.back();
            long long lastOkValue = -1, okLatency = 0, okCount = 0;
            for (auto it = g_samples.rbegin(); it != g_samples.rend(); ++it) {
                if (it->ok) { if (lastOkValue < 0) lastOkValue = it->value; okLatency += it->latencyMs; okCount++; if (okCount >= 50) break; }
            }
            swprintf_s(buf, L"last: %s  hr=0x%08X  value=%lu  latency=%lldms      last good value=%lld      avg ok latency (50)=%lldms",
                last.ok ? L"OK  " : L"FAIL", last.hr, last.value, last.latencyMs, lastOkValue,
                okCount ? okLatency / okCount : 0);
            TextAt(dc, margin, y, RGB(200, 200, 210), buf, g_fontSmall);
        }
        y += g_lineHSmall;

        // HRESULT histogram over the last 60 s.
        {
            std::map<HRESULT, int> hist;
            auto cutoff = now - std::chrono::seconds(60);
            for (auto it = g_samples.rbegin(); it != g_samples.rend() && it->t >= cutoff; ++it)
                if (!it->ok) hist[it->hr]++;
            std::wstring line = L"failures by HRESULT (60s): ";
            if (hist.empty()) line += L"none";
            for (auto& [hr, n] : hist) {
                swprintf_s(buf, L"0x%08X x%d   ", hr, n);
                line += buf;
            }
            TextAt(dc, margin, y, RGB(200, 200, 210), line, g_fontSmall); y += g_lineHSmall;
        }

        TextAt(dc, margin, y, RGB(140, 140, 150),
            L"Space pause   +/- interval   M marker (yellow)   S save CSV   C clear   H handle cached/per-read      green=DP(15)  blue=USB-C(26)  orange=other  red=failed",
            g_fontSmall);
        y += g_lineHSmall + S(8);

        // ---- strip: one bar per sample, height = latency -------------------------------
        const int stripTop = y;
        const int stripH = std::max(S(60), (h - y - S(40)) * 2 / 5);
        const int stripBottom = stripTop + stripH;
        RECT stripRect{ margin, stripTop, w - margin, stripBottom };
        FillRectColor(dc, stripRect, RGB(28, 28, 34));
        TextAt(dc, margin + S(4), stripTop + S(4), RGB(120, 120, 130), L"per-read latency (bar height, 0-400ms) and outcome", g_fontSmall);

        const int barW = S(3);
        const int maxBars = std::max(1, (w - 2 * margin) / barW);
        const int count = static_cast<int>(std::min<size_t>(g_samples.size(), maxBars));
        const int stripInnerTop = stripTop + g_lineHSmall + S(4);
        const int stripInnerBottom = stripBottom - S(4);
        const int stripInnerH = std::max(1, stripInnerBottom - stripInnerTop);
        {
            int x = w - margin - barW;
            auto it = g_samples.rbegin();
            for (int i = 0; i < count; ++i, ++it, x -= barW) {
                const long long lat = std::min<long long>(it->latencyMs, 400);
                const int bh = std::max(S(2), static_cast<int>(stripInnerH * lat / 400));
                COLORREF c = !it->ok ? RGB(220, 60, 60)
                    : it->value == 15 ? RGB(70, 200, 90)
                    : it->value == 26 ? RGB(80, 140, 240)
                    : RGB(240, 160, 50);
                RECT bar{ x, stripInnerBottom - bh, x + barW - 1, stripInnerBottom };
                FillRectColor(dc, bar, c);
            }
            for (auto m : g_markers) {
                int idx = 0;
                for (auto s = g_samples.rbegin(); s != g_samples.rend() && idx < count; ++s, ++idx) {
                    if (s->t <= m) break;
                }
                if (idx < count) DrawVLine(dc, w - margin - barW - idx * barW, stripInnerTop, stripInnerBottom, RGB(250, 220, 60), S(2));
            }
        }

        // ---- line graph: per-second fail rate over the last 120 s ------------------------
        const int graphTop = stripBottom + S(12), graphBottom = h - margin;
        if (graphBottom - graphTop < S(40)) { BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY); return; }
        RECT graphRect{ margin, graphTop, w - margin, graphBottom };
        FillRectColor(dc, graphRect, RGB(28, 28, 34));
        swprintf_s(buf, L"fail rate per second, last %d s (0%% bottom, 100%% top)", kGraphSeconds);
        TextAt(dc, margin + S(4), graphTop + S(4), RGB(120, 120, 130), buf, g_fontSmall);
        const int gTop = graphTop + g_lineHSmall + S(4), gBottom = graphBottom - S(6), gH = std::max(1, gBottom - gTop);
        const int gLeft = margin, gRight = w - margin;
        for (int pct : { 25, 50, 75 }) {
            int gy = gBottom - gH * pct / 100;
            HPEN pen = CreatePen(PS_DOT, 1, RGB(50, 50, 60));
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, gLeft, gy, nullptr); LineTo(dc, gRight, gy);
            SelectObject(dc, old); DeleteObject(pen);
        }
        {
            std::vector<Rate> buckets(kGraphSeconds);
            for (auto it = g_samples.rbegin(); it != g_samples.rend(); ++it) {
                long long age = MsBetween(it->t, now) / 1000;
                if (age >= kGraphSeconds) break;
                buckets[static_cast<size_t>(age)].total++;
                if (!it->ok) buckets[static_cast<size_t>(age)].fails++;
            }
            const double pxPerSec = static_cast<double>(gRight - gLeft) / kGraphSeconds;
            HPEN pen = CreatePen(PS_SOLID, S(2), RGB(230, 90, 90));
            HGDIOBJ old = SelectObject(dc, pen);
            bool penDown = false;
            for (int age = kGraphSeconds - 1; age >= 0; --age) {
                const Rate& b = buckets[static_cast<size_t>(age)];
                int x = static_cast<int>(gRight - (age + 0.5) * pxPerSec);
                if (b.total == 0) { penDown = false; continue; }
                int gy = gBottom - static_cast<int>(gH * b.Pct() / 100.0);
                if (!penDown) { MoveToEx(dc, x, gy, nullptr); penDown = true; }
                else LineTo(dc, x, gy);
            }
            SelectObject(dc, old); DeleteObject(pen);

            for (auto m : g_markers) {
                long long ageMs = MsBetween(m, now);
                if (ageMs / 1000 >= kGraphSeconds) continue;
                int x = static_cast<int>(gRight - (ageMs / 1000.0) * pxPerSec);
                DrawVLine(dc, x, gTop, gBottom, RGB(250, 220, 60), S(2));
            }
        }

        BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    }

    void SaveCsv() {
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return;
        std::wstring dir = std::wstring(local) + L"\\DPSwitcher";
        CoTaskMemFree(local);
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t name[128];
        swprintf_s(name, L"\\ddcprobe-%04d%02d%02d-%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::wstring path = dir + name;
        std::ofstream f(path);
        f << "t_ms,ok,hr,value,latency_ms\n";
        for (const auto& s : g_samples) {
            char line[128];
            sprintf_s(line, "%lld,%d,0x%08X,%lu,%lld\n", MsBetween(g_start, s.t), s.ok ? 1 : 0, s.hr, s.value, s.latencyMs);
            f << line;
        }
        for (auto m : g_markers) {
            char line[64];
            sprintf_s(line, "marker,%lld\n", MsBetween(g_start, m));
            f << line;
        }
        f.close();
        std::wstring title = L"DDC probe - saved " + path;
        SetWindowTextW(g_hwnd, title.c_str());
    }

    LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_TIMER:
            if (w == kPollTimer) PostRead();
            return 0;
        case WM_APP_DDC_RESULT_MSG: {
            std::unique_ptr<DdcResult> r(reinterpret_cast<DdcResult*>(w));
            Sample s{ Clock::now(), SUCCEEDED(r->hr), r->hr, r->current, MsBetween(g_postedAt, Clock::now()) };
            g_samples.push_back(s);
            if (g_samples.size() > kMaxSamples) g_samples.pop_front();
            g_pending = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            switch (w) {
            case VK_SPACE: g_paused = !g_paused; break;
            case VK_OEM_PLUS: case VK_ADD: g_intervalMs = std::min<UINT>(2000, g_intervalMs + 50); ApplyInterval(); break;
            case VK_OEM_MINUS: case VK_SUBTRACT: g_intervalMs = std::max<UINT>(50, g_intervalMs - 50); ApplyInterval(); break;
            case 'M': g_markers.push_back(Clock::now()); break;
            case 'S': SaveCsv(); break;
            case 'C': g_samples.clear(); g_markers.clear(); g_start = Clock::now(); break;
            case 'H': g_ddc.SetCacheHandles(!g_ddc.CacheHandles()); g_markers.push_back(Clock::now()); break;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            RebuildFonts(HIWORD(w));
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
            mmi->ptMinTrackSize = { S(700), S(420) };
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            Paint(dc, rc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LogInit();

    g_monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi = 96;
    { UINT dy = 96; GetDpiForMonitor(g_monitor, MDT_EFFECTIVE_DPI, &dpi, &dy); }
    RebuildFonts(dpi);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"DPSwitcher.DdcProbe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"DDC probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(600), nullptr, nullptr, hi, nullptr);
    if (!g_hwnd) return 1;

    g_ddc.Start(g_hwnd);
    ApplyInterval();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_ddc.Stop();
    return 0;
}
