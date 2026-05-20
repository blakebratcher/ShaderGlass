#include "ui/CropOverlay.h"
#include "ui/AppState.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

std::optional<CropOverlay::Point>
CropOverlay::mapToSource(Point mouseInViewport, ViewRect viewportRect, Size srcSize) {
    if (srcSize.width <= 0 || srcSize.height <= 0
        || viewportRect.w <= 0 || viewportRect.h <= 0) return std::nullopt;

    double srcAspect = double(srcSize.width) / srcSize.height;
    double vpAspect  = double(viewportRect.w) / viewportRect.h;

    double imgW, imgH, imgX, imgY;
    if (srcAspect > vpAspect) {
        // letterbox top/bottom
        imgW = viewportRect.w;
        imgH = viewportRect.w / srcAspect;
        imgX = viewportRect.x;
        imgY = viewportRect.y + (viewportRect.h - imgH) * 0.5;
    } else {
        // pillarbox left/right
        imgH = viewportRect.h;
        imgW = viewportRect.h * srcAspect;
        imgX = viewportRect.x + (viewportRect.w - imgW) * 0.5;
        imgY = viewportRect.y;
    }

    double mx = mouseInViewport.x - imgX;
    double my = mouseInViewport.y - imgY;
    if (mx < 0 || my < 0 || mx > imgW || my > imgH) return std::nullopt;

    int sx = int(std::round(mx * srcSize.width  / imgW));
    int sy = int(std::round(my * srcSize.height / imgH));
    sx = std::clamp(sx, 0, srcSize.width  - 1);
    sy = std::clamp(sy, 0, srcSize.height - 1);
    return Point{sx, sy};
}

CropRect CropOverlay::buildRect(Point a, Point b, Size srcSize) {
    int x = std::min(a.x, b.x);
    int y = std::min(a.y, b.y);
    int w = std::abs(a.x - b.x);
    int h = std::abs(a.y - b.y);

    if (w < 16 || h < 16) {
        int midX = (a.x + b.x) / 2;
        int midY = (a.y + b.y) / 2;
        w = std::max(w, 16);
        h = std::max(h, 16);
        x = midX - w / 2;
        y = midY - h / 2;
    }
    w = std::min(w, srcSize.width);
    h = std::min(h, srcSize.height);
    x = std::clamp(x, 0, srcSize.width  - w);
    y = std::clamp(y, 0, srcSize.height - h);
    return CropRect{x, y, w, h};
}

bool CropOverlay::draw(AppState& state) {
    if (!state.cropMode) return false;
    if (!state.capture)  { state.cropMode = false; return false; }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ViewRect viewportRect{
        int(vp->WorkPos.x), int(vp->WorkPos.y),
        int(vp->WorkSize.x), int(vp->WorkSize.y),
    };
    auto sz = state.capture->size();
    Size srcSize{sz.width, sz.height};

    ImGui::SetNextWindowPos({float(viewportRect.x), float(viewportRect.y)});
    ImGui::SetNextWindowSize({float(viewportRect.w), float(viewportRect.h)});
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking
                           | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("##cropOverlay", nullptr, flags)) { ImGui::End(); return true; }

    ImVec2 mouse = ImGui::GetMousePos();
    auto mp = mapToSource({int(mouse.x), int(mouse.y)}, viewportRect, srcSize);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mp) {
        m_dragging = true;
        m_dragStartSrc = *mp;
        m_dragEndSrc   = *mp;
    } else if (m_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) && mp) {
        m_dragEndSrc = *mp;
    } else if (m_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_dragging = false;
    }

    auto rect = buildRect(m_dragStartSrc, m_dragEndSrc, srcSize);
    auto toView = [&](Point sp) -> ImVec2 {
        double srcAspect = double(srcSize.width) / srcSize.height;
        double vpAspect  = double(viewportRect.w) / viewportRect.h;
        double imgW, imgH, imgX, imgY;
        if (srcAspect > vpAspect) {
            imgW = viewportRect.w; imgH = viewportRect.w / srcAspect;
            imgX = viewportRect.x; imgY = viewportRect.y + (viewportRect.h - imgH) * 0.5;
        } else {
            imgH = viewportRect.h; imgW = viewportRect.h * srcAspect;
            imgX = viewportRect.x + (viewportRect.w - imgW) * 0.5;
            imgY = viewportRect.y;
        }
        return ImVec2(float(imgX + sp.x * imgW / srcSize.width),
                      float(imgY + sp.y * imgH / srcSize.height));
    };

    ImVec2 r0 = toView({rect.x,          rect.y});
    ImVec2 r1 = toView({rect.x + rect.w, rect.y + rect.h});

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 shade = IM_COL32(0, 0, 0, 128);
    dl->AddRectFilled({float(viewportRect.x), float(viewportRect.y)},
                      {float(viewportRect.x + viewportRect.w), r0.y}, shade);
    dl->AddRectFilled({float(viewportRect.x), r1.y},
                      {float(viewportRect.x + viewportRect.w),
                       float(viewportRect.y + viewportRect.h)}, shade);
    dl->AddRectFilled({float(viewportRect.x), r0.y}, {r0.x, r1.y}, shade);
    dl->AddRectFilled({r1.x, r0.y},
                      {float(viewportRect.x + viewportRect.w), r1.y}, shade);
    dl->AddRect(r0, r1, IM_COL32(255, 255, 255, 220), 0.0f, 0, 2.0f);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d at %d,%d", rect.w, rect.h, rect.x, rect.y);
    dl->AddText({r0.x + 4, r0.y + 4}, IM_COL32(255, 255, 255, 230), buf);

    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        state.pendingCrop = rect;
        state.cropMode = false;
        m_dragging = false;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        state.cropMode = false;
        m_dragging = false;
    }

    ImGui::End();
    return true;
}
