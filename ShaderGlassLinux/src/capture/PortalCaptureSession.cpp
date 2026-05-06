#include "PortalCaptureSession.h"
#include "../render/VulkanContext.h"
#include "../util/Logging.h"
#include "../util/XdgConfig.h"

#include <dbus/dbus.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <pipewire/properties.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>

namespace {

// Generates an 8-char hex token used to disambiguate concurrent portal requests.
std::string randomToken() {
    static std::atomic<uint32_t> counter{0};
    std::random_device rd;
    uint32_t v = (rd() ^ (counter.fetch_add(1) << 16));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "sg_%08x", v);
    return buf;
}

// Compose the Request object path the portal will publish results on.
// Format: /org/freedesktop/portal/desktop/request/<sender_no_prefix>/<token>
std::string requestObjectPath(DBusConnection* bus, const std::string& token) {
    const char* unique = dbus_bus_get_unique_name(bus);
    std::string s(unique ? unique : "");
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    for (auto& c : s) if (c == '.') c = '_';
    return "/org/freedesktop/portal/desktop/request/" + s + "/" + token;
}

void appendDictEntryString(DBusMessageIter* dictIter, const char* key, const char* value) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dictIter, &entry);
}
void appendDictEntryUint32(DBusMessageIter* dictIter, const char* key, uint32_t value) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dictIter, &entry);
}
void appendDictEntryBool(DBusMessageIter* dictIter, const char* key, bool value) {
    dbus_bool_t b = value ? TRUE : FALSE;
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dictIter, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dictIter, &entry);
}

struct PortalResponse {
    uint32_t code = 0;
    DBusMessage* msg = nullptr;
};

PortalResponse waitForResponse(DBusConnection* bus, const std::string& requestPath, int timeoutMs = 60000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        dbus_connection_read_write(bus, 100);
        DBusMessage* m = dbus_connection_pop_message(bus);
        while (m) {
            if (dbus_message_is_signal(m, "org.freedesktop.portal.Request", "Response") &&
                requestPath == (dbus_message_get_path(m) ? dbus_message_get_path(m) : "")) {
                DBusMessageIter args;
                dbus_message_iter_init(m, &args);
                if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32) {
                    dbus_message_unref(m);
                    throw std::runtime_error("portal: malformed Response (no uint32 code)");
                }
                PortalResponse r;
                dbus_message_iter_get_basic(&args, &r.code);
                r.msg = m;
                return r;
            }
            dbus_message_unref(m);
            m = dbus_connection_pop_message(bus);
        }
    }
    throw std::runtime_error("portal: Response signal timed out");
}

struct spa_video_info_raw g_negotiatedFormat{};
bool                     g_haveFormat = false;

} // namespace

PortalCaptureSession::PortalCaptureSession(VulkanContext* vulkanCtxForDmaBuf) {
    m_vkCtx = vulkanCtxForDmaBuf;

    DBusError err; dbus_error_init(&err);
    m_bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        std::string msg = std::string("portal: dbus_bus_get failed: ") + err.message;
        dbus_error_free(&err);
        throw std::runtime_error(msg);
    }
    if (!m_bus) throw std::runtime_error("portal: dbus_bus_get returned null");

    dbus_bus_add_match(m_bus,
        "type='signal',interface='org.freedesktop.portal.Request',member='Response'",
        &err);
    if (dbus_error_is_set(&err)) {
        std::string msg = std::string("portal: add_match failed: ") + err.message;
        dbus_error_free(&err);
        throw std::runtime_error(msg);
    }
    dbus_connection_flush(m_bus);

    if (auto t = XdgConfig::readToken("portal-token")) {
        m_restoreToken = *t;
        LOG_INFO("portal: loaded restore token from disk");
    }
}

PortalCaptureSession::~PortalCaptureSession() {
    if (m_bus) {
        // dbus_bus_get returns a shared connection — do NOT unref it (causes
        // crash on shutdown). The session-bus connection is process-lifetime.
        m_bus = nullptr;
    }
}

