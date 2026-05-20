#pragma once

struct AppState;

// Help panel — when state.showHelp is true, draws a dockable window
// summarising hotkeys, drag-drop behaviour, and CLI pointers. F1
// toggles state.showHelp from the hotkey handler in main.cpp.
class HelpPanel {
public:
    void draw(AppState& state);
};
