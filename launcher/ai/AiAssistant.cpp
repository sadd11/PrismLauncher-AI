// SPDX-License-Identifier: GPL-3.0-only
/*
 *  AI Assistant for Prism Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 */

#include "AiAssistant.h"

#include <BuildConfig.h>
#include "Application.h"
#include "BaseVersion.h"
#include "FileSystem.h"
#include "InstanceList.h"
#include "meta/Index.h"
#include "meta/VersionList.h"
#include "meta/Version.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/VanillaInstanceCreationTask.h"
#include "minecraft/World.h"
#include "minecraft/WorldList.h"
#include "minecraft/WorldTasks.h"
#include "modplatform/ModUpdater.h"
#include "modplatform/packwiz/Packwiz.h"
#include "InstanceImportTask.h"
#include "ui/themes/ThemeManager.h"
#include "modplatform/modrinth/ModrinthInstanceCreationTask.h"
#include "settings/SettingsObject.h"
#include "tasks/Task.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QSet>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <optional>
#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace {

constexpr int MAX_TOOL_ITERATIONS = 10;

SettingsObject* aiSettings()
{
    auto* settings = APPLICATION->settings();
    if (!settings->getSetting("AiApiBaseUrl")) {
        settings->registerSetting("AiApiBaseUrl", "https://openrouter.ai/api/v1");
    }
    if (!settings->getSetting("AiApiKey")) {
        settings->registerSetting("AiApiKey", "");
    }
    if (!settings->getSetting("AiModel")) {
        settings->registerSetting("AiModel", "openrouter/free");
    }
    if (!settings->getSetting("AiChatFormat")) {
        settings->registerSetting("AiChatFormat", "auto");
    }
    return settings;
}

QString contentFolderFor(const QString& contentType)
{
    if (contentType == "shader")
        return "shaderpacks";
    if (contentType == "resourcepack" || contentType == "texturepack")
        return "resourcepacks";
    if (contentType == "datapack")
        return "datapacks";
    // mod and plugin both go into the mods folder on the client
    return "mods";
}

QString modrinthProjectType(const QString& contentType)
{
    if (contentType == "texturepack")
        return "resourcepack";
    return contentType;  // mod, plugin, datapack, shader, resourcepack, modpack
}

/// Selects the primary (or first) file of a Modrinth version object.
std::optional<std::pair<QString, QString>> primaryFileOfVersion(const QJsonObject& version)
{
    const auto files = version["files"].toArray();
    if (files.isEmpty()) {
        return std::nullopt;
    }
    QJsonObject chosen = files.first().toObject();
    for (const auto& f : files) {
        auto obj = f.toObject();
        if (obj["primary"].toBool()) {
            chosen = obj;
            break;
        }
    }
    return std::make_pair(chosen["url"].toString(), chosen["filename"].toString());
}

/// CurseForge class ids for content types (verified against /v1/categories?gameId=432).
int cfClassIdFor(const QString& contentType)
{
    if (contentType == "mod")
        return 6;
    if (contentType == "plugin")
        return 5;
    if (contentType == "shader")
        return 6552;
    if (contentType == "resourcepack" || contentType == "texturepack")
        return 12;
    if (contentType == "datapack")
        return 6945;
    if (contentType == "modpack")
        return 4471;
    if (contentType == "world")
        return 17;
    return -1;
}

QJsonDocument cfGet(const QString& path, QString* error = nullptr)
{
    QNetworkRequest request((QUrl(QStringLiteral("https://api.curseforge.com/v1%1").arg(path))));
    request.setTransferTimeout(180000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("x-api-key", BuildConfig.FLAME_API_KEY.toUtf8());
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
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString errorString = reply->errorString();
    reply->deleteLater();
    nam->deleteLater();
    if (!ok) {
        if (error) {
            *error = statusCode > 0 ? QStringLiteral("HTTP %1: %2").arg(statusCode).arg(QString::fromUtf8(body.left(300)))
                                    : errorString;
        }
        return {};
    }
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError && error) {
        *error = QStringLiteral("invalid JSON response: %1").arg(parseError.errorString());
    }
    return doc;
}

/// Picks the newest CurseForge release matching the Minecraft version; returns {url, fileName}.
std::optional<std::pair<QString, QString>> cfLatestFile(const QJsonArray& files, const QString& mcVersion)
{
    qint64 bestScore = -1;
    int bestIdx = -1;
    for (int i = 0; i < files.size(); ++i) {
        auto obj = files.at(i).toObject();
        const auto gameVersions = obj["gameVersions"].toArray();
        if (!mcVersion.isEmpty() && !gameVersions.isEmpty()) {
            bool matches = false;
            for (const auto& v : gameVersions) {
                if (v.toString() == mcVersion) {
                    matches = true;
                    break;
                }
            }
            if (!matches) {
                continue;
            }
        }
        const qint64 id = obj["id"].toVariant().toLongLong();
        // releases beat betas/snapshots, then prefer the highest file id (newest)
        const qint64 score = (obj["releaseType"].toInt() == 1 ? (1LL << 40) : 0) + id;
        if (score > bestScore) {
            bestScore = score;
            bestIdx = i;
        }
    }
    if (bestIdx < 0) {
        return std::nullopt;
    }
    auto obj = files.at(bestIdx).toObject();
    QString fileName = obj["fileName"].toString();
    QString url = obj["downloadUrl"].toString();
    if (url.isEmpty()) {
        const qint64 id = obj["id"].toVariant().toLongLong();
        url = QStringLiteral("https://edge.forgecdn.net/files/%1/%2/%3").arg(id / 1000).arg(id % 1000).arg(fileName);
    }
    return std::make_pair(url, fileName);
}

struct ShotInfo {
    QString path;
    QDateTime mtime;
    qint64 size = 0;
};

/// Newest-first screenshots of an instance (launcher saves + F2 saves).
QList<ShotInfo> collectScreenshots(MinecraftInstance* inst)
{
    QList<ShotInfo> shots;
    QSet<QString> seen;
    const QStringList dirs = { FS::PathCombine(inst->instanceRoot(), "screenshots"),
                               FS::PathCombine(inst->gameRoot(), "screenshots") };
    for (const auto& dirPath : dirs) {
        const QDir dir(dirPath);
        if (!dir.exists()) {
            continue;
        }
        const auto infos = dir.entryInfoList(QStringList{ "*.png", "*.jpg", "*.jpeg" }, QDir::Files, QDir::Time);
        for (const auto& info : infos) {
            const QString canon = info.absoluteFilePath();
            if (seen.contains(canon)) {
                continue;
            }
            seen.insert(canon);
            shots.append({ canon, info.lastModified(), info.size() });
        }
    }
    std::sort(shots.begin(), shots.end(), [](const ShotInfo& a, const ShotInfo& b) { return a.mtime > b.mtime; });
    return shots;
}

}  // namespace

AiAssistant::AiAssistant(QObject* parent) : QObject(parent), m_nam(new QNetworkAccessManager(this))
{
    aiSettings();  // ensure the settings keys exist
}

QString AiAssistant::systemPrompt()
{
    return QObject::tr("You are the built-in AI assistant of Prism Launcher, a Minecraft launcher. "
                       "You control the launcher through tools: you can list/create/delete/launch game instances, "
                       "search Modrinth for mods, plugins, datapacks, shader packs, resource packs and modpacks, "
                       "install content into instances and change instance settings. "
                       "Rules: use the tools to answer questions about the real state — never invent instance names or versions. "
                       "When the user asks for content (e.g. 'install OptiFine', 'find shaders'), first search, then present the best matches, "
                       "then install into the requested (or the only) instance. "
                       "Always confirm before destructive actions (deleting an instance). "
                       "When creating instances pick sensible defaults and report what you created. "
                       "Reply in the language the user writes in. Be concise.");
}

