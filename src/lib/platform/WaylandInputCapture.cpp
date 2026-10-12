#include "WaylandInputCapture.h"
#include "platform/WaylandScreen.h"
#include "barrier/IPrimaryScreen.h"
#include "barrier/IKeyState.h"
#include "base/Event.h"
#include "base/Log.h"

#include <libei.h>
#include <unistd.h>
#include <poll.h>
#include <dbus/dbus.h>
#include <linux/input-event-codes.h>
#include <cctype>

WaylandInputCapture::WaylandInputCapture(IEventQueue* events, WaylandScreen* screen)
    : m_events(events),
      m_screen(screen),
      m_ei(nullptr),
      m_running(false),
      m_dbusConn(nullptr),
      m_zoneSerial(0),
      m_isCaptured(false),
      m_currentActivationId(0),
      m_activeSides(0),
      m_activeModifiers(0) {
}

WaylandInputCapture::~WaylandInputCapture() {
    cleanup();
}

void WaylandInputCapture::cleanup() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_ei) {
        ei_unref(m_ei);
        m_ei = nullptr;
    }
    if (m_dbusConn) {
        std::lock_guard<std::mutex> lock(m_dbusMutex);
        if (!m_sessionHandle.empty()) {
            DBusMessage* msg = dbus_message_new_method_call(
                "org.gnome.Mutter.InputCapture",
                m_sessionHandle.c_str(),
                "org.gnome.Mutter.InputCapture.Session",
                "Close");
            if (msg) {
                DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 500, nullptr);
                if (reply) dbus_message_unref(reply);
                dbus_message_unref(msg);
            }
        }
        dbus_connection_unref(m_dbusConn);
        m_dbusConn = nullptr;
    }
}

bool WaylandInputCapture::init() {
    if (!connectToDBus()) return false;
    if (!createSession()) return false;
    if (!getZones()) return false;

    int fd = connectToEIS();
    if (fd < 0) return false;

    return setupLibEI(fd);
}

