#pragma once

#include "base/IEventQueue.h"
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>

// Forward declarations for DBus and libei
struct DBusConnection;
struct ei;
struct ei_device;
struct ei_event;

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
    void fakeKey(uint32_t keycode, bool press);

    void enter();
    void leave();

    void getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const;
    void getCursorPos(SInt32& x, SInt32& y) const;

private:
    struct DeviceState {
        struct ei_device* device;
        bool resumed;
        bool emulating;
    };

    IEventQueue* m_events;
    DBusConnection* m_dbusConn;
    std::string m_sessionHandle;

    mutable std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_running;

    // libei contexts
    struct ei* m_ei;
    struct ei_device* m_eiPointerAbs;
    struct ei_device* m_eiPointerRel;
    struct ei_device* m_eiButton;
    struct ei_device* m_eiScroll;
    struct ei_device* m_eiKeyboard;

    std::map<struct ei_device*, DeviceState> m_deviceStates;
    uint32_t m_sequence;

    // Screen geometry from regions
    int32_t m_x;
    int32_t m_y;
    uint32_t m_width;
    uint32_t m_height;

    // Cursor position tracking
    int m_lastX;
    int m_lastY;

    int connectToMutterEIS();
    bool connectToDBus();
    bool createSession();
    bool selectDevices();
    bool startSession();
    int connectToEIS();
    
    bool setupLibEI(int fd);
    void pollEvents();
    void handleEIEvent(struct ei_event* ev);
    void ensureEmulating(struct ei_device* dev);
};
