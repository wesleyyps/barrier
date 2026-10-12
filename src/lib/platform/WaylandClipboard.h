#pragma once

#include "barrier/IClipboard.h"
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

class WaylandClipboard : public IClipboard {
public:
    WaylandClipboard();
    virtual ~WaylandClipboard();

    // IClipboard overrides
    virtual bool empty() override;
    virtual void add(EFormat format, const String& data) override;
    virtual bool open(Time time) const override;
    virtual void close() const override;
    virtual Time getTime() const override;
    virtual bool has(EFormat format) const override;
    virtual String get(EFormat format) const override;

    // Custom methods
    bool hasChanged();
    void setLocalClipboard(const String& data);

private:
    void threadFunc();
    void handleXEvent(const XEvent& ev);
    std::string requestSystemText() const;
    void stop();

    Display* m_display;
    Window m_window;
    std::thread m_thread;
    std::atomic<bool> m_running;
    int m_wakePipe[2];

    Atom m_atomClipboard;
    Atom m_atomPrimary;
    Atom m_atomTargets;
    Atom m_atomUtf8;
    Atom m_atomString;
    Atom m_atomText;
    Atom m_atomProperty;

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_readCv;
    bool m_ownedByMe;
    bool m_selectionLost;
    std::string m_ownedText;
    std::string m_writeText;
    bool m_pendingWrite;
    mutable bool m_pendingRead;
    mutable bool m_readDone;
    mutable std::string m_readResultText;

    mutable bool m_open;
    mutable Time m_time;
    mutable std::string m_currentText;
    mutable std::string m_cachedSystemText;
    std::string m_lastSentText;
    mutable UInt32 m_timeOwned;
};