QJsonArray AiAssistant::toolSpecs()
{
    auto fn = [](const QString& name, const QString& description, const QJsonObject& properties,
                 const QStringList& required) -> QJsonObject {
        QJsonObject props;
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            props[it.key()] = it.value();
        }
        QJsonArray req;
        for (const auto& r : required) {
            req.append(r);
        }
        return QJsonObject{ { "type", "function" },
                            { "function",
                              QJsonObject{ { "name", name },
                                           { "description", description },
                                           { "parameters",
                                             QJsonObject{ { "type", "object" }, { "properties", props }, { "required", req } } } } } };
    };
    auto str = [](const QString& description) -> QJsonObject {
        return QJsonObject{ { "type", "string" }, { "description", description } };
    };
    auto intType = [](const QString& description) -> QJsonObject {
        return QJsonObject{ { "type", "integer" }, { "description", description } };
    };
    auto enumType = [](const QString& description, const QStringList& values) -> QJsonObject {
        QJsonArray arr;
        for (const auto& v : values) {
            arr.append(v);
        }
        return QJsonObject{ { "type", "string" }, { "description", description }, { "enum", arr } };
    };

    return QJsonArray{
        fn("list_instances", "List all game instances with their Minecraft version and mod loader.", {}, {}),
        fn("get_instance_info", "Show detailed info about one instance.", { { "instance", str("Instance id or name") } },
           { "instance" }),
        fn("create_instance",
           "Create a new game instance.",
           { { "name", str("Instance display name") },
             { "minecraft_version", str("Minecraft version, e.g. 1.21.1. Use list_minecraft_versions if unsure.") },
             { "loader", enumType("Mod loader", { "none", "fabric", "quilt", "forge", "neoforge" }) },
             { "loader_version", str("Loader version; omit for the latest") },
             { "group", str("Optional instance group") } },
           { "name", "minecraft_version" }),
        fn("delete_instance", "Delete a game instance permanently (destructive!).", { { "instance", str("Instance id or name") } },
           { "instance" }),
        fn("launch_instance",
           "Launch the game for an instance. Waits only until the game process starts.",
           { { "instance", str("Instance id or name") } },
           { "instance" }),
        fn("search_content",
           "Search Modrinth or CurseForge for content.",
           { { "query", str("Search words") },
             { "content_type", enumType("What to search", { "mod", "plugin", "datapack", "shader", "resourcepack", "modpack" }) },
             { "provider", enumType("Where to search (default modrinth)", { "modrinth", "curseforge" }) },
             { "limit", intType("How many results to return (default 8)") } },
           { "query", "content_type" }),
        fn("get_content_versions",
           "List downloadable versions of a Modrinth project.",
           { { "project_id", str("Modrinth project id or slug") },
             { "minecraft_version", str("Optional Minecraft version filter") },
             { "loader", str("Optional loader filter (fabric/quilt/forge/neoforge)") } },
           { "project_id" }),
        fn("install_content",
           "Install content into an instance from Modrinth or CurseForge. For modpacks a NEW instance is created from the pack.",
           { { "instance", str("Target instance id or name (ignored for modpacks)") },
             { "project_id", str("Modrinth project id/slug or CurseForge numeric id") },
             { "content_type", enumType("Content type", { "mod", "plugin", "datapack", "shader", "resourcepack", "modpack" }) },
             { "provider", enumType("Where to install from (default modrinth)", { "modrinth", "curseforge" }) },
             { "version_id", str("Optional specific version id; latest compatible is used otherwise") } },
           { "instance", "project_id", "content_type" }),
        fn("uninstall_mod",
           "Uninstall (delete) a mod from an instance's mods folder by name (destructive!).",
           { { "instance", str("Instance id or name") },
             { "name", str("Mod name, slug or file name (fuzzy match allowed)") } },
           { "instance", "name" }),
        fn("list_screenshots",
           "List the newest screenshots of an instance with their file paths.",
           { { "instance", str("Instance id or name") },
             { "limit", intType("How many screenshots to return (default 8)") } },
           { "instance" }),
        fn("upload_screenshot",
           "Upload a screenshot of an instance to Imgur and return its public URL.",
           { { "instance", str("Instance id or name") },
             { "name", str("Screenshot file name or 1-based index; omit for the newest one") } },
           { "instance" }),
        fn("set_instance_settings",
           "Change instance settings. Pass only the fields to change.",
           { { "instance", str("Instance id or name") },
             { "name", str("New display name") },
             { "min_memory_mb", intType("Minimum memory in MB") },
             { "max_memory_mb", intType("Maximum memory in MB") },
             { "java_path", str("Path to the Java executable") } },
           { "instance" }),
        fn("list_minecraft_versions", "List available Minecraft versions (newest first).", {}, {}),
        fn("list_loader_versions",
           "List available versions of a mod loader.",
           { { "loader", enumType("Loader", { "fabric", "quilt", "forge", "neoforge" }) } },
           { "loader" }),
        fn("get_crash_reports",
           "List crash report files of an instance (newest first).",
           { { "instance", str("Instance id or name") } },
           { "instance" }),
        fn("read_crash_report",
           "Read a crash report (the newest one if filename is omitted).",
           { { "instance", str("Instance id or name") },
             { "filename", str("Crash report filename; omit for the newest") } },
           { "instance" }),
        fn("get_game_log",
           "Return the tail of the Minecraft game log (logs/latest.log).",
           { { "instance", str("Instance id or name") },
             { "tail", intType("How many last lines to return (default 200)") } },
           { "instance" }),
        fn("list_catpacks", "List available catpacks and the currently selected one.", {}, {}),
        fn("set_catpack", "Select a catpack by name (or id).", { { "name", str("Catpack name or id") } },
           { "name" }),
        fn("list_worlds", "List saved worlds of an instance.", { { "instance", str("Instance id or name") } },
           { "instance" }),
        fn("delete_world",
           "Delete a saved world permanently (destructive!).",
           { { "instance", str("Instance id or name") }, { "world", str("World name") } },
           { "instance", "world" }),
        fn("backup_world",
           "Create a backup copy of a world into the instance backups folder.",
           { { "instance", str("Instance id or name") }, { "world", str("World name") } },
           { "instance", "world" }),
        fn("update_all_mods",
           "Update all Modrinth-hosted mods of an instance to their latest versions.",
           { { "instance", str("Instance id or name") } },
           { "instance" }),
    };
}

QString AiAssistant::toolSummary(const QString& name, const QJsonObject& args) const
{
    QStringList parts;
    for (auto it = args.begin(); it != args.end(); ++it) {
        parts << QStringLiteral("%1=%2").arg(it.key(), it.value().toVariant().toString());
    }
    return QStringLiteral("%1(%2)").arg(name, parts.join(u','));
}

// ---------------------------------------------------------------------------
// HTTP plumbing (synchronous with a local event loop)
// ---------------------------------------------------------------------------

AiAssistant::HttpReply AiAssistant::httpRequest(const QString& method, const QString& url, const QByteArray& body,
                                                const QStringList& headers, int timeoutMs)
{
    QNetworkRequest request((QUrl(url)));
    request.setTransferTimeout(timeoutMs);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", QString("%1/%2 (AI assistant)")
                                            .arg(BuildConfig.LAUNCHER_DISPLAYNAME, BuildConfig.printableVersionString())
                                            .toUtf8());
    for (const auto& header : headers) {
        const auto pair = header.split(u':');
        if (pair.size() == 2) {
            request.setRawHeader(pair[0].trimmed().toUtf8(), pair[1].trimmed().toUtf8());
        }
    }

    QNetworkReply* reply = nullptr;
    if (method == "POST") {
        reply = m_nam->post(request, body);
    } else {
        reply = m_nam->get(request);
    }

    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    HttpReply result;
    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        result.error = reply->errorString();
        result.body = reply->readAll();
    } else {
        result.ok = true;
        result.body = reply->readAll();
    }
    reply->deleteLater();
    return result;
}

AiAssistant::HttpReply AiAssistant::httpGet(const QString& url, int timeoutMs)
{
    return httpRequest("GET", url, {}, {}, timeoutMs);
}

AiAssistant::HttpReply AiAssistant::httpDownload(const QString& url, const QString& targetPath)
{
    auto reply = httpGet(url, 600000);
    if (!reply.ok) {
        return reply;
    }
    QFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly)) {
        return { false, 0, QStringLiteral("Cannot open %1 for writing").arg(targetPath), {} };
    }
    file.write(reply.body);
    file.close();
    return reply;
}

QJsonDocument AiAssistant::modrinthGet(const QString& path, QString* error)
{
    auto reply = httpGet(QStringLiteral("https://api.modrinth.com/v2%1").arg(path));
    if (!reply.ok) {
        if (error) {
            *error = reply.statusCode > 0
                         ? QStringLiteral("HTTP %1: %2").arg(reply.statusCode).arg(QString::fromUtf8(reply.body.left(300)))
                         : reply.error;
        }
        return {};
    }
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(reply.body, &parseError);
    if (parseError.error != QJsonParseError::NoError && error) {
        *error = QStringLiteral("invalid JSON response: %1").arg(parseError.errorString());
    }
    return doc;
}

