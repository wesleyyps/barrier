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

#include "ServerConfig.h"
#include "AppConfig.h"
#include "ConfModifier.h"
#include "Hotkey.h"
#include "MainWindow.h"
#include "AddClientDialog.h"
#include <array>

#include <QtCore>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>

struct NeighbourDir
{
     int x;
     int y;
     const char* name;
};
static const std::array<NeighbourDir, 4> neighbourDirs =
{
    NeighbourDir{  1,  0, "right" },
    NeighbourDir{ -1,  0, "left" },
    NeighbourDir{  0, -1, "up" },
    NeighbourDir{  0,  1, "down" },

};



ServerConfig::ServerConfig(QSettings* settings, int numColumns, int numRows ,
                QString serverName, MainWindow* mainWindow) :
    m_pSettings(settings),
    m_Screens(),
    m_NumColumns(numColumns),
    m_NumRows(numRows),
    m_ServerName(serverName),
    m_IgnoreAutoConfigClient(false),
    m_EnableDragAndDrop(false),
    m_DragDropDirectory(""),
    m_ClipboardSharing(true),
    m_pMainWindow(mainWindow)
{
    Q_ASSERT(m_pSettings);

    loadSettings();
}

ServerConfig::~ServerConfig()
{
    saveSettings();
}

bool ServerConfig::save(const QString& fileName) const
{
    QFile file(fileName);
    if (!file.exists()) {
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        save(file);
        file.close();
        return true;
    }

    ConfModifier mod;
    if (!mod.load(fileName)) return false;
    
    mod.setOption("server", m_ServerName);
    mod.setOption("port", QString::number(port()));
    mod.setOption("cryptoEnabled", cryptoEnabled() ? "true" : "false");
    mod.setOption("requireClientCertificate", requireClientCertificate() ? "true" : "false");
    mod.setOption("logLevel", QString::number(logLevel()));
    mod.setOption("logToFile", logToFile() ? "true" : "false");
    if (!logFilename().isEmpty()) {
        mod.setOption("logFilename", logFilename());
    }
    if (!networkInterface().isEmpty()) {
        mod.setOption("networkInterface", networkInterface());
    }
    if (hasHeartbeat()) mod.setOption("heartbeat", QString::number(heartbeat()));
    
    mod.setOption("relativeMouseMoves", relativeMouseMoves() ? "true" : "false");
    mod.setOption("screenSaverSync", screenSaverSync() ? "true" : "false");
    mod.setOption("win32KeepForeground", win32KeepForeground() ? "true" : "false");
    mod.setOption("clipboardSharing", clipboardSharing() ? "true" : "false");
    if (!m_DragDropDirectory.isEmpty()) {
        mod.setOption("dropTarget", "\"" + m_DragDropDirectory + "\"");
    } else {
        mod.setOption("dropTarget", QString());
    }
    
    mod.setOption("gridSize", QString("%1x%2").arg(m_NumColumns).arg(m_NumRows));

    if (hasSwitchDelay()) mod.setOption("switchDelay", QString::number(switchDelay()));
    if (hasSwitchDoubleTap()) mod.setOption("switchDoubleTap", QString::number(switchDoubleTap()));
    
    QString corners = "none ";
    for (int i = 0; i < switchCorners().size(); i++) {
        if (switchCorners()[i]) {
            corners += QString("+") + switchCornerName(static_cast<Screen::SwitchCorner>(i)) + " ";
        }
    }
    mod.setOption("switchCorners", corners.trimmed());
    mod.setOption("switchCornerSize", QString::number(switchCornerSize()));
    
    QStringList validScreens;
    for (const Screen& s : screens()) {
        if (!s.isNull()) {
            validScreens.append(s.name());
        }
    }
    mod.cleanupScreens(validScreens);
    
    for (int i = 0; i < screens().size(); ++i) {
        if (!screens()[i].isNull()) {
            const Screen& s = screens()[i];
            mod.ensureScreenExists(s.name());
            
            if (!s.networkIP().isEmpty()) mod.setNetworkOption(s.name(), "ip", s.networkIP());
            if (!s.networkSSHUser().isEmpty()) mod.setNetworkOption(s.name(), "ssh_user", s.networkSSHUser());
            if (s.networkSSHPort() > 0) mod.setNetworkOption(s.name(), "ssh_port", QString::number(s.networkSSHPort()));
            if (!s.networkClientCmd().isEmpty()) mod.setNetworkOption(s.name(), "client_cmd", "\"" + s.networkClientCmd() + "\"");
            
            bool hasCorners = false;
            QString sCorners = "none ";
            for (int c = 0; c < s.switchCorners().size(); c++) {
                if (s.switchCorners()[c]) {
                    hasCorners = true;
                    sCorners += QString("+") + switchCornerName(static_cast<Screen::SwitchCorner>(c)) + " ";
                }
            }
            if (hasCorners || s.switchCornerSize() > 0) {
                mod.setScreenOption(s.name(), "switchCorners", sCorners.trimmed());
                mod.setScreenOption(s.name(), "switchCornerSize", QString::number(s.switchCornerSize()));
            }
            
            mod.clearLinks(s.name());
            for (auto neighbourDir : neighbourDirs) {
                int idx = adjacentScreenIndex(i, neighbourDir.x, neighbourDir.y);
                if (idx != -1 && !screens()[idx].isNull()) {
                    mod.addLink(s.name(), neighbourDir.name, screens()[idx].name());
                }
            }
        }
    }
    
    mod.clearHotkeys();
    for (const Hotkey& hotkey : hotkeys()) {
        QString s;
        QTextStream stream(&s);
        stream << hotkey;
        if (!s.trimmed().isEmpty()) {
            mod.addOptionLine("\t" + s.trimmed());
        }
    }
    
    return mod.save(fileName);
}

