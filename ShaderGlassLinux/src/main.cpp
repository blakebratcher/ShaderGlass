#include "output/SdlWindow.h"
#include "util/Logging.h"

int main(int /*argc*/, char** /*argv*/) {
    try {
        SdlWindow window("ShaderGlass (Linux M1)", 1280, 720);
        LOG_INFO("Window opened. Close or press Esc to exit.");
        while (window.pollEvents()) {
            SDL_Delay(16);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("fatal: %s", e.what());
        return 1;
    }
    return 0;
}
