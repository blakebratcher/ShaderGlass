#include "SdlWindow.h"
#include "../util/Logging.h"
#include <stdexcept>

SdlWindow::SdlWindow(const std::string& title, uint32_t width, uint32_t height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    m_window = SDL_CreateWindow(
        title.c_str(),
        static_cast<int>(width), static_cast<int>(height),
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window) {
        SDL_Quit();
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
}

SdlWindow::~SdlWindow() {
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
}

bool SdlWindow::pollEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) m_open = false;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) m_open = false;
    }
    return m_open;
}

void SdlWindow::getDrawableSize(uint32_t& w, uint32_t& h) const {
    int iw = 0, ih = 0;
    SDL_GetWindowSizeInPixels(m_window, &iw, &ih);
    w = static_cast<uint32_t>(iw);
    h = static_cast<uint32_t>(ih);
}
