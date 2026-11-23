#include "hand_drawing.hpp"
#include "vr_skeleton.hpp"

#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>

namespace opencap {
namespace {
struct GraphicsRuntime {
    ULONG_PTR token{};
    GraphicsRuntime() {
        Gdiplus::GdiplusStartupInput options;
        Gdiplus::GdiplusStartup(&token, &options, nullptr);
    }
    ~GraphicsRuntime() { if (token) Gdiplus::GdiplusShutdown(token); }
};
}

void drawGlovePose(HDC dc, RECT frame, Side side, const Hand& hand, bool live, Quaternion camera) {
    using namespace Gdiplus;
    static GraphicsRuntime runtime;
    if (!runtime.token) return;
    POINT corners[]{{frame.left, frame.top}, {frame.right, frame.bottom}};
    LPtoDP(dc, corners, 2);
    const auto width = corners[1].x-corners[0].x, height = corners[1].y-corners[0].y;
    if (width < 8 || height < 8) return;
    const auto rest = makeHandPreview(side, Hand{});
    const auto pose = live ? makeHandPreview(side, hand) : rest;
    float xmin{}, xmax{}, ymin{}, ymax{};
    // Include the reference pose so that closing the hand doesn't zoom in.
    for (const auto* shape : {&rest, &pose}) for (const auto& finger : *shape) for (auto p : finger) {
        p = rotate(camera, p);
        xmin = std::min(xmin, p.x); xmax = std::max(xmax, p.x);
        ymin = std::min(ymin, p.y); ymax = std::max(ymax, p.y);
    }
    const float scale = std::min(width / (xmax-xmin+.050f), height / (ymax-ymin+.024f));
    const auto project = [&](Vec3 p) {
        p = rotate(camera, p);
        return PointF((corners[0].x+corners[1].x)*.5f + (p.x-(xmin+xmax)*.5f)*scale,
                      (corners[0].y+corners[1].y)*.5f - (p.y-(ymin+ymax)*.5f)*scale);
    };
    // GDI+ uses device coordinates. Apply the GDI layout transform exactly once.
    const auto saved = SaveDC(dc);
    SetMapMode(dc, MM_TEXT);
    SetWindowOrgEx(dc, 0, 0, nullptr);
    SetViewportOrgEx(dc, 0, 0, nullptr);
    {
        Graphics g(dc);
        g.SetPageUnit(UnitPixel);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        const Color color = live ? Color(158, 212, 219) : Color(151, 170, 183);
        const float lineWidth = std::clamp(scale * .0014f, 1.25f, 4.f);
        const float radius = 1.65f * lineWidth;
        Pen bone(color, lineWidth);
        SolidBrush joint(color);
        const auto wrist = project({});
        const auto dot = [&](PointF point) {
            g.FillEllipse(&joint, point.X - radius, point.Y - radius, 2 * radius, 2 * radius);
        };

        // Outline the palm between the wrist and the four finger knuckles.
        auto previous = wrist;
        for (std::size_t f = 1; f < pose.size(); ++f) {
            const auto knuckle = project(pose[f][1]);
            g.DrawLine(&bone, previous, knuckle);
            previous = knuckle;
        }
        g.DrawLine(&bone, previous, wrist);
        dot(wrist);

        for (std::size_t f = 0; f < pose.size(); ++f) {
            auto from = f == 0 ? wrist : project(pose[f][1]);
            dot(from);
            for (std::size_t j = f == 0 ? 1 : 2; j < pose[f].size(); ++j) {
                const auto to = project(pose[f][j]);
                g.DrawLine(&bone, from, to);
                dot(to);
                from = to;
            }
        }
    }
    RestoreDC(dc, saved);
}
}
