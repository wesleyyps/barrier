#include "WaylandPortal.h"
#include "base/Log.h"
#include "barrier/mouse_types.h"
#include <dbus/dbus.h>
#include <libei.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <poll.h>
#include <iostream>
#include <random>

WaylandPortal::WaylandPortal(IEventQueue* events)
    : m_events(events),
      m_dbusConn(nullptr),
      m_running(false),
      m_ei(nullptr),
      m_eiPointerAbs(nullptr),
      m_eiPointerRel(nullptr),
      m_eiButton(nullptr),
      m_eiScroll(nullptr),
      m_eiKeyboard(nullptr),
      m_sequence(0),
      m_x(0),
      m_y(0),
      m_width(1920),
      m_height(1080),
      m_lastX(0),
      m_lastY(0) {
}

WaylandPortal::~WaylandPortal() {
    cleanup();
}

void WaylandPortal::cleanup() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& p : m_deviceStates) {
        if (p.second.emulating) {
            ei_device_stop_emulating(p.first);
        }
    }
    m_deviceStates.clear();

    if (m_eiPointerAbs) {
        ei_device_unref(m_eiPointerAbs);
        m_eiPointerAbs = nullptr;
    }
    if (m_eiPointerRel) {
        ei_device_unref(m_eiPointerRel);
        m_eiPointerRel = nullptr;
    }
    if (m_eiButton) {
        ei_device_unref(m_eiButton);
        m_eiButton = nullptr;
    }
    if (m_eiScroll) {
        ei_device_unref(m_eiScroll);
        m_eiScroll = nullptr;
    }
    if (m_eiKeyboard) {
        ei_device_unref(m_eiKeyboard);
        m_eiKeyboard = nullptr;
    }

    if (m_ei) {
        ei_unref(m_ei);
        m_ei = nullptr;
    }

    if (m_dbusConn && !m_sessionHandle.empty() &&
        m_sessionHandle.rfind("/org/gnome/Mutter/RemoteDesktop", 0) == 0) {
        DBusMessage* msg = dbus_message_new_method_call("org.gnome.Mutter.RemoteDesktop",
                                                        m_sessionHandle.c_str(),
                                                        "org.gnome.Mutter.RemoteDesktop.Session",
                                                        "Stop");
        if (msg) {
            DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 500, nullptr);
            if (reply) dbus_message_unref(reply);
            dbus_message_unref(msg);
        }
    }

    if (m_dbusConn) {
        dbus_connection_unref(m_dbusConn);
        m_dbusConn = nullptr;
    }
}

bool WaylandPortal::init() {
    if (!connectToDBus()) return false;

    int fd = connectToMutterEIS();
    if (fd < 0) {
        LOG((CLOG_INFO "WaylandPortal: Mutter RemoteDesktop not available, falling back to XDG Desktop Portal"));
        if (!createSession()) return false;
        if (!selectDevices()) return false;
        if (!startSession()) return false;
        fd = connectToEIS();
    }

    if (fd < 0) {
        LOG((CLOG_ERR "WaylandPortal: Failed to obtain EIS file descriptor"));
        return false;
    }

    return setupLibEI(fd);
}