void ServerConfig::save(QFile& file) const
{
    QTextStream outStream(&file);
    outStream << *this;
}

void ServerConfig::init()
{
    switchCorners().clear();
    screens().clear();
    hotkeys().clear();

    // m_NumSwitchCorners is used as a fixed size array. See Screen::init()
    for (int i = 0; i < static_cast<int>(SwitchCorner::Count); i++) {
        switchCorners() << false;
    }

    // There must always be screen objects for each cell in the screens QList. Unused screens
    // are identified by having an empty name.
    for (int i = 0; i < numColumns() * numRows(); i++)
        addScreen(Screen());
}

void ServerConfig::saveSettings()
{
    settings().beginGroup("internalConfig");
    settings().remove("");

    settings().setValue("numColumns", numColumns());
    settings().setValue("numRows", numRows());

    settings().setValue("hasHeartbeat", hasHeartbeat());
    settings().setValue("heartbeat", heartbeat());
    settings().setValue("relativeMouseMoves", relativeMouseMoves());
    settings().setValue("screenSaverSync", screenSaverSync());
    settings().setValue("win32KeepForeground", win32KeepForeground());
    settings().setValue("hasSwitchDelay", hasSwitchDelay());
    settings().setValue("switchDelay", switchDelay());
    settings().setValue("hasSwitchDoubleTap", hasSwitchDoubleTap());
    settings().setValue("switchDoubleTap", switchDoubleTap());
    settings().setValue("switchCornerSize", switchCornerSize());
    settings().setValue("ignoreAutoConfigClient", ignoreAutoConfigClient());
    settings().setValue("enableDragAndDrop", enableDragAndDrop());
    settings().setValue("dragDropDirectory", dragDropDirectory());
    settings().setValue("clipboardSharing", clipboardSharing());

    writeSettings<bool>(settings(), switchCorners(), "switchCorner");

    settings().beginWriteArray("screens");
    for (int i = 0; i < screens().size(); i++)
    {
        settings().setArrayIndex(i);
        screens()[i].saveSettings(settings());
    }
    settings().endArray();

    settings().beginWriteArray("hotkeys");
    for (int i = 0; i < hotkeys().size(); i++)
    {
        settings().setArrayIndex(i);
        hotkeys()[i].saveSettings(settings());
    }
    settings().endArray();

    settings().endGroup();
}