bool WaylandInputCapture::connectToDBus() {
    dbus_threads_init_default();
    DBusError err;
    dbus_error_init(&err);
    m_dbusConn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        LOG((CLOG_ERR "WaylandInputCapture: DBus Connection Error: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }
    return m_dbusConn != nullptr;
}

bool WaylandInputCapture::createSession() {
    std::lock_guard<std::mutex> lock(m_dbusMutex);

    DBusMessage* msg = dbus_message_new_method_call(
        "org.gnome.Mutter.InputCapture",
        "/org/gnome/Mutter/InputCapture",
        "org.gnome.Mutter.InputCapture",
        "CreateSession");
    if (!msg) {
        LOG((CLOG_ERR "WaylandInputCapture: Failed to allocate CreateSession message"));
        return false;
    }

    uint32_t caps = 3; // 1 (Keyboard) | 2 (Pointer)
    dbus_message_append_args(msg, DBUS_TYPE_UINT32, &caps, DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        LOG((CLOG_ERR "WaylandInputCapture: CreateSession failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }

    const char* sessionPath = nullptr;
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &sessionPath, DBUS_TYPE_INVALID);
    if (sessionPath) {
        m_sessionHandle = sessionPath;
    }
    dbus_message_unref(reply);

    if (m_sessionHandle.empty()) {
        LOG((CLOG_ERR "WaylandInputCapture: CreateSession returned empty session path"));
        return false;
    }

    LOG((CLOG_INFO "WaylandInputCapture: Mutter session created: %s", m_sessionHandle.c_str()));

    // Subscribe to session signals
    std::string matchRule = "type='signal',sender='org.gnome.Mutter.InputCapture',interface='org.gnome.Mutter.InputCapture.Session',path='" + m_sessionHandle + "'";
    dbus_bus_add_match(m_dbusConn, matchRule.c_str(), &err);
    dbus_connection_flush(m_dbusConn);
    if (dbus_error_is_set(&err)) {
        LOG((CLOG_WARN "WaylandInputCapture: Failed to add signal match: %s", err.message));
        dbus_error_free(&err);
    }

    return true;
}

bool WaylandInputCapture::getZones() {
    std::lock_guard<std::mutex> lock(m_dbusMutex);

    DBusMessage* msg = dbus_message_new_method_call(
        "org.gnome.Mutter.InputCapture",
        m_sessionHandle.c_str(),
        "org.gnome.Mutter.InputCapture.Session",
        "GetZones");
    if (!msg) return false;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        LOG((CLOG_ERR "WaylandInputCapture: GetZones failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }

    DBusMessageIter iter, arrayIter;
    dbus_message_iter_init(reply, &iter);
    dbus_message_iter_get_basic(&iter, &m_zoneSerial);
    dbus_message_iter_next(&iter);

    m_zones.clear();
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&iter, &arrayIter);
        while (dbus_message_iter_get_arg_type(&arrayIter) == DBUS_TYPE_STRUCT) {
            DBusMessageIter structIter;
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
            LOG((CLOG_INFO "WaylandInputCapture: Zone %ux%u at (%d, %d)", z.width, z.height, z.x, z.y));
            dbus_message_iter_next(&arrayIter);
        }
    }
    dbus_message_unref(reply);

    LOG((CLOG_INFO "WaylandInputCapture: Loaded %zu zones, serial %u", m_zones.size(), m_zoneSerial));
    return !m_zones.empty();
}

bool WaylandInputCapture::clearBarriers() {
    DBusMessage* msg = dbus_message_new_method_call(
        "org.gnome.Mutter.InputCapture",
        m_sessionHandle.c_str(),
        "org.gnome.Mutter.InputCapture.Session",
        "ClearBarriers");
    if (!msg) return false;

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 1000, nullptr);
    if (reply) dbus_message_unref(reply);
    dbus_message_unref(msg);
    return true;
}

bool WaylandInputCapture::addBarrier(uint32_t serial, int32_t x1, int32_t y1, int32_t x2, int32_t y2, EDirection dir) {
    DBusMessage* msg = dbus_message_new_method_call(
        "org.gnome.Mutter.InputCapture",
        m_sessionHandle.c_str(),
        "org.gnome.Mutter.InputCapture.Session",
        "AddBarrier");
    if (!msg) return false;

    DBusMessageIter iter, bPosIter;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &serial);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_STRUCT, nullptr, &bPosIter);
    dbus_message_iter_append_basic(&bPosIter, DBUS_TYPE_INT32, &x1);
    dbus_message_iter_append_basic(&bPosIter, DBUS_TYPE_INT32, &y1);
    dbus_message_iter_append_basic(&bPosIter, DBUS_TYPE_INT32, &x2);
    dbus_message_iter_append_basic(&bPosIter, DBUS_TYPE_INT32, &y2);
    dbus_message_iter_close_container(&iter, &bPosIter);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 1000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        LOG((CLOG_ERR "WaylandInputCapture: AddBarrier (%d,%d)-(%d,%d) failed: %s",
             x1, y1, x2, y2, err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return false;
    }

    uint32_t barrierId = 0;
    dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32, &barrierId, DBUS_TYPE_INVALID);
    dbus_message_unref(reply);

    BarrierEntry entry;
    entry.id = barrierId;
    entry.direction = dir;
    entry.x1 = x1;
    entry.y1 = y1;
    entry.x2 = x2;
    entry.y2 = y2;
    m_barriers.push_back(entry);

    static const char* dirNames[] = { "none", "left", "right", "top", "bottom" };
    const char* dName = (dir >= kNoDirection && dir <= kBottom) ? dirNames[dir] : "unknown";
    LOG((CLOG_INFO "WaylandInputCapture: Added barrier %u for %s at (%d,%d)-(%d,%d)",
         barrierId, dName, x1, y1, x2, y2));
    return true;
}

bool WaylandInputCapture::enableCapture() {
    DBusMessage* msg = dbus_message_new_method_call(
        "org.gnome.Mutter.InputCapture",
        m_sessionHandle.c_str(),
        "org.gnome.Mutter.InputCapture.Session",
        "Enable");
    if (!msg) return false;

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 1000, nullptr);
    if (reply) dbus_message_unref(reply);
    dbus_message_unref(msg);
    LOG((CLOG_INFO "WaylandInputCapture: Capture barriers enabled"));
    return true;
}

