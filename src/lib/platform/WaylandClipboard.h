#pragma once

#include "barrier/IClipboard.h"
#include <string>

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

private:
    // wl_data_control integration would go here
    String m_dummyData;
};
