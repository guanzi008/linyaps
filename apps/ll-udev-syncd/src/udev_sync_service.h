/*
 * SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef LINGLONG_UDEV_SYNC_SERVICE_H_
#define LINGLONG_UDEV_SYNC_SERVICE_H_

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

class UdevSyncService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.deepin.linglong.UdevSync1")

public:
    explicit UdevSyncService(QObject *parent = nullptr);

public Q_SLOTS:
    Q_SCRIPTABLE bool AcquireAppRules(const QString &appId,
                                      const QString &sourceRulesDir,
                                      qulonglong clientPid);
    Q_SCRIPTABLE bool ReleaseAppRules(const QString &appId, qulonglong clientPid);
    Q_SCRIPTABLE bool SyncExportedAppRules(const QString &appId, const QString &sourceRulesDir);
    Q_SCRIPTABLE bool RemoveAppRules(const QString &appId);

private:
    struct AppRuleSession
    {
        QString appId;
        QString sourceRulesDir;
        qulonglong clientPid{ 0 };
    };

    bool acquireAppRules(const QString &appId, const QString &sourceRulesDir, qulonglong clientPid);
    bool releaseAppRules(const QString &appId, qulonglong clientPid);
    bool syncExportedAppRules(const QString &appId, const QString &sourceRulesDir);
    bool removeAppRules(const QString &appId);
    bool removeAppRules(const QString &appId, bool reload);
    bool reloadAndTrigger(const QSet<QString> &subsystems) const;
    bool validateRuleFile(const QString &path, QSet<QString> &subsystems) const;
    void collectStaleSessions();
    static bool isProcessAlive(qulonglong clientPid);
    static QString makeSessionKey(const QString &appId, qulonglong clientPid);

    static QString sanitizeAppId(const QString &appId);
    static bool isValidAppId(const QString &appId);

    QHash<QString, AppRuleSession> sessions;
    QHash<QString, int> appRefCounts;
    QHash<QString, QString> appSourceDirs;
    QTimer gcTimer;
};

#endif // LINGLONG_UDEV_SYNC_SERVICE_H_
