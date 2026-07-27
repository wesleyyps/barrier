/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2014-2016 Symless Ltd.
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

#include "barrier/DropHelper.h"

#include "base/Log.h"
#include "io/filesystem.h"

#include <fstream>
#include <cstdlib>
#ifdef SYSAPI_WIN32
#include <windows.h>
#endif

void
DropHelper::writeToDir(const String& destination, DragFileList& fileList, String& data)
{
    String dropTarget = destination;
    if (dropTarget.empty()) {
#ifdef SYSAPI_WIN32
        char userProfile[MAX_PATH];
        if (GetEnvironmentVariableA("USERPROFILE", userProfile, MAX_PATH) > 0) {
            dropTarget = std::string(userProfile) + "\\Downloads";
        }
#else
        const char* home = std::getenv("HOME");
        if (home != nullptr) {
            dropTarget = std::string(home) + "/Downloads";
        }
#endif
        if (!dropTarget.empty()) {
            LOG((CLOG_INFO "drop target was empty, falling back to: %s", dropTarget.c_str()));
        }
    }

    LOG((CLOG_DEBUG "dropping file, files=%i target=%s", fileList.size(), dropTarget.c_str()));

    if (!dropTarget.empty() && fileList.size() > 0) {
        std::fstream file;
        String filePath = dropTarget;
#ifdef SYSAPI_WIN32
        filePath.append("\\");
#else
        filePath.append("/");
#endif
        filePath.append(fileList.at(0).getFilename());
        barrier::open_utf8_path(file, filePath, std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            LOG((CLOG_ERR "drop file failed: can not open %s", filePath.c_str()));
            return;
        }

        file.write(data.c_str(), data.size());
        file.close();

        LOG((CLOG_INFO "dropped file \"%s\" in \"%s\"", fileList.at(0).getFilename().c_str(), dropTarget.c_str()));

        fileList.clear();
    }
    else {
        LOG((CLOG_ERR "drop file failed: drop target is empty"));
    }
}