void ServerConfig::loadSettings()
{
    settings().beginGroup("internalConfig");

    setNumColumns(settings().value("numColumns", 5).toInt());
    setNumRows(settings().value("numRows", 3).toInt());

    // we need to know the number of columns and rows before we can set up ourselves
    init();

    haveHeartbeat(settings().value("hasHeartbeat", false).toBool());
    setHeartbeat(settings().value("heartbeat", 5000).toInt());
    setRelativeMouseMoves(settings().value("relativeMouseMoves", false).toBool());
    setScreenSaverSync(settings().value("screenSaverSync", true).toBool());
    setWin32KeepForeground(settings().value("win32KeepForeground", false).toBool());
    haveSwitchDelay(settings().value("hasSwitchDelay", false).toBool());
    setSwitchDelay(settings().value("switchDelay", 250).toInt());
    haveSwitchDoubleTap(settings().value("hasSwitchDoubleTap", false).toBool());
    setSwitchDoubleTap(settings().value("switchDoubleTap", 250).toInt());
    setSwitchCornerSize(settings().value("switchCornerSize", 0).toInt());
    setIgnoreAutoConfigClient(settings().value("ignoreAutoConfigClient", false).toBool());
    setEnableDragAndDrop(settings().value("enableDragAndDrop", true).toBool());
    setDragDropDirectory(settings().value("dragDropDirectory", "").toString());
    setClipboardSharing(settings().value("clipboardSharing", true).toBool());

    readSettings<bool>(settings(), switchCorners(), "switchCorner", false,
                       static_cast<int>(SwitchCorner::Count));

    int numScreens = settings().beginReadArray("screens");
    Q_ASSERT(numScreens <= screens().size());
    for (int i = 0; i < numScreens; i++)
    {
        settings().setArrayIndex(i);
        screens()[i].loadSettings(settings());
    }
    settings().endArray();

    int numHotkeys = settings().beginReadArray("hotkeys");
    for (int i = 0; i < numHotkeys; i++)
    {
        settings().setArrayIndex(i);
        Hotkey h;
        h.loadSettings(settings());
        hotkeys().push_back(h);
    }
    settings().endArray();

    settings().endGroup();
}

#include <QQueue>
#include <QSet>
#include <QDebug>

void ServerConfig::resizeGrid(int numColumns, int numRows)
{
    if (numColumns <= 0 || numRows <= 0) return;
    
    std::vector<Screen> oldScreens = m_Screens;
    int oldCols = m_NumColumns;
    int oldRows = m_NumRows;
    
    m_NumColumns = numColumns;
    m_NumRows = numRows;
    m_Screens.assign(m_NumColumns * m_NumRows, Screen());
    
    for (int r = 0; r < std::min(oldRows, numRows); ++r) {
        for (int c = 0; c < std::min(oldCols, numColumns); ++c) {
            m_Screens[r * m_NumColumns + c] = oldScreens[r * oldCols + c];
        }
    }
}

