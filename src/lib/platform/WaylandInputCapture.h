#pragma once

#include "base/IEventQueue.h"
#include "barrier/protocol_types.h"
#include "barrier/key_types.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

struct DBusConnection;
struct DBusMessage;
struct ei;
struct ei_event;

class WaylandScreen;

class WaylandInputCapture {
public:
    WaylandInputCapture(IEventQueue* events, WaylandScreen* screen);
    ~WaylandInputCapture();

    bool init();
    void cleanup();

    void reconfigure(UInt32 activeSides);
    void releaseCapture();

    bool hasZones() const;
    void getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const;

private:
    void pollEvents();
    void handleDBusMessage(DBusMessage* msg);
    void handleEIEvent(struct ei_event* ev);

    bool connectToDBus();
    bool createSession();
    bool getZones();
    bool clearBarriers();
    bool addBarrier(uint32_t serial, int32_t x1, int32_t y1, int32_t x2, int32_t y2, EDirection dir);
    bool enableCapture();
    bool disableCapture();
    int  connectToEIS();
    bool setupLibEI(int fd);

    void updateModifiers(uint32_t scancode, bool isPress);
    KeyID scancodeToKeyID(uint32_t scancode) const;

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

    KeyModifierMask m_activeModifiers;
};
