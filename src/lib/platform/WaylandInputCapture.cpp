#include "WaylandInputCapture.h"
#include "platform/WaylandScreen.h"
#include "platform/WaylandPortal.h"
#include "platform/XWindowsUtil.h"
#include "barrier/IPrimaryScreen.h"
#include "barrier/IKeyState.h"
#include "base/Event.h"
#include "base/Log.h"

#include <libei.h>
#include <unistd.h>
#include <poll.h>
#include <dbus/dbus.h>
#include <linux/input-event-codes.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <cctype>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>

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
      m_activeModifiers(0),
      m_xDisplay(nullptr),
      m_altMask(Mod1Mask),
      m_altGrMask(Mod5Mask),
      m_superMask(Mod4Mask),
      m_numLockMask(Mod2Mask),
      m_frameHasDiscreteScroll(false),
      m_frameDiscreteX(0),
      m_frameDiscreteY(0),
      m_frameDeltaX(0.0),
      m_frameDeltaY(0.0),
      m_smoothScrollAccX(0.0),
      m_smoothScrollAccY(0.0),
      m_nextHotKeyId(1),
      m_cmdSock(-1) {
}

void WaylandInputCapture::resetScrollState() {
    m_frameHasDiscreteScroll = false;
    m_frameDiscreteX = 0;
    m_frameDiscreteY = 0;
    m_frameDeltaX = 0.0;
    m_frameDeltaY = 0.0;
    m_smoothScrollAccX = 0.0;
    m_smoothScrollAccY = 0.0;
}

WaylandInputCapture::~WaylandInputCapture() {
    cleanup();
}

void WaylandInputCapture::cleanup() {
    m_running = false;
    cleanupCommandSocket();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    {
        std::lock_guard<std::recursive_mutex> lock(m_hotKeyMutex);
        if (m_xDisplay) {
            Window root = DefaultRootWindow(m_xDisplay);
            for (const auto& kv : m_hotKeys) {
                if (kv.second.keycode != 0) {
                    XUngrabKey(m_xDisplay, kv.second.keycode, AnyModifier, root);
                }
            }
            XFlush(m_xDisplay);
        }
        m_hotKeys.clear();
        m_hotKeysByCodeMod.clear();
        m_hotKeysByKeyMask.clear();
        m_activeCapturedHotKeys.clear();
        m_activeX11HotKeys.clear();
    }
    cleanupX11();
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

bool WaylandInputCapture::initX11() {
    if (m_xDisplay) {
        return true;
    }

    if (getenv("DISPLAY") == nullptr) {
        setenv("DISPLAY", ":0", 0);
    }

    if (getenv("XAUTHORITY") == nullptr) {
        uid_t uid = getuid();
        std::string runUserDir = "/run/user/" + std::to_string(uid);
        DIR* dir = opendir(runUserDir.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (strncmp(entry->d_name, ".mutter-Xwaylandauth", 20) == 0) {
                    std::string authPath = runUserDir + "/" + entry->d_name;
                    setenv("XAUTHORITY", authPath.c_str(), 0);
                    break;
                }
            }
            closedir(dir);
        }
    }

    m_xDisplay = XOpenDisplay(NULL);
    if (!m_xDisplay) {
        LOG((CLOG_WARN "WaylandInputCapture: Could not open Xwayland display; falling back to static key table"));
        return false;
    }

    initModifierMasks();
    return true;
}

