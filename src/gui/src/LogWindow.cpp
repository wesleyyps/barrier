/*
* barrier -- mouse and keyboard sharing utility
* Copyright (C) 2018 Debauchee Open Source Group
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

#include "LogWindow.h"

#include <QDateTime>
#include <QTextStream>

static QString getTimeStamp()
{
    QDateTime current = QDateTime::currentDateTime();
    return '[' + current.toString(Qt::ISODate) + ']';
}

LogWindow::LogWindow(QWidget *parent) :
    QDialog(parent)
{
    // explicitly unset DeleteOnClose so the log window can be show and hidden
    // repeatedly until Barrier is finished
    setAttribute(Qt::WA_DeleteOnClose, false);
    setupUi(this);
}

void LogWindow::tailLogFile(const QString& path)
{
    stopTailing();
    
    if (path.isEmpty()) return;
    
    m_pLogFile = new QFile(path, this);
    if (m_pLogFile->open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Read existing content
        QTextStream in(m_pLogFile);
        QString text = in.readAll();
        if (!text.isEmpty()) {
            appendRaw(text);
        }
        
        m_pWatcher = new QFileSystemWatcher(this);
        m_pWatcher->addPath(path);
        connect(m_pWatcher, SIGNAL(fileChanged(const QString&)), this, SLOT(onLogFileChanged(const QString&)));
    } else {
        delete m_pLogFile;
        m_pLogFile = nullptr;
    }
}

void LogWindow::stopTailing()
{
    if (m_pWatcher) {
        m_pWatcher->deleteLater();
        m_pWatcher = nullptr;
    }
    if (m_pLogFile) {
        m_pLogFile->close();
        delete m_pLogFile;
        m_pLogFile = nullptr;
    }
}

void LogWindow::onLogFileChanged(const QString& path)
{
    if (m_pLogFile && m_pLogFile->isOpen()) {
        QTextStream in(m_pLogFile);
        QString newData = in.readAll();
        if (!newData.isEmpty()) {
            appendRaw(newData);
        }
    }
}

void LogWindow::startNewInstance()
{
    // put a space between last log output and new instance.
    if (!m_pLogOutput->toPlainText().isEmpty())
        appendRaw("");
}

void LogWindow::appendInfo(const QString& text)
{
    appendRaw(getTimeStamp() + " INFO: " + text);
}

void LogWindow::appendDebug(const QString& text)
{
    appendRaw(getTimeStamp() + " DEBUG: " + text);
}

void LogWindow::appendError(const QString& text)
{
    appendRaw(getTimeStamp() + " ERROR: " + text);
}

void LogWindow::appendRaw(const QString& text)
{
    m_pLogOutput->append(text);
}

void LogWindow::on_m_pButtonHide_clicked()
{
    hide();
}

void LogWindow::on_m_pButtonClearLog_clicked()
{
    m_pLogOutput->clear();
}
