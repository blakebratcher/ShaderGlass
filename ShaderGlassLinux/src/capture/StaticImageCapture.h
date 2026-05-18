#pragma once
#include "CaptureBackend.h"
#include <filesystem>
#include <vector>

class StaticImageCapture : public CaptureBackend {
public:
    explicit StaticImageCapture(std::filesystem::path image);
    ~StaticImageCapture() override;

    std::string                  kindName() const override { return "static-image"; }
    std::vector<SourceInfo>      enumerateSources() override;
    void                         selectSource(const SourceInfo&) override;
    std::optional<CapturedFrame> acquireFrame() override;
    void                         release(CapturedFrame&) override;

private:
    void load();
    std::filesystem::path m_path;
    std::vector<uint8_t>  m_pixels;  // tightly packed RGBA8
    int                   m_width  = 0;
    int                   m_height = 0;
};