bool ServerConfig::loadFromConf(const QString& path)
{
    m_Hotkeys.clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream in(&file);
    QString currentSection = "";
    struct Link { QString dir; QString target; };
    QMap<QString, QList<Link>> screenLinks;
    QMap<QString, QMap<QString, QString>> screenNetwork;
    QString currentLinkScreen = "";

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith("#")) continue;

        if (line.startsWith("section:")) {
            currentSection = line.mid(8).trimmed();
        } else if (line == "end") {
            currentSection = "";
        } else {
            if (currentSection == "options") {
                if (line.startsWith("server =")) {
                    m_ServerName = line.mid(line.indexOf('=')+1).trimmed();
                } else if (line.startsWith("port =")) {
                    setPort(line.mid(line.indexOf('=')+1).trimmed().toInt());
                } else if (line.startsWith("cryptoEnabled =")) {
                    setCryptoEnabled(line.mid(line.indexOf('=')+1).trimmed() == "true");
                } else if (line.startsWith("requireClientCertificate =")) {
                    setRequireClientCertificate(line.mid(line.indexOf('=')+1).trimmed() == "true");
                } else if (line.startsWith("logLevel =")) {
                    setLogLevel(line.mid(line.indexOf('=')+1).trimmed().toInt());
                } else if (line.startsWith("logToFile =")) {
                    setLogToFile(line.mid(line.indexOf('=')+1).trimmed() == "true");
                } else if (line.startsWith("logFilename =")) {
                    setLogFilename(line.mid(line.indexOf('=')+1).trimmed());
                } else if (line.startsWith("networkInterface =")) {
                    setNetworkInterface(line.mid(line.indexOf('=')+1).trimmed());
                } else if (line.startsWith("heartbeat =")) {
                    haveHeartbeat(true);
                    setHeartbeat(line.mid(line.indexOf('=')+1).trimmed().toInt());
                } else if (line.startsWith("relativeMouseMoves =")) {
                    setRelativeMouseMoves(line.mid(line.indexOf('=')+1).trimmed() == "true");
                } else if (line.startsWith("screenSaverSync =")) {
                    setScreenSaverSync(line.mid(line.indexOf('=')+1).trimmed() == "true");
                } else if (line.startsWith("win32KeepForeground =")) {
                    setWin32KeepForeground(line.mid(line.indexOf('=')+1).trimmed() == "true");
                } else if (line.startsWith("enableDragDrop =")) {
                    bool val = line.mid(line.indexOf('=')+1).trimmed() == "true";
                    setEnableDragAndDrop(val);
                    m_pSettings->setValue("enableDragDrop", val);
                } else if (line.startsWith("dropTarget =")) {
                    QString target = line.mid(line.indexOf('=')+1).trimmed();
                    if (target.startsWith("\"") && target.endsWith("\"")) {
                        target = target.mid(1, target.length() - 2);
                    }
                    setDragDropDirectory(target);
                    m_pSettings->setValue("dragDropDirectory", target);
                } else if (line.startsWith("gridSize =")) {
                    QString val = line.mid(line.indexOf('=')+1).trimmed();
                    QStringList parts = val.split("x");
                    if (parts.size() == 2) {
                        int c = parts[0].toInt();
                        int r = parts[1].toInt();
                        if (c > 0 && r > 0) {
                            resizeGrid(c, r);
                        }
                    }
                } else if (line.startsWith("serverIp =")) {
                    // serverIp is parsed by the client connect script, no dedicated member in GUI ServerConfig yet,
                    // but we ensure it doesn't get wiped if we ever manage it.
                } else if (line.startsWith("clipboardSharing =")) {
                    setClipboardSharing(line.mid(line.indexOf('=')+1).trimmed() == "true");
                } else if (line.startsWith("switchDelay =")) {
                    haveSwitchDelay(true);
                    setSwitchDelay(line.mid(line.indexOf('=')+1).trimmed().toInt());
                } else if (line.startsWith("switchDoubleTap =")) {
                    haveSwitchDoubleTap(true);
                    setSwitchDoubleTap(line.mid(line.indexOf('=')+1).trimmed().toInt());
                } else if (line.startsWith("switchCornerSize =")) {
                    setSwitchCornerSize(line.mid(line.indexOf('=')+1).trimmed().toInt());
                } else if (line.startsWith("switchCorners =")) {
                    QString cornersStr = line.mid(line.indexOf('=')+1).trimmed();
                    for (int i = 0; i < static_cast<int>(BaseConfig::SwitchCorner::Count); i++) {
                        switchCorners()[i] = false;
                    }
                    if (cornersStr != "none") {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
                        QStringList parts = cornersStr.split("+", Qt::SkipEmptyParts);
#else
                        QStringList parts = cornersStr.split("+", QString::SkipEmptyParts);
#endif
                        for (int i = 0; i < parts.size(); ++i) {
                            QString c = parts[i].trimmed();
                            for (int j = 0; j < static_cast<int>(BaseConfig::SwitchCorner::Count); j++) {
                                if (c == switchCornerName(static_cast<BaseConfig::SwitchCorner>(j))) {
                                    switchCorners()[j] = true;
                                    break;
                                }
                            }
                        }
                    }
                } else if (line.startsWith("keystroke(") || line.startsWith("mousebutton(")) {
                    QString keyStr;
                    int parenOpen = line.indexOf('(');
                    int parenClose = line.indexOf(')');
                    if (parenOpen != -1 && parenClose > parenOpen) {
                        keyStr = line.mid(parenOpen + 1, parenClose - parenOpen - 1).trimmed();
                        int eqIdx = line.indexOf('=', parenClose);
                        if (eqIdx != -1) {
                            QString actionsStr = line.mid(eqIdx + 1).trimmed();
                            Hotkey h;
                            h.setKeySequence(KeySequence::fromString(keyStr));
                            QStringList actionList;
                            int parenDepth = 0;
                            QString currentAction;
                            for (int i = 0; i < actionsStr.length(); ++i) {
                                QChar c = actionsStr[i];
                                if (c == '(') parenDepth++;
                                else if (c == ')') parenDepth--;
                                else if (c == ',' && parenDepth == 0) {
                                    actionList.append(currentAction.trimmed());
                                    currentAction.clear();
                                    continue;
                                }
                                currentAction += c;
                            }
                            if (!currentAction.trimmed().isEmpty()) {
                                actionList.append(currentAction.trimmed());
                            }
                            for (const QString& actStr : actionList) {
                                Action a = Action::fromString(actStr);
                                h.appendAction(a);
                            }

                            hotkeys().push_back(h);
                        }
                    }
                }
            } else if (currentSection == "links") {
                if (line.endsWith(":")) {
                    currentLinkScreen = line.left(line.length() - 1).trimmed();
                } else if (!currentLinkScreen.isEmpty() && line.contains("=")) {
                    QStringList parts = line.split("=");
                    if (parts.size() == 2) {
                        QString dir = parts[0].trimmed();
                        QString target = parts[1].trimmed();
                        if (target.contains("(")) {
                            target = target.left(target.indexOf("(")).trimmed();
                        }
                        screenLinks[currentLinkScreen].append({dir, target});
                    }
                }
            } else if (currentSection == "network") {
                if (line.endsWith(":")) {
                    currentLinkScreen = line.left(line.length() - 1).trimmed();
                } else if (!currentLinkScreen.isEmpty() && line.contains("=")) {
                    QStringList parts = line.split("=");
                    if (parts.size() >= 2) {
                        QString key = parts[0].trimmed();
                        QString val = parts.mid(1).join("=").trimmed();
                        if (val.startsWith("\"") && val.endsWith("\"")) {
                            val = val.mid(1, val.length() - 2);
                        }
                        screenNetwork[currentLinkScreen][key] = val;
                    }
                }
            }
        }
    }

    if (m_ServerName.isEmpty()) {
        qDebug() << "loadFromConf: m_ServerName is empty, aborting grid rebuild";
        return true;
    }

    // Topological centering logic
    QMap<QString, QPoint> coords;
    coords[m_ServerName] = QPoint(0, 0);
    
    QQueue<QString> graphQueue;
    graphQueue.enqueue(m_ServerName);
    
    int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    
    while (!graphQueue.isEmpty()) {
        QString curr = graphQueue.dequeue();
        QPoint p = coords[curr];
        
        QList<Link> links = screenLinks.value(curr);
        for (const Link& link : links) {
            if (coords.contains(link.target)) continue;
            
            QPoint tp = p;
            if (link.dir == "right") tp.rx() += 1;
            else if (link.dir == "left") tp.rx() -= 1;
            else if (link.dir == "down") tp.ry() += 1;
            else if (link.dir == "up") tp.ry() -= 1;
            else continue;
            
            coords[link.target] = tp;
            graphQueue.enqueue(link.target);
            
            if (tp.x() < min_x) min_x = tp.x();
            if (tp.x() > max_x) max_x = tp.x();
            if (tp.y() < min_y) min_y = tp.y();
            if (tp.y() > max_y) max_y = tp.y();
        }
    }
    
    int graph_width = max_x - min_x + 1;
    int graph_height = max_y - min_y + 1;
    
    // Auto-expand grid if the graph doesn't fit
    if (graph_width > m_NumColumns || graph_height > m_NumRows) {
        int newCols = std::max(m_NumColumns, graph_width);
        int newRows = std::max(m_NumRows, graph_height);
        qDebug() << "loadFromConf: Expanding grid from" << m_NumColumns << "x" << m_NumRows 
                 << "to" << newCols << "x" << newRows;
        resizeGrid(newCols, newRows);
    }
    
    // Clear existing screens
    for (auto& s : m_Screens) { s = Screen(); }
    
    // Calculate precise center offsets
    int offset_x = (m_NumColumns - graph_width) / 2 - min_x;
    int offset_y = (m_NumRows - graph_height) / 2 - min_y;
    
    // Place all tracked screens in the centered grid
    QMapIterator<QString, QPoint> i(coords);
    while (i.hasNext()) {
        i.next();
        QString name = i.key();
        QPoint p = i.value();
        
        int gx = p.x() + offset_x;
        int gy = p.y() + offset_y;
        
        if (gx >= 0 && gx < m_NumColumns && gy >= 0 && gy < m_NumRows) {
            int arrayPos = gy * m_NumColumns + gx;
            m_Screens[arrayPos].setName(name);
            
            QMap<QString, QString> netProps = screenNetwork.value(name);
            if (netProps.contains("ip")) m_Screens[arrayPos].setNetworkIP(netProps["ip"]);
            if (netProps.contains("ssh_user")) m_Screens[arrayPos].setNetworkSSHUser(netProps["ssh_user"]);
            if (netProps.contains("ssh_port")) m_Screens[arrayPos].setNetworkSSHPort(netProps["ssh_port"].toInt());
            if (netProps.contains("client_cmd")) m_Screens[arrayPos].setNetworkClientCmd(netProps["client_cmd"]);
            
            qDebug() << "loadFromConf: Placed" << name << "at grid(" << gx << "," << gy << ") index" << arrayPos;
        }
    }

    qDebug() << "loadFromConf: Grid rebuilt and centered successfully.";
    return true;
}

