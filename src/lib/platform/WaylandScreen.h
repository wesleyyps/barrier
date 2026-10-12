#pragma once

#include "barrier/PlatformScreen.h"

#include "platform/WaylandPortal.h"
#include "platform/WaylandInputCapture.h"
#include "platform/WaylandClipboard.h"
#include <set>
#include <map>

class EventQueueTimer;

class WaylandScreen : public PlatformScreen {
public:
    WaylandScreen(bool isPrimary, IEventQueue* events);
    virtual ~WaylandScreen();

    // IScreen overrides
    virtual void*        getEventTarget() const override;
    virtual bool         getClipboard(ClipboardID id, IClipboard*) const override;
    virtual void         getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const override;
    virtual void         getCursorPos(SInt32& x, SInt32& y) const override;

    // IPrimaryScreen overrides
    virtual void         reconfigure(UInt32 activeSides) override;
    virtual void         warpCursor(SInt32 x, SInt32 y) override;
    virtual UInt32       registerHotKey(KeyID key, KeyModifierMask mask) override;
    virtual void         unregisterHotKey(UInt32 id) override;
    virtual void         fakeInputBegin() override;
    virtual void         fakeInputEnd() override;
    virtual SInt32       getJumpZoneSize() const override;
    virtual bool         isAnyMouseButtonDown(UInt32& buttonID) const override;
    virtual void         getCursorCenter(SInt32& x, SInt32& y) const override;

    // ISecondaryScreen overrides
    virtual void         fakeMouseButton(ButtonID id, bool press) override;
    virtual void         fakeMouseMove(SInt32 x, SInt32 y) override;
    virtual void         fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const override;
    virtual void         fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const override;

    // IPlatformScreen overrides
    virtual void         enable() override;
    virtual void         disable() override;
    virtual void         enter() override;
    virtual bool         leave() override;
    virtual bool         setClipboard(ClipboardID, const IClipboard*) override;
    virtual void         checkClipboards() override;
    virtual void         openScreensaver(bool notify) override;
    virtual void         closeScreensaver() override;
    virtual void         screensaver(bool activate) override;
    virtual void         resetOptions() override;
    virtual void         setOptions(const OptionsList& options) override;
    virtual void         setSequenceNumber(UInt32) override;
    virtual bool         isPrimary() const override;

    virtual void         fakeKeyDown(KeyID id, KeyModifierMask mask, KeyButton button) override;
    virtual bool         fakeKeyRepeat(KeyID id, KeyModifierMask mask, SInt32 count, KeyButton button) override;
    virtual bool         fakeKeyUp(KeyButton button) override;
    virtual void         fakeAllKeysUp() override;
    virtual bool         isKeyDown(KeyButton button) const override;
    virtual void         pollPressedKeys(KeyButtonSet& pressedKeys) const override;

    virtual void         fakeDraggingFiles(DragFileList fileList) override {}
    virtual const String& getDropTarget() const override { return m_dropTarget; }
    virtual void         setDropTarget(const String& target) override { m_dropTarget = target; }

protected:
    // PlatformScreen overrides
    virtual void         updateButtons() override;
    virtual IKeyState*   getKeyState() const override;
    virtual void         handleSystemEvent(const Event& event, void*) override;

private:
    uint32_t             resolveKey(KeyID id, KeyButton button) const;
    void                 sendClipboardEvent(Event::Type type, ClipboardID id);
    void                 handleClipboardCheck(const Event&, void*);

    WaylandPortal*       m_portal;
    WaylandInputCapture* m_capture;
    WaylandClipboard*    m_clipboard;
    IKeyState*           m_keyState;
    IEventQueue*         m_events;
    bool                 m_isPrimary;

    EventQueueTimer*     m_clipboardTimer;
    UInt32               m_sequenceNumber;

    std::set<UInt32>     m_buttonsDown;
    std::set<uint32_t>   m_pressedKeys;
    std::map<KeyButton, uint32_t> m_serverKeyToScancode;
    String               m_dropTarget;
};
