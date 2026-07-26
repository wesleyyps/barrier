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
#include "barrier/ClientArgs.h"
#include "test/mock/barrier/MockArgParser.h"

#include "test/global/gtest.h"
#include <array>

using ::testing::_;
using ::testing::NiceMock;

bool
client_stubParseGenericArgs(int, const char* const*, int&)
{
    return false;
}

bool
client_stubCheckUnexpectedArgs()
{
    return false;
}

TEST(ClientArgsParsingTests, parseClientArgs_yScrollArg_setYScroll)
{
    NiceMock<MockArgParser> argParser;
    ON_CALL(argParser, parseGenericArgs(_, _, _)).WillByDefault(client_stubParseGenericArgs);
    ON_CALL(argParser, checkUnexpectedArgs()).WillByDefault(client_stubCheckUnexpectedArgs);
    ClientArgs clientArgs;
    constexpr int argc = 3;
    std::array<const char*, argc> kYScrollCmd = { "stub", "--yscroll", "1" };

    argParser.parseClientArgs(clientArgs, argc, kYScrollCmd.data());

    EXPECT_EQ(1, clientArgs.m_yscroll);
}

TEST(ClientArgsParsingTests, parseClientArgs_addressArg_setBarrierAddress)
{
    NiceMock<MockArgParser> argParser;
    ON_CALL(argParser, parseGenericArgs(_, _, _)).WillByDefault(client_stubParseGenericArgs);
    ON_CALL(argParser, checkUnexpectedArgs()).WillByDefault(client_stubCheckUnexpectedArgs);
    ClientArgs clientArgs;
    constexpr int argc = 2;
    std::array<const char*, argc> kAddressCmd = { "stub", "mock_address" };

    bool result = argParser.parseClientArgs(clientArgs, argc, kAddressCmd.data());

    EXPECT_EQ("mock_address", clientArgs.m_barrierAddress);
    EXPECT_EQ(true, result);
}

TEST(ClientArgsParsingTests, parseClientArgs_noAddressArg_returnFalse)
{
    NiceMock<MockArgParser> argParser;
    ON_CALL(argParser, parseGenericArgs(_, _, _)).WillByDefault(client_stubParseGenericArgs);
    ON_CALL(argParser, checkUnexpectedArgs()).WillByDefault(client_stubCheckUnexpectedArgs);
    ClientArgs clientArgs;
    constexpr int argc = 1;
    std::array<const char*, argc> kNoAddressCmd = { "stub" };

    bool result = argParser.parseClientArgs(clientArgs, argc, kNoAddressCmd.data());

    EXPECT_FALSE(result);
}

TEST(ClientArgsParsingTests, parseClientArgs_unrecognizedArg_returnFalse)
{
    NiceMock<MockArgParser> argParser;
    ON_CALL(argParser, parseGenericArgs(_, _, _)).WillByDefault(client_stubParseGenericArgs);
    ON_CALL(argParser, checkUnexpectedArgs()).WillByDefault(client_stubCheckUnexpectedArgs);
    ClientArgs clientArgs;
    constexpr int argc = 3;
    std::array<const char*, argc> kUnrecognizedCmd = { "stub", "mock_arg", "mock_address"};

    bool result = argParser.parseClientArgs(clientArgs, argc, kUnrecognizedCmd.data());

    EXPECT_FALSE(result);
}
