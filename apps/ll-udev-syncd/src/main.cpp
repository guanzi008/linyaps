/*
 * SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "udev_sync_service.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>

#include <unistd.h>

namespace {

constexpr auto kServiceName = "org.deepin.linglong.UdevSync1";
constexpr auto kObjectPath = "/org/deepin/linglong/UdevSync1";

} // namespace

auto main(int argc, char *argv[]) -> int
{
    QCoreApplication app(argc, argv);

    if (::geteuid() != 0) {
        qCritical() << "ll-udev-syncd must run as root";
        return -1;
    }

    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        qCritical() << "system bus is not available";
        return -1;
    }

    if (!bus.registerService(kServiceName)) {
        qCritical() << "register dbus service failed:" << bus.lastError().message();
        return -1;
    }

    UdevSyncService service;
    if (!bus.registerObject(kObjectPath, &service, QDBusConnection::ExportAllSlots)) {
        qCritical() << "register dbus object failed:" << bus.lastError().message();
        return -1;
    }

    return QCoreApplication::exec();
}