bool WaylandPortal::connectToDBus() {
    DBusError err;
    dbus_error_init(&err);
    m_dbusConn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        LOG((CLOG_ERR "WaylandPortal: DBus Connection Error: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
    }
    return m_dbusConn != nullptr;
}

int WaylandPortal::connectToMutterEIS() {
    DBusError err;
    dbus_error_init(&err);

    // Call CreateSession on org.gnome.Mutter.RemoteDesktop
    DBusMessage* msg = dbus_message_new_method_call("org.gnome.Mutter.RemoteDesktop",
                                                    "/org/gnome/Mutter/RemoteDesktop",
                                                    "org.gnome.Mutter.RemoteDesktop",
                                                    "CreateSession");
    if (!msg) return -1;

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 2000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err) || !reply) {
        LOG((CLOG_DEBUG1 "WaylandPortal: Mutter CreateSession not available: %s",
            err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return -1;
    }

    const char* sessionPath = nullptr;
    if (!dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &sessionPath, DBUS_TYPE_INVALID)) {
        LOG((CLOG_ERR "WaylandPortal: Failed to parse Mutter session path: %s",
            err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        dbus_message_unref(reply);
        return -1;
    }

    m_sessionHandle = sessionPath;
    dbus_message_unref(reply);
    LOG((CLOG_INFO "WaylandPortal: Mutter RemoteDesktop session created: %s", m_sessionHandle.c_str()));

    // Call Start on the session
    msg = dbus_message_new_method_call("org.gnome.Mutter.RemoteDesktop",
                                       m_sessionHandle.c_str(),
                                       "org.gnome.Mutter.RemoteDesktop.Session",
                                       "Start");
    reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 2000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err) || !reply) {
        LOG((CLOG_ERR "WaylandPortal: Mutter Session Start failed: %s",
            err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return -1;
    }
    dbus_message_unref(reply);
    LOG((CLOG_INFO "WaylandPortal: Mutter RemoteDesktop session started directly without dialog"));

    // Call ConnectToEIS on the session
    msg = dbus_message_new_method_call("org.gnome.Mutter.RemoteDesktop",
                                       m_sessionHandle.c_str(),
                                       "org.gnome.Mutter.RemoteDesktop.Session",
                                       "ConnectToEIS");
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&iter, &dict);

    reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 2000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err) || !reply) {
        LOG((CLOG_ERR "WaylandPortal: Mutter ConnectToEIS failed: %s",
            err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return -1;
    }

    int fd = -1;
    dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
    dbus_message_unref(reply);

    if (fd < 0) {
        LOG((CLOG_ERR "WaylandPortal: Invalid FD from Mutter ConnectToEIS"));
        return -1;
    }

    LOG((CLOG_INFO "WaylandPortal: Connected to Mutter EIS (FD %d)", fd));
    return fd;
}

static std::string waitForResponse(DBusConnection* conn, const std::string& requestPath) {
    std::string matchRule = "type='signal',interface='org.freedesktop.portal.Request',path='" + requestPath + "'";
    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(conn, matchRule.c_str(), &err);
    dbus_connection_flush(conn);

    std::string sessionHandle = "";
    int attempts = 0;
    while (attempts < 50) {
        attempts++;
        dbus_connection_read_write(conn, 100);
        DBusMessage* msg = dbus_connection_pop_message(conn);
        if (msg == nullptr) continue;

        if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response") &&
            dbus_message_has_path(msg, requestPath.c_str())) {
            
            DBusMessageIter iter;
            if (dbus_message_iter_init(msg, &iter)) {
                if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
                    uint32_t responseCode;
                    dbus_message_iter_get_basic(&iter, &responseCode);
                    if (responseCode == 0) {
                        dbus_message_iter_next(&iter);
                        if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
                            DBusMessageIter dict, entry, variant;
                            dbus_message_iter_recurse(&iter, &dict);
                            while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
                                dbus_message_iter_recurse(&dict, &entry);
                                const char* key;
                                dbus_message_iter_get_basic(&entry, &key);
                                dbus_message_iter_next(&entry);
                                dbus_message_iter_recurse(&entry, &variant);
                                if (std::string(key) == "session_handle") {
                                    const char* val;
                                    dbus_message_iter_get_basic(&variant, &val);
                                    sessionHandle = val;
                                }
                                dbus_message_iter_next(&dict);
                            }
                        }
                    }
                }
            }
            dbus_message_unref(msg);
            break;
        }
        dbus_message_unref(msg);
    }
    dbus_bus_remove_match(conn, matchRule.c_str(), &err);
    return sessionHandle;
}

