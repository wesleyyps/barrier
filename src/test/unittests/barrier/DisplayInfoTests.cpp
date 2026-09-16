/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/protocol_types.h"
#include "server/Config.h"
#include "test/global/gtest.h"
#include <sstream>

TEST(DisplayInfoTests, serializeAndDeserializeSingle)
{
    std::vector<DisplayInfo> input;
    input.emplace_back("SyncMaster:1129132345", "SyncMaster", -1920, 0, 1920, 1080, false);

    std::string serialized = DisplayInfo::serializeList(input);
    EXPECT_FALSE(serialized.empty());

    std::vector<DisplayInfo> output = DisplayInfo::deserializeList(serialized);
    ASSERT_EQ(output.size(), 1u);
    EXPECT_EQ(output[0].m_id, "SyncMaster:1129132345");
    EXPECT_EQ(output[0].m_name, "SyncMaster");
    EXPECT_EQ(output[0].m_x, -1920);
    EXPECT_EQ(output[0].m_y, 0);
    EXPECT_EQ(output[0].m_w, 1920);
    EXPECT_EQ(output[0].m_h, 1080);
    EXPECT_FALSE(output[0].m_isPrimary);
}

TEST(DisplayInfoTests, serializeAndDeserializeMultiple)
{
    std::vector<DisplayInfo> input;
    input.emplace_back("SyncMaster:1129132345", "SyncMaster", -1920, 0, 1920, 1080, false);
    input.emplace_back("Color LCD:4251086178", "Built-in Retina Display", 0, 0, 1512, 982, true);

    std::string serialized = DisplayInfo::serializeList(input);
    std::vector<DisplayInfo> output = DisplayInfo::deserializeList(serialized);

    ASSERT_EQ(output.size(), 2u);
    EXPECT_EQ(output[0].m_name, "SyncMaster");
    EXPECT_EQ(output[0].m_x, -1920);
    EXPECT_EQ(output[1].m_name, "Built-in Retina Display");
    EXPECT_EQ(output[1].m_x, 0);
    EXPECT_TRUE(output[1].m_isPrimary);
}

TEST(DisplayInfoTests, configParseMonitorsSection)
{
    const std::string conf =
        "section: screens\n"
        "    macbook:\n"
        "    laptop:\n"
        "end\n"
        "section: monitors\n"
        "    syncmaster:\n"
        "        name = *SyncMaster*\n"
        "        host = macbook\n"
        "    retina:\n"
        "        name = *Retina*\n"
        "end\n"
        "section: links\n"
        "    retina:\n"
        "        left = laptop\n"
        "    laptop:\n"
        "        right = retina\n"
        "        left = syncmaster\n"
        "    syncmaster:\n"
        "        right = laptop\n"
        "end\n";

    std::istringstream stream(conf);
    Config config(nullptr);
    stream >> config;

    EXPECT_TRUE(config.isMonitor("syncmaster"));
    EXPECT_TRUE(config.isMonitor("retina"));
    EXPECT_FALSE(config.isMonitor("macbook"));

    const auto& monitors = config.getMonitors();
    ASSERT_EQ(monitors.size(), 2u);

    auto sm = monitors.find("syncmaster");
    ASSERT_NE(sm, monitors.end());
    EXPECT_EQ(sm->second.matchPattern, "*SyncMaster*");
    EXPECT_EQ(sm->second.defaultHost, "macbook");

    auto ret = monitors.find("retina");
    ASSERT_NE(ret, monitors.end());
    EXPECT_EQ(ret->second.matchPattern, "*Retina*");

    // Verify links navigation with monitor names
    float t = 0.5f;
    float tDest = 0.0f;
    std::string neighbor = config.getNeighbor("retina", kLeft, t, &tDest);
    EXPECT_EQ(neighbor, "laptop");

    neighbor = config.getNeighbor("laptop", kLeft, t, &tDest);
    EXPECT_EQ(neighbor, "syncmaster");

    neighbor = config.getNeighbor("syncmaster", kRight, t, &tDest);
    EXPECT_EQ(neighbor, "laptop");
}