QString AiAssistant::syncRunTask(Task* task)
{
    QEventLoop loop;
    QString error;
    QObject::connect(task, &Task::succeeded, &loop, &QEventLoop::quit);
    QObject::connect(task, &Task::failed, &loop, [&](const QString& reason) {
        error = reason;
        loop.quit();
    });
    QObject::connect(task, &Task::aborted, &loop, [&]() {
        error = tr("Aborted");
        loop.quit();
    });
    task->start();
    loop.exec();
    return error;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

MinecraftInstance* AiAssistant::findInstance(const QString& idOrName) const
{
    auto* instances = APPLICATION->instances();
    if (auto* inst = instances->getInstanceById(idOrName)) {
        return inst;
    }
    // exact name, then case-insensitive substring
    for (int i = 0; i < instances->count(); ++i) {
        auto* inst = instances->at(i);
        if (inst->name().compare(idOrName, Qt::CaseInsensitive) == 0) {
            return inst;
        }
    }
    for (int i = 0; i < instances->count(); ++i) {
        auto* inst = instances->at(i);
        if (inst->name().contains(idOrName, Qt::CaseInsensitive)) {
            return inst;
        }
    }
    return nullptr;
}

QString AiAssistant::loaderUid(const QString& loader)
{
    if (loader == "fabric")
        return "net.fabricmc.fabric-loader";
    if (loader == "quilt")
        return "org.quiltmc.quilt-loader";
    if (loader == "forge")
        return "net.minecraftforge";
    if (loader == "neoforge")
        return "net.neoforged";
    return {};
}

QString AiAssistant::loaderDisplayName(const QString& uid)
{
    if (uid == "net.fabricmc.fabric-loader")
        return "Fabric";
    if (uid == "org.quiltmc.quilt-loader")
        return "Quilt";
    if (uid == "net.minecraftforge")
        return "Forge";
    if (uid == "net.neoforged")
        return "NeoForge";
    return {};
}

QString AiAssistant::instanceMcVersion(MinecraftInstance* inst) const
{
    return inst->getPackProfile()->getComponentVersion("net.minecraft");
}

QString AiAssistant::instanceLoaderUid(MinecraftInstance* inst) const
{
    for (const QString& uid : { "net.fabricmc.fabric-loader", "org.quiltmc.quilt-loader", "net.minecraftforge", "net.neoforged" }) {
        if (!inst->getPackProfile()->getComponentVersion(uid).isEmpty()) {
            return uid;
        }
    }
    return {};
}

QString AiAssistant::currentLauncherState() const
{
    QStringList lines;
    auto* instances = APPLICATION->instances();
    if (instances->count() == 0) {
        lines << "Instances: none";
    } else {
        lines << "Instances:";
        for (int i = 0; i < instances->count(); ++i) {
            auto* inst = instances->at(i);
            const auto loader = loaderDisplayName(instanceLoaderUid(inst));
            lines << QStringLiteral("- id=%1 | name=%2 | MC %3%4")
                         .arg(inst->id(), inst->name(), instanceMcVersion(inst),
                              loader.isEmpty() ? QString() : QStringLiteral(" | %1").arg(loader));
        }
    }
    auto* accounts = APPLICATION->accounts();
    if (auto account = accounts->defaultAccount()) {
        lines << QStringLiteral("Active account: %1").arg(account->profileName());
    } else {
        lines << "Active account: none (launch will require login)";
    }
    return lines.join(u'\n');
}

// ---------------------------------------------------------------------------
// conversation (streaming)
// ---------------------------------------------------------------------------

void AiAssistant::sendUserMessage(const QString& message)
{
    if (m_busy) {
        return;
    }
    m_busy = true;
    emit busyChanged(true);

    aiSettings();
    const QString baseUrl = APPLICATION->settings()->get("AiApiBaseUrl").toString().trimmed();
    const QString apiKey = APPLICATION->settings()->get("AiApiKey").toString().trimmed();
    const QString model = APPLICATION->settings()->get("AiModel").toString().trimmed();

    do {
        if (apiKey.isEmpty() && !baseUrl.isEmpty()) {
            emit errorOccurred(tr("No API key configured. Enter a key in the AI settings (OpenRouter needs a key even for free "
                                   "models), or point the base URL at a local server that does not require one."));
            break;
        }

        QString resolveError;
        const QString actualModel = resolveModel(baseUrl, apiKey, model, &resolveError);
        if (actualModel.isEmpty()) {
            emit errorOccurred(tr("Failed to resolve the model: %1").arg(resolveError));
            break;
        }

        m_history.append(QJsonObject{ { "role", "user" }, { "content", message } });

        bool finished = false;
        for (int iteration = 0; iteration < MAX_TOOL_ITERATIONS && !finished; ++iteration) {
            // build messages: base system prompt + live launcher-state snapshot + history
            QJsonArray messages;
            messages.append(QJsonObject{ { "role", "system" }, { "content", systemPrompt() } });
            messages.append(QJsonObject{ { "role", "system" }, { "content", currentLauncherState() } });
            for (const auto& msg : m_history) {
                messages.append(msg);
            }

            QJsonObject body{ { "model", actualModel },
                              { "messages", messages },
                              { "tools", toolSpecs() },
                              { "temperature", 0.2 },
                              { "stream", true } };
            QStringList headers;
            if (!apiKey.isEmpty()) {
                headers << QStringLiteral("Authorization: Bearer %1").arg(apiKey);
            }
            if (isOpenRouter(baseUrl)) {
                headers << QStringLiteral("HTTP-Referer: https://github.com/PrismLauncher/PrismLauncher");
                headers << QStringLiteral("X-Title: Prism Launcher");
            }

            QNetworkRequest request((QUrl(QStringLiteral("%1/chat/completions").arg(baseUrl))));
            request.setTransferTimeout(300000);
            request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            request.setRawHeader("Accept", "application/json");
            request.setRawHeader("User-Agent", QString("%1/%2 (AI assistant)")
                                                    .arg(BuildConfig.LAUNCHER_DISPLAYNAME, BuildConfig.printableVersionString())
                                                    .toUtf8());
            for (const auto& header : headers) {
                const auto pair = header.split(u':');
                if (pair.size() == 2) {
                    request.setRawHeader(pair[0].trimmed().toUtf8(), pair[1].trimmed().toUtf8());
                }
            }

            m_aborted = false;
            m_currentReply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

            // ---- streaming state ----
            QString content;
            bool gotToolCalls = false;
            bool streamBegan = false;
            bool streamDone = false;
            QString failReason;
            QByteArray sseBuffer;
            QMap<int, QJsonObject> toolAcc;

            QEventLoop loop;
            QObject::connect(m_currentReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

            auto processSseLine = [&](QByteArray line) {
                if (line.endsWith('\r')) {
                    line.chop(1);
                }
                if (line.isEmpty() || line.startsWith(':')) {
                    return;  // keep-alive comment from OpenRouter
                }
                if (!line.startsWith("data:")) {
                    return;
                }
                const QByteArray payload = line.mid(5).trimmed();
                if (payload.isEmpty()) {
                    return;
                }
                if (payload == "[DONE]") {
                    streamDone = true;
                    loop.quit();
                    return;
                }
                auto doc = QJsonDocument::fromJson(payload);
                if (!doc.isObject()) {
                    return;
                }
                const auto choices = doc["choices"].toArray();
                if (choices.isEmpty()) {
                    return;
                }
                const auto choice = choices.at(0).toObject();
                const auto delta = choice["delta"].toObject();
                if (delta.contains("content")) {
                    const QString deltaText = delta["content"].toString();
                    if (!deltaText.isEmpty()) {
                        if (!streamBegan) {
                            streamBegan = true;
                            emit streamStarted();
                        }
                        content += deltaText;
                        emit streamDelta(deltaText);
                    }
                }
                if (delta.contains("tool_calls")) {
                    gotToolCalls = true;
                    for (const auto& tc : delta["tool_calls"].toArray()) {
                        auto call = tc.toObject();
                        const int index = call["index"].toInt();
                        QJsonObject entry = toolAcc.value(index);
                        if (call.contains("id") && !call["id"].toString().isEmpty()) {
                            entry["id"] = call["id"].toString();
                        }
                        if (call.contains("type")) {
                            entry["type"] = call["type"];
                        }
                        const auto fn = call["function"].toObject();
                        if (!fn.isEmpty()) {
                            auto fnAcc = entry["function"].toObject();
                            if (fn.contains("name") && !fn["name"].toString().isEmpty()) {
                                fnAcc["name"] = fn["name"];
                            }
                            if (fn.contains("arguments")) {
                                fnAcc["arguments"] = fnAcc["arguments"].toString() + fn["arguments"].toString();
                            }
                            entry["function"] = fnAcc;
                        }
                        toolAcc.insert(index, entry);
                    }
                }
                const auto finishReason = choice["finish_reason"].toString();
                if (finishReason == "tool_calls") {
                    gotToolCalls = true;
                }
            };

            QObject::connect(m_currentReply, &QNetworkReply::readyRead, &loop, [&]() {
                sseBuffer += m_currentReply->readAll();
                qsizetype nl = 0;
                while ((nl = sseBuffer.indexOf('\n')) >= 0) {
                    const QByteArray line = sseBuffer.left(nl);
                    sseBuffer.remove(0, nl + 1);
                    processSseLine(line);
                }
            });

            loop.exec();

            const bool netFailed = (m_currentReply->error() != QNetworkReply::NoError) && !m_aborted;
            const int statusCode = m_currentReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray bodyBytes = m_currentReply->readAll();
            m_currentReply = nullptr;

            if (m_aborted) {
                emit errorOccurred(tr("Generation stopped"));
                break;
            }
            if (netFailed) {
                emit errorOccurred(tr("AI request failed: %1").arg(failReason.isEmpty() ? QString() : failReason));
                break;
            }
            if (statusCode >= 400 && content.isEmpty()) {
                auto bodyDoc = QJsonDocument::fromJson(bodyBytes);
                QString detail;
                if (bodyDoc.isObject()) {
                    const auto err = bodyDoc["error"];
                    if (err.isObject()) {
                        detail = err.toObject()["message"].toString();
                    } else if (err.isString()) {
                        detail = err.toString();
                    }
                }
                if (detail.isEmpty()) {
                    detail = QStringLiteral("HTTP %1").arg(statusCode);
                }
                emit errorOccurred(tr("AI request failed: %1").arg(detail));
                break;
            }
            if (!streamDone && content.isEmpty() && !gotToolCalls) {
                emit errorOccurred(tr("The connection was closed before the response finished"));
                break;
            }

            // build the assistant message
            QJsonObject msg{ { "role", "assistant" } };
            if (!content.isEmpty()) {
                msg["content"] = content;
            }
            QJsonArray calls;
            if (gotToolCalls) {
                for (auto it = toolAcc.constBegin(); it != toolAcc.constEnd(); ++it) {
                    auto entry = it.value();
                    if (!entry.contains("id")) {
                        entry["id"] = QStringLiteral("call_%1").arg(it.key());
                    }
                    entry["type"] = "function";
                    calls.append(entry);
                }
                msg["tool_calls"] = calls;
            }
            m_history.append(msg);
            emit streamFinished(gotToolCalls, content);

            if (gotToolCalls) {
                for (const auto& callValue : calls) {
                    const auto call = callValue.toObject();
                    const QString callId = call["id"].toString();
                    const QString toolName = call["function"]["name"].toString();
                    const QString argsJson = call["function"]["arguments"].toString();
                    QJsonObject args;
                    auto argsDoc = QJsonDocument::fromJson(argsJson.toUtf8());
                    if (argsDoc.isObject()) {
                        args = argsDoc.object();
                    }
                    emit toolStarted(toolName, toolSummary(toolName, args));
                    auto result = executeTool(toolName, args);
                    emit toolFinished(toolName, result.text.left(300), result.ok);
                    m_history.append(QJsonObject{ { "role", "tool" },
                                                  { "tool_call_id", callId },
                                                  { "content", result.text } });
                }
                continue;  // next iteration streams the follow-up
            }
            if (!content.isEmpty()) {
                finished = true;
                break;
            }
            emit errorOccurred(tr("The model returned an empty response"));
            break;
        }
    } while (false);

    saveHistory();
    m_busy = false;
    emit busyChanged(false);
}

void AiAssistant::abortGeneration()
{
    m_aborted = true;
    if (m_currentReply) {
        m_currentReply->abort();
    }
}

void AiAssistant::resetConversation()
{
    m_history.clear();
    saveHistory();
}

void AiAssistant::loadHistory()
{
    const QString path = FS::PathCombine(APPLICATION->dataRoot(), "ai-chat.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    auto doc = QJsonDocument::fromJson(file.readAll());
    m_history.clear();
    for (const auto& v : doc.array()) {
        if (v.isObject()) {
            m_history.append(v.toObject());
        }
    }
}

void AiAssistant::saveHistory()
{
    QJsonArray arr;
    for (const auto& m : m_history) {
        arr.append(m);
    }
    FS::ensureFolderPathExists(APPLICATION->dataRoot());
    QFile file(FS::PathCombine(APPLICATION->dataRoot(), "ai-chat.json"));
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    file.close();
}

bool AiAssistant::fetchModelIds(const QString& baseUrl, const QString& apiKey, QStringList* free,
                                QStringList* paid, QString* error)
{
    QNetworkRequest request((QUrl(QStringLiteral("%1/models").arg(baseUrl))));
    request.setTransferTimeout(60000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent",
                         QString("%1/%2 (AI assistant)")
                             .arg(BuildConfig.LAUNCHER_DISPLAYNAME, BuildConfig.printableVersionString())
                             .toUtf8());
    if (!apiKey.isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());
    }

    QEventLoop loop;
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    auto* reply = m_nam->get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        if (error) {
            *error = reply->errorString();
        }
        reply->deleteLater();
        return false;
    }
    auto doc = QJsonDocument::fromJson(reply->readAll());
    reply->deleteLater();

    const auto models = doc["data"].toArray();
    for (const auto& m : models) {
        auto obj = m.toObject();
        const QString id = obj["id"].toString();
        const auto pricing = obj["pricing"].toObject();
        const bool isFree = pricing["prompt"].toString() == "0" && pricing["completion"].toString() == "0";
        if (id.isEmpty()) {
            continue;
        }
        if (isFree && free) {
            free->append(id);
        }
        if (!isFree && paid) {
            paid->append(id);
        }
    }
    return true;
}

QString AiAssistant::resolveModel(const QString& baseUrl, const QString& apiKey, const QString& model, QString* error)
{
    if (!(isOpenRouter(baseUrl) && (model.isEmpty() || model == autoFreeModelName()))) {
        return model;
    }
    // "openrouter/free": pick the best free model
    QStringList free;
    if (!fetchModelIds(baseUrl, apiKey, &free, nullptr, error)) {
        return {};
    }
    // drop obviously non-chat models
    QStringList chat;
    for (const auto& id : free) {
        static const QStringList banned = { "clip", "lyria", "image", "vision-only", "tts", "whisper" };
        bool bad = false;
        for (const auto& b : banned) {
            if (id.contains(b, Qt::CaseInsensitive)) {
                bad = true;
                break;
            }
        }
        if (!bad) {
            chat.append(id);
        }
    }
    if (chat.isEmpty()) {
        chat = free;
    }
    if (chat.isEmpty()) {
        if (error) {
            *error = tr("No free models are currently available on OpenRouter");
        }
        return {};
    }
    // prefer known-good free chat models, then the first remaining one
    static const QStringList preferred = { "deepseek/deepseek-chat-v3-0324:free",
                                           "meta-llama/llama-3.3-70b-instruct:free",
                                           "qwen/qwen-2.5-72b-instruct:free",
                                           "google/gemini-2.0-flash-exp:free" };
    for (const auto& p : preferred) {
        if (chat.contains(p)) {
            return p;
        }
    }
    return chat.first();
}

// ---------------------------------------------------------------------------
// tools
// ---------------------------------------------------------------------------

AiToolResult AiAssistant::toolListInstances()
{
    auto* instances = APPLICATION->instances();
    if (instances->count() == 0) {
        return { true, tr("No instances exist yet.") };
    }
    QStringList lines;
    for (int i = 0; i < instances->count(); ++i) {
        auto* inst = instances->at(i);
        const auto loader = loaderDisplayName(instanceLoaderUid(inst));
        lines << QStringLiteral("- id=%1 | name=%2 | MC %3%4")
                     .arg(inst->id(), inst->name(), instanceMcVersion(inst),
                          loader.isEmpty() ? QString() : QStringLiteral(" | %1").arg(loader));
    }
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolInstanceInfo(const QString& idOrName)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    auto* settings = inst->settings();
    QStringList lines;
    lines << QStringLiteral("id: %1").arg(inst->id());
    lines << QStringLiteral("name: %1").arg(inst->name());
    lines << QStringLiteral("minecraft: %1").arg(instanceMcVersion(inst));
    lines << QStringLiteral("loader: %1").arg(instanceLoaderUid(inst).isEmpty() ? "none" : instanceLoaderUid(inst));
    lines << QStringLiteral("game_root: %1").arg(inst->gameRoot());
    lines << QStringLiteral("java: %1").arg(settings->get("JavaPath").toString());
    lines << QStringLiteral("memory: %1..%2 MB")
                 .arg(settings->get("MinMemAlloc").toInt())
                 .arg(settings->get("MaxMemAlloc").toInt());
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolCreateInstance(const QString& name,
                                             const QString& mcVersion,
                                             const QString& loader,
                                             const QString& loaderVersion,
                                             const QString& group)
{
    auto* meta = APPLICATION->metadataIndex();
    auto version = meta->getLoadedVersion("net.minecraft", mcVersion);
    if (!version) {
        return { false, tr("Minecraft version '%1' was not found").arg(mcVersion) };
    }

    VanillaCreationTask* task = nullptr;
    if (loader != "none" && !loader.isEmpty()) {
        const QString uid = loaderUid(loader);
        if (uid.isEmpty()) {
            return { false, tr("Unknown loader '%1'").arg(loader) };
        }
        BaseVersion::Ptr loaderVer;
        if (!loaderVersion.isEmpty()) {
            loaderVer = meta->getLoadedVersion(uid, loaderVersion);
        }
        if (!loaderVer && !loaderVersion.isEmpty()) {
            return { false, tr("Loader version '%1' for %2 was not found").arg(loaderVersion, loader) };
        }
        if (!loaderVer) {
            // fetch the loader version list and take the newest
            auto listPtr = meta->get(uid);
            auto loadTask = listPtr->getLoadTask();
            auto error = syncRunTask(loadTask.get());
            if (listPtr->count() == 0) {
                return { false, tr("Failed to load %1 versions: %2").arg(loader, error) };
            }
            loaderVer = listPtr->at(0);
        }
        task = new VanillaCreationTask(version, uid, loaderVer);
    } else {
        task = new VanillaCreationTask(version);
    }

    task->setName(name);
    task->setIcon("default");
    task->setGroup(group);

    auto wrapped = std::unique_ptr<Task>(APPLICATION->instances()->wrapInstanceTask(task));
    auto error = syncRunTask(wrapped.get());
    if (!error.isEmpty()) {
        return { false, tr("Instance creation failed: %1").arg(error) };
    }

    // find the created instance by the name the task set
    auto* instances = APPLICATION->instances();
    for (int i = 0; i < instances->count(); ++i) {
        auto* inst = instances->at(i);
        if (inst->name() == task->name()) {
            return { true,
                     tr("Created instance '%1' (id=%2) with Minecraft %3%4")
                         .arg(inst->name(), inst->id(), mcVersion,
                              loader == "none" || loader.isEmpty()
                                  ? QString()
                                  : QStringLiteral(" and %1 %2").arg(loader, loaderVersion)) };
        }
    }
    return { true, tr("Instance created") };
}

AiToolResult AiAssistant::toolDeleteInstance(const QString& idOrName)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    const QString id = inst->id();
    const QString name = inst->name();
    APPLICATION->instances()->deleteInstance(id);
    return { true, tr("Deleted instance '%1' (id=%2)").arg(name, id) };
}

AiToolResult AiAssistant::toolLaunchInstance(const QString& idOrName)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    const bool started = APPLICATION->launch(inst);
    return { started,
             started ? tr("Launch started for '%1'. The game process runs separately.")
                           .arg(inst->name())
                     : tr("Failed to start '%1' (another launch may already be running)").arg(inst->name()) };
}