std::vector<SourceInfo> PortalCaptureSession::selectSource() {
    doPortalHandshake();
    return { { "wayland-screen://" + std::to_string(m_pipewireNodeId),
               "wayland-screen (node " + std::to_string(m_pipewireNodeId) + ")" } };
}

void PortalCaptureSession::start(std::function<void(const CapturedFrame&)> onFrame) {
    m_onFrame = std::move(onFrame);
    initPipeWire();
    m_running.store(true);
}

void PortalCaptureSession::releaseBuffer(void* sessionHandle) {
    if (!sessionHandle || !m_pwStream) return;
    uint64_t handle = uintptr_t(sessionHandle);
    pw_buffer* pb = nullptr;
    {
        std::lock_guard<std::mutex> g(m_bufferMapMutex);
        auto it = m_bufferMap.find(handle);
        if (it == m_bufferMap.end()) return;
        pb = it->second;
        m_bufferMap.erase(it);
    }
    pw_thread_loop_lock(m_pwLoop);
    pw_stream_queue_buffer(m_pwStream, pb);
    pw_thread_loop_unlock(m_pwLoop);
}

void PortalCaptureSession::stop() {
    if (m_running.exchange(false)) {
        teardownPipeWire();
    }
}

void PortalCaptureSession::doPortalHandshake() {
    // ---- 1. CreateSession ----
    std::string sessionToken = randomToken();
    std::string handleToken  = randomToken();

    DBusMessage* call = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
        "CreateSession");
    if (!call) throw std::runtime_error("portal: dbus_message_new_method_call failed");

    DBusMessageIter args, dict;
    dbus_message_iter_init_append(call, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    appendDictEntryString(&dict, "handle_token", handleToken.c_str());
    appendDictEntryString(&dict, "session_handle_token", sessionToken.c_str());
    dbus_message_iter_close_container(&args, &dict);

    std::string requestPath = requestObjectPath(m_bus, handleToken);

    DBusError err; dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_bus, call, 5000, &err);
    dbus_message_unref(call);
    if (dbus_error_is_set(&err)) {
        std::string msg = std::string("portal: CreateSession send failed: ") + err.message;
        dbus_error_free(&err);
        throw std::runtime_error(msg);
    }
    if (reply) dbus_message_unref(reply);

    PortalResponse cs = waitForResponse(m_bus, requestPath);
    if (cs.code != 0) {
        dbus_message_unref(cs.msg);
        throw std::runtime_error("portal: CreateSession failed (code " + std::to_string(cs.code) + ")");
    }
    {
        DBusMessageIter ri; dbus_message_iter_init(cs.msg, &ri);
        dbus_message_iter_next(&ri);
        DBusMessageIter respDict; dbus_message_iter_recurse(&ri, &respDict);
        while (dbus_message_iter_get_arg_type(&respDict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ent; dbus_message_iter_recurse(&respDict, &ent);
            const char* k = nullptr; dbus_message_iter_get_basic(&ent, &k);
            dbus_message_iter_next(&ent);
            DBusMessageIter var; dbus_message_iter_recurse(&ent, &var);
            if (k && std::strcmp(k, "session_handle") == 0 &&
                dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
                const char* v = nullptr; dbus_message_iter_get_basic(&var, &v);
                if (v) m_sessionHandle = v;
            }
            dbus_message_iter_next(&respDict);
        }
        dbus_message_unref(cs.msg);
    }
    if (m_sessionHandle.empty()) throw std::runtime_error("portal: CreateSession Response missing session_handle");

    // ---- 2. SelectSources ----
    {
        std::string ht = randomToken();
        DBusMessage* c = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "SelectSources");
        DBusMessageIter a, d;
        dbus_message_iter_init_append(c, &a);
        const char* sh = m_sessionHandle.c_str();
        dbus_message_iter_append_basic(&a, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&a, DBUS_TYPE_ARRAY, "{sv}", &d);
        appendDictEntryString(&d, "handle_token", ht.c_str());
        appendDictEntryUint32(&d, "types", 3);
        appendDictEntryUint32(&d, "cursor_mode", 2);
        appendDictEntryBool  (&d, "multiple", false);
        appendDictEntryUint32(&d, "persist_mode", 2);
        if (!m_restoreToken.empty())
            appendDictEntryString(&d, "restore_token", m_restoreToken.c_str());
        dbus_message_iter_close_container(&a, &d);

        std::string rp = requestObjectPath(m_bus, ht);
        DBusMessage* rep = dbus_connection_send_with_reply_and_block(m_bus, c, 5000, &err);
        dbus_message_unref(c);
        if (dbus_error_is_set(&err)) {
            std::string msg = std::string("portal: SelectSources send failed: ") + err.message;
            dbus_error_free(&err);
            throw std::runtime_error(msg);
        }
        if (rep) dbus_message_unref(rep);

        PortalResponse ss = waitForResponse(m_bus, rp);
        uint32_t code = ss.code;
        dbus_message_unref(ss.msg);
        if (code != 0) throw std::runtime_error("portal: SelectSources cancelled (code " + std::to_string(code) + ")");
    }

    // ---- 3. Start ----
    {
        std::string ht = randomToken();
        DBusMessage* c = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "Start");
        DBusMessageIter a, d;
        dbus_message_iter_init_append(c, &a);
        const char* sh = m_sessionHandle.c_str();
        dbus_message_iter_append_basic(&a, DBUS_TYPE_OBJECT_PATH, &sh);
        const char* parent = "";
        dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &parent);
        dbus_message_iter_open_container(&a, DBUS_TYPE_ARRAY, "{sv}", &d);
        appendDictEntryString(&d, "handle_token", ht.c_str());
        dbus_message_iter_close_container(&a, &d);

        std::string rp = requestObjectPath(m_bus, ht);
        DBusMessage* rep = dbus_connection_send_with_reply_and_block(m_bus, c, 60000, &err);
        dbus_message_unref(c);
        if (dbus_error_is_set(&err)) {
            std::string msg = std::string("portal: Start send failed: ") + err.message;
            dbus_error_free(&err);
            throw std::runtime_error(msg);
        }
        if (rep) dbus_message_unref(rep);

        PortalResponse st = waitForResponse(m_bus, rp);
        if (st.code != 0) {
            uint32_t code = st.code;
            dbus_message_unref(st.msg);
            throw std::runtime_error("portal: Start cancelled (code " + std::to_string(code) + ")");
        }
        DBusMessageIter ri; dbus_message_iter_init(st.msg, &ri);
        dbus_message_iter_next(&ri);
        DBusMessageIter rd; dbus_message_iter_recurse(&ri, &rd);
        while (dbus_message_iter_get_arg_type(&rd) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ent; dbus_message_iter_recurse(&rd, &ent);
            const char* k = nullptr; dbus_message_iter_get_basic(&ent, &k);
            dbus_message_iter_next(&ent);
            DBusMessageIter var; dbus_message_iter_recurse(&ent, &var);
            if (k && std::strcmp(k, "streams") == 0) {
                DBusMessageIter arr; dbus_message_iter_recurse(&var, &arr);
                if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
                    DBusMessageIter str; dbus_message_iter_recurse(&arr, &str);
                    uint32_t node = 0; dbus_message_iter_get_basic(&str, &node);
                    m_pipewireNodeId = node;
                }
            } else if (k && std::strcmp(k, "restore_token") == 0 &&
                       dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
                const char* v = nullptr; dbus_message_iter_get_basic(&var, &v);
                if (v) m_restoreToken = v;
                try {
                    XdgConfig::writeToken("portal-token", m_restoreToken);
                    LOG_INFO("portal: persisted restore token");
                } catch (const std::exception& e) {
                    LOG_WARN("portal: failed to persist restore token: %s", e.what());
                }
            }
            dbus_message_iter_next(&rd);
        }
        dbus_message_unref(st.msg);
    }
    if (m_pipewireNodeId == 0) throw std::runtime_error("portal: Start missing pipewire node id");

    // ---- 4. OpenPipeWireRemote ----
    {
        DBusMessage* c = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast",
            "OpenPipeWireRemote");
        DBusMessageIter a, d;
        dbus_message_iter_init_append(c, &a);
        const char* sh = m_sessionHandle.c_str();
        dbus_message_iter_append_basic(&a, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&a, DBUS_TYPE_ARRAY, "{sv}", &d);
        dbus_message_iter_close_container(&a, &d);

        DBusMessage* rep = dbus_connection_send_with_reply_and_block(m_bus, c, 5000, &err);
        dbus_message_unref(c);
        if (dbus_error_is_set(&err)) {
            std::string msg = std::string("portal: OpenPipeWireRemote failed: ") + err.message;
            dbus_error_free(&err);
            throw std::runtime_error(msg);
        }
        if (!rep) throw std::runtime_error("portal: OpenPipeWireRemote no reply");

        DBusMessageIter ri; dbus_message_iter_init(rep, &ri);
        if (dbus_message_iter_get_arg_type(&ri) != DBUS_TYPE_UNIX_FD) {
            dbus_message_unref(rep);
            throw std::runtime_error("portal: OpenPipeWireRemote reply not h");
        }
        int fd = -1; dbus_message_iter_get_basic(&ri, &fd);
        m_pipewireFd = fd;
        dbus_message_unref(rep);
    }
    if (m_pipewireFd < 0) throw std::runtime_error("portal: OpenPipeWireRemote returned invalid fd");

    LOG_INFO("portal: handshake complete (node=%u, fd=%d, restore_token=%s)",
             m_pipewireNodeId, m_pipewireFd,
             m_restoreToken.empty() ? "(none)" : "(present)");
}

