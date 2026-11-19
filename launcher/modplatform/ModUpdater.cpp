// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher AI fork
 *  Copyright (C) 2026 Prism Launcher Contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 */

#include "ModUpdater.h"

#include "Application.h"
#include "FileSystem.h"
#include "minecraft/MinecraftInstance.h"
#include "modplatform/packwiz/Packwiz.h"

#include <QObject>

#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

using namespace Qt::Literals::StringLiterals;

namespace {

struct VersionFile {
    QString versionId;
    QString versionNumber;
    QString fileName;
    QString downloadUrl;
    QString sha512;
};

QJsonDocument modrinthGet(const QString& path)
{
    QNetworkRequest request((QUrl(QStringLiteral("https://api.modrinth.com/v2%1").arg(path))));
    request.setTransferTimeout(180000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", QString("%1/%2 (AI assistant)")
                                            .arg(BuildConfig.LAUNCHER_DISPLAYNAME, BuildConfig.printableVersionString())
                                            .toUtf8());

    auto* nam = new QNetworkAccessManager();
    QEventLoop loop;
    QTimer::singleShot(180000, &loop, &QEventLoop::quit);
    auto* reply = nam->get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray body = reply->readAll();
    const bool ok = reply->error() == QNetworkReply::NoError;
    reply->deleteLater();
    nam->deleteLater();
    if (!ok) {
        return {};
    }
    return QJsonDocument::fromJson(body);
}

bool sha512Matches(const QString& filePath, const QString& expected)
{
    if (expected.isEmpty()) {
        return true;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha512);
    hash.addData(file.readAll());
    return QString::fromLatin1(hash.result().toHex()) == expected.toLower();
}

}  // namespace

namespace ModUpdater {

UpdateSummary updateInstanceMods(MinecraftInstance* instance,
                                 const std::function<void(int, int, const QString&)>& progress)
{
    UpdateSummary summary;
    const QDir modsDir(FS::PathCombine(instance->gameRoot(), "mods"));
    if (!modsDir.exists()) {
        summary.failed << QObject::tr("Mods folder does not exist: %1").arg(modsDir.path());
        if (progress) progress(0, 1, summary.failed.last());
        return summary;
    }

    const auto pwFiles = modsDir.entryList(QStringList{ "*.pw.toml" }, QDir::Files, QDir::Name);
    if (pwFiles.isEmpty()) {
        summary.upToDate << QObject::tr("No packwiz metadata (*.pw.toml) found — nothing to update");
        if (progress) progress(1, 1, summary.upToDate.last());
        return summary;
    }

    const int total = int(pwFiles.size());
    int idx = 0;
    for (const auto& pwName : pwFiles) {
        ++idx;
        if (progress) progress(idx, total, pwName);
        const QString slug = pwName.left(pwName.size() - QString(".pw.toml").size());
        auto mod = Packwiz::V1::getIndexForMod(modsDir, slug);
        if (!mod.isValid()) {
            summary.skippedNoMetadata << slug;
            continue;
        }
        if (mod.provider != ModPlatform::ResourceProvider::MODRINTH) {
            summary.skippedNoMetadata << slug;
            continue;
        }

        const QString projectId = mod.modId().toString();
        const QString currentVersion = mod.version().toString();

        // versions compatible with any of the mod's declared MC versions
        auto doc = modrinthGet(QStringLiteral("/project/%1/version").arg(projectId));
        const auto versions = doc.array();
        if (versions.isEmpty()) {
            summary.upToDate << QObject::tr("%1: no versions found").arg(slug);
            continue;
        }

        // find the newest entry that matches the mod's MC versions and loader
        std::optional<VersionFile> latest;
        for (const auto& v : versions) {
            auto obj = v.toObject();
            // check game versions compatibility
            const auto gameVersions = obj["game_versions"].toArray();
            bool compatible = gameVersions.isEmpty();
            if (!compatible) {
                for (const auto& gv : gameVersions) {
                    if (mod.mcVersions.contains(gv.toString())) {
                        compatible = true;
                        break;
                    }
                }
            }
            if (!compatible) {
                continue;
            }
            // pick primary file
            auto files = obj["files"].toArray();
            QJsonObject chosen;
            bool fileFound = false;
            for (const auto& f : files) {
                auto fo = f.toObject();
                if (fo["primary"].toBool()) {
                    chosen = fo;
                    fileFound = true;
                    break;
                }
            }
            if (!fileFound && !files.isEmpty()) {
                chosen = files.first().toObject();
            }
            if (!fileFound) {
                continue;
            }
            latest = VersionFile{ obj["id"].toString(),
                                  obj["version_number"].toString(),
                                  chosen["filename"].toString(),
                                  chosen["url"].toString(),
                                  chosen["hashes"].toObject()["sha512"].toString() };
            break;
        }

        if (!latest || latest->versionNumber == currentVersion || latest->fileName == mod.filename) {
            summary.upToDate << QObject::tr("%1 (id=%2) is up to date").arg(slug, currentVersion);
            if (progress) progress(idx, total, slug);
            continue;
        }

        // download the new file
        const QString newFilePath = modsDir.filePath(latest->fileName);
        if (!downloadToFile(latest->downloadUrl, newFilePath)) {
            summary.failed << QObject::tr("%1: failed to download %2").arg(slug, latest->fileName);
            continue;
        }
        if (!sha512Matches(newFilePath, latest->sha512)) {
            QFile::remove(newFilePath);
            summary.failed << QObject::tr("%1: SHA512 mismatch for %2").arg(slug, latest->fileName);
            continue;
        }

        // remove the old file if the name changed
        if (mod.filename != latest->fileName) {
            QFile::remove(modsDir.filePath(mod.filename));
        }

        // update the metadata
        mod.filename = latest->fileName;
        mod.version() = latest->versionId;
        Packwiz::V1::updateModIndex(modsDir, mod);

        summary.updated << QObject::tr("%1: %2 → %3 (%4)").arg(slug, currentVersion, latest->versionNumber, latest->fileName);
        if (progress) progress(idx, total, summary.updated.last());
    }

    return summary;
}

QString summarize(const UpdateSummary& summary)
{
    QStringList lines;
    if (!summary.updated.isEmpty()) {
        lines << QObject::tr("Updated:");
        lines += summary.updated;
    }
    if (!summary.upToDate.isEmpty()) {
        lines << QObject::tr("Up to date:");
        lines += summary.upToDate;
    }
    if (!summary.skippedNoMetadata.isEmpty()) {
        lines << QObject::tr("Skipped (non-Modrinth or no metadata):");
        lines += summary.skippedNoMetadata;
    }
    if (!summary.failed.isEmpty()) {
        lines << QObject::tr("Failed:");
        lines += summary.failed;
    }
    return lines.join(u'\n');
}

UpdateSummary updateInstanceModsSync(MinecraftInstance* instance,
                                     const std::function<void(int, int, const QString&)>& progress)
{
    return updateInstanceMods(instance, progress);
}

}  // namespace ModUpdater