int ServerConfig::adjacentScreenIndex(int idx, int deltaColumn, int deltaRow) const
{
    if (screens()[idx].isNull())
        return -1;

    // if we're at the left or right end of the table, don't find results going further left or right
    if ((deltaColumn > 0 && (idx+1) % numColumns() == 0)
            || (deltaColumn < 0 && idx % numColumns() == 0))
        return -1;

    int arrayPos = idx + deltaColumn + deltaRow * numColumns();

    if (arrayPos >= screens().size() || arrayPos < 0)
        return -1;

    return arrayPos;
}

QTextStream& operator<<(QTextStream& outStream, const ServerConfig& config)
{
    outStream << "section: screens" << Qt::endl;

    for (const Screen& s : config.screens()) {
        if (!s.isNull())
            s.writeScreensSection(outStream);
    }

    outStream << "end" << Qt::endl << Qt::endl;

    outStream << "section: aliases" << Qt::endl;

    for (const Screen& s : config.screens()) {
        if (!s.isNull())
            s.writeAliasesSection(outStream);
    }

    outStream << "end" << Qt::endl << Qt::endl;

    outStream << "section: links" << Qt::endl;

    for (int i = 0; i < config.screens().size(); i++)
        if (!config.screens()[i].isNull())
        {
            outStream << "\t" << config.screens()[i].name() << ":" << Qt::endl;

            for (auto neighbourDir : neighbourDirs)
            {
                int idx = config.adjacentScreenIndex(i, neighbourDir.x, neighbourDir.y);
                if (idx != -1 && !config.screens()[idx].isNull())
                    outStream << "\t\t" << neighbourDir.name << " = " << config.screens()[idx].name() << Qt::endl;
            }
        }

    outStream << "end" << Qt::endl << Qt::endl;
    
    outStream << "section: network" << Qt::endl;
    for (int i = 0; i < config.screens().size(); i++) {
        if (!config.screens()[i].isNull()) {
            const Screen& s = config.screens()[i];
            if (!s.networkIP().isEmpty() || !s.networkSSHUser().isEmpty() || s.networkSSHPort() > 0 || !s.networkClientCmd().isEmpty()) {
                outStream << "\t" << s.name() << ":" << Qt::endl;
                if (!s.networkIP().isEmpty()) outStream << "\t\t" << "ip = " << s.networkIP() << Qt::endl;
                if (!s.networkSSHUser().isEmpty()) outStream << "\t\t" << "ssh_user = " << s.networkSSHUser() << Qt::endl;
                if (s.networkSSHPort() > 0) outStream << "\t\t" << "ssh_port = " << s.networkSSHPort() << Qt::endl;
                if (!s.networkClientCmd().isEmpty()) outStream << "\t\t" << "client_cmd = \"" << s.networkClientCmd() << "\"" << Qt::endl;
            }
        }
    }
    outStream << "end" << Qt::endl << Qt::endl;

    outStream << "section: options" << Qt::endl;
    outStream << "\t" << "server = " << config.m_ServerName << Qt::endl;
    outStream << "\t" << "port = " << config.port() << Qt::endl;
    outStream << "\t" << "cryptoEnabled = " << (config.cryptoEnabled() ? "true" : "false") << Qt::endl;
    outStream << "\t" << "requireClientCertificate = " << (config.requireClientCertificate() ? "true" : "false") << Qt::endl;
    outStream << "\t" << "logLevel = " << config.logLevel() << Qt::endl;
    outStream << "\t" << "logToFile = " << (config.logToFile() ? "true" : "false") << Qt::endl;
    outStream << "\t" << "logFilename = " << config.logFilename() << Qt::endl;
    if (!config.networkInterface().isEmpty()) {
        outStream << "\t" << "networkInterface = " << config.networkInterface() << Qt::endl;
    }
    
    if (config.hasHeartbeat())
        outStream << "\t" << "heartbeat = " << config.heartbeat() << Qt::endl;

    outStream << "\t" << "relativeMouseMoves = " << (config.relativeMouseMoves() ? "true" : "false") << Qt::endl;
    outStream << "\t" << "screenSaverSync = " << (config.screenSaverSync() ? "true" : "false") << Qt::endl;
    outStream << "\t" << "win32KeepForeground = " << (config.win32KeepForeground() ? "true" : "false") << Qt::endl;
    outStream << "\t" << "clipboardSharing = " << (config.clipboardSharing() ? "true" : "false") << Qt::endl;
    outStream << "\t" << "enableDragDrop = " << (config.enableDragAndDrop() ? "true" : "false") << Qt::endl;
    if (!config.dragDropDirectory().isEmpty()) {
        outStream << "\t" << "dropTarget = \"" << config.dragDropDirectory() << "\"" << Qt::endl;
    }
    outStream << "\t" << "gridSize = " << config.numColumns() << "x" << config.numRows() << Qt::endl;

    if (config.hasSwitchDelay())
        outStream << "\t" << "switchDelay = " << config.switchDelay() << Qt::endl;

    if (config.hasSwitchDoubleTap())
        outStream << "\t" << "switchDoubleTap = " << config.switchDoubleTap() << Qt::endl;

    outStream << "\t" << "switchCorners = none ";
    for (int i = 0; i < config.switchCorners().size(); i++) {
        auto corner = static_cast<Screen::SwitchCorner>(i);
        if (config.switchCorners()[i]) {
            outStream << "+" << config.switchCornerName(corner) << " ";
        }
    }
    outStream << Qt::endl;

    outStream << "\t" << "switchCornerSize = " << config.switchCornerSize() << Qt::endl;

    for (const Hotkey& hotkey : config.hotkeys()) {
        outStream << hotkey;
    }

    outStream << "end" << Qt::endl << Qt::endl;

    return outStream;
}

