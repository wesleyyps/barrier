/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2008 Volker Lanz (vl@fidra.de)
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

#include "KeySequence.h"

#include <QtCore>
#include <QtGui>
#include <array>

// this table originally comes from Qt sources (gui/kernel/qkeysequence.cpp)
// and is heavily modified
struct KeyNameStruct
{
    int key;
    const char* name;
};
static const std::array<KeyNameStruct, 49> keyname =
{{
    { Qt::Key_Space,        "Space" },
    { Qt::Key_Escape,       "Escape" },
    { Qt::Key_Tab,          "Tab" },
    { Qt::Key_Backtab,      "LeftTab" },
    { Qt::Key_Backspace,    "BackSpace" },
    { Qt::Key_Return,       "Return" },
    { Qt::Key_Insert,       "Insert" },
    { Qt::Key_Delete,       "Delete" },
    { Qt::Key_Pause,        "Pause" },
    { Qt::Key_Print,        "Print" },
    { Qt::Key_SysReq,       "SysReq" },
    { Qt::Key_Home,         "Home" },
    { Qt::Key_End,          "End" },
    { Qt::Key_Left,         "Left" },
    { Qt::Key_Up,           "Up" },
    { Qt::Key_Right,        "Right" },
    { Qt::Key_Down,         "Down" },
    { Qt::Key_PageUp,       "PageUp" },
    { Qt::Key_PageDown,     "PageDown" },
    { Qt::Key_CapsLock,     "CapsLock" },
    { Qt::Key_NumLock,      "NumLock" },
    { Qt::Key_ScrollLock,   "ScrollLock" },
    { Qt::Key_Menu,         "Menu" },
    { Qt::Key_Help,         "Help" },
    { Qt::Key_Enter,        "KP_Enter" },
    { Qt::Key_Clear,        "Clear" },
    { Qt::Key_Comma,        "Comma" },
    { Qt::Key_Semicolon,    "Semicolon" },

    { Qt::Key_Back,         "WWWBack" },
    { Qt::Key_Forward,      "WWWForward" },
    { Qt::Key_Stop,         "WWWStop" },
    { Qt::Key_Refresh,      "WWWRefresh" },
    { Qt::Key_VolumeDown,   "AudioDown" },
    { Qt::Key_VolumeMute,   "AudioMute" },
    { Qt::Key_VolumeUp,     "AudioUp" },
    { Qt::Key_MediaPlay,    "AudioPlay" },
    { Qt::Key_MediaStop,    "AudioStop" },
    { Qt::Key_MediaPrevious,"AudioPrev" },
    { Qt::Key_MediaNext,    "AudioNext" },
    { Qt::Key_HomePage,     "WWWHome" },
    { Qt::Key_Favorites,    "WWWFavorites" },
    { Qt::Key_Search,       "WWWSearch" },
    { Qt::Key_Standby,      "Sleep" },
    { Qt::Key_LaunchMail,   "AppMail" },
    { Qt::Key_LaunchMedia,  "AppMedia" },
    { Qt::Key_Launch0,      "AppUser1" },
    { Qt::Key_Launch1,      "AppUser2" },
    { Qt::Key_Select,       "Select" },

    { 0, nullptr }
}};

KeySequence::KeySequence() :
    m_Sequence(),
    m_Modifiers(0),
    m_IsValid(false)
{
}

bool KeySequence::isMouseButton() const
{
    return !m_Sequence.isEmpty() && m_Sequence.last() < Qt::Key_Space;
}

QString KeySequence::toString() const
{
    QString result;

    for (int i = 0; i < m_Sequence.size(); i++)
    {
        result += keyToString(m_Sequence[i]);

        if (i != m_Sequence.size() - 1)
            result += "+";
    }

    return result;
}