TEST(DisplayInfoTests, monitorTargetCoordinateCalculation)
{
    // Simulate user setup:
    // Host MacBook has:
    //   SyncMaster: [-1360, 0, 1360, 768]
    //   Retina:     [0, 0, 1512, 982]
    // Host Laptop has:
    //   Screen:     [0, 0, 1366, 768]
    DisplayInfo syncmaster("SyncMaster:1", "SyncMaster", -1360, 0, 1360, 768, false);
    DisplayInfo retina("Retina:1", "Built-in Retina Display", 0, 0, 1512, 982, true);

    float t = 0.5f;

    // 1. Entering Retina from right (moving Left from .101)
    // Must enter Retina's right edge: disp.m_x + disp.m_w - 1 = 0 + 1512 - 1 = 1511
    SInt32 x = retina.m_x + retina.m_w - 1;
    SInt32 y = retina.m_y + static_cast<SInt32>(t * retina.m_h);
    EXPECT_EQ(x, 1511);
    EXPECT_EQ(y, 491);
    EXPECT_GE(x, retina.m_x);
    EXPECT_LT(x, retina.m_x + retina.m_w);

    // 2. Entering SyncMaster from right (moving Left from laptop .102)
    // Must enter SyncMaster's right edge: disp.m_x + disp.m_w - 1 = -1360 + 1360 - 1 = -1
    x = syncmaster.m_x + syncmaster.m_w - 1;
    y = syncmaster.m_y + static_cast<SInt32>(t * syncmaster.m_h);
    EXPECT_EQ(x, -1);
    EXPECT_EQ(y, 384);
    EXPECT_GE(x, syncmaster.m_x);
    EXPECT_LT(x, syncmaster.m_x + syncmaster.m_w);

    // 3. Entering Retina from left (moving Right from laptop .102)
    // Must enter Retina's left edge: disp.m_x = 0
    x = retina.m_x;
    y = retina.m_y + static_cast<SInt32>(t * retina.m_h);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 491);

    // 4. Entering SyncMaster from left (moving Right)
    // Must enter SyncMaster's left edge: disp.m_x = -1360
    x = syncmaster.m_x;
    y = syncmaster.m_y + static_cast<SInt32>(t * syncmaster.m_h);
    EXPECT_EQ(x, -1360);
    EXPECT_EQ(y, 384);
}

TEST(DisplayInfoTests, hostTargetCoordinateExcludesClaimedMonitors)
{
    // Simulate setup where host has 2 displays:
    // Display 0: SyncMaster [0, 0, 1360, 768] (assigned to active monitor "syncmaster")
    // Display 1: Laptop LCD [1360, 0, 1920, 1080] (host's primary/unassigned display)
    DisplayInfo syncmaster("SyncMaster:1", "SyncMaster", 0, 0, 1360, 768, false);
    DisplayInfo laptopLcd("eDP-1", "Built-in Display", 1360, 0, 1920, 1080, true);

    std::vector<DisplayInfo> hostDisplays = {syncmaster, laptopLcd};

    // Filter displays for the host screen (excluding claimed monitor displays)
    std::vector<DisplayInfo> targetDisplays;
    for (const auto& disp : hostDisplays) {
        if (disp.m_x == syncmaster.m_x && disp.m_y == syncmaster.m_y &&
            disp.m_w == syncmaster.m_w && disp.m_h == syncmaster.m_h) {
            // Claimed by "syncmaster"
            continue;
        }
        targetDisplays.push_back(disp);
    }

    ASSERT_EQ(targetDisplays.size(), 1u);
    EXPECT_EQ(targetDisplays[0].m_x, 1360);
    EXPECT_EQ(targetDisplays[0].m_w, 1920);

    SInt32 sx = targetDisplays[0].m_x;
    SInt32 sy = targetDisplays[0].m_y;
    SInt32 sw = targetDisplays[0].m_w;
    SInt32 sh = targetDisplays[0].m_h;

    float t = 0.5f;

    // Moving Right from "syncmaster" into the host computer:
    // Must enter the laptop display at x = 1360, NOT x = 0 (which would wrap back to syncmaster!)
    SInt32 x = sx;
    SInt32 y = sy + static_cast<SInt32>(t * sh);
    EXPECT_EQ(x, 1360);
    EXPECT_EQ(y, 540);
    EXPECT_GE(x, laptopLcd.m_x);
    EXPECT_LT(x, laptopLcd.m_x + laptopLcd.m_w);

    // Moving Left into the host computer from its right:
    // Must enter at the right edge of laptop LCD: 1360 + 1920 - 1 = 3279
    x = sx + sw - 1;
    EXPECT_EQ(x, 3279);
    EXPECT_GE(x, laptopLcd.m_x);
    EXPECT_LT(x, laptopLcd.m_x + laptopLcd.m_w);
}

