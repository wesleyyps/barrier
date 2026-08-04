#include "WaylandInputCapture.h"
#include "barrier/IPrimaryScreen.h"
#include <libei.h>
#include <unistd.h>
#include <poll.h>
#include <iostream>
#include "base/Event.h"
#include "base/Log.h"
#include <dbus/dbus.h>

WaylandInputCapture::WaylandInputCapture(IEventQueue* events)
    : m_events(events), m_dbusConn(nullptr), m_ei(nullptr), m_zoneSet(0) {
}

WaylandInputCapture::~WaylandInputCapture() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    cleanup();
}

void WaylandInputCapture::cleanup() {
    if (m_ei) {
        ei_unref(m_ei);
        m_ei = nullptr;
    }
    if (m_dbusConn) {
        dbus_connection_unref(m_dbusConn);
        m_dbusConn = nullptr;
    }
}

bool WaylandInputCapture::init() {
    if (!connectToDBus()) return false;
    if (!createSession()) return false;
    if (!getZones()) return false;
    if (!setPointerBarriers()) return false;
    if (!enableCapture()) return false;
    
    int fd = connectToEIS();
    if (fd < 0) return false;
    
    return setupLibEI(fd);
}

bool WaylandInputCapture::connectToDBus() {
    DBusError err;
    dbus_error_init(&err);
    m_dbusConn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        LOG((CLOG_ERR "WaylandInputCapture: DBus Connection Error: %s", err.message ? err.message : "unknown"));
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
                        // Success, extract a{sv}
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

bool WaylandInputCapture::createSession() {
    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.InputCapture",
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
        LOG((CLOG_ERR "WaylandInputCapture: CreateSession failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }

    const char* requestPath = nullptr;
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &requestPath, DBUS_TYPE_INVALID);
    std::string reqPathStr = requestPath;
    dbus_message_unref(reply);

    m_sessionHandle = waitForResponse(m_dbusConn, reqPathStr);
    if (m_sessionHandle.empty()) {
        LOG((CLOG_ERR "WaylandInputCapture: CreateSession signal failed"));
        return false;
    }
    
    LOG((CLOG_INFO "WaylandInputCapture: Session created %s", m_sessionHandle.c_str()));
    return true;
}


bool WaylandInputCapture::waitForZonesResponse(DBusConnection* conn, const std::string& requestPath) {
    std::string matchRule = "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" + requestPath + "'";
    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(conn, matchRule.c_str(), &err);
    if (dbus_error_is_set(&err)) {
        LOG((CLOG_ERR "WaylandInputCapture: Failed to add match rule: %s", err.message));
        dbus_error_free(&err);
        return false;
    }

    bool success = false;
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
                        // Success, extract a{sv}
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
                                
                                if (std::string(key) == "zone_set") {
                                    dbus_message_iter_get_basic(&variant, &m_zoneSet);
                                    success = true;
                                } else if (std::string(key) == "zones") {
                                    // Parse a(uuii)
                                    DBusMessageIter arrayIter, structIter;
                                    dbus_message_iter_recurse(&variant, &arrayIter);
                                    m_zones.clear();
                                    while (dbus_message_iter_get_arg_type(&arrayIter) == DBUS_TYPE_STRUCT) {
                                        dbus_message_iter_recurse(&arrayIter, &structIter);
                                        Zone z;
                                        dbus_message_iter_get_basic(&structIter, &z.width);
                                        dbus_message_iter_next(&structIter);
                                        dbus_message_iter_get_basic(&structIter, &z.height);
                                        dbus_message_iter_next(&structIter);
                                        dbus_message_iter_get_basic(&structIter, &z.x);
                                        dbus_message_iter_next(&structIter);
                                        dbus_message_iter_get_basic(&structIter, &z.y);
                                        m_zones.push_back(z);
                                        dbus_message_iter_next(&arrayIter);
                                    }
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
    return success;
}

bool WaylandInputCapture::getZones() {
    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.InputCapture",
                                                    "GetZones");
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
        LOG((CLOG_ERR "WaylandInputCapture: GetZones failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }

    const char* requestPath = nullptr;
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &requestPath, DBUS_TYPE_INVALID);
    std::string reqPathStr = requestPath;
    dbus_message_unref(reply);

    if (!waitForZonesResponse(m_dbusConn, reqPathStr)) {
        LOG((CLOG_ERR "WaylandInputCapture: GetZones signal failed"));
        return false;
    }

    LOG((CLOG_INFO "WaylandInputCapture: getZones returned %d zones in zone_set %u", (int)m_zones.size(), m_zoneSet));
    return true;
}

bool WaylandInputCapture::waitForBarriersResponse(DBusConnection* conn, const std::string& requestPath) {
    std::string matchRule = "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" + requestPath + "'";
    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(conn, matchRule.c_str(), &err);
    
    bool success = false;
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
                        success = true; // Optionally parse failed_barriers
                    }
                }
            }
            dbus_message_unref(msg);
            break;
        }
        dbus_message_unref(msg);
    }
    dbus_bus_remove_match(conn, matchRule.c_str(), &err);
    return success;
}

