#include "platform/WaylandClipboard.h"
#include "base/Log.h"
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <chrono>

static UInt32 getCurrentTimeMs() {
    return static_cast<UInt32>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

WaylandClipboard::WaylandClipboard()
    : m_display(nullptr),
      m_window(0),
      m_running(false),
      m_atomClipboard(None),
      m_atomPrimary(None),
      m_atomTargets(None),
      m_atomUtf8(None),
      m_atomString(None),
      m_atomText(None),
      m_atomProperty(None),
      m_ownedByMe(false),
      m_selectionLost(false),
      m_pendingWrite(false),
      m_pendingRead(false),
      m_readDone(false),
      m_open(false),
      m_time(0),
      m_timeOwned(getCurrentTimeMs())
{
    m_wakePipe[0] = -1;
    m_wakePipe[1] = -1;

    m_display = XOpenDisplay(NULL);
    if (!m_display) {
        LOG((CLOG_WARN "WaylandClipboard: could not open X display for clipboard bridge"));
        return;
    }

    Window root = DefaultRootWindow(m_display);
    m_window = XCreateSimpleWindow(m_display, root, -10, -10, 1, 1, 0, 0, 0);

    m_atomClipboard = XInternAtom(m_display, "CLIPBOARD", False);
    m_atomPrimary   = XInternAtom(m_display, "PRIMARY", False);
    m_atomTargets   = XInternAtom(m_display, "TARGETS", False);
    m_atomUtf8      = XInternAtom(m_display, "UTF8_STRING", False);
    m_atomString    = XA_STRING;
    m_atomText      = XInternAtom(m_display, "TEXT", False);
    m_atomProperty  = XInternAtom(m_display, "BARRIER_SELECTION", False);
    m_atomTextUriList = XInternAtom(m_display, "text/uri-list", False);
    m_atomGnomeCopiedFiles = XInternAtom(m_display, "x-special/gnome-copied-files", False);

    if (pipe(m_wakePipe) != 0) {
        LOG((CLOG_ERR "WaylandClipboard: failed to create wake pipe"));
        XDestroyWindow(m_display, m_window);
        XCloseDisplay(m_display);
        m_display = nullptr;
        return;
    }
    fcntl(m_wakePipe[0], F_SETFL, O_NONBLOCK);
    fcntl(m_wakePipe[1], F_SETFL, O_NONBLOCK);

    m_running = true;
    m_thread = std::thread(&WaylandClipboard::threadFunc, this);
    LOG((CLOG_INFO "WaylandClipboard: Xwayland clipboard bridge initialized"));
}

WaylandClipboard::~WaylandClipboard() {
    stop();
}

void WaylandClipboard::stop() {
    if (!m_running) {
        return;
    }
    m_running = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_readDone = true;
        m_readCv.notify_all();
    }

    if (m_wakePipe[1] != -1) {
        ssize_t res = ::write(m_wakePipe[1], "Q", 1);
        (void)res;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    if (m_wakePipe[0] != -1) {
        ::close(m_wakePipe[0]);
        ::close(m_wakePipe[1]);
        m_wakePipe[0] = -1;
        m_wakePipe[1] = -1;
    }

    if (m_display) {
        XDestroyWindow(m_display, m_window);
        XCloseDisplay(m_display);
        m_display = nullptr;
    }
}

void WaylandClipboard::threadFunc() {
    int x11Fd = ConnectionNumber(m_display);
    struct pollfd pfd[2];
    pfd[0].fd = x11Fd;
    pfd[0].events = POLLIN;
    pfd[1].fd = m_wakePipe[0];
    pfd[1].events = POLLIN;

    while (m_running) {
        while (XPending(m_display)) {
            XEvent ev;
            XNextEvent(m_display, &ev);
            handleXEvent(ev);
        }

        int ret = poll(pfd, 2, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        while (XPending(m_display)) {
            XEvent ev;
            XNextEvent(m_display, &ev);
            handleXEvent(ev);
        }

        if (pfd[1].revents & POLLIN) {
            char buf[64];
            while (read(m_wakePipe[0], buf, sizeof(buf)) > 0) {}

            bool doWrite = false;
            bool doRead = false;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_pendingWrite) {
                    m_pendingWrite = false;
                    doWrite = true;
                }
                if (m_pendingRead) {
                    m_pendingRead = false;
                    doRead = true;
                }
            }

            if (doWrite) {
                XSetSelectionOwner(m_display, m_atomClipboard, m_window, CurrentTime);
                XSetSelectionOwner(m_display, m_atomPrimary, m_window, CurrentTime);
                XFlush(m_display);
            }

            if (doRead) {
                XConvertSelection(m_display, m_atomClipboard, m_atomUtf8, m_atomProperty, m_window, CurrentTime);
                XFlush(m_display);
            }
        }
    }
}

