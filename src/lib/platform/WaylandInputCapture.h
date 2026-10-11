#pragma once

#include "base/IEventQueue.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>

struct DBusConnection;
struct ei;
struct ei_device;

class WaylandInputCapture {
public:
    WaylandInputCapture(IEventQueue* events);
    ~WaylandInputCapture();

    bool init();
    void cleanup();

    // To be called when Barrier determines it should give the pointer back
    void releaseCapture();

private:
    void pollEvents();

    IEventQueue* m_events;
    struct ei* m_ei;
    
    std::thread m_thread;
    std::atomic<bool> m_running;
    
    DBusConnection* m_dbusConn;
    std::string m_sessionHandle;

    // libei contexts (eis receiver)
    uint32_t m_zoneSet;
    struct Zone {
        uint32_t width, height;
        int32_t x, y;
    };
    std::vector<Zone> m_zones;

    bool connectToDBus();
    bool createSession();
    bool getZones();
    bool setPointerBarriers();
    bool enableCapture();
    int connectToEIS();

    bool waitForZonesResponse(DBusConnection* conn, const std::string& requestPath);
    bool waitForBarriersResponse(DBusConnection* conn, const std::string& requestPath);
    
    bool setupLibEI(int fd);
};