bool WaylandInputCapture::setPointerBarriers() {
    if (m_zones.empty()) {
        LOG((CLOG_ERR "WaylandInputCapture: No zones available to set pointer barriers"));
        return false;
    }

    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.InputCapture",
                                                    "SetPointerBarriers");
    DBusMessageIter iter, optionsDict, barriersArray;
    dbus_message_iter_init_append(msg, &iter);
    
    // session_handle (o)
    const char* sessionStr = m_sessionHandle.c_str();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &sessionStr);
    
    // options (a{sv})
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &optionsDict);
    dbus_message_iter_close_container(&iter, &optionsDict);

    // barriers (aa{sv})
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "a{sv}", &barriersArray);
    
    // Add one barrier for now: right edge of the first zone
    Zone z = m_zones[0];
    uint32_t barrierId = 1;
    
    DBusMessageIter barrierDict;
    dbus_message_iter_open_container(&barriersArray, DBUS_TYPE_ARRAY, "{sv}", &barrierDict);
    
    // barrier_id (u)
    DBusMessageIter entryId, variantId;
    const char* keyId = "barrier_id";
    dbus_message_iter_open_container(&barrierDict, DBUS_TYPE_DICT_ENTRY, nullptr, &entryId);
    dbus_message_iter_append_basic(&entryId, DBUS_TYPE_STRING, &keyId);
    dbus_message_iter_open_container(&entryId, DBUS_TYPE_VARIANT, "u", &variantId);
    dbus_message_iter_append_basic(&variantId, DBUS_TYPE_UINT32, &barrierId);
    dbus_message_iter_close_container(&entryId, &variantId);
    dbus_message_iter_close_container(&barrierDict, &entryId);
    
    // position (iiii) -> x1, y1, x2, y2
    DBusMessageIter entryPos, variantPos, structPos;
    const char* keyPos = "position";
    dbus_message_iter_open_container(&barrierDict, DBUS_TYPE_DICT_ENTRY, nullptr, &entryPos);
    dbus_message_iter_append_basic(&entryPos, DBUS_TYPE_STRING, &keyPos);
    dbus_message_iter_open_container(&entryPos, DBUS_TYPE_VARIANT, "(iiii)", &variantPos);
    dbus_message_iter_open_container(&variantPos, DBUS_TYPE_STRUCT, nullptr, &structPos);
    
    int32_t x1 = z.x + z.width - 1; // right edge
    int32_t y1 = z.y;
    int32_t x2 = z.x + z.width - 1;
    int32_t y2 = z.y + z.height;
    
    dbus_message_iter_append_basic(&structPos, DBUS_TYPE_INT32, &x1);
    dbus_message_iter_append_basic(&structPos, DBUS_TYPE_INT32, &y1);
    dbus_message_iter_append_basic(&structPos, DBUS_TYPE_INT32, &x2);
    dbus_message_iter_append_basic(&structPos, DBUS_TYPE_INT32, &y2);
    
    dbus_message_iter_close_container(&variantPos, &structPos);
    dbus_message_iter_close_container(&entryPos, &variantPos);
    dbus_message_iter_close_container(&barrierDict, &entryPos);
    
    dbus_message_iter_close_container(&barriersArray, &barrierDict);
    dbus_message_iter_close_container(&iter, &barriersArray);

    // zone_set (u)
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &m_zoneSet);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, -1, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        LOG((CLOG_ERR "WaylandInputCapture: SetPointerBarriers failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }

    const char* requestPath = nullptr;
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &requestPath, DBUS_TYPE_INVALID);
    std::string reqPathStr = requestPath;
    dbus_message_unref(reply);

    if (!waitForBarriersResponse(m_dbusConn, reqPathStr)) {
        LOG((CLOG_ERR "WaylandInputCapture: SetPointerBarriers signal failed"));
        return false;
    }

    LOG((CLOG_INFO "WaylandInputCapture: SetPointerBarriers success"));
    return true;
}