bool WaylandInputCapture::disableCapture() {
    DBusMessage* msg = dbus_message_new_method_call(
        "org.gnome.Mutter.InputCapture",
        m_sessionHandle.c_str(),
        "org.gnome.Mutter.InputCapture.Session",
        "Disable");
    if (!msg) return false;

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 1000, nullptr);
    if (reply) dbus_message_unref(reply);
    dbus_message_unref(msg);
    LOG((CLOG_DEBUG "WaylandInputCapture: Capture disabled"));
    return true;
}

void WaylandInputCapture::reconfigure(UInt32 activeSides) {
    std::lock_guard<std::mutex> lock(m_dbusMutex);
    LOG((CLOG_INFO "WaylandInputCapture: reconfigure activeSides=0x%04x", activeSides));
    m_activeSides = activeSides;

    if (!m_dbusConn || m_sessionHandle.empty()) return;

    disableCapture();
    clearBarriers();
    m_barriers.clear();

    if (m_zones.empty()) {
        getZones();
    }

    if (m_zones.empty()) {
        LOG((CLOG_WARN "WaylandInputCapture: No zones available to configure barriers"));
        return;
    }

    for (const auto& z : m_zones) {
        if (activeSides & kLeftMask) {
            addBarrier(m_zoneSerial, z.x, z.y, z.x, z.y + z.height, kLeft);
        }
        if (activeSides & kRightMask) {
            addBarrier(m_zoneSerial, z.x + z.width, z.y, z.x + z.width, z.y + z.height, kRight);
        }
        if (activeSides & kTopMask) {
            addBarrier(m_zoneSerial, z.x, z.y, z.x + z.width, z.y, kTop);
        }
        if (activeSides & kBottomMask) {
            addBarrier(m_zoneSerial, z.x, z.y + z.height, z.x + z.width, z.y + z.height, kBottom);
        }
    }

    enableCapture();
}

int WaylandInputCapture::connectToEIS() {
    std::lock_guard<std::mutex> lock(m_dbusMutex);

    DBusMessage* msg = dbus_message_new_method_call(
        "org.gnome.Mutter.InputCapture",
        m_sessionHandle.c_str(),
        "org.gnome.Mutter.InputCapture.Session",
        "ConnectToEIS");
    if (!msg) return -1;

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 2000, &err);
    dbus_message_unref(msg);

    if (!reply) {
        LOG((CLOG_ERR "WaylandInputCapture: ConnectToEIS failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
        return -1;
    }

    int fd = -1;
    dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
    dbus_message_unref(reply);

    if (fd < 0) {
        LOG((CLOG_ERR "WaylandInputCapture: ConnectToEIS returned invalid FD"));
    } else {
        LOG((CLOG_INFO "WaylandInputCapture: ConnectToEIS connected with FD %d", fd));
    }
    return fd;
}

bool WaylandInputCapture::setupLibEI(int fd) {
    m_ei = ei_new_receiver(nullptr);
    if (!m_ei) {
        LOG((CLOG_ERR "WaylandInputCapture: ei_new_receiver failed"));
        close(fd);
        return false;
    }

    if (ei_setup_backend_fd(m_ei, fd) < 0) {
        LOG((CLOG_ERR "WaylandInputCapture: ei_setup_backend_fd failed"));
        ei_unref(m_ei);
        m_ei = nullptr;
        close(fd);
        return false;
    }

    LOG((CLOG_INFO "WaylandInputCapture: libei receiver initialized"));
    m_running = true;
    m_thread = std::thread(&WaylandInputCapture::pollEvents, this);
    return true;
}

void WaylandInputCapture::releaseCapture() {
    std::lock_guard<std::mutex> lock(m_dbusMutex);
    if (!m_dbusConn || m_sessionHandle.empty() || !m_isCaptured) {
        return;
    }

    DBusMessage* msg = dbus_message_new_method_call(
        "org.gnome.Mutter.InputCapture",
        m_sessionHandle.c_str(),
        "org.gnome.Mutter.InputCapture.Session",
        "Release");
    if (!msg) return;

    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(m_dbusConn, msg, 500, &err);
    if (reply) dbus_message_unref(reply);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        LOG((CLOG_WARN "WaylandInputCapture: Release failed: %s", err.message ? err.message : "unknown"));
        dbus_error_free(&err);
    } else {
        LOG((CLOG_INFO "WaylandInputCapture: Capture successfully released"));
    }
    m_isCaptured = false;
}