bool WaylandPortal::createSession() {
    std::string token_val = "barrier_sess_" + std::to_string(rand() % 100000);
    std::string handle_val = "barrier_cr_" + std::to_string(rand() % 100000);

    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.RemoteDesktop",
                                                    "CreateSession");
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char* token_key = "session_handle_token";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &token_key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    const char* token_c = token_val.c_str();
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &token_c);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);

    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char* handle_key = "handle_token";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &handle_key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    const char* handle_c = handle_val.c_str();
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &handle_c);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);

    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        LOG((CLOG_ERR "WaylandPortal: CreateSession failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }

    const char* requestPath = nullptr;
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &requestPath, DBUS_TYPE_INVALID);
    std::string reqPathStr = requestPath;
    dbus_message_unref(reply);

    m_sessionHandle = waitForResponse(m_dbusConn, reqPathStr);
    if (m_sessionHandle.empty()) {
        LOG((CLOG_ERR "WaylandPortal: CreateSession signal failed"));
        return false;
    }
    
    LOG((CLOG_INFO "WaylandPortal: Session created %s", m_sessionHandle.c_str()));
    return true;
}

static void appendTypesOption(DBusMessageIter* dict) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char* key = "types";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
    uint32_t types = 7; // KEYBOARD(1) | POINTER(2) | TOUCHSCREEN(4)
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &types);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

bool WaylandPortal::selectDevices() {
    std::string handle_val = "barrier_sel_" + std::to_string(rand() % 100000);

    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.RemoteDesktop",
                                                    "SelectDevices");
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    const char* sessionStr = m_sessionHandle.c_str();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &sessionStr);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    appendTypesOption(&dict);

    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char* handle_key = "handle_token";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &handle_key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    const char* handle_c = handle_val.c_str();
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &handle_c);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);

    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        LOG((CLOG_ERR "WaylandPortal: SelectDevices failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }

    const char* requestPath = nullptr;
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &requestPath, DBUS_TYPE_INVALID);
    std::string reqPathStr = requestPath;
    dbus_message_unref(reply);

    waitForResponse(m_dbusConn, reqPathStr);
    LOG((CLOG_INFO "WaylandPortal: Devices selected"));
    return true;
}

bool WaylandPortal::startSession() {
    std::string handle_val = "barrier_st_" + std::to_string(rand() % 100000);

    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.RemoteDesktop",
                                                    "Start");
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    const char* sessionStr = m_sessionHandle.c_str();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &sessionStr);
    const char* parentWindow = "";
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parentWindow);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    const char* handle_key = "handle_token";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &handle_key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    const char* handle_c = handle_val.c_str();
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &handle_c);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);

    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        LOG((CLOG_ERR "WaylandPortal: Start failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }

    const char* requestPath = nullptr;
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &requestPath, DBUS_TYPE_INVALID);
    std::string reqPathStr = requestPath;
    dbus_message_unref(reply);

    waitForResponse(m_dbusConn, reqPathStr);
    LOG((CLOG_INFO "WaylandPortal: Session started"));
    return true;
}

int WaylandPortal::connectToEIS() {
    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.RemoteDesktop",
                                                    "ConnectToEIS");
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    const char* sessionStr = m_sessionHandle.c_str();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &sessionStr);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        LOG((CLOG_ERR "WaylandPortal: ConnectToEIS failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return -1;
    }

    int fd = -1;
    dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
    dbus_message_unref(reply);
    
    if (fd < 0) {
        LOG((CLOG_ERR "WaylandPortal: Invalid FD from ConnectToEIS"));
    } else {
        LOG((CLOG_INFO "WaylandPortal: ConnectToEIS got FD %d", fd));
    }
    return fd;
}

bool WaylandPortal::setupLibEI(int fd) {
    m_ei = ei_new_sender(nullptr);
    if (!m_ei) {
        LOG((CLOG_ERR "WaylandPortal: Failed to create libei sender"));
        close(fd);
        return false;
    }
    
    if (ei_setup_backend_fd(m_ei, fd) < 0) {
        LOG((CLOG_ERR "WaylandPortal: Failed to setup libei backend fd"));
        ei_unref(m_ei);
        m_ei = nullptr;
        close(fd);
        return false;
    }
    
    m_running = true;
    m_thread = std::thread(&WaylandPortal::pollEvents, this);

    // Wait for initial handshake and devices to be added/resumed (up to 1.5s)
    for (int i = 0; i < 75; ++i) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if ((m_eiPointerAbs || m_eiPointerRel) && m_eiKeyboard) {
                bool ready = true;
                for (const auto& p : m_deviceStates) {
                    if (!p.second.resumed) {
                        ready = false;
                        break;
                    }
                }
                if (ready) {
                    LOG((CLOG_INFO "WaylandPortal: Initial devices ready and resumed"));
                    break;
                }
            }
        }
        usleep(20000);
    }
    
    LOG((CLOG_INFO "WaylandPortal: libei successfully set up"));
    return true;
}

