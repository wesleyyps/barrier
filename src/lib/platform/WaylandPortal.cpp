#include "WaylandPortal.h"
#include "base/Log.h"
#include <dbus/dbus.h>
#include <libei.h>
#include <unistd.h>
#include <iostream>

WaylandPortal::WaylandPortal(IEventQueue* events)
    : m_events(events), m_dbusConn(nullptr), m_ei(nullptr), m_eiPointer(nullptr), m_eiKeyboard(nullptr) {
}

WaylandPortal::~WaylandPortal() {
    cleanup();
}

void WaylandPortal::cleanup() {
    if (m_ei) {
        ei_unref(m_ei);
        m_ei = nullptr;
    }
    if (m_dbusConn) {
        dbus_connection_unref(m_dbusConn);
        m_dbusConn = nullptr;
    }
}

bool WaylandPortal::init() {
    if (!connectToDBus()) return false;
    if (!createSession()) return false;
    if (!selectDevices()) return false;
    if (!startSession()) return false;
    
    int fd = connectToEIS();
    if (fd < 0) return false;
    
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

static std::string waitForResponse(DBusConnection* conn, const std::string& requestPath) {
    std::string matchRule = "type='signal',interface='org.freedesktop.portal.Request',path='" + requestPath + "'";
    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(conn, matchRule.c_str(), &err);
    dbus_connection_flush(conn);

    std::string sessionHandle = "";
    while (true) {
        dbus_connection_read_write(conn, 1000);
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
                        // Success, extract a{sv} if needed
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
    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.RemoteDesktop",
                                                    "CreateSession");
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
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
    
    // Pump events to get the devices
    // This requires an event loop in reality, but for PoC we wait a bit
    ei_dispatch(m_ei);
    
    // In a full implementation, we'd listen to EI_EVENT_DEVICE_ADDED
    // We will leave the rest of the libei logic for the actual WaylandScreen implementation.
    
    LOG((CLOG_INFO "WaylandPortal: libei successfully set up"));
    return true;
}

void WaylandPortal::fakeMouseMove(int x, int y) {
    if (m_eiPointer) {
        // ei_device_frame(m_eiPointer, ei_now(m_ei));
        // ei_device_pointer_motion_absolute(m_eiPointer, x, y);
    }
}

void WaylandPortal::fakeMouseRelativeMove(int dx, int dy) {
    if (m_eiPointer) {
        // ei_device_pointer_motion(m_eiPointer, dx, dy);
    }
}

void WaylandPortal::fakeMouseButton(int button, bool press) {
    if (m_eiPointer) {
        // ei_device_pointer_button(m_eiPointer, button, press);
    }
}

void WaylandPortal::fakeMouseWheel(int xDelta, int yDelta) {
    if (m_eiPointer) {
        // ei_device_pointer_scroll(m_eiPointer, xDelta, yDelta);
    }
}