void WaylandInputCapture::initModifierMasks() {
    if (!m_xDisplay) return;

    m_altMask = Mod1Mask;
    m_altGrMask = Mod5Mask;
    m_superMask = Mod4Mask;
    m_numLockMask = Mod2Mask;

    XModifierKeymap* mmap = XGetModifierMapping(m_xDisplay);
    if (mmap) {
        for (int modIndex = 0; modIndex < 8; ++modIndex) {
            unsigned int mask = (1u << modIndex);
            for (int k = 0; k < mmap->max_keypermod; ++k) {
                KeyCode kc = mmap->modifiermap[modIndex * mmap->max_keypermod + k];
                if (kc == 0) continue;
                KeySym sym = XkbKeycodeToKeysym(m_xDisplay, kc, 0, 0);
                if (sym == XK_ISO_Level3_Shift || sym == XK_Mode_switch) {
                    m_altGrMask = mask;
                } else if (sym == XK_Alt_L || sym == XK_Alt_R) {
                    m_altMask = mask;
                } else if (sym == XK_Super_L || sym == XK_Super_R) {
                    m_superMask = mask;
                } else if (sym == XK_Num_Lock) {
                    m_numLockMask = mask;
                }
            }
        }
        XFreeModifiermap(mmap);
    }
    LOG((CLOG_INFO "WaylandInputCapture: Key layout initialized (alt=0x%x, altgr=0x%x, super=0x%x, numlock=0x%x)",
         m_altMask, m_altGrMask, m_superMask, m_numLockMask));
}

void WaylandInputCapture::cleanupX11() {
    if (m_xDisplay) {
        XCloseDisplay(m_xDisplay);
        m_xDisplay = nullptr;
    }
}

