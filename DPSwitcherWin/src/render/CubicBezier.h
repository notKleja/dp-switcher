#pragma once
#include <cmath>

// Same algorithm as WebKit's UnitBezier / CSS cubic-bezier(). Used by Tier B, which has to
// sample the easing curve itself each frame (Tier A hands the curve to DWM natively via
// CompositionCubicBezierEasingFunction and never needs this).
struct CubicBezier {
    double cx, bx, ax, cy, by, ay;
    constexpr CubicBezier(double x1, double y1, double x2, double y2)
        : cx(3 * x1), bx(3 * (x2 - x1) - 3 * x1), ax(1 - 3 * x1 - (3 * (x2 - x1) - 3 * x1)),
          cy(3 * y1), by(3 * (y2 - y1) - 3 * y1), ay(1 - 3 * y1 - (3 * (y2 - y1) - 3 * y1)) {}
    double X(double t) const { return ((ax * t + bx) * t + cx) * t; }
    double Y(double t) const { return ((ay * t + by) * t + cy) * t; }
    double dX(double t) const { return (3 * ax * t + 2 * bx) * t + cx; }
    double operator()(double x) const {
        if (x <= 0) return 0; if (x >= 1) return 1;
        double t = x;
        for (int i = 0; i < 8; ++i) {                          // Newton
            double e = X(t) - x; if (std::fabs(e) < 1e-7) return Y(t);
            double d = dX(t);    if (std::fabs(d) < 1e-6) break;
            t -= e / d;
        }
        double lo = 0, hi = 1; t = x;                          // bisection fallback
        for (int i = 0; i < 40; ++i) {
            double v = X(t); if (std::fabs(v - x) < 1e-7) break;
            (v < x ? lo : hi) = t; t = (lo + hi) / 2;
        }
        return Y(t);
    }
};