AiToolResult AiAssistant::toolSearchContent(const QString& query, const QString& contentType, int limit, const QString& provider)
{
    if (limit <= 0 || limit > 25) {
        limit = 8;
    }

    if (provider == QStringLiteral("curseforge")) {
        const int classId = cfClassIdFor(contentType);
        if (classId <= 0) {
            return { false, tr("Content type '%1' is not supported on CurseForge").arg(contentType) };
        }
        QUrlQuery urlQuery;
        urlQuery.addQueryItem("gameId", "432");
        urlQuery.addQueryItem("classId", QString::number(classId));
        urlQuery.addQueryItem("searchFilter", query);
        urlQuery.addQueryItem("sortField", "2");
        urlQuery.addQueryItem("sortOrder", "desc");
        urlQuery.addQueryItem("pageSize", QString::number(limit));
        QString error;
        auto doc = cfGet(QStringLiteral("/mods/search?%1").arg(urlQuery.toString(QUrl::FullyEncoded)), &error);
        if (!error.isEmpty()) {
            return { false, tr("CurseForge search failed: %1").arg(error) };
        }
        const auto hits = doc.object()["data"].toArray();
        if (hits.isEmpty()) {
            return { true, tr("Nothing found for '%1'").arg(query) };
        }
        QStringList lines;
        for (const auto& hit : hits) {
            auto obj = hit.toObject();
            lines << QStringLiteral("- %1 (id=%2, %3 downloads): %4")
                         .arg(obj["name"].toString(), obj["id"].toVariant().toString(),
                              obj["downloadCount"].toVariant().toString(), obj["summary"].toString());
        }
        return { true, lines.join(u'
') };
    }

    const QString projectType = modrinthProjectType(contentType);
    if (projectType.isEmpty()) {
        return { false, tr("Unknown content type '%1'").arg(contentType) };
    }

    // one facet group: project_type
    QJsonArray facets;
    facets.append(QJsonArray{ QStringLiteral("project_type:%1").arg(projectType) });

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("limit", QString::number(limit));
    urlQuery.addQueryItem("query", query);
    urlQuery.addQueryItem("facets", QString::fromUtf8(QJsonDocument(facets).toJson(QJsonDocument::Compact)));

    QString error;
    auto doc = modrinthGet(QStringLiteral("/search?%1").arg(urlQuery.toString(QUrl::FullyEncoded)), &error);
    if (!error.isEmpty()) {
        return { false, tr("Modrinth search failed: %1").arg(error) };
    }
    // /search returns an object {"hits":[...], ...}, not a bare array
    const QJsonObject searchResult = doc.object();
    const auto hits = searchResult.contains("hits") ? searchResult["hits"].toArray() : doc.array();
    if (hits.isEmpty()) {
        return { true, tr("Nothing found for '%1'").arg(query) };
    }
    QStringList lines;
    for (const auto& hit : hits) {
        auto obj = hit.toObject();
        lines << QStringLiteral("- [%1] %2 (id=%3, slug=%4, %5 downloads): %6")
                     .arg(obj["project_type"].toString(), obj["title"].toString(), obj["project_id"].toString(),
                          obj["slug"].toString(), obj["downloads"].toVariant().toString(), obj["description"].toString());
    }
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolContentVersions(const QString& projectId, const QString& mcVersion, const QString& loader)
{
    QString path = QStringLiteral("/project/%1/version").arg(projectId);
    QStringList filters;
    if (!mcVersion.isEmpty()) {
        filters << QStringLiteral("game_versions==[\"%1\"]").arg(mcVersion);
    }
    if (!loader.isEmpty()) {
        filters << QStringLiteral("loaders==[\"%1\"]").arg(loader);
    }
    if (!filters.isEmpty()) {
        path += QStringLiteral("?facets=[%1]").arg(QStringList{ filters }.join(u','));
    }
    QString error;
    auto doc = modrinthGet(path, &error);
    if (!error.isEmpty()) {
        return { false, tr("Modrinth request failed: %1").arg(error) };
    }
    const auto versions = doc.array();
    if (versions.isEmpty()) {
        return { true, tr("No versions found for project '%1'%2")
                           .arg(projectId, mcVersion.isEmpty() ? QString() : QStringLiteral(" for MC %1").arg(mcVersion)) };
    }
    QStringList lines;
    int shown = 0;
    for (const auto& v : versions) {
        auto obj = v.toObject();
        auto file = primaryFileOfVersion(obj);
        lines << QStringLiteral("- id=%1 | %2 | MC %3 | %4")
                     .arg(obj["id"].toString(), obj["version_number"].toString(),
                          obj["game_versions"].toArray().isEmpty()
                              ? "?"
                              : obj["game_versions"].toArray().last().toString(),
                          file ? file->second : QString("?"));
        if (++shown >= 15) {
            break;
        }
    }
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolInstallContent(const QString& idOrName,
                                             const QString& projectId,
                                             const QString& contentType,
                                             const QString& versionId,
                                             const QString& provider)
{
    if (provider == QStringLiteral("curseforge")) {
        const int classId = cfClassIdFor(contentType);
        if (classId <= 0) {
            return { false, tr("Content type '%1' is not supported on CurseForge").arg(contentType) };
        }
        QString error;
        auto doc = cfGet(QStringLiteral("/mods/%1/files").arg(projectId), &error);
        if (!error.isEmpty()) {
            return { false, tr("CurseForge request failed: %1").arg(error) };
        }
        const auto files = doc.object()["data"].toArray();
        if (files.isEmpty()) {
            return { false, tr("No files found for CurseForge project '%1'").arg(projectId) };
        }

        if (contentType == "modpack") {
            // download the newest pack archive and import it as a new instance
            auto packFile = cfLatestFile(files, QString());
            if (!packFile) {
                return { false, tr("No downloadable pack archive found for '%1'").arg(projectId) };
            }
            QTemporaryDir tempDir;
            if (!tempDir.isValid()) {
                return { false, tr("Could not create a temporary folder") };
            }
            const QString zipPath = tempDir.filePath(packFile->second);
            auto dl = httpDownload(packFile->first, zipPath);
            if (!dl.ok) {
                return { false, tr("Download failed: %1").arg(dl.error) };
            }
            auto stagingPath = APPLICATION->instances()->getStagedInstancePath({});
            if (stagingPath.isEmpty()) {
                return { false, tr("Could not create a staging folder") };
            }
            auto task = std::make_unique<InstanceImportTask>(QUrl::fromLocalFile(zipPath), true /* trustedSource */);
            task->setIcon("default");
            auto importError = syncRunTask(task.get());
            if (!importError.isEmpty()) {
                return { false, tr("Import failed: %1").arg(importError) };
            }
            if (!APPLICATION->instances()->commitStagedInstance(stagingPath, *task, QString())) {
                return { false, tr("Failed to commit the imported instance") };
            }
            return { true, tr("Installed CurseForge modpack '%1' as a new instance").arg(projectId) };
        }

        auto* inst = findInstance(idOrName);
        if (!inst) {
            return { false, tr("Instance '%1' not found").arg(idOrName) };
        }
        const QString mcVersion = inst->getPackProfile()->getComponentVersion("net.minecraft");
        auto file = cfLatestFile(files, mcVersion);
        if (!file) {
            return { false,
                     tr("No compatible version of the project '%1' for Minecraft %2 was found")
                         .arg(projectId, mcVersion.isEmpty() ? tr("(unknown)") : mcVersion) };
        }
        const QString folder = contentFolderFor(contentType);
        const QString targetDir = FS::PathCombine(inst->gameRoot(), folder);
        const QString targetPath = FS::PathCombine(targetDir, file->second);
        FS::ensureFolderPathExists(targetDir);
        auto dl = httpDownload(file->first, targetPath);
        if (!dl.ok) {
            return { false, tr("Download failed: %1").arg(dl.error) };
        }
        return { true, tr("Installed %1 into %2 from CurseForge").arg(file->second, targetDir) };
    }

    // modpacks become their own instance via ModrinthCreationTask
    if (contentType == "modpack") {
        auto stagingPath = APPLICATION->instances()->getStagedInstancePath({});
        if (stagingPath.isEmpty()) {
            return { false, tr("Could not create a staging folder") };
        }
        auto task = std::make_unique<ModrinthCreationTask>(stagingPath, true /* trustedSource */, APPLICATION->settings(), nullptr,
                                                           projectId, versionId);
        task->setIcon("default");
        auto error = syncRunTask(task.get());
        // commit like InstanceStaging does
        if (error.isEmpty()) {
            if (!APPLICATION->instances()->commitStagedInstance(stagingPath, *task, QString())) {
                return { false, tr("Failed to commit the installed modpack") };
            }
            // find the new instance by recent modification
            auto* instances = APPLICATION->instances();
            int last = instances->count() - 1;
            if (last >= 0) {
                auto* inst = instances->at(last);
                return { true, tr("Modpack installed as instance '%1' (id=%2)").arg(inst->name(), inst->id()) };
            }
            return { true, tr("Modpack installed") };
        }
        // try to clean the staging folder
        FS::deletePath(stagingPath);
        return { false, tr("Modpack install failed: %1").arg(error) };
    }

    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }

    // resolve the version to install
    QString path = QStringLiteral("/project/%1/version").arg(projectId);
    const QString mcVersion = instanceMcVersion(inst);
    const QString loaderUidStr = instanceLoaderUid(inst);
    QStringList facets;
    if (contentType == "mod" || contentType == "plugin") {
        if (!mcVersion.isEmpty()) {
            facets << QStringLiteral("game_versions==[\"%1\"]").arg(mcVersion);
        }
        if (!loaderUidStr.isEmpty()) {
            QString shortLoader;
            if (loaderUidStr == "net.fabricmc.fabric-loader")
                shortLoader = "fabric";
            else if (loaderUidStr == "org.quiltmc.quilt-loader")
                shortLoader = "quilt";
            else if (loaderUidStr == "net.minecraftforge")
                shortLoader = "forge";
            else if (loaderUidStr == "net.neoforged")
                shortLoader = "neoforge";
            if (!shortLoader.isEmpty()) {
                facets << QStringLiteral("loaders==[\"%1\"]").arg(shortLoader);
            }
        }
    } else if (!mcVersion.isEmpty() && contentType != "shader") {
        // resource packs/datapacks also carry game_versions
        facets << QStringLiteral("game_versions==[\"%1\"]").arg(mcVersion);
    }
    if (!facets.isEmpty()) {
        path += QStringLiteral("?facets=[%1]").arg(QStringList{ facets }.join(u','));
    }

    QString error;
    auto doc = modrinthGet(path, &error);
    if (!error.isEmpty()) {
        return { false, tr("Modrinth request failed: %1").arg(error) };
    }
    const auto versions = doc.array();
    QJsonObject chosen;
    bool found = false;
    for (const auto& v : versions) {
        auto obj = v.toObject();
        if (versionId.isEmpty() || obj["id"].toString() == versionId) {
            chosen = obj;
            found = true;
            break;
        }
    }
    if (!found) {
        return { false,
                 tr("No compatible version of the project '%1' for Minecraft %2 was found")
                     .arg(projectId, mcVersion.isEmpty() ? tr("(unknown)") : mcVersion) };
    }

    auto file = primaryFileOfVersion(chosen);
    if (!file) {
        return { false, tr("The version has no downloadable files") };
    }

    const QString folder = contentFolderFor(contentType);
    const QString targetDir = FS::PathCombine(inst->gameRoot(), folder);
    const QString targetPath = FS::PathCombine(targetDir, file->second);
    FS::ensureFolderPathExists(targetDir);

    auto reply = httpDownload(file->first, targetPath);
    if (!reply.ok) {
        return { false, tr("Download failed: %1").arg(reply.error) };
    }

    return { true,
             tr("Installed %1 into %2 (version %3)").arg(file->second, targetDir, chosen["version_number"].toString()) };
}

AiToolResult AiAssistant::toolUninstallMod(const QString& idOrName, const QString& name)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    const QDir modsDir(FS::PathCombine(inst->gameRoot(), "mods"));
    if (!modsDir.exists()) {
        return { false, tr("Mods folder does not exist: %1").arg(modsDir.path()) };
    }
    const QString needle = name.trimmed().toLower();
    if (needle.isEmpty()) {
        return { false, tr("Mod name is empty") };
    }

    QSet<QString> toDelete;
    const auto files = modsDir.entryList(QDir::Files, QDir::Name);
    // pass 1: packwiz metadata (match by mod name, slug or file name; also pull the jar it points to)
    for (const auto& f : files) {
        if (!f.endsWith(QStringLiteral(".pw.toml"))) {
            continue;
        }
        auto mod = Packwiz::V1::getIndexForMod(modsDir, f.left(f.size() - 8));
        if (!mod.isValid()) {
            continue;
        }
        if (mod.name.toLower().contains(needle) || mod.slug.toLower().contains(needle) ||
            mod.filename.toLower().contains(needle)) {
            toDelete.insert(f);
            if (!mod.filename.isEmpty()) {
                toDelete.insert(mod.filename);
            }
        }
    }
    // pass 2: plain files (jar name match)
    for (const auto& f : files) {
        if (f.endsWith(QStringLiteral(".pw.toml"))) {
            continue;
        }
        if (f.toLower().contains(needle)) {
            toDelete.insert(f);
        }
    }
    if (toDelete.isEmpty()) {
        return { false, tr("No mod matching '%1' found in %2").arg(name, modsDir.path()) };
    }

    QStringList deleted, failed;
    for (const auto& fname : toDelete) {
        const QString path = modsDir.filePath(fname);
        if (!QFile::exists(path)) {
            continue;
        }
        if (QFile::remove(path)) {
            deleted << fname;
        } else {
            failed << fname;
        }
    }
    if (!failed.isEmpty()) {
        return { false, tr("Failed to delete: %1").arg(failed.join(QStringLiteral(", "))) };
    }
    return { true, tr("Deleted from %1: %2").arg(modsDir.path(), deleted.join(QStringLiteral(", "))) };
}

AiToolResult AiAssistant::toolListScreenshots(const QString& idOrName, int limit)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    if (limit <= 0 || limit > 50) {
        limit = 8;
    }
    const auto shots = collectScreenshots(inst);
    if (shots.isEmpty()) {
        return { true, tr("No screenshots found for '%1'").arg(inst->name()) };
    }
    QStringList lines;
    int shown = 0;
    for (const auto& shot : shots) {
        lines << QStringLiteral("- %1 | %2 | %3 KB")
                     .arg(QFileInfo(shot.path).fileName(),
                          shot.mtime.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                          QString::number(shot.size / 1024))
              << QStringLiteral("  path: %1").arg(shot.path);
        if (++shown >= limit) {
            break;
        }
    }
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolUploadScreenshot(const QString& idOrName, const QString& name)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    const auto shots = collectScreenshots(inst);
    if (shots.isEmpty()) {
        return { false, tr("No screenshots found for '%1'").arg(inst->name()) };
    }
    QString path;
    const QString needle = name.trimmed();
    if (needle.isEmpty()) {
        path = shots.first().path;
    } else {
        for (const auto& shot : shots) {
            if (QFileInfo(shot.path).fileName().contains(needle, Qt::CaseInsensitive)) {
                path = shot.path;
                break;
            }
        }
        if (path.isEmpty()) {
            bool ok = false;
            const int idx = needle.toInt(&ok);
            if (ok && idx >= 1 && idx <= shots.size()) {
                path = shots.at(idx - 1).path;
            }
        }
        if (path.isEmpty()) {
            return { false, tr("No screenshot matching '%1' found").arg(name) };
        }
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return { false, tr("Cannot open %1").arg(path) };
    }
    auto* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart imagePart;
    imagePart.setHeader(QNetworkRequest::ContentTypeHeader, "image/png");
    imagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QStringLiteral("form-data; name=\"image\"; filename=\"%1\"").arg(QFileInfo(path).fileName()));
    imagePart.setBodyDevice(&file);
    multiPart->append(imagePart);
    QHttpPart typePart;
    typePart.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"type\""));
    typePart.setBody("image");
    multiPart->append(typePart);
    QHttpPart titlePart;
    titlePart.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"title\""));
    titlePart.setBody(QFileInfo(path).fileName().toUtf8());
    multiPart->append(titlePart);

    QNetworkRequest request((QUrl(QStringLiteral("%1image").arg(BuildConfig.IMGUR_BASE_URL))));
    request.setTransferTimeout(120000);
    request.setRawHeader("Authorization", QStringLiteral("Client-ID %1").arg(BuildConfig.IMGUR_CLIENT_ID).toUtf8());
    auto* reply = m_nam->post(request, multiPart);
    multiPart->setParent(reply);  // free the multiPart together with the reply
    QEventLoop loop;
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray bodyBytes = reply->readAll();
    const bool ok = reply->error() == QNetworkReply::NoError;
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString errorString = reply->errorString();
    reply->deleteLater();
    if (!ok) {
        return { false,
                 tr("Imgur upload failed: %1").arg(statusCode > 0 ? QStringLiteral("HTTP %1").arg(statusCode) : errorString) };
    }
    auto doc = QJsonDocument::fromJson(bodyBytes);
    const QString link = doc.object()["data"].toObject()["link"].toString();
    if (link.isEmpty()) {
        return { false, tr("Imgur did not return a link") };
    }
    return { true, tr("Uploaded %1 -> %2").arg(QFileInfo(path).fileName(), link) };
}

