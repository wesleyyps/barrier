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

#include <sstream>
#include <sys/stat.h>

static std::string s_lastReceivedFile;

void DropHelper::setLastReceivedFile(const std::string& file)
{
    s_lastReceivedFile = file;
}

const std::string& DropHelper::getLastReceivedFile()
{
    return s_lastReceivedFile;
}

std::string DropHelper::decodeUri(const std::string& uri)
{
    std::string result;
    result.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            char hex[3] = { uri[i+1], uri[i+2], '\0' };
            char* end = nullptr;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                result += static_cast<char>(val);
                i += 2;
                continue;
            }
        }
        result += uri[i];
    }
    return result;
}

std::string DropHelper::getLocalFilePathFromUri(const std::string& text)
{
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 5, "copy\n") == 0) line = line.substr(5);
        if (line == "copy") continue;

        const std::string prefix = "file://";
        if (line.compare(0, prefix.size(), prefix) == 0) {
            std::string path = line.substr(prefix.size());
            if (!path.empty() && path[0] != '/') {
                size_t slash = path.find('/');
                if (slash != std::string::npos) {
                    path = path.substr(slash);
                }
            }
            std::string decodedPath = decodeUri(path);
            struct stat st;
            if (stat(decodedPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                return decodedPath;
            }
        }
    }
    return "";
}

String
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
        std::string filename = fileList.at(0).getFilename();
        filePath.append(filename);
        barrier::open_utf8_path(file, filePath, std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            String fallback = getDefaultDownloadsDir();
            if (dropTarget != fallback && !fallback.empty()) {
                LOG((CLOG_WARN "Could not open %s, retrying in Downloads: %s", filePath.c_str(), fallback.c_str()));
                dropTarget = fallback;
                filePath = dropTarget + "/" + filename;
                barrier::fs::create_directories(barrier::fs::u8path(dropTarget), ec);
                barrier::open_utf8_path(file, filePath, std::ios::out | std::ios::binary);
            }
        }

        if (!file.is_open()) {
            LOG((CLOG_ERR "drop file failed: can not open %s", filePath.c_str()));
            return "";
        }

        file.write(data.c_str(), data.size());
        file.close();

        LOG((CLOG_INFO "dropped file \"%s\" in \"%s\"", filename.c_str(), dropTarget.c_str()));
        setLastReceivedFile(filePath);

#if !defined(SYSAPI_WIN32) && !defined(__APPLE__)
        // Trigger desktop notification on Linux if notify-send is available
        std::string notifyTitle = "Barrier";
        std::string notifyMsg = "Arquivo salvo em: " + dropTarget + "/" + filename + " (Pronto para Colar!)";
        for (size_t pos = 0; (pos = notifyMsg.find('"', pos)) != std::string::npos; pos += 2) {
            notifyMsg.insert(pos, "\\");
        }
        std::string notifyCmd = "notify-send -i document-save \"" + notifyTitle + "\" \"" + notifyMsg + "\" 2>/dev/null &";
        int sysRet = std::system(notifyCmd.c_str());
        (void)sysRet;

        // Also update the Linux clipboard using wl-copy / xclip if available
        std::string fileUri = "file://" + filePath;
        std::string clipCmd =
            "if command -v wl-copy >/dev/null 2>&1; then "
            "  printf 'copy\\n" + fileUri + "\\n' | wl-copy -t x-special/gnome-copied-files 2>/dev/null & "
            "  printf '" + fileUri + "\\r\\n' | wl-copy -t text/uri-list 2>/dev/null & "
            "elif command -v xclip >/dev/null 2>&1; then "
            "  printf 'copy\\n" + fileUri + "\\n' | xclip -selection clipboard -t x-special/gnome-copied-files 2>/dev/null & "
            "  printf '" + fileUri + "\\r\\n' | xclip -selection clipboard -t text/uri-list 2>/dev/null & "
            "fi &";
        int clipRet = std::system(clipCmd.c_str());
        (void)clipRet;
#elif defined(__APPLE__)
        // Trigger desktop notification and set pasteboard on macOS
        std::string script = "osascript -e 'set the clipboard to (POSIX file \"" + filePath + "\")' "
                             "-e 'display notification \"Salvo em: " + dropTarget + "\" with title \"Barrier: " + filename + " (Pronto para Colar)\"' 2>/dev/null &";
        int sysRet = std::system(script.c_str());
        (void)sysRet;
#endif

        fileList.clear();
        return filePath;
    }
    else {
        LOG((CLOG_ERR "drop file failed: drop target is empty"));
        return "";
    }
}


