#pragma once
#include "WaylandCaptureSession.h"
#include "../render/DmaBufImport.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cstdint>

// Forward-declare libdbus / libpipewire types to keep the header free of
// system includes — the implementation uses them.
struct DBusConnection;
struct pw_thread_loop;
struct pw_stream;
struct pw_context;
struct pw_core;

class VulkanContext;

class PortalCaptureSession : public WaylandCaptureSession {
public:
    struct PerBufferDmaBuf {
        ImportedDmaBuf imported;
        bool           valid = false;
    };

    explicit PortalCaptureSession(VulkanContext* vulkanCtxForDmaBuf = nullptr);
    ~PortalCaptureSession() override;

    PortalCaptureSession(const PortalCaptureSession&)            = delete;
    PortalCaptureSession& operator=(const PortalCaptureSession&) = delete;

    std::vector<SourceInfo> selectSource() override;
    void start(std::function<void(const CapturedFrame&)> onFrame) override;
    void releaseBuffer(void* sessionHandle) override;
    void stop() override;

    // Internal — exposed for the --debug-portal CLI mode.
    int      pipewireFd()      const { return m_pipewireFd; }
    uint32_t pipewireNodeId()  const { return m_pipewireNodeId; }

private:
    // D-Bus connection (the bus the portal lives on).
    DBusConnection* m_bus = nullptr;
    std::string     m_sessionHandle;       // /org/freedesktop/portal/desktop/session/...
    std::string     m_restoreToken;        // returned by Start; we persist it
    int             m_pipewireFd = -1;
    uint32_t        m_pipewireNodeId = 0;

    // PipeWire — initialised in start(), cleaned up in stop()
    pw_thread_loop* m_pwLoop    = nullptr;
    pw_context*     m_pwContext = nullptr;
    pw_core*        m_pwCore    = nullptr;
    pw_stream*      m_pwStream  = nullptr;

    std::function<void(const CapturedFrame&)> m_onFrame;
    std::atomic<bool>                          m_running{false};

    // Buffer-handle bookkeeping. PipeWire delivers a `struct pw_buffer*`
    // per frame; we map our opaque sessionHandle (uintptr_t cast) to the
    // pw_buffer for re-queue.
    std::mutex                                       m_bufferMapMutex;
    std::unordered_map<uint64_t, struct pw_buffer*>  m_bufferMap;
    uint64_t                                         m_nextHandleId = 1;

    VulkanContext* m_vkCtx = nullptr;
    bool           m_useDmaBuf = false;
    std::unordered_map<struct pw_buffer*, PerBufferDmaBuf> m_dmaCache;
    std::mutex                                              m_dmaCacheMutex;

    // Implemented in Task 6.
    void  doPortalHandshake();
    // Implemented in Task 8.
    void  initPipeWire();
    // Implemented in Task 8.
    void  teardownPipeWire();
    // Implemented in Task 8.
    static void onProcessThunk(void* userdata);
    static void onParamChangedThunk(void* userdata, uint32_t id, const struct spa_pod* param);
    void onProcess();
    void onParamChanged(uint32_t id, const struct spa_pod* param);
};
