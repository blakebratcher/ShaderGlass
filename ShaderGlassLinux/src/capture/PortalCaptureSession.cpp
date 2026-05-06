#include "PortalCaptureSession.h"
#include "../util/Logging.h"

#include <dbus/dbus.h>
#include <pipewire/pipewire.h>

#include <atomic>
#include <chrono>
#include <cstdint>
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

} // namespace

PortalCaptureSession::PortalCaptureSession() {
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

void PortalCaptureSession::releaseBuffer(void* /*sessionHandle*/) {
    // Task 8.
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
    throw std::runtime_error("PortalCaptureSession::initPipeWire — not yet implemented (Task 8)");
}

void PortalCaptureSession::teardownPipeWire() {
    // Stub for Task 8.
}

void PortalCaptureSession::onProcessThunk(void* /*userdata*/) {}
void PortalCaptureSession::onParamChangedThunk(void* /*userdata*/, uint32_t /*id*/, const struct spa_pod* /*param*/) {}
void PortalCaptureSession::onProcess() {}
void PortalCaptureSession::onParamChanged(uint32_t /*id*/, const struct spa_pod* /*param*/) {}