int ServerConfig::numScreens() const
{
    int rval = 0;

    for (const Screen& s : screens()) {
        if (!s.isNull())
            rval++;
    }

    return rval;
}

int ServerConfig::autoAddScreen(const QString name)
{
    int serverIndex = -1;
    int targetIndex = -1;
    if (!findScreenName(m_ServerName, serverIndex)) {
        if (!fixNoServer(m_ServerName, serverIndex)) {
            return kAutoAddScreenManualServer;
        }
    }
    if (findScreenName(name, targetIndex)) {
        // already exists.
        return kAutoAddScreenIgnore;
    }

    int result = showAddClientDialog(name);

    if (result == kAddClientIgnore) {
        return kAutoAddScreenIgnore;
    }

    if (result == kAddClientOther) {
        addToFirstEmptyGrid(name);
        return kAutoAddScreenManualClient;
    }

    bool success = false;
    int startIndex = serverIndex;
    int offset = 1;
    int dirIndex = 0;

    if (result == kAddClientLeft) {
        offset = -1;
        dirIndex = 1;
    }
    else if (result == kAddClientUp) {
        offset = -5;
        dirIndex = 2;
    }
    else if (result == kAddClientDown) {
        offset = 5;
        dirIndex = 3;
    }


    int idx = adjacentScreenIndex(startIndex, neighbourDirs[dirIndex].x,
                    neighbourDirs[dirIndex].y);
    while (idx != -1) {
        if (screens()[idx].isNull()) {
            m_Screens[idx].setName(name);
            success = true;
            break;
        }

        startIndex += offset;
        idx = adjacentScreenIndex(startIndex, neighbourDirs[dirIndex].x,
                    neighbourDirs[dirIndex].y);
    }

    if (!success) {
        addToFirstEmptyGrid(name);
        return kAutoAddScreenManualClient;
    }

    saveSettings();
    return kAutoAddScreenOk;
}