void WaylandClipboard::handleXEvent(const XEvent& ev) {
    if (ev.type == SelectionRequest) {
        const XSelectionRequestEvent& req = ev.xselectionrequest;
        XEvent reply;
        memset(&reply, 0, sizeof(reply));
        reply.xselection.type = SelectionNotify;
        reply.xselection.display = req.display;
        reply.xselection.requestor = req.requestor;
        reply.xselection.selection = req.selection;
        reply.xselection.target = req.target;
        reply.xselection.time = req.time;
        reply.xselection.property = None;

        if (req.target == m_atomTargets) {
            Atom targets[6];
            int ntargets = 0;
            targets[ntargets++] = m_atomTargets;
            targets[ntargets++] = m_atomUtf8;
            targets[ntargets++] = m_atomString;
            targets[ntargets++] = m_atomText;
            bool isFile = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                isFile = (m_ownedText.rfind("file://", 0) == 0);
            }
            if (isFile) {
                targets[ntargets++] = m_atomTextUriList;
                targets[ntargets++] = m_atomGnomeCopiedFiles;
            }
            XChangeProperty(m_display, req.requestor, req.property, XA_ATOM, 32,
                            PropModeReplace, reinterpret_cast<unsigned char*>(targets), ntargets);
            reply.xselection.property = req.property;
        } else if (req.target == m_atomTextUriList) {
            std::string uriList;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                uriList = m_ownedText + "\r\n";
            }
            XChangeProperty(m_display, req.requestor, req.property, req.target, 8,
                            PropModeReplace, reinterpret_cast<const unsigned char*>(uriList.data()), uriList.size());
            reply.xselection.property = req.property;
        } else if (req.target == m_atomGnomeCopiedFiles) {
            std::string gnomeFiles;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                gnomeFiles = "copy\n" + m_ownedText + "\n";
            }
            XChangeProperty(m_display, req.requestor, req.property, req.target, 8,
                            PropModeReplace, reinterpret_cast<const unsigned char*>(gnomeFiles.data()), gnomeFiles.size());
            reply.xselection.property = req.property;
        } else if (req.target == m_atomUtf8 || req.target == m_atomString || req.target == m_atomText) {
            std::string text;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                text = m_ownedText;
            }
            XChangeProperty(m_display, req.requestor, req.property, req.target, 8,
                            PropModeReplace, reinterpret_cast<const unsigned char*>(text.data()), text.size());
            reply.xselection.property = req.property;
        }

        XSendEvent(m_display, req.requestor, False, 0, &reply);
        XFlush(m_display);
    } else if (ev.type == SelectionClear) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ownedByMe = false;
        m_selectionLost = true;
    } else if (ev.type == SelectionNotify) {
        const XSelectionEvent& sel = ev.xselection;
        std::string result;
        if (sel.property != None) {
            Atom actualType;
            int actualFormat;
            unsigned long nitems = 0;
            unsigned long bytesAfter = 0;
            unsigned char* propVal = nullptr;

            int status = XGetWindowProperty(
                m_display, m_window, sel.property, 0, 1024 * 1024 / 4, False,
                AnyPropertyType, &actualType, &actualFormat, &nitems,
                &bytesAfter, &propVal
            );

            if (status == 0 && propVal && nitems > 0) {
                result.assign(reinterpret_cast<char*>(propVal), nitems);
                XFree(propVal);
                XDeleteProperty(m_display, m_window, sel.property);
            }
        }

        if (result.empty() && sel.target == m_atomUtf8) {
            // Fallback to text/uri-list if UTF8_STRING was empty (e.g. file manager copy)
            XConvertSelection(m_display, m_atomClipboard, m_atomTextUriList, m_atomProperty, m_window, CurrentTime);
            XFlush(m_display);
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_readResultText = result;
        m_readDone = true;
        m_readCv.notify_all();
    }
}

