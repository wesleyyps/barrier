#pragma once

#include "base/IEventQueue.h"
#include "barrier/protocol_types.h"
#include "barrier/key_types.h"
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <X11/Xlib.h>

struct DBusConnection;
struct DBusMessage;
struct ei;
struct ei_event;

class WaylandScreen;
class WaylandPortal;

class WaylandInputCapture {
public:
    WaylandInputCapture(IEventQueue* events, WaylandScreen* screen);
    ~WaylandInputCapture();

    bool init();
    void cleanup();

    void reconfigure(UInt32 activeSides);
    void releaseCapture();
    bool forceCapture(WaylandPortal* portal);
    bool isCaptured() const { return m_isCaptured; }

    UInt32 registerHotKey(KeyID key, KeyModifierMask mask);
    void unregisterHotKey(UInt32 id);
    void triggerHotKey(UInt32 id);

    bool hasZones() const;
    void getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const;

private:
    void pollEvents();
    void handleDBusMessage(DBusMessage* msg);
    void handleEIEvent(struct ei_event* ev);
    void setupCommandSocket();
    void cleanupCommandSocket();
    void processCommandSocket();
    void handleSocketCommand(const std::string& cmd);

    bool connectToDBus();
    bool createSession();
    bool getZones();
    bool clearBarriers();
    bool addBarrier(uint32_t serial, int32_t x1, int32_t y1, int32_t x2, int32_t y2, EDirection dir);
    bool enableCapture();
    bool disableCapture();
    int  connectToEIS();
    bool setupLibEI(int fd);

    bool initX11();
    void cleanupX11();
    void initModifierMasks();

    void updateModifiers(uint32_t scancode, bool isPress);
    KeyID scancodeToKeyID(uint32_t scancode) const;
    KeyID fallbackScancodeToKeyID(uint32_t scancode) const;

    IEventQueue* m_events;
    WaylandScreen* m_screen;
    struct ei* m_ei;

    std::thread m_thread;
    std::atomic<bool> m_running;

    DBusConnection* m_dbusConn;
    std::mutex m_dbusMutex;
    std::string m_sessionHandle;

    uint32_t m_zoneSerial;
    struct Zone {
        uint32_t width, height;
        int32_t x, y;
    };
    std::vector<Zone> m_zones;

    struct BarrierEntry {
        uint32_t id;
        EDirection direction;
        int32_t x1, y1, x2, y2;
    };
    std::vector<BarrierEntry> m_barriers;

    std::atomic<bool> m_isCaptured;
    uint32_t m_currentActivationId;
    UInt32 m_activeSides;

    void resetScrollState();

    KeyModifierMask m_activeModifiers;
    Display* m_xDisplay;
    unsigned int m_altMask;
    unsigned int m_altGrMask;
    unsigned int m_superMask;
    unsigned int m_numLockMask;
    std::map<uint32_t, KeyID> m_pressedKeys;

    // Scroll state tracking across EI frame events
    bool m_frameHasDiscreteScroll;
    int32_t m_frameDiscreteX;
    int32_t m_frameDiscreteY;
    double m_frameDeltaX;
    double m_frameDeltaY;
    double m_smoothScrollAccX;
    double m_smoothScrollAccY;

    // Hotkey support
    KeyCode keyIDToKeyCode(KeyID key) const;
    unsigned int mapModifiersToX(KeyModifierMask mask) const;
    void handleX11KeyEvent(const XKeyEvent* xkey);

    struct HotKeyItem {
        UInt32 id;
        KeyID key;
        KeyModifierMask mask;
        KeyCode keycode;
        unsigned int xMods;
    };

    std::recursive_mutex m_hotKeyMutex;
    UInt32 m_nextHotKeyId;
    std::map<UInt32, HotKeyItem> m_hotKeys;
    std::map<std::pair<KeyCode, unsigned int>, UInt32> m_hotKeysByCodeMod;
    std::map<std::pair<KeyID, KeyModifierMask>, UInt32> m_hotKeysByKeyMask;
    std::map<uint32_t, UInt32> m_activeCapturedHotKeys;
    std::map<KeyCode, UInt32> m_activeX11HotKeys;

    int m_cmdSock;
    std::string m_cmdSockPath;
};