AiToolResult AiAssistant::toolSetInstanceSettings(const QString& idOrName, const QJsonObject& changes)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    QStringList applied;
    auto* settings = inst->settings();
    const SettingsObject::Lock lock(settings);

    if (changes.contains("name")) {
        const QString newName = changes["name"].toString();
        inst->setName(newName);
        applied << QStringLiteral("name=%1").arg(newName);
    }
    if (changes.contains("max_memory_mb")) {
        const int mb = changes["max_memory_mb"].toInt();
        settings->set("MaxMemAlloc", mb);
        applied << QStringLiteral("max_memory=%1MB").arg(mb);
    }
    if (changes.contains("min_memory_mb")) {
        const int mb = changes["min_memory_mb"].toInt();
        settings->set("MinMemAlloc", mb);
        applied << QStringLiteral("min_memory=%1MB").arg(mb);
    }
    if (changes.contains("java_path")) {
        const QString path = changes["java_path"].toString();
        settings->set("OverrideJavaLocation", true);
        settings->set("JavaPath", path);
        applied << QStringLiteral("java=%1").arg(path);
    }
    if (applied.isEmpty()) {
        return { false, tr("No known settings were passed") };
    }
    return { true, tr("Updated instance '%1': %2").arg(inst->name(), applied.join(u", ")) };
}