void WaylandInputCapture::handleDBusMessage(DBusMessage* msg) {
    if (dbus_message_is_signal(msg, "org.gnome.Mutter.InputCapture.Session", "Activated")) {
        uint32_t barrier_id = 0;
        uint32_t activation_id = 0;
        double cx = 0.0, cy = 0.0;
        DBusMessageIter iter, posIter;
        if (dbus_message_iter_init(msg, &iter)) {
            dbus_message_iter_get_basic(&iter, &barrier_id);
            dbus_message_iter_next(&iter);
            dbus_message_iter_get_basic(&iter, &activation_id);
            dbus_message_iter_next(&iter);
            if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRUCT) {
                dbus_message_iter_recurse(&iter, &posIter);
                dbus_message_iter_get_basic(&posIter, &cx);
                dbus_message_iter_next(&posIter);
                dbus_message_iter_get_basic(&posIter, &cy);
            }
        }
        LOG((CLOG_INFO "WaylandInputCapture: Activated signal! barrier=%u, activation=%u, pos=(%.1f, %.1f)",
             barrier_id, activation_id, cx, cy));

        m_isCaptured = true;
        m_currentActivationId = activation_id;

        SInt32 posX = static_cast<SInt32>(cx);
        SInt32 posY = static_cast<SInt32>(cy);
        for (const auto& b : m_barriers) {
            if (b.id == barrier_id) {
                if (b.direction == kLeft) {
                    posX = b.x1;
                } else if (b.direction == kRight) {
                    posX = b.x1;
                } else if (b.direction == kTop) {
                    posY = b.y1;
                } else if (b.direction == kBottom) {
                    posY = b.y1;
                }
                break;
            }
        }

        m_events->addEvent(Event(
            m_events->forIPrimaryScreen().motionOnPrimary(),
            m_screen->getEventTarget(),
            IPrimaryScreen::MotionInfo::alloc(posX, posY)));
    }
    else if (dbus_message_is_signal(msg, "org.gnome.Mutter.InputCapture.Session", "Deactivated")) {
        uint32_t activation_id = 0;
        dbus_message_get_args(msg, nullptr, DBUS_TYPE_UINT32, &activation_id, DBUS_TYPE_INVALID);
        LOG((CLOG_INFO "WaylandInputCapture: Deactivated signal! activation=%u", activation_id));
        m_isCaptured = false;
    }
    else if (dbus_message_is_signal(msg, "org.gnome.Mutter.InputCapture.Session", "ZonesChanged")) {
        LOG((CLOG_INFO "WaylandInputCapture: ZonesChanged signal received"));
        getZones();
        reconfigure(m_activeSides);
    }
}

