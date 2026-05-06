#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

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

    void getDrawableSize(uint32_t& w, uint32_t& h) const;

    std::vector<const char*> requiredVulkanInstanceExtensions() const;
    VkSurfaceKHR createVulkanSurface(VkInstance inst) const;

private:
    SDL_Window* m_window = nullptr;
    bool        m_open   = true;
};
