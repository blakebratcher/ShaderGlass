#pragma once
#include <string>

struct AppState;

class SourcePickerPanel {
public:
    // The capture-backend kind name ("x11-screen", "wayland-screen") drives
    // the Wayland short-circuit. Pass the empty string for static-image mode
    // (then draw() shows a "no capture backend active" placeholder).
    explicit SourcePickerPanel(std::string captureKind)
        : m_captureKind(std::move(captureKind)) {}

    // Renders the panel for one frame. Reads state.sources +
    // state.activeSourceId, writes state.pendingSourceId on click.
    void draw(AppState& state);

private:
    std::string m_captureKind;
};