void WaylandInputCapture::handleEIEvent(struct ei_event* ev) {
    enum ei_event_type type = ei_event_get_type(ev);
    switch (type) {
    case EI_EVENT_SEAT_ADDED: {
        struct ei_seat* seat = ei_event_get_seat(ev);
        LOG((CLOG_INFO "WaylandInputCapture: Seat added (%s), binding capabilities",
             ei_seat_get_name(seat) ? ei_seat_get_name(seat) : "default"));
        ei_seat_bind_capabilities(
            seat,
            EI_DEVICE_CAP_POINTER,
            EI_DEVICE_CAP_POINTER_ABSOLUTE,
            EI_DEVICE_CAP_KEYBOARD,
            EI_DEVICE_CAP_BUTTON,
            EI_DEVICE_CAP_SCROLL,
            NULL
        );
        break;
    }
    case EI_EVENT_POINTER_MOTION: {
        double dx = ei_event_pointer_get_dx(ev);
        double dy = ei_event_pointer_get_dy(ev);
        if (m_isCaptured) {
            m_events->addEvent(Event(
                m_events->forIPrimaryScreen().motionOnSecondary(),
                m_screen->getEventTarget(),
                IPrimaryScreen::MotionInfo::alloc(static_cast<SInt32>(dx), static_cast<SInt32>(dy))));
        }
        break;
    }
    case EI_EVENT_BUTTON_BUTTON: {
        uint32_t button = ei_event_button_get_button(ev);
        bool is_press = ei_event_button_get_is_press(ev);
        ButtonID bid = kButtonNone;
        if (button == BTN_LEFT) bid = kButtonLeft;
        else if (button == BTN_MIDDLE) bid = kButtonMiddle;
        else if (button == BTN_RIGHT) bid = kButtonRight;
        else if (button == BTN_SIDE) bid = kButtonExtra0;
        else if (button == BTN_EXTRA) bid = kButtonExtra1;
        else if (button >= BTN_SIDE) bid = kButtonExtra0 + (button - BTN_SIDE);

        if (bid != kButtonNone && m_isCaptured) {
            m_events->addEvent(Event(
                is_press ? m_events->forIPrimaryScreen().buttonDown() : m_events->forIPrimaryScreen().buttonUp(),
                m_screen->getEventTarget(),
                IPrimaryScreen::ButtonInfo::alloc(bid, m_activeModifiers)));
        }
        break;
    }
    case EI_EVENT_SCROLL_DELTA: {
        double dx = ei_event_scroll_get_dx(ev);
        double dy = ei_event_scroll_get_dy(ev);
        SInt32 yDelta = static_cast<SInt32>(-dy * 12.0);
        SInt32 xDelta = static_cast<SInt32>(dx * 12.0);
        if ((xDelta != 0 || yDelta != 0) && m_isCaptured) {
            m_events->addEvent(Event(
                m_events->forIPrimaryScreen().wheel(),
                m_screen->getEventTarget(),
                IPrimaryScreen::WheelInfo::alloc(xDelta, yDelta)));
        }
        break;
    }
    case EI_EVENT_SCROLL_DISCRETE: {
        int32_t ddy = ei_event_scroll_get_discrete_dy(ev);
        int32_t ddx = ei_event_scroll_get_discrete_dx(ev);
        SInt32 yDelta = -ddy * 120;
        SInt32 xDelta = ddx * 120;
        if ((xDelta != 0 || yDelta != 0) && m_isCaptured) {
            m_events->addEvent(Event(
                m_events->forIPrimaryScreen().wheel(),
                m_screen->getEventTarget(),
                IPrimaryScreen::WheelInfo::alloc(xDelta, yDelta)));
        }
        break;
    }
    case EI_EVENT_KEYBOARD_KEY: {
        uint32_t scancode = ei_event_keyboard_get_key(ev);
        bool is_press = ei_event_keyboard_get_key_is_press(ev);
        updateModifiers(scancode, is_press);
        KeyID key = scancodeToKeyID(scancode);
        KeyButton button = static_cast<KeyButton>(scancode + 8);
        if (m_isCaptured) {
            m_events->addEvent(Event(
                is_press ? m_events->forIKeyState().keyDown() : m_events->forIKeyState().keyUp(),
                m_screen->getEventTarget(),
                IKeyState::KeyInfo::alloc(key, m_activeModifiers, button, 1)));
        }
        break;
    }
    default:
        break;
    }
}

