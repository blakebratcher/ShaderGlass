#pragma once
#include <optional>
#include <vector>
#include <string>
#include "CapturedFrame.h"

struct SourceInfo {
    std::string id;
    std::string displayName;
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual std::vector<SourceInfo>      enumerateSources() = 0;
    virtual void                         selectSource(const SourceInfo&) = 0;
    virtual std::optional<CapturedFrame> acquireFrame() = 0;
    virtual void                         release(CapturedFrame&) = 0;
};
