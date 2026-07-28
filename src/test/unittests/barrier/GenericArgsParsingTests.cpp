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

#include "barrier/ArgParser.h"
#include "barrier/ArgsBase.h"
#include "test/mock/barrier/MockApp.h"

#include "test/global/gtest.h"
#include <array>

using namespace barrier;
using ::testing::_;
using ::testing::NiceMock;

bool g_helpShowed = false;
bool g_versionShowed = false;

void
showMockHelp()
{
    g_helpShowed = true;
}

void
showMockVersion()
{
    g_versionShowed = true;
}

TEST(GenericArgsParsingTests, parseGenericArgs_logLevelCmd_setLogLevel)
{
    int i = 1;
    constexpr int argc = 3;
    std::array<const char*, argc> kLogLevelCmd = { "stub", "--debug", "DEBUG" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kLogLevelCmd.data(), i);

    String logFilter(argsBase.m_logFilter);

    EXPECT_EQ("DEBUG", logFilter);
    EXPECT_EQ(2, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_logFileCmd_saveLogFilename)
{
    int i = 1;
    constexpr int argc = 3;
    std::array<const char*, argc> kLogFileCmd = { "stub", "--log", "mock_filename" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kLogFileCmd.data(), i);

    String logFile(argsBase.m_logFile);

    EXPECT_EQ("mock_filename", logFile);
    EXPECT_EQ(2, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_logFileCmdWithSpace_saveLogFilename)
{
    int i = 1;
    constexpr int argc = 3;
    std::array<const char*, argc> kLogFileCmdWithSpace = { "stub", "--log", "mo ck_filename" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kLogFileCmdWithSpace.data(), i);

    String logFile(argsBase.m_logFile);

    EXPECT_EQ("mo ck_filename", logFile);
    EXPECT_EQ(2, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_noDeamonCmd_daemonFalse)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kNoDeamonCmd = { "stub", "-f" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kNoDeamonCmd.data(), i);

    EXPECT_FALSE(argsBase.m_daemon);
    EXPECT_EQ(1, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_deamonCmd_daemonTrue)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kDeamonCmd = { "stub", "--daemon" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kDeamonCmd.data(), i);

    EXPECT_EQ(true, argsBase.m_daemon);
    EXPECT_EQ(1, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_nameCmd_saveName)
{
    int i = 1;
    constexpr int argc = 3;
    std::array<const char*, argc> kNameCmd = { "stub", "--name", "mock" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kNameCmd.data(), i);

    EXPECT_EQ("mock", argsBase.m_name);
    EXPECT_EQ(2, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_noRestartCmd_restartFalse)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kNoRestartCmd = { "stub", "--no-restart" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kNoRestartCmd.data(), i);

    EXPECT_FALSE(argsBase.m_restartable);
    EXPECT_EQ(1, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_restartCmd_restartTrue)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kRestartCmd = { "stub", "--restart" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kRestartCmd.data(), i);

    EXPECT_EQ(true, argsBase.m_restartable);
    EXPECT_EQ(1, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_backendCmd_backendTrue)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kBackendCmd = { "stub", "-z" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kBackendCmd.data(), i);

    EXPECT_EQ(true, argsBase.m_backend);
    EXPECT_EQ(1, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_noHookCmd_noHookTrue)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kNoHookCmd = { "stub", "--no-hooks" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kNoHookCmd.data(), i);

    EXPECT_EQ(true, argsBase.m_noHooks);
    EXPECT_EQ(1, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_helpCmd_showHelp)
{
    g_helpShowed = false;
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kHelpCmd = { "stub", "--help" };

    NiceMock<MockApp> app;
    ArgParser argParser(&app);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);
    ON_CALL(app, help()).WillByDefault(showMockHelp);

    argParser.parseGenericArgs(argc, kHelpCmd.data(), i);

    EXPECT_EQ(true, g_helpShowed);
    EXPECT_EQ(1, i);
}


TEST(GenericArgsParsingTests, parseGenericArgs_versionCmd_showVersion)
{
    g_versionShowed = false;
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kVersionCmd = { "stub", "--version" };

    NiceMock<MockApp> app;
    ArgParser argParser(&app);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);
    ON_CALL(app, version()).WillByDefault(showMockVersion);

    argParser.parseGenericArgs(argc, kVersionCmd.data(), i);

    EXPECT_EQ(true, g_versionShowed);
    EXPECT_EQ(1, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_noTrayCmd_disableTrayTrue)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kNoTrayCmd = { "stub", "--no-tray" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kNoTrayCmd.data(), i);

    EXPECT_EQ(true, argsBase.m_disableTray);
    EXPECT_EQ(1, i);
}

TEST(GenericArgsParsingTests, parseGenericArgs_ipcCmd_enableIpcTrue)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kIpcCmd = { "stub", "--ipc" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kIpcCmd.data(), i);

    EXPECT_EQ(true, argsBase.m_enableIpc);
    EXPECT_EQ(1, i);
}

#ifndef  WINAPI_XWINDOWS
TEST(GenericArgsParsingTests, parseGenericArgs_dragDropCmdOnNonLinux_enableDragDropTrue)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kDragDropCmd = { "stub", "--enable-drag-drop" };

    ArgParser argParser(nullptr);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kDragDropCmd.data(), i);

    EXPECT_EQ(true, argsBase.m_enableDragDrop);
    EXPECT_EQ(1, i);
}
#endif

#ifdef  WINAPI_XWINDOWS
TEST(GenericArgsParsingTests, parseGenericArgs_dragDropCmdOnLinux_enableDragDropTrue)
{
    int i = 1;
    constexpr int argc = 2;
    std::array<const char*, argc> kDragDropCmd = { "stub", "--enable-drag-drop" };

    ArgParser argParser(NULL);
    ArgsBase argsBase;
    argParser.setArgsBase(argsBase);

    argParser.parseGenericArgs(argc, kDragDropCmd.data(), i);

    // X11 drag-and-drop is now supported; --enable-drag-drop must work on Linux.
    EXPECT_TRUE(argsBase.m_enableDragDrop);
    EXPECT_EQ(1, i);
}
#endif