void WaylandInputCapture::pollEvents() {
    int checkSwitchTimeoutCount = 0;

    while (m_running) {
        // 1. Process DBus messages
        {
            std::lock_guard<std::mutex> lock(m_dbusMutex);
            if (m_dbusConn) {
                dbus_connection_read_write(m_dbusConn, 0);
                while (DBusMessage* msg = dbus_connection_pop_message(m_dbusConn)) {
                    handleDBusMessage(msg);
                    dbus_message_unref(msg);
                }
            }
        }

        // 2. Process libei events
        if (m_ei) {
            ei_dispatch(m_ei);
            while (struct ei_event* ev = ei_get_event(m_ei)) {
                handleEIEvent(ev);
                ei_event_unref(ev);
            }
        }

        // 3. Safety check: if capture is active but Barrier never switched to secondary screen within ~100ms, release capture
        if (m_isCaptured && !m_screen->isOnSecondary()) {
            checkSwitchTimeoutCount++;
            if (checkSwitchTimeoutCount > 10) { // ~100ms
                LOG((CLOG_WARN "WaylandInputCapture: Screen switch did not occur, releasing capture safety valve"));
                releaseCapture();
                checkSwitchTimeoutCount = 0;
            }
        } else {
            checkSwitchTimeoutCount = 0;
        }

        // 4. Poll on libei and DBus fds
        struct pollfd pfds[2];
        int numFds = 0;
        if (m_ei) {
            pfds[numFds].fd = ei_get_fd(m_ei);
            pfds[numFds].events = POLLIN;
            numFds++;
        }
        int dbusFd = -1;
        if (m_dbusConn && dbus_connection_get_unix_fd(m_dbusConn, &dbusFd) && dbusFd >= 0) {
            pfds[numFds].fd = dbusFd;
            pfds[numFds].events = POLLIN;
            numFds++;
        }

        if (numFds > 0) {
            poll(pfds, numFds, 10);
        } else {
            usleep(10000);
        }
    }
}

void WaylandInputCapture::updateModifiers(uint32_t scancode, bool isPress) {
    KeyModifierMask mod = 0;
    switch (scancode) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        mod = KeyModifierShift;
        break;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        mod = KeyModifierControl;
        break;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
        mod = KeyModifierAlt;
        break;
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        mod = KeyModifierSuper;
        break;
    case KEY_CAPSLOCK:
        if (isPress) m_activeModifiers ^= KeyModifierCapsLock;
        return;
    case KEY_NUMLOCK:
        if (isPress) m_activeModifiers ^= KeyModifierNumLock;
        return;
    case KEY_SCROLLLOCK:
        if (isPress) m_activeModifiers ^= KeyModifierScrollLock;
        return;
    default:
        return;
    }

    if (isPress) {
        m_activeModifiers |= mod;
    } else {
        m_activeModifiers &= ~mod;
    }
}