AiToolResult AiAssistant::toolListMinecraftVersions()
{
    auto* meta = APPLICATION->metadataIndex();
    auto listPtr = meta->get("net.minecraft");
    auto loadTask = listPtr->getLoadTask();
    auto error = syncRunTask(loadTask.get());
    if (listPtr->count() == 0) {
        return { false, tr("Failed to load the Minecraft version list: %1").arg(error) };
    }
    QStringList lines;
    const int total = listPtr->count();
    const int shown = qMin(total, 40);
    for (int i = 0; i < shown; ++i) {
        lines << QStringLiteral("- %1").arg(listPtr->at(i)->descriptor());
    }
    if (total > shown) {
        lines << tr("... and %1 more (showing the newest %2)").arg(total - shown).arg(shown);
    }
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolListLoaderVersions(const QString& loader)
{
    const QString uid = loaderUid(loader);
    if (uid.isEmpty()) {
        return { false, tr("Unknown loader '%1'").arg(loader) };
    }
    auto* meta = APPLICATION->metadataIndex();
    auto listPtr = meta->get(uid);
    auto loadTask = listPtr->getLoadTask();
    auto error = syncRunTask(loadTask.get());
    if (listPtr->count() == 0) {
        return { false, tr("Failed to load %1 versions: %2").arg(loader, error) };
    }
    QStringList lines;
    const int total = listPtr->count();
    const int shown = qMin(total, 25);
    for (int i = 0; i < shown; ++i) {
        lines << QStringLiteral("- %1").arg(listPtr->at(i)->descriptor());
    }
    if (total > shown) {
        lines << tr("... and %1 more").arg(total - shown);
    }
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolGetCrashReports(const QString& idOrName)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    QDir crashDir(FS::PathCombine(inst->gameRoot(), "crash-reports"));
    const auto infos = crashDir.entryInfoList(QStringList{ "*.txt" }, QDir::Files, QDir::Time | QDir::Reversed);
    if (infos.isEmpty()) {
        return { true, tr("No crash reports found in %1").arg(crashDir.path()) };
    }
    QStringList lines;
    lines << tr("Crash reports (newest first):");
    for (const auto& info : infos) {
        lines << QStringLiteral("- %1 (%2 KB)").arg(info.fileName()).arg(info.size() / 1024);
    }
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolReadCrashReport(const QString& idOrName, const QString& filename)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    QDir crashDir(FS::PathCombine(inst->gameRoot(), "crash-reports"));
    QString target;
    if (filename.isEmpty()) {
        const auto infos = crashDir.entryInfoList(QStringList{ "*.txt" }, QDir::Files, QDir::Time | QDir::Reversed);
        if (infos.isEmpty()) {
            return { false, tr("No crash reports found") };
        }
        target = infos.first().absoluteFilePath();
    } else {
        target = crashDir.filePath(filename);
    }
    QFile file(target);
    if (!file.open(QIODevice::ReadOnly)) {
        return { false, tr("Cannot open %1").arg(target) };
    }
    // crash reports are large; cap at ~40 KB like the game log viewer
    const QByteArray data = file.read(40 * 1024);
    const QString text = QString::fromUtf8(data);
    return { true, tr("Crash report %1:\n%2").arg(QFileInfo(target).fileName(), text) };
}

AiToolResult AiAssistant::toolGetGameLog(const QString& idOrName, int tail)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    QFile logFile(FS::PathCombine(inst->gameRoot(), "logs", "latest.log"));
    if (!logFile.open(QIODevice::ReadOnly)) {
        return { false, tr("No game log found at %1").arg(FS::PathCombine(inst->gameRoot(), "logs", "latest.log")) };
    }
    const QStringList lines = QString::fromUtf8(logFile.readAll()).split(u'\n');
    const int count = tail > 0 ? qMin(tail, int(lines.size())) : 200;
    QStringList out;
    for (int i = lines.size() - count; i < lines.size(); ++i) {
        out << lines.at(i);
    }
    return { true, out.join(u'\n') };
}

