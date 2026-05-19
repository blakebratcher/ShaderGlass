#pragma once
#include "util/ConfigStore.h"   // for CropRect
#include <optional>

struct AppState;

class CropOverlay {
public:
    bool draw(AppState& state);

    struct Point   { int x, y; };
    struct ViewRect{ int x, y, w, h; };
    struct Size    { int width, height; };

    // Maps a viewport-space mouse point into source-pixel coordinates.
    // Returns nullopt when the mouse is in a letter/pillar-box bar.
    static std::optional<Point> mapToSource(Point  mouseInViewport,
                                            ViewRect viewportRect,
                                            Size     srcSize);

    // Constructs a CropRect from two source-space points (drag start/end).
    // Normalizes corners, snaps <16 sides to 16 around midpoint, clamps.
    static CropRect buildRect(Point a, Point b, Size srcSize);

private:
    bool  m_dragging = false;
    Point m_dragStartSrc{};
    Point m_dragEndSrc{};
};
