#pragma once

#include "base/IEventQueue.h"
#include <string>

// Forward declarations for DBus and libei
struct DBusConnection;
struct ei;
struct ei_device;

class WaylandPortal {
public:
    WaylandPortal(IEventQueue* events);
    ~WaylandPortal();

    bool init();
    void cleanup();

    // Input injection methods
    void fakeMouseMove(int x, int y);
    void fakeMouseRelativeMove(int dx, int dy);
    void fakeMouseButton(int button, bool press);
    void fakeMouseWheel(int xDelta, int yDelta);

private:
    IEventQueue* m_events;
    DBusConnection* m_dbusConn;
    std::string m_sessionHandle;

    // libei contexts
    struct ei* m_ei;
    struct ei_device* m_eiPointer;
    struct ei_device* m_eiKeyboard;

    bool connectToDBus();
    bool createSession();
    bool selectDevices();
    bool startSession();
    int connectToEIS();
    
    bool setupLibEI(int fd);
};
