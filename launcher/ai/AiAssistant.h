// SPDX-License-Identifier: GPL-3.0-only
/*
 *  AI Assistant for Prism Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 */

#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class MinecraftInstance;

/** One message of the LLM conversation (kept as the raw protocol object). */
struct AiChatMessage {
    QJsonObject raw;

    static AiChatMessage make(const QString& role, const QString& content)
    {
        AiChatMessage m;
        m.raw = QJsonObject{ { "role", role }, { "content", content } };
        return m;
    }
};

/** Result of one tool invocation. */
struct AiToolResult {
    bool ok = true;
    QString text;
};

/**
 * The AI assistant backend: talks to an OpenAI-compatible chat completions
 * endpoint with tool calling and streaming, and executes the tools against
 * the launcher (instances, Modrinth content, launch, settings).
 *
 * Settings (global SettingsObject keys, registered on first use):
 *   AiApiBaseUrl - default "https://openrouter.ai/api/v1"
 *   AiApiKey     - the API token
 *   AiModel      - default "openrouter/free" (auto: best free OpenRouter model;
 *                  also compatible with local OpenAI-compatible servers)
 */
class AiAssistant : public QObject {
    Q_OBJECT

   public:
    explicit AiAssistant(QObject* parent = nullptr);
    ~AiAssistant() override = default;

    /** The model name meaning "pick the best free OpenRouter model". */
    static QString autoFreeModelName() { return "openrouter/free"; }
    /** True when the base URL points at OpenRouter. */
    static bool isOpenRouter(const QString& baseUrl) { return baseUrl.contains("openrouter.ai"); }

    /** Sends a user message; the reply streams via the stream* signals. */
    void sendUserMessage(const QString& message);

    /** Aborts the in-flight request (streaming/tool loop stops). */
    void abortGeneration();

    /** Clears the conversation history (and the persisted chat file). */
    void resetConversation();

    /** Loads / saves the persisted conversation (ai-chat.json). */
    void loadHistory();
    void saveHistory();

    /** The tool definitions exposed to the model. */
    static QJsonArray toolSpecs();

    /** The base system prompt. */
    static QString systemPrompt();

    /** A snapshot of the launcher state injected as context each turn. */
    QString currentLauncherState() const;

    const QList<QJsonObject>& history() const { return m_history; }

   signals:
    void assistantReply(const QString& text);
    void toolStarted(const QString& toolName, const QString& argsSummary);
    void toolFinished(const QString& toolName, const QString& resultSummary, bool ok);
    void errorOccurred(const QString& error);
    void busyChanged(bool busy);

    void streamStarted();
    void streamDelta(const QString& delta);
    void streamFinished(bool hadToolCalls, const QString& fullText);

   private:
    struct HttpReply {
        bool ok = false;
        int statusCode = 0;
        QString error;
        QByteArray body;
    };

    HttpReply httpRequest(const QString& method, const QString& url, const QByteArray& body,
                          const QStringList& headers, int timeoutMs);
    HttpReply httpGet(const QString& url, int timeoutMs = 180000);
    HttpReply httpDownload(const QString& url, const QString& targetPath);
    QJsonDocument modrinthGet(const QString& path);
    QString syncRunTask(class Task* task);

    MinecraftInstance* findInstance(const QString& idOrName) const;
    static QString loaderUid(const QString& loader);
    static QString loaderDisplayName(const QString& uid);
    QString instanceMcVersion(MinecraftInstance* inst) const;
    QString instanceLoaderUid(MinecraftInstance* inst) const;

    // ---- tool implementations ----
    AiToolResult toolListInstances();
    AiToolResult toolInstanceInfo(const QString& idOrName);
    AiToolResult toolCreateInstance(const QString& name,
                                    const QString& mcVersion,
                                    const QString& loader,
                                    const QString& loaderVersion,
                                    const QString& group);
    AiToolResult toolDeleteInstance(const QString& idOrName);
    AiToolResult toolLaunchInstance(const QString& idOrName);
    AiToolResult toolSearchContent(const QString& query, const QString& contentType, int limit);
    AiToolResult toolContentVersions(const QString& projectId, const QString& mcVersion, const QString& loader);
    AiToolResult toolInstallContent(const QString& idOrName,
                                    const QString& projectId,
                                    const QString& contentType,
                                    const QString& versionId);
    AiToolResult toolSetInstanceSettings(const QString& idOrName, const QJsonObject& changes);
    AiToolResult toolListMinecraftVersions();
    AiToolResult toolListLoaderVersions(const QString& loader);

    AiToolResult executeTool(const QString& name, const QJsonObject& args);

    QString toolSummary(const QString& name, const QJsonObject& args) const;

   private:
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_currentReply = nullptr;
    QList<QJsonObject> m_history;
    bool m_busy = false;
    bool m_aborted = false;
};