bool WaylandInputCapture::enableCapture() {
    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.InputCapture",
                                                    "Enable");
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
        LOG((CLOG_ERR "WaylandInputCapture: Enable failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }
    
    if (reply) dbus_message_unref(reply);
    
    LOG((CLOG_INFO "WaylandInputCapture: Capture Enabled"));
    return true;
}
int WaylandInputCapture::connectToEIS() {
    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.InputCapture",
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
        LOG((CLOG_ERR "WaylandInputCapture: ConnectToEIS failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return -1;
    }

    int fd = -1;
    dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
    dbus_message_unref(reply);
    
    if (fd < 0) {
        LOG((CLOG_ERR "WaylandInputCapture: Invalid FD from ConnectToEIS"));
    } else {
        LOG((CLOG_INFO "WaylandInputCapture: ConnectToEIS got FD %d", fd));
    }
    return fd;
}

bool WaylandInputCapture::setupLibEI(int fd) {
    m_ei = ei_new_receiver(nullptr); // InputCapture uses passive/receiver context
    if (!m_ei) {
        LOG((CLOG_ERR "WaylandInputCapture: Failed to create libei receiver"));
        close(fd);
        return false;
    }
    
    if (ei_setup_backend_fd(m_ei, fd) < 0) {
        LOG((CLOG_ERR "WaylandInputCapture: Failed to setup libei backend fd"));
        ei_unref(m_ei);
        m_ei = nullptr;
        close(fd);
        return false;
    }
    
    LOG((CLOG_INFO "WaylandInputCapture: libei successfully set up as receiver"));
    
    m_running = true;
    m_thread = std::thread(&WaylandInputCapture::pollEvents, this);
    
    return true;
}

void WaylandInputCapture::pollEvents() {
    struct pollfd fds[1];
    fds[0].fd = ei_get_fd(m_ei);
    fds[0].events = POLLIN;

    while (m_running) {
        ei_dispatch(m_ei);
        int ret = poll(fds, 1, 100);
        if (ret > 0 && (fds[0].revents & POLLIN)) {
            while (struct ei_event* ev = ei_get_event(m_ei)) {
                enum ei_event_type type = ei_event_get_type(ev);
                
                // Event already has a refcount of 1 from ei_get_event.
                // We pass ownership to the EventQueue via kSystem.
                // It will be unref'd in WaylandScreen::handleSystemEvent.
                Event sysEvent(Event::kSystem, nullptr, ev);
                m_events->addEvent(sysEvent);
            }
        }
    }
}

void WaylandInputCapture::releaseCapture() {
    if (!m_dbusConn || m_sessionHandle.empty()) return;
    
    DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                    "/org/freedesktop/portal/desktop",
                                                    "org.freedesktop.portal.InputCapture",
                                                    "Release");
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    const char* sessionStr = m_sessionHandle.c_str();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &sessionStr);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, -1, &err);
    if (reply) dbus_message_unref(reply);
    dbus_message_unref(msg);
}