bool WaylandInputCapture::init() {
    initX11();
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
    m_pressedKeys.clear();
    {
        std::lock_guard<std::recursive_mutex> lock(m_hotKeyMutex);
        m_activeCapturedHotKeys.clear();
    }
    resetScrollState();
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
        resetScrollState();

        if (m_xDisplay) {
            XkbStateRec xkbState;
            if (XkbGetState(m_xDisplay, XkbUseCoreKbd, &xkbState) == Success) {
                if (xkbState.locked_mods & LockMask) {
                    m_activeModifiers |= KeyModifierCapsLock;
                } else {
                    m_activeModifiers &= ~KeyModifierCapsLock;
                }
                if (xkbState.locked_mods & m_numLockMask) {
                    m_activeModifiers |= KeyModifierNumLock;
                } else {
                    m_activeModifiers &= ~KeyModifierNumLock;
                }
            }
        }

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

        if (!m_screen->isOnSecondary()) {
            m_events->addEvent(Event(
                m_events->forIPrimaryScreen().motionOnPrimary(),
                m_screen->getEventTarget(),
                IPrimaryScreen::MotionInfo::alloc(posX, posY)));
        }
    }
    else if (dbus_message_is_signal(msg, "org.gnome.Mutter.InputCapture.Session", "Deactivated")) {
        uint32_t activation_id = 0;
        dbus_message_get_args(msg, nullptr, DBUS_TYPE_UINT32, &activation_id, DBUS_TYPE_INVALID);
        LOG((CLOG_INFO "WaylandInputCapture: Deactivated signal! activation=%u", activation_id));
        m_isCaptured = false;
        m_pressedKeys.clear();
        resetScrollState();
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
        m_frameDeltaX += ei_event_scroll_get_dx(ev);
        m_frameDeltaY += ei_event_scroll_get_dy(ev);
        break;
    }
    case EI_EVENT_SCROLL_DISCRETE: {
        m_frameHasDiscreteScroll = true;
        m_frameDiscreteX += ei_event_scroll_get_discrete_dx(ev);
        m_frameDiscreteY += ei_event_scroll_get_discrete_dy(ev);
        break;
    }
    case EI_EVENT_SCROLL_STOP:
    case EI_EVENT_SCROLL_CANCEL: {
        if (m_isCaptured) {
            const double kStep = 40.0;
            if (m_smoothScrollAccX >= 20.0) {
                m_events->addEvent(Event(
                    m_events->forIPrimaryScreen().wheel(),
                    m_screen->getEventTarget(),
                    IPrimaryScreen::WheelInfo::alloc(static_cast<SInt32>(kStep), 0)));
            } else if (m_smoothScrollAccX <= -20.0) {
                m_events->addEvent(Event(
                    m_events->forIPrimaryScreen().wheel(),
                    m_screen->getEventTarget(),
                    IPrimaryScreen::WheelInfo::alloc(static_cast<SInt32>(-kStep), 0)));
            }
            if (m_smoothScrollAccY >= 20.0) {
                m_events->addEvent(Event(
                    m_events->forIPrimaryScreen().wheel(),
                    m_screen->getEventTarget(),
                    IPrimaryScreen::WheelInfo::alloc(0, static_cast<SInt32>(kStep))));
            } else if (m_smoothScrollAccY <= -20.0) {
                m_events->addEvent(Event(
                    m_events->forIPrimaryScreen().wheel(),
                    m_screen->getEventTarget(),
                    IPrimaryScreen::WheelInfo::alloc(0, static_cast<SInt32>(-kStep))));
            }
        }
        m_smoothScrollAccX = 0.0;
        m_smoothScrollAccY = 0.0;
        break;
    }
    case EI_EVENT_FRAME: {
        if (m_isCaptured) {
            if (m_frameHasDiscreteScroll) {
                int32_t discY = m_frameDiscreteY;
                int32_t discX = m_frameDiscreteX;

                if (discY != 0 && std::abs(discY) < 15) {
                    discY *= 120;
                }
                if (discX != 0 && std::abs(discX) < 15) {
                    discX *= 120;
                }

                SInt32 yDelta = -discY;
                SInt32 xDelta = discX;

                // Send X and Y wheel events separately because X11 clients (and Windows)
                // drop or ignore events that have both xDelta and yDelta non-zero.
                if (xDelta != 0) {
                    m_events->addEvent(Event(
                        m_events->forIPrimaryScreen().wheel(),
                        m_screen->getEventTarget(),
                        IPrimaryScreen::WheelInfo::alloc(xDelta, 0)));
                }
                if (yDelta != 0) {
                    m_events->addEvent(Event(
                        m_events->forIPrimaryScreen().wheel(),
                        m_screen->getEventTarget(),
                        IPrimaryScreen::WheelInfo::alloc(0, yDelta)));
                }

                m_smoothScrollAccX = 0.0;
                m_smoothScrollAccY = 0.0;
            } else if (m_frameDeltaX != 0.0 || m_frameDeltaY != 0.0) {
                // Continuous smooth scroll (touchpad)
                // 15 logical pixels = 120 Barrier units (8.0 units per pixel)
                m_smoothScrollAccX += m_frameDeltaX * 8.0;
                m_smoothScrollAccY += -m_frameDeltaY * 8.0;

                const double kStep = 40.0;

                while (m_smoothScrollAccX >= kStep) {
                    m_events->addEvent(Event(
                        m_events->forIPrimaryScreen().wheel(),
                        m_screen->getEventTarget(),
                        IPrimaryScreen::WheelInfo::alloc(static_cast<SInt32>(kStep), 0)));
                    m_smoothScrollAccX -= kStep;
                }
                while (m_smoothScrollAccX <= -kStep) {
                    m_events->addEvent(Event(
                        m_events->forIPrimaryScreen().wheel(),
                        m_screen->getEventTarget(),
                        IPrimaryScreen::WheelInfo::alloc(static_cast<SInt32>(-kStep), 0)));
                    m_smoothScrollAccX += kStep;
                }

                while (m_smoothScrollAccY >= kStep) {
                    m_events->addEvent(Event(
                        m_events->forIPrimaryScreen().wheel(),
                        m_screen->getEventTarget(),
                        IPrimaryScreen::WheelInfo::alloc(0, static_cast<SInt32>(kStep))));
                    m_smoothScrollAccY -= kStep;
                }
                while (m_smoothScrollAccY <= -kStep) {
                    m_events->addEvent(Event(
                        m_events->forIPrimaryScreen().wheel(),
                        m_screen->getEventTarget(),
                        IPrimaryScreen::WheelInfo::alloc(0, static_cast<SInt32>(-kStep))));
                    m_smoothScrollAccY += kStep;
                }
            }
        }

        m_frameHasDiscreteScroll = false;
        m_frameDiscreteX = 0;
        m_frameDiscreteY = 0;
        m_frameDeltaX = 0.0;
        m_frameDeltaY = 0.0;
        break;
    }
    case EI_EVENT_KEYBOARD_KEY: {
        uint32_t scancode = ei_event_keyboard_get_key(ev);
        bool is_press = ei_event_keyboard_get_key_is_press(ev);
        KeyID key = kKeyNone;
        if (is_press) {
            updateModifiers(scancode, true);
            key = scancodeToKeyID(scancode);
            m_pressedKeys[scancode] = key;
        } else {
            auto it = m_pressedKeys.find(scancode);
            if (it != m_pressedKeys.end()) {
                key = it->second;
                m_pressedKeys.erase(it);
            } else {
                key = scancodeToKeyID(scancode);
            }
            updateModifiers(scancode, false);
        }

        KeyButton button = static_cast<KeyButton>(scancode + 8);

        // Check if this key matches a registered hotkey
        {
            std::lock_guard<std::recursive_mutex> lock(m_hotKeyMutex);
            if (is_press) {
                KeyModifierMask normMask = m_activeModifiers & (KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierSuper | KeyModifierAltGr);
                auto hit = m_hotKeysByKeyMask.find(std::make_pair(key, normMask));
                if (hit != m_hotKeysByKeyMask.end()) {
                    UInt32 hotKeyId = hit->second;
                    m_activeCapturedHotKeys[scancode] = hotKeyId;
                    LOG((CLOG_INFO "WaylandInputCapture: Captured hotkey triggered id=%u key=0x%x mask=0x%x Press",
                         hotKeyId, key, normMask));

                    m_events->addEvent(Event(
                        m_events->forIPrimaryScreen().hotKeyDown(),
                        m_screen->getEventTarget(),
                        IPlatformScreen::HotKeyInfo::alloc(hotKeyId)));
                    break;
                }
            } else {
                auto it = m_activeCapturedHotKeys.find(scancode);
                if (it != m_activeCapturedHotKeys.end()) {
                    UInt32 hotKeyId = it->second;
                    m_activeCapturedHotKeys.erase(it);
                    LOG((CLOG_INFO "WaylandInputCapture: Captured hotkey released id=%u key=0x%x Release",
                         hotKeyId, key));

                    m_events->addEvent(Event(
                        m_events->forIPrimaryScreen().hotKeyUp(),
                        m_screen->getEventTarget(),
                        IPlatformScreen::HotKeyInfo::alloc(hotKeyId)));
                    break;
                }
            }
        }

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
    setupCommandSocket();
    int checkSwitchTimeoutCount = 0;

    while (m_running) {
        // 0. Process Xwayland events (e.g. keyboard layout switch, hotkeys)
        if (m_xDisplay) {
            std::lock_guard<std::recursive_mutex> lock(m_hotKeyMutex);
            while (XPending(m_xDisplay) > 0) {
                XEvent xev;
                XNextEvent(m_xDisplay, &xev);
                if (xev.type == MappingNotify) {
                    XRefreshKeyboardMapping(&xev.xmapping);
                    initModifierMasks();
                } else if (xev.type == KeyPress || xev.type == KeyRelease) {
                    handleX11KeyEvent(&xev.xkey);
                }
            }
        }

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

        // 4. Poll on libei, DBus, X11, and control socket fds
        struct pollfd pfds[4];
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
        if (m_xDisplay) {
            int xfd = ConnectionNumber(m_xDisplay);
            if (xfd >= 0) {
                pfds[numFds].fd = xfd;
                pfds[numFds].events = POLLIN;
                numFds++;
            }
        }
        if (m_cmdSock >= 0) {
            pfds[numFds].fd = m_cmdSock;
            pfds[numFds].events = POLLIN;
            numFds++;
        }

        if (numFds > 0) {
            poll(pfds, numFds, 10);
            if (m_cmdSock >= 0) {
                for (int i = 0; i < numFds; ++i) {
                    if (pfds[i].fd == m_cmdSock && (pfds[i].revents & POLLIN)) {
                        processCommandSocket();
                        break;
                    }
                }
            }
        } else {
            usleep(10000);
        }
    }
    cleanupCommandSocket();
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
        mod = KeyModifierAlt;
        break;
    case KEY_RIGHTALT: {
        KeyModifierMask altMod = KeyModifierAlt;
        if (m_xDisplay) {
            KeySym sym = XkbKeycodeToKeysym(m_xDisplay, scancode + 8, 0, 0);
            if (sym == XK_ISO_Level3_Shift || sym == XK_Mode_switch) {
                altMod = KeyModifierAltGr;
            }
        }
        mod = altMod;
        break;
    }
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
    if (m_xDisplay) {
        KeyCode kc = static_cast<KeyCode>(scancode + 8);
        unsigned int state = 0;
        if (m_activeModifiers & KeyModifierShift) state |= ShiftMask;
        if (m_activeModifiers & KeyModifierCapsLock) state |= LockMask;
        if (m_activeModifiers & KeyModifierControl) state |= ControlMask;
        if (m_activeModifiers & KeyModifierAlt) state |= m_altMask;
        if (m_activeModifiers & KeyModifierNumLock) state |= m_numLockMask;
        if (m_activeModifiers & KeyModifierAltGr) state |= m_altGrMask;

        XKeyEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = KeyPress;
        ev.display = m_xDisplay;
        ev.keycode = kc;
        ev.state = state;

        KeySym keysym = 0;
        char dummy[32];
        XLookupString(&ev, dummy, sizeof(dummy), &keysym, NULL);

        if (keysym != 0) {
            KeyID key = XWindowsUtil::mapKeySymToKeyID(keysym);
            if (key != kKeyNone) {
                return key;
            }
        }
    }

    return fallbackScancodeToKeyID(scancode);
}