void WaylandPortal::pollEvents() {
    int ei_fd = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_ei) return;
        ei_fd = ei_get_fd(m_ei);
    }
    if (ei_fd < 0) {
        LOG((CLOG_ERR "WaylandPortal: invalid ei_fd: %d", ei_fd));
        return;
    }

    struct pollfd fds[1];
    fds[0].fd = ei_fd;
    fds[0].events = POLLIN;

    while (m_running) {
        int ret = poll(fds, 1, 50);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_ei) break;

        ei_dispatch(m_ei);
        while (struct ei_event* ev = ei_get_event(m_ei)) {
            handleEIEvent(ev);
            ei_event_unref(ev);
        }
    }
}

void WaylandPortal::handleEIEvent(struct ei_event* ev) {
    enum ei_event_type type = ei_event_get_type(ev);
    const char* type_str = ei_event_type_to_string(type);
    LOG((CLOG_DEBUG1 "WaylandPortal: ei event %s (%d)", type_str ? type_str : "unknown", type));

    switch (type) {
    case EI_EVENT_CONNECT:
        LOG((CLOG_INFO "WaylandPortal: libei connected to server"));
        break;

    case EI_EVENT_DISCONNECT:
        LOG((CLOG_WARN "WaylandPortal: libei disconnected from server"));
        break;

    case EI_EVENT_SEAT_ADDED: {
        struct ei_seat* seat = ei_event_get_seat(ev);
        LOG((CLOG_INFO "WaylandPortal: Seat added: %s, binding capabilities",
            ei_seat_get_name(seat) ? ei_seat_get_name(seat) : "unnamed"));
        ei_seat_bind_capabilities(seat,
            EI_DEVICE_CAP_POINTER,
            EI_DEVICE_CAP_POINTER_ABSOLUTE,
            EI_DEVICE_CAP_KEYBOARD,
            EI_DEVICE_CAP_BUTTON,
            EI_DEVICE_CAP_SCROLL,
            NULL);
        break;
    }

    case EI_EVENT_SEAT_REMOVED:
        LOG((CLOG_INFO "WaylandPortal: Seat removed"));
        break;

    case EI_EVENT_DEVICE_ADDED: {
        struct ei_device* dev = ei_event_get_device(ev);
        const char* name = ei_device_get_name(dev);
        LOG((CLOG_INFO "WaylandPortal: Device added: %s (type %d)", name ? name : "unnamed", ei_device_get_type(dev)));

        DeviceState state;
        state.device = dev;
        state.resumed = false;
        state.emulating = false;
        m_deviceStates[dev] = state;

        if (ei_device_has_capability(dev, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
            LOG((CLOG_INFO "WaylandPortal: -> Device has POINTER_ABSOLUTE"));
            if (!m_eiPointerAbs) {
                m_eiPointerAbs = ei_device_ref(dev);
            }
            for (size_t i = 0; ; ++i) {
                struct ei_region* r = ei_device_get_region(dev, i);
                if (!r) break;
                uint32_t rx = ei_region_get_x(r);
                uint32_t ry = ei_region_get_y(r);
                uint32_t rw = ei_region_get_width(r);
                uint32_t rh = ei_region_get_height(r);
                LOG((CLOG_INFO "WaylandPortal: -> Region %zu: %ux%u at (%d,%d)", i, rw, rh, rx, ry));
                if (i == 0 || rw > m_width) {
                    m_x = rx;
                    m_y = ry;
                    m_width = rw;
                    m_height = rh;
                }
            }
        }
        if (ei_device_has_capability(dev, EI_DEVICE_CAP_POINTER)) {
            LOG((CLOG_INFO "WaylandPortal: -> Device has POINTER (relative)"));
            if (!m_eiPointerRel) {
                m_eiPointerRel = ei_device_ref(dev);
            }
        }
        if (ei_device_has_capability(dev, EI_DEVICE_CAP_BUTTON)) {
            LOG((CLOG_INFO "WaylandPortal: -> Device has BUTTON"));
            if (!m_eiButton) {
                m_eiButton = ei_device_ref(dev);
            }
        }
        if (ei_device_has_capability(dev, EI_DEVICE_CAP_SCROLL)) {
            LOG((CLOG_INFO "WaylandPortal: -> Device has SCROLL"));
            if (!m_eiScroll) {
                m_eiScroll = ei_device_ref(dev);
            }
        }
        if (ei_device_has_capability(dev, EI_DEVICE_CAP_KEYBOARD)) {
            LOG((CLOG_INFO "WaylandPortal: -> Device has KEYBOARD"));
            if (!m_eiKeyboard) {
                m_eiKeyboard = ei_device_ref(dev);
            }
        }
        break;
    }

    case EI_EVENT_DEVICE_REMOVED: {
        struct ei_device* dev = ei_event_get_device(ev);
        LOG((CLOG_INFO "WaylandPortal: Device removed: %s",
            ei_device_get_name(dev) ? ei_device_get_name(dev) : "unnamed"));
        if (m_eiPointerAbs == dev) {
            ei_device_unref(m_eiPointerAbs);
            m_eiPointerAbs = nullptr;
        }
        if (m_eiPointerRel == dev) {
            ei_device_unref(m_eiPointerRel);
            m_eiPointerRel = nullptr;
        }
        if (m_eiButton == dev) {
            ei_device_unref(m_eiButton);
            m_eiButton = nullptr;
        }
        if (m_eiScroll == dev) {
            ei_device_unref(m_eiScroll);
            m_eiScroll = nullptr;
        }
        if (m_eiKeyboard == dev) {
            ei_device_unref(m_eiKeyboard);
            m_eiKeyboard = nullptr;
        }
        m_deviceStates.erase(dev);
        break;
    }

    case EI_EVENT_DEVICE_RESUMED: {
        struct ei_device* dev = ei_event_get_device(ev);
        LOG((CLOG_INFO "WaylandPortal: Device resumed: %s",
            ei_device_get_name(dev) ? ei_device_get_name(dev) : "unnamed"));
        m_deviceStates[dev].resumed = true;
        break;
    }

    case EI_EVENT_DEVICE_PAUSED: {
        struct ei_device* dev = ei_event_get_device(ev);
        LOG((CLOG_INFO "WaylandPortal: Device paused: %s",
            ei_device_get_name(dev) ? ei_device_get_name(dev) : "unnamed"));
        m_deviceStates[dev].resumed = false;
        m_deviceStates[dev].emulating = false;
        break;
    }

    default:
        break;
    }
}

void WaylandPortal::ensureEmulating(struct ei_device* dev) {
    if (!dev) return;
    auto it = m_deviceStates.find(dev);
    if (it != m_deviceStates.end()) {
        if (it->second.resumed && !it->second.emulating) {
            ei_device_start_emulating(dev, ++m_sequence);
            it->second.emulating = true;
        }
    }
}

void WaylandPortal::fakeMouseMove(int x, int y) {
    std::lock_guard<std::mutex> lock(m_mutex);
    struct ei_device* dev = m_eiPointerAbs ? m_eiPointerAbs : m_eiPointerRel;
    if (!dev) return;

    ensureEmulating(dev);

    if (m_eiPointerAbs && dev == m_eiPointerAbs) {
        if (m_width > 0 && m_height > 0) {
            if (x < (int)m_x) x = m_x;
            if (x >= (int)(m_x + m_width)) x = m_x + m_width - 1;
            if (y < (int)m_y) y = m_y;
            if (y >= (int)(m_y + m_height)) y = m_y + m_height - 1;
        }
        ei_device_pointer_motion_absolute(dev, x, y);
    } else {
        int dx = x - m_lastX;
        int dy = y - m_lastY;
        ei_device_pointer_motion(dev, dx, dy);
    }
    ei_device_frame(dev, ei_now(m_ei));
    ei_dispatch(m_ei);
    m_lastX = x;
    m_lastY = y;
}

void WaylandPortal::fakeMouseRelativeMove(int dx, int dy) {
    std::lock_guard<std::mutex> lock(m_mutex);
    struct ei_device* dev = m_eiPointerRel ? m_eiPointerRel : m_eiPointerAbs;
    if (!dev) return;

    ensureEmulating(dev);

    if (m_eiPointerRel && dev == m_eiPointerRel) {
        ei_device_pointer_motion(dev, dx, dy);
    } else {
        m_lastX += dx;
        m_lastY += dy;
        if (m_width > 0 && m_height > 0) {
            if (m_lastX < (int)m_x) m_lastX = m_x;
            if (m_lastX >= (int)(m_x + m_width)) m_lastX = m_x + m_width - 1;
            if (m_lastY < (int)m_y) m_lastY = m_y;
            if (m_lastY >= (int)(m_y + m_height)) m_lastY = m_y + m_height - 1;
        }
        ei_device_pointer_motion_absolute(dev, m_lastX, m_lastY);
    }
    ei_device_frame(dev, ei_now(m_ei));
    ei_dispatch(m_ei);
}

void WaylandPortal::fakeMouseButton(int button, bool press) {
    std::lock_guard<std::mutex> lock(m_mutex);
    struct ei_device* dev = m_eiButton;
    if (!dev) dev = m_eiPointerAbs ? m_eiPointerAbs : m_eiPointerRel;
    if (!dev) return;

    ensureEmulating(dev);

    uint32_t linuxBtn = BTN_LEFT;
    switch (button) {
    case kButtonLeft:   linuxBtn = BTN_LEFT; break;
    case kButtonMiddle: linuxBtn = BTN_MIDDLE; break;
    case kButtonRight:  linuxBtn = BTN_RIGHT; break;
    case kButtonExtra0: linuxBtn = BTN_SIDE; break;
    case kButtonExtra1: linuxBtn = BTN_EXTRA; break;
    default:
        linuxBtn = BTN_LEFT + button - 1;
        break;
    }

    ei_device_button_button(dev, linuxBtn, press);
    ei_device_frame(dev, ei_now(m_ei));
    ei_dispatch(m_ei);
}

void WaylandPortal::fakeMouseWheel(int xDelta, int yDelta) {
    std::lock_guard<std::mutex> lock(m_mutex);
    struct ei_device* dev = m_eiScroll;
    if (!dev) dev = m_eiPointerAbs ? m_eiPointerAbs : m_eiPointerRel;
    if (!dev) return;

    ensureEmulating(dev);

    int32_t discreteX = xDelta;
    int32_t discreteY = -yDelta;

    ei_device_scroll_discrete(dev, discreteX, discreteY);
    ei_device_frame(dev, ei_now(m_ei));
    ei_dispatch(m_ei);
}

void WaylandPortal::fakeKey(uint32_t keycode, bool press) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_eiKeyboard) return;

    ensureEmulating(m_eiKeyboard);

    ei_device_keyboard_key(m_eiKeyboard, keycode, press);
    ei_device_frame(m_eiKeyboard, ei_now(m_ei));
    ei_dispatch(m_ei);
}

void WaylandPortal::enter() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& p : m_deviceStates) {
        if (p.second.resumed && !p.second.emulating) {
            ei_device_start_emulating(p.first, ++m_sequence);
            p.second.emulating = true;
        }
    }
}

void WaylandPortal::leave() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& p : m_deviceStates) {
        if (p.second.emulating) {
            ei_device_stop_emulating(p.first);
            p.second.emulating = false;
        }
    }
}

void WaylandPortal::getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    x = m_x;
    y = m_y;
    width = m_width > 0 ? m_width : 1920;
    height = m_height > 0 ? m_height : 1080;
}

void WaylandPortal::getCursorPos(SInt32& x, SInt32& y) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    x = m_lastX;
    y = m_lastY;
}