AiToolResult AiAssistant::toolListCatpacks()
{
    auto* themes = APPLICATION->themeManager();
    const auto packs = themes->getValidCatPacks();
    const QString current = APPLICATION->settings()->get("BackgroundCat").toString();
    QStringList lines;
    lines << tr("Current catpack: %1").arg(current);
    for (auto* pack : packs) {
        lines << QStringLiteral("- id=%1 | name=%2").arg(pack->id(), pack->name());
    }
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolSetCatpack(const QString& name)
{
    auto* themes = APPLICATION->themeManager();
    // validate via getCatPack (falls back to the first pack on invalid names)
    themes->getCatPack(name);
    APPLICATION->settings()->set("BackgroundCat", name);
    return { true, tr("Catpack set to '%1' (applies after theme refresh)").arg(name) };
}

AiToolResult AiAssistant::toolListWorlds(const QString& idOrName)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    WorldList worldList(QDir(inst->gameRoot()).filePath("saves"), inst);
    worldList.update();
    const auto worlds = worldList.allWorlds();
    if (worlds.isEmpty()) {
        return { true, tr("No worlds found") };
    }
    QStringList lines;
    for (const auto& world : worlds) {
        const QDateTime lastPlayed = world.lastPlayed();
        lines << QStringLiteral("- %1 | played: %2 | %3 | seed %4")
                     .arg(world.name(),
                          lastPlayed.isValid() ? lastPlayed.toString("yyyy-MM-dd HH:mm") : tr("never"),
                          world.gameType().toLogString(),
                          QString::number(world.seed()));
    }
    return { true, lines.join(u'\n') };
}

