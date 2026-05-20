#include "StaticImageCapture.h"
#include <stb_image.h>
#include <stdexcept>

StaticImageCapture::StaticImageCapture(std::filesystem::path image)
    : m_path(std::move(image)) {}

StaticImageCapture::~StaticImageCapture() = default;

void StaticImageCapture::load() {
    if (!m_pixels.empty()) return;
    int channels = 0;
    unsigned char* data = stbi_load(m_path.string().c_str(),
                                    &m_width, &m_height, &channels, 4);
    if (!data) throw std::runtime_error("stbi_load failed for " + m_path.string());
    size_t n = (size_t)m_width * (size_t)m_height * 4;
    m_pixels.assign(data, data + n);
    stbi_image_free(data);
}

std::vector<SourceInfo> StaticImageCapture::enumerateSources() {
    return { { m_path.string(), m_path.filename().string() } };
}

void StaticImageCapture::selectSource(const SourceInfo&) { load(); }

std::optional<CapturedFrame> StaticImageCapture::acquireFrame() {
    if (m_pixels.empty()) return std::nullopt;
    CapturedFrame f;
    f.kind   = CapturedFrame::Kind::CpuBuffer;
    f.width  = (uint32_t)m_width;
    f.height = (uint32_t)m_height;
    f.stride = (size_t)m_width * 4;
    f.data   = m_pixels.data();
    // DRM_FORMAT_ABGR8888: little-endian R,G,B,A in memory.
    f.fourcc = 0x34324241; // 'AB24'
    return f;
}

void StaticImageCapture::release(CapturedFrame&) { /* memory owned by us */ }

CaptureBackend::Size StaticImageCapture::size() const {
    return { m_width, m_height };
}
