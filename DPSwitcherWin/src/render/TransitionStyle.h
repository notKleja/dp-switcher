#pragma once
#include <chrono>

namespace style {
    inline constexpr float kBezierX1 = 0.93f, kBezierY1 = 0.02f, kBezierX2 = 0.74f, kBezierY2 = 0.98f;
    inline constexpr std::chrono::milliseconds kDuration{ 650 };
    // A cancel while the cover is still animating in reverses smoothly from wherever it got
    // to, rather than popping the desktop back instantly.
    inline constexpr std::chrono::milliseconds kCancelDuration{ 200 };
    inline constexpr float kMaxBlurDip = 40.0f;   // macOS maxBlurRadius; tune visually side by side
    // Fully opaque at rest. A partial value (e.g. the ~235/255 carried over from the earlier
    // PowerShell prototype) leaves a permanent sliver of the live blurred backdrop showing
    // through, which is barely visible on a dark desktop but reads as a "jump"/pop the moment
    // the desktop behind it is bright -- most noticeably right when this PC becomes the active
    // input again and the leak becomes visible against real content.
    inline constexpr float kMaxDim = 1.0f;
}
