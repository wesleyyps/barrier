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
#include <ghc/filesystem.hpp>

#include <fstream>
#include <cstdlib>
#ifdef SYSAPI_WIN32
#include <windows.h>
#endif

static String cleanDropPath(String path)
{
    if (path.empty()) {
        return "";
    }
    size_t first = path.find_first_not_of(" \t\r\n");
    if (first == String::npos) {
        return "";
    }
    size_t last = path.find_last_not_of(" \t\r\n");
    path = path.substr(first, last - first + 1);

    if (path.size() >= 2) {
        if ((path.front() == '"' && path.back() == '"') ||
            (path.front() == '\'' && path.back() == '\'')) {
            path = path.substr(1, path.size() - 2);
            first = path.find_first_not_of(" \t\r\n");
            if (first == String::npos) {
                return "";
            }
            last = path.find_last_not_of(" \t\r\n");
            path = path.substr(first, last - first + 1);
        }
    }

    if (path == "~" || path.rfind("~/", 0) == 0 || path.rfind("~\\", 0) == 0) {
        const char* home = std::getenv("HOME");
#ifdef SYSAPI_WIN32
        if (home == nullptr) {
            home = std::getenv("USERPROFILE");
        }
#endif
        if (home != nullptr) {
            if (path == "~") {
                path = home;
            }
            else {
                path = std::string(home) + path.substr(1);
            }
        }
    }

    return path;
}

static String getDefaultDownloadsDir()
{
    String defDir;
#ifdef SYSAPI_WIN32
    char userProfile[MAX_PATH];
    if (GetEnvironmentVariableA("USERPROFILE", userProfile, MAX_PATH) > 0) {
        defDir = std::string(userProfile) + "\\Downloads";
    }
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        defDir = std::string(home) + "/Downloads";
    }
#endif
    return defDir;
}

void
DropHelper::writeToDir(const String& destination, DragFileList& fileList, String& data)
{
    String dropTarget = cleanDropPath(destination);
    if (dropTarget.empty()) {
        dropTarget = getDefaultDownloadsDir();
        if (!dropTarget.empty()) {
            LOG((CLOG_INFO "drop target was empty, falling back to: %s", dropTarget.c_str()));
        }
    }

    LOG((CLOG_DEBUG "dropping file, files=%i target=%s", fileList.size(), dropTarget.c_str()));

    if (!dropTarget.empty() && fileList.size() > 0) {
        std::error_code ec;
        barrier::fs::create_directories(barrier::fs::u8path(dropTarget), ec);
        if (ec || !barrier::fs::is_directory(barrier::fs::u8path(dropTarget), ec)) {
            LOG((CLOG_WARN "Failed to create/access drop target directory %s (%s), falling back to Downloads",
                 dropTarget.c_str(), ec ? ec.message().c_str() : "not a directory"));
            dropTarget = getDefaultDownloadsDir();
            ec.clear();
            barrier::fs::create_directories(barrier::fs::u8path(dropTarget), ec);
        }

        std::fstream file;
        String filePath = dropTarget;
#ifdef SYSAPI_WIN32
        if (!filePath.empty() && filePath.back() != '\\' && filePath.back() != '/') {
            filePath.append("\\");
        }
#else
        if (!filePath.empty() && filePath.back() != '/') {
            filePath.append("/");
        }
#endif
        filePath.append(fileList.at(0).getFilename());
        barrier::open_utf8_path(file, filePath, std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            String fallback = getDefaultDownloadsDir();
            if (dropTarget != fallback && !fallback.empty()) {
                LOG((CLOG_WARN "Could not open %s, retrying in Downloads: %s", filePath.c_str(), fallback.c_str()));
                dropTarget = fallback;
                filePath = dropTarget + "/" + fileList.at(0).getFilename();
                barrier::fs::create_directories(barrier::fs::u8path(dropTarget), ec);
                barrier::open_utf8_path(file, filePath, std::ios::out | std::ios::binary);
            }
        }

        if (!file.is_open()) {
            LOG((CLOG_ERR "drop file failed: can not open %s", filePath.c_str()));
            return;
        }

        file.write(data.c_str(), data.size());
        file.close();

        LOG((CLOG_INFO "dropped file \"%s\" in \"%s\"", fileList.at(0).getFilename().c_str(), dropTarget.c_str()));

#if !defined(SYSAPI_WIN32) && !defined(__APPLE__)
        // Trigger desktop notification on Linux if notify-send is available
        std::string notifyTitle = "Barrier";
        std::string notifyMsg = "Arquivo salvo em: " + dropTarget + "/" + fileList.at(0).getFilename();
        for (size_t pos = 0; (pos = notifyMsg.find('"', pos)) != std::string::npos; pos += 2) {
            notifyMsg.insert(pos, "\\");
        }
        std::string notifyCmd = "notify-send -i document-save \"" + notifyTitle + "\" \"" + notifyMsg + "\" 2>/dev/null &";
        int sysRet = std::system(notifyCmd.c_str());
        (void)sysRet;
#elif defined(__APPLE__)
        // Trigger desktop notification on macOS
        std::string script = "osascript -e 'display notification \"Salvo em: " + dropTarget + "\" with title \"Barrier: " + fileList.at(0).getFilename() + "\"' 2>/dev/null &";
        int sysRet = std::system(script.c_str());
        (void)sysRet;
#endif

        fileList.clear();
    }
    else {
        LOG((CLOG_ERR "drop file failed: drop target is empty"));
    }
}

