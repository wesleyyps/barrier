#pragma once

#include "barrier/KeyState.h"

class WaylandKeyState : public KeyState {
public:
    WaylandKeyState(IEventQueue* events) : KeyState(events) {}
    virtual ~WaylandKeyState() {}

    virtual bool fakeCtrlAltDel() { return false; }
    virtual KeyModifierMask pollActiveModifiers() const { return 0; }
    virtual SInt32 pollActiveGroup() const { return 0; }
    virtual void pollPressedKeys(KeyButtonSet& pressedKeys) const {}

protected:
    virtual void getKeyMap(barrier::KeyMap& keyMap) {}
    virtual void fakeKey(const Keystroke& keystroke) {}
};