KeyID WaylandInputCapture::fallbackScancodeToKeyID(uint32_t scancode) const {
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
    case KEY_RO: return (m_activeModifiers & KeyModifierShift) ? '?' : '/';
    case KEY_102ND: return (m_activeModifiers & KeyModifierShift) ? '|' : '\\';
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
    case KEY_RIGHTALT: return (m_activeModifiers & KeyModifierAltGr) ? kKeyAltGr : kKeyAlt_R;
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

KeyCode WaylandInputCapture::keyIDToKeyCode(KeyID key) const {
    if (!m_xDisplay) return 0;

    KeySym sym = NoSymbol;
    if (key >= 0x20 && key <= 0x7e) {
        sym = static_cast<KeySym>(key);
    } else if (key >= 0xef00 && key <= 0xefff) {
        sym = static_cast<KeySym>(key - 0xef00 + 0xff00);
    }
    if (sym != NoSymbol) {
        KeyCode kc = XKeysymToKeycode(m_xDisplay, sym);
        if (kc != 0) return kc;
    }

    int minKeycode = 8, maxKeycode = 255;
    XDisplayKeycodes(m_xDisplay, &minKeycode, &maxKeycode);
    for (int kc = minKeycode; kc <= maxKeycode; ++kc) {
        KeySym ksym = XkbKeycodeToKeysym(m_xDisplay, kc, 0, 0);
        if (ksym != NoSymbol && XWindowsUtil::mapKeySymToKeyID(ksym) == key) {
            return static_cast<KeyCode>(kc);
        }
    }
    return 0;
}

unsigned int WaylandInputCapture::mapModifiersToX(KeyModifierMask mask) const {
    unsigned int xMods = 0;
    if (mask & KeyModifierShift) xMods |= ShiftMask;
    if (mask & KeyModifierControl) xMods |= ControlMask;
    if (mask & KeyModifierAlt) xMods |= m_altMask;
    if (mask & KeyModifierSuper) xMods |= m_superMask;
    if (mask & KeyModifierAltGr) xMods |= m_altGrMask;
    return xMods;
}

UInt32 WaylandInputCapture::registerHotKey(KeyID key, KeyModifierMask mask) {
    std::lock_guard<std::recursive_mutex> lock(m_hotKeyMutex);

    if (!m_xDisplay) {
        initX11();
    }

    UInt32 id = m_nextHotKeyId++;
    HotKeyItem item;
    item.id = id;
    item.key = key;
    item.mask = mask & (KeyModifierShift | KeyModifierControl | KeyModifierAlt | KeyModifierSuper | KeyModifierAltGr);
    item.keycode = keyIDToKeyCode(key);
    item.xMods = mapModifiersToX(item.mask);

    LOG((CLOG_INFO "WaylandInputCapture: Registering hotkey id=%u key=0x%x mask=0x%x keycode=%u xMods=0x%x",
         id, key, item.mask, (unsigned int)item.keycode, item.xMods));

    if (m_xDisplay && item.keycode != 0) {
        Window root = DefaultRootWindow(m_xDisplay);
        unsigned int ignoredMods[] = {
            0,
            LockMask,
            m_numLockMask,
            LockMask | m_numLockMask
        };
        for (unsigned int ig : ignoredMods) {
            XGrabKey(m_xDisplay, item.keycode, item.xMods | ig, root, True, GrabModeAsync, GrabModeAsync);
        }
        XFlush(m_xDisplay);
    }

    m_hotKeys[id] = item;
    if (item.keycode != 0) {
        m_hotKeysByCodeMod[std::make_pair(item.keycode, item.xMods)] = id;
    }
    m_hotKeysByKeyMask[std::make_pair(item.key, item.mask)] = id;

    if (item.key >= 'A' && item.key <= 'Z') {
        KeyID lower = item.key - 'A' + 'a';
        m_hotKeysByKeyMask[std::make_pair(lower, item.mask)] = id;
    } else if (item.key >= 'a' && item.key <= 'z') {
        KeyID upper = item.key - 'a' + 'A';
        m_hotKeysByKeyMask[std::make_pair(upper, item.mask)] = id;
    }

    return id;
}

void WaylandInputCapture::unregisterHotKey(UInt32 id) {
    std::lock_guard<std::recursive_mutex> lock(m_hotKeyMutex);

    auto it = m_hotKeys.find(id);
    if (it == m_hotKeys.end()) return;

    const HotKeyItem& item = it->second;
    if (m_xDisplay && item.keycode != 0) {
        Window root = DefaultRootWindow(m_xDisplay);
        unsigned int ignoredMods[] = {
            0,
            LockMask,
            m_numLockMask,
            LockMask | m_numLockMask
        };
        for (unsigned int ig : ignoredMods) {
            XUngrabKey(m_xDisplay, item.keycode, item.xMods | ig, root);
        }
        XFlush(m_xDisplay);
    }

    if (item.keycode != 0) {
        m_hotKeysByCodeMod.erase(std::make_pair(item.keycode, item.xMods));
    }
    m_hotKeysByKeyMask.erase(std::make_pair(item.key, item.mask));
    if (item.key >= 'A' && item.key <= 'Z') {
        m_hotKeysByKeyMask.erase(std::make_pair(item.key - 'A' + 'a', item.mask));
    } else if (item.key >= 'a' && item.key <= 'z') {
        m_hotKeysByKeyMask.erase(std::make_pair(item.key - 'a' + 'A', item.mask));
    }

    m_hotKeys.erase(it);
}

void WaylandInputCapture::handleX11KeyEvent(const XKeyEvent* xkey) {
    unsigned int state = xkey->state & (ShiftMask | ControlMask | m_altMask | m_superMask | m_altGrMask);
    UInt32 hotKeyId = 0;

    if (xkey->type == KeyPress) {
        auto it = m_hotKeysByCodeMod.find(std::make_pair(xkey->keycode, state));
        if (it != m_hotKeysByCodeMod.end()) {
            hotKeyId = it->second;
            m_activeX11HotKeys[xkey->keycode] = hotKeyId;
        }
    } else {
        auto it = m_activeX11HotKeys.find(xkey->keycode);
        if (it != m_activeX11HotKeys.end()) {
            hotKeyId = it->second;
            m_activeX11HotKeys.erase(it);
        } else {
            auto it2 = m_hotKeysByCodeMod.find(std::make_pair(xkey->keycode, state));
            if (it2 != m_hotKeysByCodeMod.end()) {
                hotKeyId = it2->second;
            }
        }
    }

    if (hotKeyId == 0) {
        return;
    }

    LOG((CLOG_INFO "WaylandInputCapture: X11 hotkey triggered id=%u keycode=%u type=%s",
         hotKeyId, (unsigned int)xkey->keycode, (xkey->type == KeyPress ? "Press" : "Release")));

    Event::Type type = (xkey->type == KeyPress) ?
        m_events->forIPrimaryScreen().hotKeyDown() :
        m_events->forIPrimaryScreen().hotKeyUp();

    m_events->addEvent(Event(
        type,
        m_screen->getEventTarget(),
        IPlatformScreen::HotKeyInfo::alloc(hotKeyId)));
}

void WaylandInputCapture::triggerHotKey(UInt32 id) {
    LOG((CLOG_INFO "WaylandInputCapture: Triggering hotkey id=%u", id));
    m_events->addEvent(Event(
        m_events->forIPrimaryScreen().hotKeyDown(),
        m_screen->getEventTarget(),
        IPlatformScreen::HotKeyInfo::alloc(id)));
    m_events->addEvent(Event(
        m_events->forIPrimaryScreen().hotKeyUp(),
        m_screen->getEventTarget(),
        IPlatformScreen::HotKeyInfo::alloc(id)));
}

bool WaylandInputCapture::forceCapture(WaylandPortal* portal) {
    if (m_isCaptured) {
        return true;
    }
    if (!portal || m_barriers.empty()) {
        LOG((CLOG_WARN "WaylandInputCapture: forceCapture cannot proceed: portal=%p, barriers=%zu",
             portal, m_barriers.size()));
        return false;
    }

    SInt32 curX = 960, curY = 540;
    portal->getCursorPos(curX, curY);

    EDirection preferredDir = (curX >= 960) ? kRight : kLeft;
    const BarrierEntry* targetBarrier = nullptr;
    for (const auto& b : m_barriers) {
        if (b.direction == preferredDir) {
            targetBarrier = &b;
            break;
        }
    }
    if (!targetBarrier) {
        for (const auto& b : m_barriers) {
            if (b.direction == kRight || b.direction == kLeft) {
                targetBarrier = &b;
                break;
            }
        }
    }
    if (!targetBarrier) {
        targetBarrier = &m_barriers[0];
    }

    auto tryBarrier = [&](const BarrierEntry* b) -> bool {
        int startX = 0, startY = 0;
        int hitX = 0, hitY = 0;

        hitY = (b->y1 + b->y2) / 2;

        if (b->direction == kRight) {
            hitX = b->x1;
            startX = hitX - 10;
            startY = hitY;
        } else if (b->direction == kLeft) {
            hitX = b->x1;
            startX = hitX + 10;
            startY = hitY;
        } else if (b->direction == kBottom) {
            hitX = (b->x1 + b->x2) / 2;
            hitY = b->y1;
            startX = hitX;
            startY = hitY - 10;
        } else {
            hitX = (b->x1 + b->x2) / 2;
            hitY = b->y1;
            startX = hitX;
            startY = hitY + 10;
        }

        LOG((CLOG_INFO "WaylandInputCapture: Warping cursor to trigger barrier %u (%d,%d) -> (%d,%d)",
             b->id, startX, startY, hitX, hitY));

        portal->fakeMouseMove(startX, startY);
        usleep(10000);
        portal->fakeMouseMove(hitX, hitY);

        for (int i = 0; i < 30; ++i) {
            if (m_isCaptured) {
                LOG((CLOG_INFO "WaylandInputCapture: forceCapture succeeded on barrier %u in %d ms!", b->id, i * 5));
                return true;
            }
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
            if (m_isCaptured) {
                LOG((CLOG_INFO "WaylandInputCapture: forceCapture succeeded on barrier %u in %d ms!", b->id, i * 5));
                return true;
            }
            usleep(5000);
        }
        return false;
    };

    if (tryBarrier(targetBarrier)) {
        return true;
    }

    // Fallback: try any other barrier
    for (const auto& b : m_barriers) {
        if (&b != targetBarrier) {
            if (tryBarrier(&b)) {
                return true;
            }
        }
    }

    LOG((CLOG_WARN "WaylandInputCapture: forceCapture timed out waiting for Activated signal"));
    return false;
}

void WaylandInputCapture::setupCommandSocket() {
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    if (runtimeDir && runtimeDir[0] != '\0') {
        m_cmdSockPath = std::string(runtimeDir) + "/barrier.sock";
    } else {
        m_cmdSockPath = "/tmp/barrier-" + std::to_string(getuid()) + ".sock";
    }

    unlink(m_cmdSockPath.c_str());

    m_cmdSock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (m_cmdSock < 0) {
        LOG((CLOG_WARN "WaylandInputCapture: Failed to create control socket: %s", strerror(errno)));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_cmdSockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(m_cmdSock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG((CLOG_WARN "WaylandInputCapture: Failed to bind control socket to %s: %s",
             m_cmdSockPath.c_str(), strerror(errno)));
        close(m_cmdSock);
        m_cmdSock = -1;
        return;
    }

    if (listen(m_cmdSock, 5) < 0) {
        LOG((CLOG_WARN "WaylandInputCapture: Failed to listen on control socket: %s", strerror(errno)));
        close(m_cmdSock);
        m_cmdSock = -1;
        return;
    }

    LOG((CLOG_INFO "WaylandInputCapture: Control socket listening on %s", m_cmdSockPath.c_str()));
}

void WaylandInputCapture::cleanupCommandSocket() {
    if (m_cmdSock >= 0) {
        close(m_cmdSock);
        m_cmdSock = -1;
    }
    if (!m_cmdSockPath.empty()) {
        unlink(m_cmdSockPath.c_str());
        m_cmdSockPath.clear();
    }
}

void WaylandInputCapture::processCommandSocket() {
    if (m_cmdSock < 0) return;

    int clientFd = accept(m_cmdSock, nullptr, nullptr);
    if (clientFd < 0) return;

    char buf[256];
    ssize_t n = read(clientFd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string cmd(buf);
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
            cmd.pop_back();
        }
        handleSocketCommand(cmd);
        const char* reply = "OK\n";
        write(clientFd, reply, strlen(reply));
    }
    close(clientFd);
}

void WaylandInputCapture::handleSocketCommand(const std::string& cmd) {
    if (cmd.empty()) return;

    std::string arg = cmd;
    if (arg.rfind("switch ", 0) == 0) {
        arg = arg.substr(7);
    } else if (arg.rfind("hotkey ", 0) == 0) {
        arg = arg.substr(7);
    } else if (arg.rfind("key ", 0) == 0) {
        arg = arg.substr(4);
    }

    while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t')) arg.erase(arg.begin());
    while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t')) arg.pop_back();

    if (arg.empty()) return;

    LOG((CLOG_INFO "WaylandInputCapture: Handling socket command for target: '%s'", arg.c_str()));

    UInt32 targetId = 0;
    if (arg.size() == 1 && arg[0] >= '0' && arg[0] <= '9') {
        KeyID targetKey = static_cast<KeyID>(arg[0]);
        std::lock_guard<std::recursive_mutex> lock(m_hotKeyMutex);
        for (const auto& kv : m_hotKeys) {
            if (kv.second.key == targetKey) {
                targetId = kv.first;
                break;
            }
        }
    }

    if (targetId == 0) {
        try {
            unsigned long val = std::stoul(arg);
            if (val > 0) {
                targetId = static_cast<UInt32>(val);
            }
        } catch (...) {}
    }

    if (targetId == 0) {
        std::string lowerArg = arg;
        for (char& c : lowerArg) c = tolower(c);

        if (lowerArg.find("sync") != std::string::npos) targetId = 1;
        else if (lowerArg.find("wesley") != std::string::npos || lowerArg.find("inspiron") != std::string::npos) targetId = 2;
        else if (lowerArg.find("mac") != std::string::npos) targetId = 3;
        else if (lowerArg.find("nb0140") != std::string::npos || lowerArg.find("its") != std::string::npos || lowerArg.find("local") != std::string::npos) targetId = 4;
        else if (lowerArg.find("w1934") != std::string::npos) targetId = 5;
        else if (lowerArg.find("dmais") != std::string::npos || lowerArg.find("oem") != std::string::npos) targetId = 6;
    }

    if (targetId > 0) {
        triggerHotKey(targetId);
    } else {
        LOG((CLOG_WARN "WaylandInputCapture: Unrecognized socket command target: '%s'", arg.c_str()));
    }
}
