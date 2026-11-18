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

#include "Application.h"
#include <BuildConfig.h>
#include "BaseVersion.h"
#include "FileSystem.h"
#include "settings/SettingsObject.h"
#include "tasks/Task.h"
#include "InstanceList.h"
#include "BaseInstance.h"
#include "meta/Index.h"
#include "meta/VersionList.h"
#include "meta/Version.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/VanillaInstanceCreationTask.h"
#include "modplatform/modrinth/ModrinthInstanceCreationTask.h"

#include <QEventLoop>
#include <QFile>
#include <QUrl>

#include <optional>
#include <utility>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

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

/// Extracts the primary download URL (and filename) from a Modrinth version
/// JSON object.
std::optional<std::pair<QString, QString>> primaryFileOfVersion(const QJsonObject& version)
{
    const auto files = version["files"].toArray();
    QJsonObject chosen;
    bool found = false;
    for (const auto& f : files) {
        chosen = f.toObject();
        if (chosen["primary"].toBool()) {
            found = true;
            break;
        }
    }
    if (!found && !files.isEmpty()) {
        chosen = files.first().toObject();
    }
    if (!found) {
        return std::nullopt;
    }
    return std::make_pair(chosen["url"].toString(), chosen["filename"].toString());
}

}  // namespace

AiAssistant::AiAssistant(QObject* parent) : QObject(parent), m_nam(new QNetworkAccessManager(this))
{
    aiSettings();  // ensure the settings keys exist
}

void AiAssistant::resetConversation()
{
    m_history.clear();
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
    // Small helper so the schemas stay readable.
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
           "Search Modrinth for content.",
           { { "query", str("Search words") },
             { "content_type", enumType("What to search", { "mod", "plugin", "datapack", "shader", "resourcepack", "modpack" }) },
             { "limit", intType("How many results to return (default 8)") } },
           { "query", "content_type" }),
        fn("get_content_versions",
           "List downloadable versions of a Modrinth project.",
           { { "project_id", str("Modrinth project id or slug") },
             { "minecraft_version", str("Optional Minecraft version filter") },
             { "loader", str("Optional loader filter (fabric/quilt/forge/neoforge)") } },
           { "project_id" }),
        fn("install_content",
           "Install content into an instance. For modpacks a NEW instance is created from the pack.",
           { { "instance", str("Target instance id or name (ignored for modpacks)") },
             { "project_id", str("Modrinth project id or slug") },
             { "content_type", enumType("Content type", { "mod", "plugin", "datapack", "shader", "resourcepack", "modpack" }) },
             { "version_id", str("Optional specific Modrinth version id; latest compatible is used otherwise") } },
           { "instance", "project_id", "content_type" }),
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

AiAssistant::HttpReply AiAssistant::httpRequest(const QString& method, const QString& url, const QByteArray& body, int timeoutMs)
{
    QNetworkRequest request((QUrl(url)));
    request.setTransferTimeout(timeoutMs);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", QString("%1/%2 (AI assistant)")
                                            .arg(BuildConfig.LAUNCHER_DISPLAYNAME, BuildConfig.printableVersionString())
                                            .toUtf8());

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
    return httpRequest("GET", url, {}, timeoutMs);
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