std::string WaylandClipboard::requestSystemText() const {
    if (!m_display || m_wakePipe[1] == -1) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ownedText;
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_pendingRead = true;
    m_readDone = false;
    m_readResultText.clear();

    ssize_t res = ::write(m_wakePipe[1], "R", 1);
    (void)res;

    m_readCv.wait_for(lock, std::chrono::milliseconds(250), [this]() {
        return m_readDone;
    });

    return m_readResultText;
}

bool WaylandClipboard::hasChanged() {
    if (!m_display) {
        return false;
    }

    bool lost = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_selectionLost) {
            lost = true;
            m_selectionLost = false;
        }
    }

    if (lost) {
        std::string text = requestSystemText();
        if (text != m_lastSentText) {
            LOG((CLOG_INFO "WaylandClipboard: selection lost and text changed (%zu bytes)", text.size()));
            m_lastSentText = text;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_cachedSystemText = text;
                m_timeOwned = getCurrentTimeMs();
            }
            return true;
        }
    } else if (!m_ownedByMe) {
        std::string text = requestSystemText();
        if (!text.empty() && text != m_lastSentText) {
            LOG((CLOG_INFO "WaylandClipboard: local text changed (%zu bytes)", text.size()));
            m_lastSentText = text;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_cachedSystemText = text;
                m_timeOwned = getCurrentTimeMs();
            }
            return true;
        }
    }

    return false;
}

void WaylandClipboard::setLocalClipboard(const String& data) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ownedText = data;
        m_writeText = data;
        m_lastSentText = data;
        m_pendingWrite = true;
        m_ownedByMe = true;
        m_selectionLost = false;
        m_timeOwned = getCurrentTimeMs();
    }
    if (m_wakePipe[1] != -1) {
        ssize_t res = ::write(m_wakePipe[1], "W", 1);
        (void)res;
    }
}

bool WaylandClipboard::empty() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ownedText.clear();
        m_writeText.clear();
        m_pendingWrite = true;
        m_ownedByMe = true;
        m_selectionLost = false;
        m_timeOwned = getCurrentTimeMs();
    }
    if (m_wakePipe[1] != -1) {
        ssize_t res = ::write(m_wakePipe[1], "W", 1);
        (void)res;
    }
    return true;
}

void WaylandClipboard::add(EFormat format, const String& data) {
    if (format == kText) {
        setLocalClipboard(data);
    }
}

bool WaylandClipboard::open(Time time) const {
    m_open = true;
    m_time = time;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_ownedByMe) {
            m_currentText = m_ownedText;
            return true;
        }
        if (!m_cachedSystemText.empty()) {
            m_currentText = m_cachedSystemText;
            return true;
        }
    }
    m_currentText = requestSystemText();
    return true;
}

void WaylandClipboard::close() const {
    m_open = false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cachedSystemText.clear();
}

IClipboard::Time WaylandClipboard::getTime() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_timeOwned;
}

bool WaylandClipboard::has(EFormat format) const {
    return (format == kText && !m_currentText.empty());
}

String WaylandClipboard::get(EFormat format) const {
    if (format == kText) {
        return m_currentText;
    }
    return String();
}
