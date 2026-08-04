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

#include "ScreenSetupModel.h"
#include "Screen.h"

#include <QtCore>
#include <QtGui>

const QString ScreenSetupModel::m_MimeType = "application/x-qbarrier-screen";

ScreenSetupModel::ScreenSetupModel(std::vector<Screen>& screens, int numColumns, int numRows, const QString& serverName) :
    m_NumColumns(numColumns),
    m_NumRows(numRows),
    m_Screens(screens),
    m_ServerName(serverName)
{
    if (screens.size() < static_cast<std::vector<Screen>::size_type>(m_NumColumns * m_NumRows))
    {
        qFatal("Not enough elements (%u) in screens QList for %d columns and %d rows", static_cast<unsigned int>(screens.size()), m_NumColumns, m_NumRows);
    }
}

void ScreenSetupModel::updateGridSize(int numColumns, int numRows)
{
    beginResetModel();
    m_NumColumns = numColumns;
    m_NumRows = numRows;
    endResetModel();
}

void ScreenSetupModel::setConnectedClients(const QSet<QString>& clients)
{
    m_ConnectedClients = clients;
    if (m_NumRows > 0 && m_NumColumns > 0) {
        emit dataChanged(index(0, 0), index(m_NumRows - 1, m_NumColumns - 1));
    }
}

void ScreenSetupModel::onClientConnected(const QString& name)
{
    m_ConnectedClients.insert(name);
    for (int r = 0; r < m_NumRows; ++r) {
        for (int c = 0; c < m_NumColumns; ++c) {
            if (!screen(c, r).isNull() && screen(c, r).name() == name) {
                emit dataChanged(index(r, c), index(r, c));
                return;
            }
        }
    }
}

void ScreenSetupModel::onClientDisconnected(const QString& name)
{
    m_ConnectedClients.remove(name);
    for (int r = 0; r < m_NumRows; ++r) {
        for (int c = 0; c < m_NumColumns; ++c) {
            if (!screen(c, r).isNull() && screen(c, r).name() == name) {
                emit dataChanged(index(r, c), index(r, c));
                return;
            }
        }
    }
}

QVariant ScreenSetupModel::data(const QModelIndex& index, int role) const
{
    if (index.isValid() && index.row() < m_NumRows && index.column() < m_NumColumns)
    {
        switch(role)
        {
            case Qt::DecorationRole:
                if (screen(index).isNull())
                    break;
                return QIcon(*screen(index).pixmap());

            case Qt::ToolTipRole:
                if (screen(index).isNull())
                    break;
                return QString(tr(
                            "<center>Screen: <b>%1</b></center>"
                            "<br>Double click to edit settings"
                            "<br>Drag screen to the trashcan to remove it")).arg(screen(index).name());

            case Qt::DisplayRole:
                if (screen(index).isNull())
                    break;
                return screen(index).name();
            case Qt::BackgroundRole:
                if (!screen(index).isNull()) {
                    if (screen(index).name() == m_ServerName) {
                        return QBrush(QColor(25, 118, 210)); // Blue brush
                    } else if (m_ConnectedClients.contains(screen(index).name())) {
                        return QBrush(QColor(46, 125, 50)); // Green brush
                    }
                }
                break;
            case Qt::ForegroundRole:
                if (!screen(index).isNull()) {
                    if (screen(index).name() == m_ServerName || m_ConnectedClients.contains(screen(index).name())) {
                        return QBrush(Qt::white);
                    }
                }
                break;
            default:
                break;
        }
    }

    return QVariant();
}

Qt::ItemFlags ScreenSetupModel::flags(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() >= m_NumRows || index.column() >= m_NumColumns)
        return Qt::ItemFlags();

    if (!screen(index).isNull())
        return Qt::ItemIsEnabled | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;

    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;
}

Qt::DropActions ScreenSetupModel::supportedDropActions() const
{
    return Qt::MoveAction | Qt::CopyAction;
}

QStringList ScreenSetupModel::mimeTypes() const
{
    return QStringList() << m_MimeType;
}

QMimeData* ScreenSetupModel::mimeData(const QModelIndexList& indexes) const
{
    auto* pMimeData = new QMimeData();
    QByteArray encodedData;

    QDataStream stream(&encodedData, QIODevice::WriteOnly);

    for (const QModelIndex& index : indexes) {
        if (index.isValid())
            stream << index.column() << index.row() << screen(index);
    }

    pMimeData->setData(m_MimeType, encodedData);

    return pMimeData;
}

bool ScreenSetupModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent)
{
    if (action == Qt::IgnoreAction)
        return true;

    if (!data->hasFormat(m_MimeType))
        return false;

     if (!parent.isValid() || row != -1 || column != -1)
        return false;

    QByteArray encodedData = data->data(m_MimeType);
    QDataStream stream(&encodedData, QIODevice::ReadOnly);

    int sourceColumn = -1;
    int sourceRow = -1;

    stream >> sourceColumn;
    stream >> sourceRow;

    // don't drop screen onto itself
    if (sourceColumn == parent.column() && sourceRow == parent.row())
        return false;

    Screen droppedScreen;
    stream >> droppedScreen;

    Screen oldScreen = screen(parent.column(), parent.row());
    if (!oldScreen.isNull() && sourceColumn != -1 && sourceRow != -1)
    {
        // mark the screen so it isn't deleted after the dragndrop succeeded
        // see ScreenSetupView::startDrag()
        oldScreen.setSwapped(true);
        screen(sourceColumn, sourceRow) = oldScreen;
    }

    screen(parent.column(), parent.row()) = droppedScreen;

    return true;
}
