/*
 * SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "udev_sync_service.h"

#include "configure.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <array>

namespace {

constexpr auto kHostRulesDir = "/etc/udev/rules.d";
constexpr auto kManagedPrefix = "90-linglong-";
constexpr auto kExpectedSuffix = "/files/etc/udev/rules.d";
constexpr auto kSessionGcIntervalMs = 30000;

auto findUdevadm() -> QString
{
    static const std::array<QString, 3> candidates = { "/usr/bin/udevadm",
                                                        "/bin/udevadm",
                                                        "/sbin/udevadm" };
    for (const auto &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return "udevadm";
}

auto parseSubsystems(const QString &line, QSet<QString> &subsystems) -> void
{
    static const QRegularExpression subsystemPattern(
      R"RULE(\bSUBSYSTEMS?\s*==\s*"([A-Za-z0-9_.-]+)")RULE");
    auto it = subsystemPattern.globalMatch(line);
    while (it.hasNext()) {
        const auto match = it.next();
        const auto value = match.captured(1);
        if (!value.isEmpty()) {
            subsystems.insert(value);
        }
    }
}

auto hasDangerousDirective(const QString &line) -> bool
{
    static const QRegularExpression dangerousPattern(
      R"(\b(?:RUN|PROGRAM)\s*[+:]?=|\bIMPORT\{program\}\s*[+:]?=)",
      QRegularExpression::CaseInsensitiveOption);
    return dangerousPattern.match(line).hasMatch();
}

auto isAllowedRulesSourceDir(const QString &path) -> bool
{
    const auto clean = QDir::cleanPath(path);
    if (!clean.endsWith(kExpectedSuffix)) {
        return false;
    }

    const auto expectedPrefix = QStringLiteral(LINGLONG_ROOT) + "/layers/";
    return clean.startsWith(expectedPrefix);
}

} // namespace

UdevSyncService::UdevSyncService(QObject *parent)
    : QObject(parent)
{
    gcTimer.setInterval(kSessionGcIntervalMs);
    gcTimer.setSingleShot(false);
    connect(&gcTimer, &QTimer::timeout, this, [this] {
        collectStaleSessions();
    });
    gcTimer.start();
}

bool UdevSyncService::AcquireAppRules(const QString &appId,
                                      const QString &sourceRulesDir,
                                      qulonglong clientPid)
{
    if (!isValidAppId(appId)) {
        qWarning() << "invalid app id for acquire udev rules:" << appId;
        return false;
    }

    if (!isAllowedRulesSourceDir(sourceRulesDir)) {
        qWarning() << "invalid udev source dir:" << sourceRulesDir;
        return false;
    }

    if (clientPid <= 1) {
        qWarning() << "invalid client pid for acquire udev rules:" << clientPid;
        return false;
    }

    collectStaleSessions();
    return acquireAppRules(appId, sourceRulesDir, clientPid);
}

bool UdevSyncService::ReleaseAppRules(const QString &appId, qulonglong clientPid)
{
    if (!isValidAppId(appId)) {
        qWarning() << "invalid app id for release udev rules:" << appId;
        return false;
    }

    if (clientPid <= 1) {
        qWarning() << "invalid client pid for release udev rules:" << clientPid;
        return false;
    }

    collectStaleSessions();
    return releaseAppRules(appId, clientPid);
}

bool UdevSyncService::acquireAppRules(const QString &appId,
                                      const QString &sourceRulesDir,
                                      qulonglong clientPid)
{
    const auto sessionKey = makeSessionKey(appId, clientPid);
    const auto existingSession = sessions.constFind(sessionKey);
    if (existingSession != sessions.cend()) {
        if (existingSession->sourceRulesDir != sourceRulesDir) {
            qWarning() << "conflicting source dir for existing udev session:"
                       << existingSession->sourceRulesDir << sourceRulesDir;
            return false;
        }

        return true;
    }

    const auto refCount = appRefCounts.value(appId, 0);
    if (refCount == 0) {
        if (!syncExportedAppRules(appId, sourceRulesDir)) {
            return false;
        }
        appSourceDirs.insert(appId, sourceRulesDir);
    } else {
        const auto existingSourceDir = appSourceDirs.value(appId);
        if (!existingSourceDir.isEmpty() && existingSourceDir != sourceRulesDir) {
            qWarning() << "conflicting source dir for app udev rules:" << appId
                       << existingSourceDir << sourceRulesDir;
            return false;
        }
    }

    sessions.insert(sessionKey, AppRuleSession{ appId, sourceRulesDir, clientPid });
    appRefCounts.insert(appId, refCount + 1);
    return true;
}

bool UdevSyncService::releaseAppRules(const QString &appId, qulonglong clientPid)
{
    const auto sessionKey = makeSessionKey(appId, clientPid);
    const auto sessionIt = sessions.find(sessionKey);
    if (sessionIt == sessions.end()) {
        return true;
    }

    const auto session = *sessionIt;
    sessions.erase(sessionIt);

    auto refIt = appRefCounts.find(session.appId);
    if (refIt == appRefCounts.end()) {
        appSourceDirs.remove(session.appId);
        return true;
    }

    *refIt -= 1;
    if (*refIt > 0) {
        return true;
    }

    appRefCounts.erase(refIt);
    appSourceDirs.remove(session.appId);
    return removeAppRules(session.appId);
}

bool UdevSyncService::SyncExportedAppRules(const QString &appId, const QString &sourceRulesDir)
{
    if (!isValidAppId(appId)) {
        qWarning() << "invalid app id for udev sync:" << appId;
        return false;
    }

    if (!isAllowedRulesSourceDir(sourceRulesDir)) {
        qWarning() << "invalid udev source dir:" << sourceRulesDir;
        return false;
    }

    return syncExportedAppRules(appId, sourceRulesDir);
}

bool UdevSyncService::RemoveAppRules(const QString &appId)
{
    if (!isValidAppId(appId)) {
        qWarning() << "invalid app id for udev remove:" << appId;
        return false;
    }

    const auto appPrefix = QStringLiteral("%1:").arg(appId);
    for (auto it = sessions.begin(); it != sessions.end();) {
        if (it.key().startsWith(appPrefix)) {
            it = sessions.erase(it);
        } else {
            ++it;
        }
    }
    appRefCounts.remove(appId);
    appSourceDirs.remove(appId);

    return removeAppRules(appId);
}

bool UdevSyncService::syncExportedAppRules(const QString &appId, const QString &sourceRulesDir)
{
    const QDir sourceDir(sourceRulesDir);

    auto copiedSubsystems = QSet<QString>{};

    if (!sourceDir.exists()) {
        if (!removeAppRules(appId)) {
            return false;
        }
        return true;
    }

    auto entries = sourceDir.entryInfoList({ "*.rules" }, QDir::Files, QDir::Name);
    if (entries.isEmpty()) {
        if (!removeAppRules(appId)) {
            return false;
        }
        return true;
    }

    if (!removeAppRules(appId, false)) {
        return false;
    }

    const auto appKey = sanitizeAppId(appId);
    for (const auto &entry : entries) {
        if (!validateRuleFile(entry.absoluteFilePath(), copiedSubsystems)) {
            qWarning() << "invalid udev rule file:" << entry.absoluteFilePath();
            return false;
        }

        QFile input(entry.absoluteFilePath());
        if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "failed to read udev file:" << entry.absoluteFilePath()
                       << input.errorString();
            return false;
        }

        const auto target =
          QStringLiteral("%1/%2%3-%4")
            .arg(kHostRulesDir, kManagedPrefix, appKey, entry.fileName());
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning() << "failed to write udev file:" << target << output.errorString();
            return false;
        }

        output.write(input.readAll());
        if (!output.commit()) {
            qWarning() << "failed to commit udev file:" << target << output.errorString();
            return false;
        }

        QFile::setPermissions(target,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }

    return reloadAndTrigger(copiedSubsystems);
}

bool UdevSyncService::removeAppRules(const QString &appId)
{
    return removeAppRules(appId, true);
}

bool UdevSyncService::removeAppRules(const QString &appId, bool reload)
{
    const auto appKey = sanitizeAppId(appId);
    const auto pattern = QStringLiteral("%1%2-*.rules").arg(kManagedPrefix, appKey);

    QDir rulesDir(kHostRulesDir);
    auto entries = rulesDir.entryInfoList({ pattern }, QDir::Files, QDir::Name);

    bool changed = false;
    for (const auto &entry : entries) {
        if (!QFile::remove(entry.absoluteFilePath())) {
            qWarning() << "failed to remove managed udev file:" << entry.absoluteFilePath();
            return false;
        }
        changed = true;
    }

    if (!changed || !reload) {
        return true;
    }

    return reloadAndTrigger({});
}

bool UdevSyncService::reloadAndTrigger(const QSet<QString> &subsystems) const
{
    const auto udevadm = findUdevadm();
    if (QProcess::execute(udevadm, { "control", "--reload-rules" }) != 0) {
        qWarning() << "udevadm --reload-rules failed";
        return false;
    }

    QStringList triggerArgs{ "trigger", "--action=change" };
    if (subsystems.isEmpty()) {
        triggerArgs << "--subsystem-match=usb" << "--subsystem-match=hidraw";
    } else {
        auto sorted = subsystems.values();
        std::sort(sorted.begin(), sorted.end());
        for (const auto &subsystem : sorted) {
            triggerArgs << QStringLiteral("--subsystem-match=%1").arg(subsystem);
        }
    }

    if (QProcess::execute(udevadm, triggerArgs) != 0) {
        qWarning() << "udevadm trigger failed";
        return false;
    }

    return true;
}

bool UdevSyncService::validateRuleFile(const QString &path, QSet<QString> &subsystems) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "failed to open rule file for validation:" << path;
        return false;
    }

    QTextStream input(&file);
    while (!input.atEnd()) {
        const auto line = input.readLine();
        const auto trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }

        if (hasDangerousDirective(trimmed)) {
            qWarning() << "forbidden directive in udev rule:" << trimmed;
            return false;
        }

        parseSubsystems(trimmed, subsystems);
    }

    return true;
}

void UdevSyncService::collectStaleSessions()
{
    if (sessions.isEmpty()) {
        return;
    }

    QSet<QString> staleApps;
    for (auto it = sessions.begin(); it != sessions.end();) {
        if (isProcessAlive(it->clientPid)) {
            ++it;
            continue;
        }

        const auto appId = it->appId;
        it = sessions.erase(it);

        auto refIt = appRefCounts.find(appId);
        if (refIt == appRefCounts.end()) {
            continue;
        }

        *refIt -= 1;
        if (*refIt <= 0) {
            appRefCounts.erase(refIt);
            appSourceDirs.remove(appId);
            staleApps.insert(appId);
        }
    }

    for (const auto &appId : staleApps) {
        if (!removeAppRules(appId)) {
            qWarning() << "failed to cleanup stale udev rules for app:" << appId;
        }
    }
}

bool UdevSyncService::isProcessAlive(qulonglong clientPid)
{
    return QFileInfo::exists(QStringLiteral("/proc/%1").arg(clientPid));
}

QString UdevSyncService::makeSessionKey(const QString &appId, qulonglong clientPid)
{
    return QStringLiteral("%1:%2").arg(appId).arg(clientPid);
}

QString UdevSyncService::sanitizeAppId(const QString &appId)
{
    auto key = appId;
    key.replace('.', '-');
    return key;
}

bool UdevSyncService::isValidAppId(const QString &appId)
{
    static const QRegularExpression pattern(R"(^[A-Za-z0-9][A-Za-z0-9._-]*$)");
    return pattern.match(appId).hasMatch();
}
