/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform/OSXEventQueueBuffer.h"
#include "base/EventQueue.h"
#include "base/Event.h"
#include <chrono>

class EventQueueTimer { };

OSXEventQueueBuffer::OSXEventQueueBuffer(IEventQueue* events) :
    m_eventQueue(events)
{
}

OSXEventQueueBuffer::~OSXEventQueueBuffer()
{
}

void
OSXEventQueueBuffer::init()
{
    // No initialization needed for condition variables
}

void
OSXEventQueueBuffer::waitForEvent(double timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_dataQueue.empty()) {
        if (timeout > 0.0) {
            m_cond.wait_for(lock, std::chrono::duration<double>(timeout));
        } else {
            m_cond.wait(lock);
        }
    }
}

IEventQueueBuffer::Type
OSXEventQueueBuffer::getEvent(Event& event, UInt32& dataID)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dataQueue.empty()) {
        dataID = m_dataQueue.front();
        m_dataQueue.pop();
        return kUser;
    }
    return kNone;
}

bool
OSXEventQueueBuffer::addEvent(UInt32 dataID)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dataQueue.push(dataID);
    }
    m_cond.notify_one();
    return true;
}

bool
OSXEventQueueBuffer::isEmpty() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dataQueue.empty();
}

EventQueueTimer*
OSXEventQueueBuffer::newTimer(double, bool) const
{
    return new EventQueueTimer;
}

void
OSXEventQueueBuffer::deleteTimer(EventQueueTimer* timer) const
{
    delete timer;
}
