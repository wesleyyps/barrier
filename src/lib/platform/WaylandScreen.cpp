#include "platform/WaylandScreen.h"
#include "platform/WaylandPortal.h"
#include "platform/WaylandClipboard.h"
#include "platform/WaylandKeyState.h"
#include "barrier/Clipboard.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"
#include "barrier/IPrimaryScreen.h"
#include <libei.h>
#include <linux/input-event-codes.h>

static uint32_t keyIDToEvdev(KeyID id) {
    if (id >= 'a' && id <= 'z') {
        static const uint32_t letters[] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
            KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
            KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
        };
        return letters[id - 'a'];
    }
    if (id >= 'A' && id <= 'Z') {
        static const uint32_t letters[] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
            KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
            KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
        };
        return letters[id - 'A'];
    }
    if (id >= '1' && id <= '9') {
        return KEY_1 + (id - '1');
    }
    if (id == '0') return KEY_0;

    switch (id) {
    case '!': return KEY_1;
    case '@': return KEY_2;
    case '#': return KEY_3;
    case '$': return KEY_4;
    case '%': return KEY_5;
    case '^': return KEY_6;
    case '&': return KEY_7;
    case '*': return KEY_8;
    case '(': return KEY_9;
    case ')': return KEY_0;
    case '-': case '_': return KEY_MINUS;
    case '=': case '+': return KEY_EQUAL;
    case '[': case '{': return KEY_LEFTBRACE;
    case ']': case '}': return KEY_RIGHTBRACE;
    case ';': case ':': return KEY_SEMICOLON;
    case '\'': case '"': return KEY_APOSTROPHE;
    case '`': case '~': return KEY_GRAVE;
    case '\\': case '|': return KEY_BACKSLASH;
    case ',': case '<': return KEY_COMMA;
    case '.': case '>': return KEY_DOT;
    case '/': case '?': return KEY_SLASH;
    case ' ': return KEY_SPACE;

    case kKeyReturn: return KEY_ENTER;
    case kKeyEscape: return KEY_ESC;
    case kKeyBackSpace: return KEY_BACKSPACE;
    case kKeyTab: return KEY_TAB;
    case kKeyLinefeed: return KEY_ENTER;

    case kKeyShift_L: return KEY_LEFTSHIFT;
    case kKeyShift_R: return KEY_RIGHTSHIFT;
    case kKeyControl_L: return KEY_LEFTCTRL;
    case kKeyControl_R: return KEY_RIGHTCTRL;
    case kKeyAlt_L: return KEY_LEFTALT;
    case kKeyAlt_R: return KEY_RIGHTALT;
    case kKeySuper_L: return KEY_LEFTMETA;
    case kKeySuper_R: return KEY_RIGHTMETA;
    case kKeyCapsLock: return KEY_CAPSLOCK;
    case kKeyScrollLock: return KEY_SCROLLLOCK;
    case kKeyNumLock: return KEY_NUMLOCK;

    case kKeyInsert: return KEY_INSERT;
    case kKeyDelete: return KEY_DELETE;
    case kKeyHome: return KEY_HOME;
    case kKeyEnd: return KEY_END;
    case kKeyPageUp: return KEY_PAGEUP;
    case kKeyPageDown: return KEY_PAGEDOWN;
    case kKeyLeft: return KEY_LEFT;
    case kKeyUp: return KEY_UP;
    case kKeyRight: return KEY_RIGHT;
    case kKeyDown: return KEY_DOWN;
    case kKeyPrint: return KEY_SYSRQ;
    case kKeyPause: return KEY_PAUSE;
    case kKeyMenu: return KEY_COMPOSE;

    case kKeyF1: return KEY_F1;
    case kKeyF2: return KEY_F2;
    case kKeyF3: return KEY_F3;
    case kKeyF4: return KEY_F4;
    case kKeyF5: return KEY_F5;
    case kKeyF6: return KEY_F6;
    case kKeyF7: return KEY_F7;
    case kKeyF8: return KEY_F8;
    case kKeyF9: return KEY_F9;
    case kKeyF10: return KEY_F10;
    case kKeyF11: return KEY_F11;
    case kKeyF12: return KEY_F12;

    case kKeyKP_0: return KEY_KP0;
    case kKeyKP_1: return KEY_KP1;
    case kKeyKP_2: return KEY_KP2;
    case kKeyKP_3: return KEY_KP3;
    case kKeyKP_4: return KEY_KP4;
    case kKeyKP_5: return KEY_KP5;
    case kKeyKP_6: return KEY_KP6;
    case kKeyKP_7: return KEY_KP7;
    case kKeyKP_8: return KEY_KP8;
    case kKeyKP_9: return KEY_KP9;
    case kKeyKP_Enter: return KEY_KPENTER;
    case kKeyKP_Add: return KEY_KPPLUS;
    case kKeyKP_Subtract: return KEY_KPMINUS;
    case kKeyKP_Multiply: return KEY_KPASTERISK;
    case kKeyKP_Divide: return KEY_KPSLASH;
    case kKeyKP_Decimal: return KEY_KPDOT;
    default:
        return 0;
    }
}

