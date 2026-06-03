#include "SdlWindow.h"
#include "ui/ImGuiLayer.h"
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
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (m_imguiLayer) {
            m_imguiLayer->processSdlEvent(e);
        }
        if (e.type == SDL_EVENT_QUIT) {
            m_open = false;
        } else if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                   e.window.windowID == SDL_GetWindowID(m_window)) {
            m_open = false;
        } else if (e.type == SDL_EVENT_KEY_DOWN) {
            // No special-case for Escape here. The key handler in main.cpp
            // checks ImGui::WantCaptureKeyboard / IsAnyItemActive and only
            // closes the window when neither claims the key — so Escape can
            // dismiss popups + clear text-input focus cleanly.
            if (m_keyHandler) {
                m_keyHandler(e.key.scancode, e.key.mod);
            }
        } else if ((e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                    e.type == SDL_EVENT_WINDOW_RESIZED) &&
                   e.window.windowID == SDL_GetWindowID(m_window)) {
            // Drawable size or DPI changed — the swapchain extent is now stale.
            // Flag it so the frame loop recreates proactively rather than
            // waiting for the next VK_ERROR_OUT_OF_DATE_KHR from acquire/present.
            m_resizePending = true;
        } else if (e.type == SDL_EVENT_DROP_FILE) {
            if (m_dropHandler && e.drop.data) m_dropHandler(e.drop.data);
        }
    }
    return m_open;
}

void SdlWindow::getDrawableSize(uint32_t& w, uint32_t& h) const {
    int iw = 0, ih = 0;
    SDL_GetWindowSizeInPixels(m_window, &iw, &ih);
    w = static_cast<uint32_t>(iw);
    h = static_cast<uint32_t>(ih);
}

std::vector<const char*> SdlWindow::requiredVulkanInstanceExtensions() const {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) {
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ")
                                 + SDL_GetError());
    }
    return { names, names + count };
}

VkSurfaceKHR SdlWindow::createVulkanSurface(VkInstance inst) const {
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(m_window, inst, nullptr, &surf)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ")
                                 + SDL_GetError());
    }
    return surf;
}