bool ServerConfig::findScreenName(const QString& name, int& index)
{
    bool found = false;
    for (int i = 0; i < screens().size(); i++) {
        if (!screens()[i].isNull() &&
            screens()[i].name().compare(name) == 0) {
            index = i;
            found = true;
            break;
        }
    }
    return found;
}

bool ServerConfig::fixNoServer(const QString& name, int& index)
{
    bool fixed = false;
    if (screens()[serverDefaultIndex()].isNull()) {
        m_Screens[serverDefaultIndex()].setName(name);
        index = serverDefaultIndex();
        fixed = true;
    }

    return fixed;
}

int ServerConfig::showAddClientDialog(const QString& clientName)
{
    int result = kAddClientIgnore;

    if (!m_pMainWindow->isActiveWindow()) {
        m_pMainWindow->showNormal();
        m_pMainWindow->activateWindow();
    }

    AddClientDialog addClientDialog(clientName, m_pMainWindow);
    addClientDialog.exec();
    result = addClientDialog.addResult();
    m_IgnoreAutoConfigClient = addClientDialog.ignoreAutoConfigClient();

    return result;
}

void::ServerConfig::addToFirstEmptyGrid(const QString &clientName)
{
    for (int i = 0; i < screens().size(); i++) {
        if (screens()[i].isNull()) {
            m_Screens[i].setName(clientName);
            break;
        }
    }
}