AiToolResult AiAssistant::toolDeleteWorld(const QString& idOrName, const QString& world)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    WorldList worldList(QDir(inst->gameRoot()).filePath("saves"), inst);
    worldList.update();
    for (World w : worldList.allWorlds()) {
        if (w.name() == world || w.folderName() == world) {
            const QString folder = w.container().absoluteFilePath();
            if (w.destroy()) {
                return { true, tr("Deleted world '%1' (%2)").arg(world, folder) };
            }
            return { false, tr("Failed to delete world '%1'").arg(world) };
        }
    }
    return { false, tr("World '%1' not found in %2").arg(world, idOrName) };
}

AiToolResult AiAssistant::toolBackupWorld(const QString& idOrName, const QString& world)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    WorldList worldList(QDir(inst->gameRoot()).filePath("saves"), inst);
    worldList.update();
    for (auto& w : worldList.allWorlds()) {
        if (w.name() == world || w.folderName() == world) {
            const QString src = w.container().absoluteFilePath();
            const QString backupRoot = FS::PathCombine(inst->gameRoot(), "backups");
            FS::ensureFolderPathExists(backupRoot);
            const QString backupPath =
                FS::PathCombine(backupRoot, QStringLiteral("%1-%2").arg(w.folderName(), QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
            FS::copy folderCopy(src, backupPath);
            folderCopy.followSymlinks(false).copyDirectories(true);
            if (folderCopy()) {
                return { true, tr("Backup created: %1").arg(backupPath) };
            }
            return { false, tr("Backup failed for '%1'").arg(world) };
        }
    }
    return { false, tr("World '%1' not found").arg(world) };
}

AiToolResult AiAssistant::toolUpdateAllMods(const QString& idOrName)
{
    auto* inst = findInstance(idOrName);
    if (!inst) {
        return { false, tr("Instance '%1' not found").arg(idOrName) };
    }
    const auto summary = ModUpdater::updateInstanceModsSync(inst);
    return { true, ModUpdater::summarize(summary) };
}

AiToolResult AiAssistant::executeTool(const QString& name, const QJsonObject& args)
{
    if (name == "list_instances")
        return toolListInstances();
    if (name == "get_instance_info")
        return toolInstanceInfo(args["instance"].toString());
    if (name == "create_instance")
        return toolCreateInstance(args["name"].toString(), args["minecraft_version"].toString(),
                                  args.contains("loader") ? args["loader"].toString() : "none",
                                  args["loader_version"].toString(), args["group"].toString());
    if (name == "delete_instance")
        return toolDeleteInstance(args["instance"].toString());
    if (name == "launch_instance")
        return toolLaunchInstance(args["instance"].toString());
    if (name == "search_content")
        return toolSearchContent(args["query"].toString(), args["content_type"].toString(), args["limit"].toInt(),
                                 args["provider"].toString());
    if (name == "get_content_versions")
        return toolContentVersions(args["project_id"].toString(), args["minecraft_version"].toString(), args["loader"].toString());
    if (name == "install_content")
        return toolInstallContent(args["instance"].toString(), args["project_id"].toString(), args["content_type"].toString(),
                                  args["version_id"].toString(), args["provider"].toString());
    if (name == "uninstall_mod")
        return toolUninstallMod(args["instance"].toString(), args["name"].toString());
    if (name == "list_screenshots")
        return toolListScreenshots(args["instance"].toString(), args["limit"].toInt());
    if (name == "upload_screenshot")
        return toolUploadScreenshot(args["instance"].toString(), args["name"].toString());
    if (name == "set_instance_settings")
        return toolSetInstanceSettings(args["instance"].toString(), args);
    if (name == "list_minecraft_versions")
        return toolListMinecraftVersions();
    if (name == "list_loader_versions")
        return toolListLoaderVersions(args["loader"].toString());
    if (name == "get_crash_reports")
        return toolGetCrashReports(args["instance"].toString());
    if (name == "read_crash_report")
        return toolReadCrashReport(args["instance"].toString(), args["filename"].toString());
    if (name == "get_game_log")
        return toolGetGameLog(args["instance"].toString(), args["tail"].toInt());
    if (name == "list_catpacks")
        return toolListCatpacks();
    if (name == "set_catpack")
        return toolSetCatpack(args["name"].toString());
    if (name == "list_worlds")
        return toolListWorlds(args["instance"].toString());
    if (name == "delete_world")
        return toolDeleteWorld(args["instance"].toString(), args["world"].toString());
    if (name == "backup_world")
        return toolBackupWorld(args["instance"].toString(), args["world"].toString());
    if (name == "update_all_mods")
        return toolUpdateAllMods(args["instance"].toString());
    return { false, tr("Unknown tool '%1'").arg(name) };
}