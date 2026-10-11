#include "platform/WaylandClipboard.h"

WaylandClipboard::WaylandClipboard() {
}

WaylandClipboard::~WaylandClipboard() {
}

bool WaylandClipboard::empty() {
    m_dummyData.clear();
    return true;
}

void WaylandClipboard::add(EFormat format, const String& data) {
    if (format == kText) {
        m_dummyData = data;
    }
}

bool WaylandClipboard::open(Time time) const {
    return true;
}

void WaylandClipboard::close() const {
}

IClipboard::Time WaylandClipboard::getTime() const {
    return 0;
}

bool WaylandClipboard::has(EFormat format) const {
    return format == kText && !m_dummyData.empty();
}

String WaylandClipboard::get(EFormat format) const {
    if (format == kText) return m_dummyData;
    return String();
}