KeyID WaylandInputCapture::scancodeToKeyID(uint32_t scancode) const {
    if (scancode >= KEY_1 && scancode <= KEY_9) {
        if (m_activeModifiers & KeyModifierShift) {
            static const KeyID shifted[] = { '!', '@', '#', '$', '%', '^', '&', '*', '(' };
            return shifted[scancode - KEY_1];
        }
        return '1' + (scancode - KEY_1);
    }
    if (scancode == KEY_0) {
        return (m_activeModifiers & KeyModifierShift) ? ')' : '0';
    }

    switch (scancode) {
    case KEY_Q: return (m_activeModifiers & KeyModifierShift) ? 'Q' : 'q';
    case KEY_W: return (m_activeModifiers & KeyModifierShift) ? 'W' : 'w';
    case KEY_E: return (m_activeModifiers & KeyModifierShift) ? 'E' : 'e';
    case KEY_R: return (m_activeModifiers & KeyModifierShift) ? 'R' : 'r';
    case KEY_T: return (m_activeModifiers & KeyModifierShift) ? 'T' : 't';
    case KEY_Y: return (m_activeModifiers & KeyModifierShift) ? 'Y' : 'y';
    case KEY_U: return (m_activeModifiers & KeyModifierShift) ? 'U' : 'u';
    case KEY_I: return (m_activeModifiers & KeyModifierShift) ? 'I' : 'i';
    case KEY_O: return (m_activeModifiers & KeyModifierShift) ? 'O' : 'o';
    case KEY_P: return (m_activeModifiers & KeyModifierShift) ? 'P' : 'p';
    case KEY_A: return (m_activeModifiers & KeyModifierShift) ? 'A' : 'a';
    case KEY_S: return (m_activeModifiers & KeyModifierShift) ? 'S' : 's';
    case KEY_D: return (m_activeModifiers & KeyModifierShift) ? 'D' : 'd';
    case KEY_F: return (m_activeModifiers & KeyModifierShift) ? 'F' : 'f';
    case KEY_G: return (m_activeModifiers & KeyModifierShift) ? 'G' : 'g';
    case KEY_H: return (m_activeModifiers & KeyModifierShift) ? 'H' : 'h';
    case KEY_J: return (m_activeModifiers & KeyModifierShift) ? 'J' : 'j';
    case KEY_K: return (m_activeModifiers & KeyModifierShift) ? 'K' : 'k';
    case KEY_L: return (m_activeModifiers & KeyModifierShift) ? 'L' : 'l';
    case KEY_Z: return (m_activeModifiers & KeyModifierShift) ? 'Z' : 'z';
    case KEY_X: return (m_activeModifiers & KeyModifierShift) ? 'X' : 'x';
    case KEY_C: return (m_activeModifiers & KeyModifierShift) ? 'C' : 'c';
    case KEY_V: return (m_activeModifiers & KeyModifierShift) ? 'V' : 'v';
    case KEY_B: return (m_activeModifiers & KeyModifierShift) ? 'B' : 'b';
    case KEY_N: return (m_activeModifiers & KeyModifierShift) ? 'N' : 'n';
    case KEY_M: return (m_activeModifiers & KeyModifierShift) ? 'M' : 'm';

    case KEY_MINUS: return (m_activeModifiers & KeyModifierShift) ? '_' : '-';
    case KEY_EQUAL: return (m_activeModifiers & KeyModifierShift) ? '+' : '=';
    case KEY_LEFTBRACE: return (m_activeModifiers & KeyModifierShift) ? '{' : '[';
    case KEY_RIGHTBRACE: return (m_activeModifiers & KeyModifierShift) ? '}' : ']';
    case KEY_SEMICOLON: return (m_activeModifiers & KeyModifierShift) ? ':' : ';';
    case KEY_APOSTROPHE: return (m_activeModifiers & KeyModifierShift) ? '"' : '\'';
    case KEY_GRAVE: return (m_activeModifiers & KeyModifierShift) ? '~' : '`';
    case KEY_BACKSLASH: return (m_activeModifiers & KeyModifierShift) ? '|' : '\\';
    case KEY_COMMA: return (m_activeModifiers & KeyModifierShift) ? '<' : ',';
    case KEY_DOT: return (m_activeModifiers & KeyModifierShift) ? '>' : '.';
    case KEY_SLASH: return (m_activeModifiers & KeyModifierShift) ? '?' : '/';
    case KEY_SPACE: return ' ';

    case KEY_ENTER: return kKeyReturn;
    case KEY_ESC: return kKeyEscape;
    case KEY_BACKSPACE: return kKeyBackSpace;
    case KEY_TAB: return kKeyTab;

    case KEY_LEFTSHIFT: return kKeyShift_L;
    case KEY_RIGHTSHIFT: return kKeyShift_R;
    case KEY_LEFTCTRL: return kKeyControl_L;
    case KEY_RIGHTCTRL: return kKeyControl_R;
    case KEY_LEFTALT: return kKeyAlt_L;
    case KEY_RIGHTALT: return kKeyAlt_R;
    case KEY_LEFTMETA: return kKeySuper_L;
    case KEY_RIGHTMETA: return kKeySuper_R;
    case KEY_CAPSLOCK: return kKeyCapsLock;
    case KEY_SCROLLLOCK: return kKeyScrollLock;
    case KEY_NUMLOCK: return kKeyNumLock;

    case KEY_INSERT: return kKeyInsert;
    case KEY_DELETE: return kKeyDelete;
    case KEY_HOME: return kKeyHome;
    case KEY_END: return kKeyEnd;
    case KEY_PAGEUP: return kKeyPageUp;
    case KEY_PAGEDOWN: return kKeyPageDown;
    case KEY_LEFT: return kKeyLeft;
    case KEY_UP: return kKeyUp;
    case KEY_RIGHT: return kKeyRight;
    case KEY_DOWN: return kKeyDown;
    case KEY_SYSRQ: return kKeyPrint;
    case KEY_PAUSE: return kKeyPause;
    case KEY_COMPOSE: return kKeyMenu;

    case KEY_F1: return kKeyF1;
    case KEY_F2: return kKeyF2;
    case KEY_F3: return kKeyF3;
    case KEY_F4: return kKeyF4;
    case KEY_F5: return kKeyF5;
    case KEY_F6: return kKeyF6;
    case KEY_F7: return kKeyF7;
    case KEY_F8: return kKeyF8;
    case KEY_F9: return kKeyF9;
    case KEY_F10: return kKeyF10;
    case KEY_F11: return kKeyF11;
    case KEY_F12: return kKeyF12;

    case KEY_KP0: return kKeyKP_0;
    case KEY_KP1: return kKeyKP_1;
    case KEY_KP2: return kKeyKP_2;
    case KEY_KP3: return kKeyKP_3;
    case KEY_KP4: return kKeyKP_4;
    case KEY_KP5: return kKeyKP_5;
    case KEY_KP6: return kKeyKP_6;
    case KEY_KP7: return kKeyKP_7;
    case KEY_KP8: return kKeyKP_8;
    case KEY_KP9: return kKeyKP_9;
    case KEY_KPENTER: return kKeyKP_Enter;
    case KEY_KPPLUS: return kKeyKP_Add;
    case KEY_KPMINUS: return kKeyKP_Subtract;
    case KEY_KPASTERISK: return kKeyKP_Multiply;
    case KEY_KPSLASH: return kKeyKP_Divide;
    case KEY_KPDOT: return kKeyKP_Decimal;

    default:
        return kKeyNone;
    }
}

bool WaylandInputCapture::hasZones() const {
    return !m_zones.empty();
}

void WaylandInputCapture::getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const {
    if (m_zones.empty()) {
        x = 0; y = 0; width = 1920; height = 1080;
        return;
    }
    int32_t minX = m_zones[0].x;
    int32_t minY = m_zones[0].y;
    int32_t maxX = m_zones[0].x + m_zones[0].width;
    int32_t maxY = m_zones[0].y + m_zones[0].height;
    for (size_t i = 1; i < m_zones.size(); ++i) {
        if (m_zones[i].x < minX) minX = m_zones[i].x;
        if (m_zones[i].y < minY) minY = m_zones[i].y;
        if ((int32_t)(m_zones[i].x + m_zones[i].width) > maxX) maxX = m_zones[i].x + m_zones[i].width;
        if ((int32_t)(m_zones[i].y + m_zones[i].height) > maxY) maxY = m_zones[i].y + m_zones[i].height;
    }
    x = minX;
    y = minY;
    width = maxX - minX;
    height = maxY - minY;
}