uint32_t WaylandScreen::resolveKey(KeyID id, KeyButton button) const {
    uint32_t scancode = 0;
    if (button >= 8 && button <= 255) {
        scancode = button - 8;
    }
    if (scancode == 0) {
        scancode = keyIDToEvdev(id);
    }
    return scancode;
}

WaylandScreen::WaylandScreen(bool isPrimary, IEventQueue* events)
    : PlatformScreen(events),
      m_portal(new WaylandPortal(events)),
      m_capture(new WaylandInputCapture(events)),
      m_clipboard(new WaylandClipboard()),
      m_keyState(new WaylandKeyState(events)),
      m_events(events),
      m_isPrimary(isPrimary),
      m_clipboardTimer(nullptr),
      m_sequenceNumber(0) {
    LOG((CLOG_WARN "Wayland screen initialized (libei input injection enabled)"));
    
    if (!m_isPrimary) {
        LOG((CLOG_INFO "Initializing WaylandScreen via DBus Portal for Client..."));
        if (!m_portal->init()) {
            LOG((CLOG_ERR "WaylandPortal failed to initialize. Input injection will not work."));
        }
    } else {
        LOG((CLOG_INFO "Initializing WaylandScreen InputCapture for Server..."));
        if (!m_capture->init()) {
            LOG((CLOG_ERR "WaylandInputCapture failed to initialize."));
        }
    }
}

WaylandScreen::~WaylandScreen() {
    disable();
    delete m_portal;
    delete m_capture;
    delete m_clipboard;
    delete m_keyState;
}

void* WaylandScreen::getEventTarget() const { return const_cast<WaylandScreen*>(this); }
bool WaylandScreen::getClipboard(ClipboardID id, IClipboard* c) const {
    if (!m_clipboard || !c) {
        return false;
    }
    return Clipboard::copy(c, m_clipboard);
}
void WaylandScreen::getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const {
    if (!m_isPrimary && m_portal) {
        m_portal->getShape(x, y, width, height);
    } else {
        x = 0; y = 0; width = 1920; height = 1080;
    }
}
void WaylandScreen::getCursorPos(SInt32& x, SInt32& y) const {
    if (!m_isPrimary && m_portal) {
        m_portal->getCursorPos(x, y);
    } else {
        x = 0; y = 0;
    }
}

void WaylandScreen::reconfigure(UInt32 activeSides) { }
void WaylandScreen::warpCursor(SInt32 x, SInt32 y) {
    fakeMouseMove(x, y);
}
UInt32 WaylandScreen::registerHotKey(KeyID key, KeyModifierMask mask) { return 0; }
void WaylandScreen::unregisterHotKey(UInt32 id) { }
void WaylandScreen::fakeInputBegin() { }
void WaylandScreen::fakeInputEnd() { }
SInt32 WaylandScreen::getJumpZoneSize() const { return 0; }
bool WaylandScreen::isAnyMouseButtonDown(UInt32& buttonID) const {
    if (!m_buttonsDown.empty()) {
        buttonID = *m_buttonsDown.begin();
        return true;
    }
    buttonID = 0;
    return false;
}
void WaylandScreen::getCursorCenter(SInt32& x, SInt32& y) const {
    SInt32 sx, sy, sw, sh;
    getShape(sx, sy, sw, sh);
    x = sx + sw / 2;
    y = sy + sh / 2;
}

