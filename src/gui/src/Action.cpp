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

#include "Action.h"

#include <QSettings>
#include <QTextStream>

const std::array<const char*, 10> Action::m_ActionTypeNames =
{
    "keyDown", "keyUp", "keystroke",
    "switchToScreen", "toggleScreen",
    "switchInDirection", "lockCursorToScreen",
    "mouseDown", "mouseUp", "mousebutton"
};

const std::array<const char*, 4> Action::m_SwitchDirectionNames = { "left", "right", "up", "down" };
const std::array<const char*, 3> Action::m_LockCursorModeNames = { "toggle", "on", "off" };

Action::Action() :
    m_KeySequence(),
    m_Type(keystroke),
    m_TypeScreenNames(),
    m_SwitchScreenName(),
    m_SwitchDirection(switchLeft),
    m_LockCursorMode(lockCursorToggle),
    m_ActiveOnRelease(false),
    m_HasScreens(false)
{
}

QString Action::text() const
{
    /* This function is used to save to config file which is for barriers to
     * read. However the server config parse does not support functions with ()
     * in the end but now argument inside. If you need a function with no
     * argument, it can not have () in the end.
     */
    QString text = QString(m_ActionTypeNames[m_KeySequence.isMouseButton() ?
                                             type() + static_cast<int>(mouseDown) : type()]);

    switch (type())
    {
        case keyDown:
        case keyUp:
        case keystroke:
            {
                text += "(";
                text += m_KeySequence.toString();

                if (!m_KeySequence.isMouseButton())
                {
                    const QStringList& screens = typeScreenNames();
                    if (haveScreens() && !screens.isEmpty())
                    {
                        text += ",";

                        for (int i = 0; i < screens.size(); i++)
                        {
                            text += screens[i];
                            if (i != screens.size() - 1)
                                text += ":";
                        }
                    }
                    else
                        text += ",*";
                }
                text += ")";
            }
            break;

        case switchToScreen:
            text += "(";
            text += switchScreenName();
            text += ")";
            break;

        case toggleScreen:
            break;

        case switchInDirection:
            text += "(";
            text += m_SwitchDirectionNames[m_SwitchDirection];
            text += ")";
            break;

        case lockCursorToScreen:
            text += "(";
            text += m_LockCursorModeNames[m_LockCursorMode];
            text += ")";
            break;

        default:
            Q_ASSERT(0);
            break;
    }


    return text;
}

Action Action::fromString(const QString& str)
{
    Action a;
    QString s = str.trimmed();
    if (s.isEmpty()) return a;

    if (s.startsWith(";")) {
        a.setActiveOnRelease(true);
        s = s.mid(1).trimmed();
    }

    int parenOpen = s.indexOf('(');
    int parenClose = s.lastIndexOf(')');
    QString funcName;
    QString argsStr;

    if (parenOpen != -1 && parenClose != -1 && parenClose > parenOpen) {
        funcName = s.left(parenOpen).trimmed();
        argsStr = s.mid(parenOpen + 1, parenClose - parenOpen - 1).trimmed();
    } else {
        funcName = s;
    }

    for (int i = 0; i < m_ActionTypeNames.size(); ++i) {
        if (funcName.compare(QString::fromUtf8(m_ActionTypeNames[i]), Qt::CaseInsensitive) == 0) {
            a.setType(i);
            break;
        }
    }

    if (a.type() == keyDown || a.type() == keyUp || a.type() == keystroke ||
        a.type() == mouseDown || a.type() == mouseUp || a.type() == mousebutton) 
    {
        if (a.type() >= mouseDown) {
            a.setType(a.type() - static_cast<int>(mouseDown));
        }

        int commaIdx = argsStr.indexOf(',');
        QString keyStr = argsStr;
        if (commaIdx != -1) {
            keyStr = argsStr.left(commaIdx).trimmed();
            QString screensStr = argsStr.mid(commaIdx + 1).trimmed();
            if (screensStr == "*") {
                a.setHaveScreens(false);
            } else {
                a.setHaveScreens(true);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
                QStringList screens = screensStr.split(":", Qt::SkipEmptyParts);
#else
                QStringList screens = screensStr.split(":", QString::SkipEmptyParts);
#endif
                for (const QString& screenName : screens) {
                    a.appendTypeScreenName(screenName.trimmed());
                }
            }
        } else {
            a.setHaveScreens(false);
        }
        a.setKeySequence(KeySequence::fromString(keyStr));
    }
    else if (a.type() == switchToScreen) {
        a.setSwitchScreenName(argsStr);
    }
    else if (a.type() == switchInDirection) {
        for (int i = 0; i < m_SwitchDirectionNames.size(); ++i) {
            if (argsStr.compare(QString::fromUtf8(m_SwitchDirectionNames[i]), Qt::CaseInsensitive) == 0) {
                a.setSwitchDirection(i);
                break;
            }
        }
    }
    else if (a.type() == lockCursorToScreen) {
        for (int i = 0; i < m_LockCursorModeNames.size(); ++i) {
            if (argsStr.compare(QString::fromUtf8(m_LockCursorModeNames[i]), Qt::CaseInsensitive) == 0) {
                a.setLockCursorMode(i);
                break;
            }
        }
    }

    return a;
}

void Action::loadSettings(QSettings& settings)
{
    m_KeySequence.loadSettings(settings);
    setType(settings.value("type", keyDown).toInt());

    m_TypeScreenNames.clear();
    int numTypeScreens = settings.beginReadArray("typeScreenNames");
    for (int i = 0; i < numTypeScreens; i++)
    {
        settings.setArrayIndex(i);
        m_TypeScreenNames.append(settings.value("typeScreenName").toString());
    }
    settings.endArray();

    setSwitchScreenName(settings.value("switchScreenName").toString());
    setSwitchDirection(settings.value("switchInDirection", switchLeft).toInt());
    setLockCursorMode(settings.value("lockCursorToScreen", lockCursorToggle).toInt());
    setActiveOnRelease(settings.value("activeOnRelease", false).toBool());
    setHaveScreens(settings.value("hasScreens", false).toBool());
}

void Action::saveSettings(QSettings& settings) const
{
    m_KeySequence.saveSettings(settings);
    settings.setValue("type", type());

    settings.beginWriteArray("typeScreenNames");
    for (int i = 0; i < typeScreenNames().size(); i++)
    {
        settings.setArrayIndex(i);
        settings.setValue("typeScreenName", typeScreenNames()[i]);
    }
    settings.endArray();

    settings.setValue("switchScreenName", switchScreenName());
    settings.setValue("switchInDirection", switchDirection());
    settings.setValue("lockCursorToScreen", lockCursorMode());
    settings.setValue("activeOnRelease", activeOnRelease());
    settings.setValue("hasScreens", haveScreens());
}

QTextStream& operator<<(QTextStream& outStream, const Action& action)
{
    if (action.activeOnRelease())
        outStream << ";";

    outStream << action.text();

    return outStream;
}
