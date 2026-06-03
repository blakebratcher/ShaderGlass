#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class ImGuiLayer;

class SdlWindow {
public:
    SdlWindow(const std::string& title, uint32_t width, uint32_t height);
    ~SdlWindow();

    SdlWindow(const SdlWindow&)            = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;
    SdlWindow(SdlWindow&&)                 = delete;
    SdlWindow& operator=(SdlWindow&&)      = delete;

    SDL_Window* handle() const { return m_window; }

    // Returns false when the user requested close.
    bool pollEvents();

    // External close request (used by the F-key handler in main.cpp so it
    // can route Esc to ImGui modals first instead of always closing).
    void requestClose() noexcept { m_open = false; }

    // Set by pollEvents() when the drawable size / DPI changes
    // (SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED). The frame loop reads this to
    // recreate the swapchain proactively instead of waiting for the next
    // VK_ERROR_OUT_OF_DATE_KHR. Cleared by takeResizePending().
    bool takeResizePending() noexcept {
        bool was = m_resizePending;
        m_resizePending = false;
        return was;
    }

    void getDrawableSize(uint32_t& w, uint32_t& h) const;

    std::vector<const char*> requiredVulkanInstanceExtensions() const;
    VkSurfaceKHR createVulkanSurface(VkInstance inst) const;

    void setImGuiLayer(ImGuiLayer* layer) noexcept { m_imguiLayer = layer; }

    // Called for each file dropped onto the window. main.cpp wires this to
    // AppState::pendingPresetPath so .slangp files can be imported via DnD.
    void setDropFileHandler(std::function<void(const std::string&)> h) {
        m_dropHandler = std::move(h);
    }

    // Called for each unhandled key-down event (after ImGui has had its
    // chance). Hotkeys live here. Escape is consumed by the window itself
    // (closes) and never forwarded.
    void setKeyDownHandler(std::function<void(SDL_Scancode, SDL_Keymod)> h) {
        m_keyHandler = std::move(h);
    }

private:
    SDL_Window* m_window        = nullptr;
    bool        m_open          = true;
    bool        m_resizePending = false;
    ImGuiLayer* m_imguiLayer    = nullptr;
    std::function<void(const std::string&)>            m_dropHandler;
    std::function<void(SDL_Scancode, SDL_Keymod)>      m_keyHandler;
};
