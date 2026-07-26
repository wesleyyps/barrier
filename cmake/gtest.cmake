# barrier -- mouse and keyboard sharing utility
# Copyright (C) 2012-2016 Symless Ltd.
# Copyright (C) 2011 Nick Bolton
#
# This package is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# found in the file LICENSE that should have accompanied this file.
#
# This package is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

if (BARRIER_USE_EXTERNAL_GTEST)
    include (FindPkgConfig)
    find_package(GTest REQUIRED)
    pkg_check_modules(GMOCK REQUIRED gmock)
    include_directories(
        ${GTEST_INCLUDE_DIRS}
        ${GMOCK_INCLUDE_DIRS}
    )
else()
    # Add the unified googletest directory which provides the gtest and gmock targets automatically
    add_subdirectory(../ext/googletest ext_googletest)

    set(GTEST_LIBRARIES gtest)
    set(GMOCK_LIBRARIES gmock)

    if (UNIX)
        # ignore warnings in gtest and gmock
        set_target_properties(gtest PROPERTIES COMPILE_FLAGS "-w")
        set_target_properties(gmock PROPERTIES COMPILE_FLAGS "-w")
    endif()
endif()