void WaylandScreen::fakeMouseButton(ButtonID id, bool press) {
    LOG((CLOG_DEBUG "WaylandScreen: fakeMouseButton %d %s", id, press ? "press" : "release"));
    if (press) {
        m_buttonsDown.insert(id);
    } else {
        m_buttonsDown.erase(id);
    }
    m_portal->fakeMouseButton(id, press);
}

void WaylandScreen::fakeMouseMove(SInt32 x, SInt32 y) {
    LOG((CLOG_DEBUG "WaylandScreen: fakeMouseMove %d %d", x, y));
    m_portal->fakeMouseMove(x, y);
}

void WaylandScreen::fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const {
    LOG((CLOG_DEBUG "WaylandScreen: fakeMouseRelativeMove %d %d", dx, dy));
    m_portal->fakeMouseRelativeMove(dx, dy);
}

void WaylandScreen::fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const {
    LOG((CLOG_DEBUG "WaylandScreen: fakeMouseWheel %d %d", xDelta, yDelta));
    m_portal->fakeMouseWheel(xDelta, yDelta);
}

void WaylandScreen::fakeKeyDown(KeyID id, KeyModifierMask mask, KeyButton button) {
    uint32_t scancode = resolveKey(id, button);
    LOG((CLOG_DEBUG "WaylandScreen: fakeKeyDown id=0x%04x button=0x%04x -> scancode=%u", id, button, scancode));
    if (scancode != 0) {
        m_serverKeyToScancode[button] = scancode;
        m_pressedKeys.insert(scancode);
        m_portal->fakeKey(scancode, true);
    }
}

bool WaylandScreen::fakeKeyUp(KeyButton button) {
    LOG((CLOG_DEBUG "WaylandScreen: fakeKeyUp button=0x%04x", button));
    auto it = m_serverKeyToScancode.find(button);
    if (it != m_serverKeyToScancode.end()) {
        uint32_t scancode = it->second;
        m_portal->fakeKey(scancode, false);
        m_pressedKeys.erase(scancode);
        m_serverKeyToScancode.erase(it);
        return true;
    } else if (button >= 8 && button <= 255) {
        uint32_t scancode = button - 8;
        m_portal->fakeKey(scancode, false);
        m_pressedKeys.erase(scancode);
        return true;
    }
    return false;
}

bool WaylandScreen::fakeKeyRepeat(KeyID id, KeyModifierMask mask, SInt32 count, KeyButton button) {
    auto it = m_serverKeyToScancode.find(button);
    if (it != m_serverKeyToScancode.end()) {
        m_portal->fakeKey(it->second, true);
        return true;
    }
    return false;
}

void WaylandScreen::fakeAllKeysUp() {
    LOG((CLOG_DEBUG "WaylandScreen: fakeAllKeysUp"));
    for (uint32_t scancode : m_pressedKeys) {
        m_portal->fakeKey(scancode, false);
    }
    m_pressedKeys.clear();
    m_serverKeyToScancode.clear();
}

bool WaylandScreen::isKeyDown(KeyButton button) const {
    auto it = m_serverKeyToScancode.find(button);
    if (it != m_serverKeyToScancode.end()) {
        return m_pressedKeys.count(it->second) > 0;
    }
    return false;
}

void WaylandScreen::pollPressedKeys(KeyButtonSet& pressedKeys) const {
    for (const auto& p : m_serverKeyToScancode) {
        pressedKeys.insert(p.first);
    }
}