KeySequence KeySequence::fromString(const QString& str)
{
    KeySequence ks;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QStringList parts = str.split("+", Qt::SkipEmptyParts);
#else
    QStringList parts = str.split("+", QString::SkipEmptyParts);
#endif
    for (int i = 0; i < parts.size(); ++i) {
        QString p = parts[i].trimmed();
        if (p.isEmpty()) continue;
        
        int key = 0;
        if (p.compare("Shift", Qt::CaseInsensitive) == 0) key = Qt::Key_Shift;
        else if (p.compare("Control", Qt::CaseInsensitive) == 0) key = Qt::Key_Control;
        else if (p.compare("Alt", Qt::CaseInsensitive) == 0) key = Qt::Key_Alt;
        else if (p.compare("Meta", Qt::CaseInsensitive) == 0 || p.compare("Super", Qt::CaseInsensitive) == 0) key = Qt::Key_Meta;
        else {
            bool found = false;
            int idx = 0;
            while (keyname[idx].name != nullptr) {
                if (p.compare(QString::fromUtf8(keyname[idx].name), Qt::CaseInsensitive) == 0) {
                    key = keyname[idx].key;
                    found = true;
                    break;
                }
                idx++;
            }
            if (!found) {
                if (p.startsWith("F", Qt::CaseInsensitive) && p.length() > 1) {
                    bool ok;
                    int fNum = p.mid(1).toInt(&ok);
                    if (ok && fNum >= 1 && fNum <= 35) {
                        key = Qt::Key_F1 + fNum - 1;
                        found = true;
                    }
                }
                else if (p.startsWith("\\u", Qt::CaseInsensitive) && p.length() == 6) {
                    bool ok;
                    key = p.mid(2).toInt(&ok, 16);
                    if (ok) found = true;
                }
                else if (p.length() == 1) {
                    key = p[0].toUpper().unicode();
                    found = true;
                }
                else if (p == "1" || p == "2" || p == "3" || p == "4" || p == "5") {
                    if (p == "1") key = Qt::LeftButton;
                    else if (p == "2") key = Qt::RightButton;
                    else if (p == "3") key = Qt::MiddleButton;
                    else key = Qt::ExtraButton1;
                    found = true;
                }
            }
        }
        
        if (key != 0) {
            if (key == Qt::Key_Shift) {
                ks.appendKey(key, Qt::ShiftModifier);
            } else if (key == Qt::Key_Control) {
                ks.appendKey(key, Qt::ControlModifier);
            } else if (key == Qt::Key_Alt) {
                ks.appendKey(key, Qt::AltModifier);
            } else if (key == Qt::Key_Meta) {
                ks.appendKey(key, Qt::MetaModifier);
            } else {
                ks.appendKey(key, 0);
            }
        }
    }
    return ks;
}

bool KeySequence::appendMouseButton(int button)
{
    return appendKey(button, 0);
}

bool KeySequence::appendKey(int key, int modifiers)
{
    if (m_Sequence.size() == 4)
        return true;

    switch(key)
    {
        case Qt::Key_AltGr:
            return false;

        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_Shift:
        case Qt::Key_Meta:
        case Qt::Key_Menu:
            {
                int mod = modifiers & (~m_Modifiers);
                if (mod != 0)
                {
                    m_Sequence.append(mod);
                    m_Modifiers |= mod;
                }
            }
            break;

        default:
            // see if we can handle this key, if not, don't accept it
            if (keyToString(key).isEmpty())
                break;

            m_Sequence.append(key);
            setValid(true);
            return true;
    }

    return false;
}

void KeySequence::loadSettings(QSettings& settings)
{
    m_Sequence.clear();
    int num = settings.beginReadArray("keys");
    for (int i = 0; i < num; i++)
    {
        settings.setArrayIndex(i);
        m_Sequence.append(settings.value("key", 0).toInt());
    }
    settings.endArray();

    setModifiers(0);
    setValid(true);
}

void KeySequence::saveSettings(QSettings& settings) const
{
    settings.beginWriteArray("keys");
    for (int i = 0; i < m_Sequence.size(); i++)
    {
        settings.setArrayIndex(i);
        settings.setValue("key", m_Sequence[i]);
    }
    settings.endArray();
}

QString KeySequence::keyToString(int key)
{
    // nothing there?
    if (key == 0)
        return "";

    // a hack to handle mouse buttons as if they were keys
    if (key < Qt::Key_Space)
    {
        switch(key)
        {
            case Qt::LeftButton: return "1";
            case Qt::RightButton: return "2";
            case Qt::MiddleButton: return "3";
            default: break;
        }

        return "4"; // qt only knows three mouse buttons, so assume it's an unknown fourth one
    }

    // modifiers?
    if ((key & Qt::ShiftModifier) != 0u)
        return "Shift";

    if ((key & Qt::ControlModifier) != 0u)
        return "Control";

    if ((key & Qt::AltModifier) != 0u)
        return "Alt";

    if ((key & Qt::MetaModifier) != 0u)
        return "Meta";

    // treat key pad like normal keys (FIXME: we should have another lookup table for keypad keys instead)
     key &= ~Qt::KeypadModifier;

    // a special key?
    int i = 0;
    while (keyname[i].name != nullptr) {
        if (key == keyname[i].key)
            return QString::fromUtf8(keyname[i].name);
        i++;
    }

    // a printable 7 bit character?
     if (key < 0x80)
         return QChar(key & 0x7f).toLower();

    // a function key?
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return QString::fromUtf8("F%1").arg(key - Qt::Key_F1 + 1);

    // representable in ucs2?
    if (key < 0x10000)
        return QString("\\u%1").arg(QChar(key).toLower().unicode(), 4, 16, QChar('0'));

    // give up, barrier probably won't handle this
    return "";
}
