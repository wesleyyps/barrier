#include "platform/WaylandScreen.h"
#include "platform/WaylandPortal.h"
#include "platform/WaylandClipboard.h"
#include "platform/WaylandKeyState.h"
#include "base/Log.h"
#include "barrier/IPrimaryScreen.h"
#include <libei.h>

WaylandScreen::WaylandScreen(bool isPrimary, IEventQueue* events) : PlatformScreen(events), m_portal(new WaylandPortal(events)), m_capture(new WaylandInputCapture(events)), m_clipboard(new WaylandClipboard()), m_keyState(new WaylandKeyState(events)), m_events(events), m_isPrimary(isPrimary) {
    LOG((CLOG_WARN "WARNING: Wayland support is highly experimental and incomplete!"));
    
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
    delete m_portal;
    delete m_capture;
    delete m_clipboard;
    delete m_keyState;
}

void* WaylandScreen::getEventTarget() const { return const_cast<WaylandScreen*>(this); }
bool WaylandScreen::getClipboard(ClipboardID id, IClipboard* c) const {
    return false;
}
void WaylandScreen::getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const { x = 0; y = 0; width = 1920; height = 1080; }
void WaylandScreen::getCursorPos(SInt32& x, SInt32& y) const { x = 0; y = 0; }

void WaylandScreen::reconfigure(UInt32 activeSides) { }
void WaylandScreen::warpCursor(SInt32 x, SInt32 y) { }
UInt32 WaylandScreen::registerHotKey(KeyID key, KeyModifierMask mask) { return 0; }
void WaylandScreen::unregisterHotKey(UInt32 id) { }
void WaylandScreen::fakeInputBegin() { }
void WaylandScreen::fakeInputEnd() { }
SInt32 WaylandScreen::getJumpZoneSize() const { return 0; }
bool WaylandScreen::isAnyMouseButtonDown(UInt32& buttonID) const { return false; }
void WaylandScreen::getCursorCenter(SInt32& x, SInt32& y) const { x = 0; y = 0; }

void WaylandScreen::fakeMouseButton(ButtonID id, bool press) {
    LOG((CLOG_DEBUG "WaylandScreen: fakeMouseButton %d %s", id, press ? "press" : "release"));
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

void WaylandScreen::enable() { }
void WaylandScreen::disable() { }
void WaylandScreen::enter() { }
bool WaylandScreen::leave() { return true; }
bool WaylandScreen::setClipboard(ClipboardID id, const IClipboard* c) {
    return false;
}
void WaylandScreen::checkClipboards() { }
void WaylandScreen::openScreensaver(bool notify) { }
void WaylandScreen::closeScreensaver() { }
void WaylandScreen::screensaver(bool activate) { }
void WaylandScreen::resetOptions() { }
void WaylandScreen::setOptions(const OptionsList& options) { }
void WaylandScreen::setSequenceNumber(UInt32) { }
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
