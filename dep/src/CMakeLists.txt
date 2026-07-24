#
# This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#

include(FetchContent)
# avoid online check when already downloaded (little speed up)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

if(BUILD_EXTRACTORS AND NOT (BZip2_FOUND OR BZIP2_FOUND))
  add_subdirectory(bzip2)
endif()

# zlib
#By leewheel 2026-07-24 使用本地bundled zlib源码，不再从GitHub下载
find_package(ZLIB CONFIG COMPONENTS static)
if (NOT ZLIB_FOUND OR ZLIB_VERSION_STRING VERSION_LESS 1.3.2)
  message(STATUS "Setting up zlib (local bundled source) ...")
  set(ZLIB_BUILD_SHARED  false)
  set(ZLIB_BUILD_STATIC  true)
  set(ZLIB_BUILD_TESTING false)
  set(ZLIB_INSTALL       false)
  add_subdirectory(zlib)
  # CMaNGOS links a target named "zlib"; zlib's static target is "zlibstatic".
  add_library(zlib ALIAS zlibstatic)
  message(STATUS "zlib successfully set up!")
else()
  add_library(zlib INTERFACE)
  target_include_directories(zlib INTERFACE ${ZLIB_INCLUDE_DIRS})
  target_link_libraries(zlib INTERFACE ${ZLIB_LIBRARIES})
endif()

if (BUILD_GAME_SERVER OR BUILD_LOGIN_SERVER)
  add_subdirectory(gsoap)
endif()

if (BUILD_GAME_SERVER OR BUILD_LOGIN_SERVER OR BUILD_EXTRACTORS)
  add_subdirectory(utf8cpp)
endif()