void WaylandScreen::enable() {
    LOG((CLOG_INFO "WaylandScreen: enable() called, starting clipboard timer"));
    if (m_clipboardTimer == nullptr) {
        m_clipboardTimer = m_events->newTimer(1.0, nullptr);
        m_events->adoptHandler(Event::kTimer, m_clipboardTimer,
                                new TMethodEventJob<WaylandScreen>(this,
                                    &WaylandScreen::handleClipboardCheck));
    }
}

void WaylandScreen::disable() {
    if (m_clipboardTimer != nullptr) {
        m_events->removeHandler(Event::kTimer, m_clipboardTimer);
        m_events->deleteTimer(m_clipboardTimer);
        m_clipboardTimer = nullptr;
    }
}

void WaylandScreen::enter() {
    LOG((CLOG_DEBUG "WaylandScreen: enter"));
    m_portal->enter();
}

bool WaylandScreen::leave() {
    LOG((CLOG_DEBUG "WaylandScreen: leave"));
    checkClipboards();
    fakeAllKeysUp();
    m_buttonsDown.clear();
    m_portal->leave();
    return true;
}

bool WaylandScreen::setClipboard(ClipboardID id, const IClipboard* c) {
    if (!m_clipboard) {
        return false;
    }
    if (c != nullptr) {
        if (const_cast<IClipboard*>(c)->open(0)) {
            String text;
            if (c->has(IClipboard::kText)) {
                text = c->get(IClipboard::kText);
            }
            c->close();
            LOG((CLOG_INFO "WaylandScreen: setClipboard received %zu bytes from remote", text.size()));
            m_clipboard->setLocalClipboard(text);
            return true;
        }
        return false;
    } else {
        m_clipboard->setLocalClipboard("");
        return true;
    }
}

void WaylandScreen::checkClipboards() {
    if (!m_clipboard) {
        return;
    }
    if (m_clipboard->hasChanged()) {
        LOG((CLOG_INFO "WaylandScreen: clipboard changed, notifying Barrier"));
        sendClipboardEvent(m_events->forClipboard().clipboardGrabbed(), kClipboardClipboard);
        sendClipboardEvent(m_events->forClipboard().clipboardGrabbed(), kClipboardSelection);
    }
}

void WaylandScreen::sendClipboardEvent(Event::Type type, ClipboardID id) {
    ClipboardInfo* info = (ClipboardInfo*)malloc(sizeof(ClipboardInfo));
    if (!info) {
        return;
    }
    info->m_id = id;
    info->m_sequenceNumber = m_sequenceNumber;
    m_events->addEvent(Event(type, getEventTarget(), info));
}

void WaylandScreen::handleClipboardCheck(const Event&, void*) {
    checkClipboards();
}

void WaylandScreen::openScreensaver(bool notify) { }
void WaylandScreen::closeScreensaver() { }
void WaylandScreen::screensaver(bool activate) { }
void WaylandScreen::resetOptions() { }
void WaylandScreen::setOptions(const OptionsList& options) { }
void WaylandScreen::setSequenceNumber(UInt32 seq) {
    m_sequenceNumber = seq;
}
bool WaylandScreen::isPrimary() const { return m_isPrimary; }

void WaylandScreen::updateButtons() { }
IKeyState* WaylandScreen::getKeyState() const { return m_keyState; }

void WaylandScreen::handleSystemEvent(const Event& event, void*) {
    if (event.getType() != Event::kSystem) return;
    
    struct ei_event* ev = static_cast<struct ei_event*>(event.getData());
    if (!ev) return;
    
    enum ei_event_type type = ei_event_get_type(ev);
    
    if (type == EI_EVENT_POINTER_MOTION) {
        double dx = ei_event_pointer_get_dx(ev);
        double dy = ei_event_pointer_get_dy(ev);
        
        LOG((CLOG_DEBUG "WaylandScreen: got motion %f, %f", dx, dy));
        
        m_events->addEvent(Event(
            m_events->forIPrimaryScreen().motionOnSecondary(),
            getEventTarget(),
            IPrimaryScreen::MotionInfo::alloc(dx, dy)));
    }
    
    ei_event_unref(ev);
}