void PortalCaptureSession::initPipeWire() {
    pw_init(nullptr, nullptr);

    m_pwLoop = pw_thread_loop_new("shaderglass-pw", nullptr);
    if (!m_pwLoop) throw std::runtime_error("portal: pw_thread_loop_new failed");

    pw_thread_loop_lock(m_pwLoop);
    m_pwContext = pw_context_new(pw_thread_loop_get_loop(m_pwLoop), nullptr, 0);
    if (!m_pwContext) {
        pw_thread_loop_unlock(m_pwLoop);
        throw std::runtime_error("portal: pw_context_new failed");
    }

    // Connect via the fd OpenPipeWireRemote handed us. fcntl-dup so PipeWire
    // gets its own descriptor and we keep a reference to close on teardown.
    m_pwCore = pw_context_connect_fd(m_pwContext,
                                     fcntl(m_pipewireFd, F_DUPFD_CLOEXEC, 5),
                                     nullptr, 0);
    if (!m_pwCore) {
        pw_thread_loop_unlock(m_pwLoop);
        throw std::runtime_error("portal: pw_context_connect_fd failed");
    }

    auto* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,     "Screen",
        nullptr);
    m_pwStream = pw_stream_new(m_pwCore, "shaderglass-capture", props);
    if (!m_pwStream) {
        pw_thread_loop_unlock(m_pwLoop);
        throw std::runtime_error("portal: pw_stream_new failed");
    }

    static const struct pw_stream_events kStreamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .destroy        = nullptr,
        .state_changed  = nullptr,
        .control_info   = nullptr,
        .io_changed     = nullptr,
        .param_changed  = &PortalCaptureSession::onParamChangedThunk,
        .add_buffer     = nullptr,
        .remove_buffer  = nullptr,
        .process        = &PortalCaptureSession::onProcessThunk,
        .drained        = nullptr,
        .command        = nullptr,
        .trigger_done   = nullptr,
    };
    static struct spa_hook listener_hook;  // local-static; one stream per session
    pw_stream_add_listener(m_pwStream, &listener_hook, &kStreamEvents, this);

    // Build SPA params: prefer BGRA, then RGBA. CPU buffers only for now.
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod* params[3];

    m_useDmaBuf = (m_vkCtx && DmaBufImport::isSupported(*m_vkCtx)
                   && std::getenv("SHADERGLASS_DISABLE_DMABUF") == nullptr);
    LOG_INFO("portal: DMA-BUF import %s", m_useDmaBuf ? "enabled" : "disabled");

    auto buildFormatPod = [&](spa_video_format fmt) -> const spa_pod* {
        spa_rectangle minR = SPA_RECTANGLE(1, 1);
        spa_rectangle maxR = SPA_RECTANGLE(8192, 8192);
        spa_rectangle defR = SPA_RECTANGLE(1920, 1080);
        spa_fraction  minF = SPA_FRACTION(0, 1);
        spa_fraction  maxF = SPA_FRACTION(240, 1);
        spa_fraction  defF = SPA_FRACTION(60, 1);
        return (const spa_pod*)spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(fmt),
            SPA_FORMAT_VIDEO_size,
                SPA_POD_CHOICE_RANGE_Rectangle(&defR, &minR, &maxR),
            SPA_FORMAT_VIDEO_framerate,
                SPA_POD_CHOICE_RANGE_Fraction(&defF, &minF, &maxF));
    };

    params[0] = buildFormatPod(SPA_VIDEO_FORMAT_BGRA);
    params[1] = buildFormatPod(SPA_VIDEO_FORMAT_RGBA);
    int paramCount = 2;
    if (m_useDmaBuf) {
        params[paramCount++] = (const spa_pod*)spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType,
                SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) |
                                         (1 << SPA_DATA_MemFd) |
                                         (1 << SPA_DATA_MemPtr)));
    }

    int rc = pw_stream_connect(m_pwStream,
        PW_DIRECTION_INPUT, m_pipewireNodeId,
        (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS),
        params, paramCount);
    pw_thread_loop_unlock(m_pwLoop);

    if (rc < 0) throw std::runtime_error(std::string("portal: pw_stream_connect failed: ")
                                         + spa_strerror(rc));

    if (pw_thread_loop_start(m_pwLoop) < 0)
        throw std::runtime_error("portal: pw_thread_loop_start failed");

    LOG_INFO("portal: pipewire stream connected (node=%u)", m_pipewireNodeId);
}

