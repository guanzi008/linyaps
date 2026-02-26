# SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
#
# SPDX-License-Identifier: LGPL-3.0-or-later

pfl_add_executable(
  SOURCES
  ./src/main.cpp
  ./src/udev_sync_service.cpp
  ./src/udev_sync_service.h
  OUTPUT_NAME
  ll-udev-syncd
  LIBEXEC
  linglong
  LINK_LIBRARIES
  PUBLIC
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::DBus)
