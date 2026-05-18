#pragma once
#include <string>

struct AppState;

class PresetBrowserPanel {
public:
    void draw(AppState& state);

private:
    char m_searchBuf[128] = "";
};