void PortalCaptureSession::teardownPipeWire() {
    if (m_pwLoop) {
        pw_thread_loop_stop(m_pwLoop);
    }
    {
        std::lock_guard<std::mutex> g(m_dmaCacheMutex);
        if (m_vkCtx) {
            for (auto& [_, perBuf] : m_dmaCache) {
                if (perBuf.valid) DmaBufImport::destroy(*m_vkCtx, perBuf.imported);
            }
        }
        m_dmaCache.clear();
    }
    if (m_pwStream) { pw_stream_destroy(m_pwStream); m_pwStream = nullptr; }
    if (m_pwCore)   { pw_core_disconnect(m_pwCore);  m_pwCore   = nullptr; }
    if (m_pwContext){ pw_context_destroy(m_pwContext); m_pwContext = nullptr; }
    if (m_pwLoop)   { pw_thread_loop_destroy(m_pwLoop); m_pwLoop  = nullptr; }
    pw_deinit();
}

void PortalCaptureSession::onParamChangedThunk(void* userdata, uint32_t id, const struct spa_pod* param) {
    static_cast<PortalCaptureSession*>(userdata)->onParamChanged(id, param);
}
void PortalCaptureSession::onProcessThunk(void* userdata) {
    static_cast<PortalCaptureSession*>(userdata)->onProcess();
}