QJsonDocument AiAssistant::modrinthGet(const QString& path)
{
    auto reply = httpGet(QStringLiteral("https://api.modrinth.com/v2%1").arg(path));
    if (!reply.ok) {
        return {};
    }
    return QJsonDocument::fromJson(reply.body);
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
// conversation
// ---------------------------------------------------------------------------

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

void AiAssistant::sendUserMessage(const QString& message)
{
    if (m_busy) {
        return;
    }
    m_busy = true;
    emit busyChanged(true);

    aiSettings();  // ensure the settings exist
    const QString baseUrl = APPLICATION->settings()->get("AiApiBaseUrl").toString().trimmed();
    const QString apiKey = APPLICATION->settings()->get("AiApiKey").toString().trimmed();
    const QString model = APPLICATION->settings()->get("AiModel").toString().trimmed();

    do {
        if (apiKey.isEmpty() && !baseUrl.isEmpty()) {
            emit errorOccurred(tr("No API key configured. Enter a key in the AI settings (OpenRouter needs a key even for free "
                                   "models), or point the base URL at a local server that does not require one."));
            break;
        }

        m_history.append(AiChatMessage::make("user", message));

        QString resolveError;
        const QString actualModel = resolveModel(baseUrl, apiKey, model, &resolveError);
        if (actualModel.isEmpty()) {
            emit errorOccurred(tr("Failed to resolve the model: %1").arg(resolveError));
            break;
        }

        bool finished = false;
        for (int iteration = 0; iteration < MAX_TOOL_ITERATIONS && !finished; ++iteration) {
            QJsonArray messages;
            messages.append(AiChatMessage::make("system", systemPrompt()).raw);
            for (const auto& msg : m_history) {
                messages.append(msg.raw);
            }

            QJsonObject body{ { "model", actualModel },
                              { "messages", messages },
                              { "tools", toolSpecs() },
                              { "temperature", 0.2 } };
            QStringList headers;
            if (!apiKey.isEmpty()) {
                headers << QStringLiteral("Authorization: Bearer %1").arg(apiKey);
            }
            if (isOpenRouter(baseUrl)) {
                headers << QStringLiteral("HTTP-Referer: https://github.com/PrismLauncher/PrismLauncher");
                headers << QStringLiteral("X-Title: Prism Launcher");
            }
            auto reply = httpRequest("POST",
                                     QStringLiteral("%1/chat/completions").arg(baseUrl),
                                     QJsonDocument(body).toJson(QJsonDocument::Compact),
                                     300000);
            if (!reply.ok) {
                QString detail = reply.error;
                auto doc = QJsonDocument::fromJson(reply.body);
                if (doc.isObject()) {
                    const auto err = doc["error"];
                    if (err.isObject()) {
                        detail = err.toObject()["message"].toString(detail);
                    }
                }
                emit errorOccurred(tr("AI request failed: %1").arg(detail));
                break;
            }

            auto doc = QJsonDocument::fromJson(reply.body);
            if (!doc.isObject()) {
                emit errorOccurred(tr("AI returned an unexpected response"));
                break;
            }
            const auto choice = doc["choices"].toArray().at(0).toObject();
            const auto msg = choice["message"].toObject();
            const auto toolCalls = msg["tool_calls"].toArray();

            if (toolCalls.isEmpty()) {
                const QString text = msg["content"].toString();
                m_history.append(AiChatMessage::make("assistant", text));
                emit assistantReply(text);
                finished = true;
                break;
            }

            // store the assistant message with the tool calls, then execute
            m_history.append(AiChatMessage{ msg });
            for (const auto& callValue : toolCalls) {
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

                m_history.append(AiChatMessage{ QJsonObject{ { "role", "tool" },
                                                              { "tool_call_id", callId },
                                                              { "content", result.text } } });
            }
        }
        if (!finished) {
            emit errorOccurred(tr("The assistant stopped after too many tool calls"));
        }
    } while (false);

    m_busy = false;
    emit busyChanged(false);
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
        QString loader;
        const auto uid = instanceLoaderUid(inst);
        if (uid == "net.fabricmc.fabric-loader")
            loader = "Fabric";
        else if (uid == "org.quiltmc.quilt-loader")
            loader = "Quilt";
        else if (uid == "net.minecraftforge")
            loader = "Forge";
        else if (uid == "net.neoforged")
            loader = "NeoForge";
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
            if (!error.isEmpty() && listPtr->count() == 0) {
                return { false, tr("Failed to load %1 versions: %2").arg(loader, error) };
            }
            if (listPtr->count() == 0) {
                return { false, tr("No %1 versions available").arg(loader) };
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

AiToolResult AiAssistant::toolSearchContent(const QString& query, const QString& contentType, int limit)
{
    const QString projectType = modrinthProjectType(contentType);
    if (projectType.isEmpty()) {
        return { false, tr("Unknown content type '%1'").arg(contentType) };
    }
    if (limit <= 0 || limit > 25) {
        limit = 8;
    }

    // one facet group: project_type
    QJsonArray facets;
    facets.append(QJsonArray{ QStringLiteral("project_type:%1").arg(projectType) });

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("limit", QString::number(limit));
    urlQuery.addQueryItem("query", query);
    urlQuery.addQueryItem("facets", QString::fromUtf8(QJsonDocument(facets).toJson(QJsonDocument::Compact)));

    auto doc = modrinthGet(QStringLiteral("/search?%1").arg(urlQuery.toString(QUrl::FullyEncoded)));
    const auto hits = doc.array();
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
    auto doc = modrinthGet(path);
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
                          QJsonArray(obj["game_versions"].toArray()).isEmpty()
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
                                             const QString& versionId)
{
    // modpacks become their own instance via ModrinthCreationTask
    if (contentType == "modpack") {
        auto* meta = APPLICATION->metadataIndex();
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
    if (!versionId.isEmpty()) {
        path = QStringLiteral("/project/%1/version").arg(projectId);
    }
    if (!facets.isEmpty()) {
        path += QStringLiteral("?facets=[%1]").arg(QStringList{ facets }.join(u','));
    }

    auto doc = modrinthGet(path);
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

    // validate the sha1 if provided by the version entry
    return { true,
             tr("Installed %1 into %2 (version %3)")
                 .arg(file->second, targetDir, chosen["version_number"].toString()) };
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
        return toolSearchContent(args["query"].toString(), args["content_type"].toString(), args["limit"].toInt());
    if (name == "get_content_versions")
        return toolContentVersions(args["project_id"].toString(), args["minecraft_version"].toString(), args["loader"].toString());
    if (name == "install_content")
        return toolInstallContent(args["instance"].toString(), args["project_id"].toString(), args["content_type"].toString(),
                                  args["version_id"].toString());
    if (name == "set_instance_settings")
        return toolSetInstanceSettings(args["instance"].toString(), args);
    if (name == "list_minecraft_versions")
        return toolListMinecraftVersions();
    if (name == "list_loader_versions")
        return toolListLoaderVersions(args["loader"].toString());
    return { false, tr("Unknown tool '%1'").arg(name) };
}
