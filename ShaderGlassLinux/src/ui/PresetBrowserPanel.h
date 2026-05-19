#pragma once
#include "util/PresetLibrary.h"
#include <string>
#include <vector>

struct AppState;

class PresetBrowserPanel {
public:
    void draw(AppState& state);

private:
    char                     m_searchBuf[128]  = "";
    char                     m_importBuf[1024] = "";
    std::vector<PresetEntry> m_cached;        // populated lazily; user-controlled refresh
    bool                     m_scanned = false;
};