void PortalCaptureSession::onParamChanged(uint32_t id, const struct spa_pod* param) {
    if (!param || id != SPA_PARAM_Format) return;

    uint32_t mediaType = 0, mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
    if (mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) return;

    if (spa_format_video_raw_parse(param, &g_negotiatedFormat) < 0) {
        LOG_WARN("portal: failed to parse negotiated video format");
        return;
    }
    g_haveFormat = true;
    LOG_INFO("portal: negotiated format=%d size=%dx%d framerate=%d/%d",
             g_negotiatedFormat.format,
             g_negotiatedFormat.size.width, g_negotiatedFormat.size.height,
             g_negotiatedFormat.framerate.num, g_negotiatedFormat.framerate.denom);

    if (g_negotiatedFormat.format != SPA_VIDEO_FORMAT_BGRA &&
        g_negotiatedFormat.format != SPA_VIDEO_FORMAT_RGBA) {
        LOG_ERROR("portal: unsupported negotiated format %d (want BGRA/RGBA)",
                  g_negotiatedFormat.format);
        // Cannot throw from callback context. The next on_process will be
        // a no-op and acquireFrame() will keep returning nullopt.
    }
}

void PortalCaptureSession::onProcess() {
    if (!g_haveFormat) return;

    pw_buffer* pb = pw_stream_dequeue_buffer(m_pwStream);
    if (!pb) return;

    spa_buffer* sb = pb->buffer;
    if (sb->n_datas == 0) {
        pw_stream_queue_buffer(m_pwStream, pb);
        return;
    }
    spa_data& d0 = sb->datas[0];
    if (d0.type == SPA_DATA_DmaBuf) {
        if (!m_useDmaBuf || !m_vkCtx) {
            pw_stream_queue_buffer(m_pwStream, pb);
            return;
        }
        PerBufferDmaBuf* perBuf = nullptr;
        {
            std::lock_guard<std::mutex> g(m_dmaCacheMutex);
            auto& slot = m_dmaCache[pb];
            if (!slot.valid) {
                try {
                    uint64_t modifier = g_negotiatedFormat.modifier;
                    slot.imported = DmaBufImport::importFd(*m_vkCtx,
                        static_cast<int>(d0.fd),
                        g_negotiatedFormat.size.width,
                        g_negotiatedFormat.size.height,
                        (g_negotiatedFormat.format == SPA_VIDEO_FORMAT_BGRA)
                            ? 0x34325241 : 0x34324241,
                        modifier,
                        d0.mapoffset,
                        d0.chunk ? d0.chunk->stride : g_negotiatedFormat.size.width * 4);
                    slot.valid = true;
                } catch (const std::exception& e) {
                    LOG_WARN("portal: DMA-BUF import failed (%s); disabling for session", e.what());
                    m_useDmaBuf = false;
                    pw_stream_queue_buffer(m_pwStream, pb);
                    return;
                }
            }
            perBuf = &slot;
        }
        uint64_t handle;
        {
            std::lock_guard<std::mutex> g(m_bufferMapMutex);
            handle = m_nextHandleId++;
            m_bufferMap[handle] = pb;
        }
        CapturedFrame frame;
        frame.kind            = CapturedFrame::Kind::DmaBuf;
        frame.width           = g_negotiatedFormat.size.width;
        frame.height          = g_negotiatedFormat.size.height;
        frame.fourcc          = (g_negotiatedFormat.format == SPA_VIDEO_FORMAT_BGRA)
                                ? 0x34325241 : 0x34324241;
        frame.fd              = static_cast<int>(d0.fd);
        frame.offset          = d0.mapoffset;
        frame.sessionHandle   = reinterpret_cast<void*>(uintptr_t(handle));
        frame.importedDmaBuf  = &perBuf->imported;
        if (m_onFrame) m_onFrame(frame);
        return;
    }
    if (d0.type != SPA_DATA_MemPtr && d0.type != SPA_DATA_MemFd) {
        pw_stream_queue_buffer(m_pwStream, pb);
        return;
    }
    if (!d0.data) {
        pw_stream_queue_buffer(m_pwStream, pb);
        return;
    }

    uint64_t handle;
    {
        std::lock_guard<std::mutex> g(m_bufferMapMutex);
        handle = m_nextHandleId++;
        m_bufferMap[handle] = pb;
    }

    CapturedFrame frame;
    frame.kind   = CapturedFrame::Kind::CpuBuffer;
    frame.width  = g_negotiatedFormat.size.width;
    frame.height = g_negotiatedFormat.size.height;
    frame.fourcc = (g_negotiatedFormat.format == SPA_VIDEO_FORMAT_BGRA)
                   ? 0x34325241 /* AR24 = ARGB8888 little-endian */
                   : 0x34324241 /* AB24 = ABGR8888 (RGBA in mem) */;
    frame.stride = d0.chunk ? d0.chunk->stride : (frame.width * 4);
    frame.data   = static_cast<const uint8_t*>(d0.data);
    frame.sessionHandle = reinterpret_cast<void*>(uintptr_t(handle));

    if (m_onFrame) m_onFrame(frame);
}
