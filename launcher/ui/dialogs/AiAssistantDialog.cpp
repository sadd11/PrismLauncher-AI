// SPDX-License-Identifier: GPL-3.0-only
/*
 *  AI Assistant for Prism Launcher
 *  Copyright (C) 2026 Prism Launcher Contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 */

#include "AiAssistantDialog.h"

#include "ai/AiAssistant.h"
#include "Application.h"
#include "settings/SettingsObject.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/// Telegram MarkdownV2 escapes: backslash before these characters must be
/// removed before rendering.
QString unescapeMarkdownV2(const QString& text)
{
    static const QString specials = QStringLiteral("_*[]()~`>#+-=|{}.!");
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == u'\\' && i + 1 < text.size() && specials.contains(text.at(i + 1))) {
            out += text.at(i + 1);
            ++i;
        } else {
            out += ch;
        }
    }
    return out;
}

int countMarkdownV2Escapes(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("\\\\[_*{}\\[\\]()#+\\-.!]"));
    int n = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++n;
    }
    return n;
}

/// Renders markdown (standard / GitHub dialect) into an HTML fragment.
QString markdownToHtml(const QString& text)
{
    QTextDocument doc;
    doc.setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
    QString html = doc.toHtml();
    // extract the body contents so it can be appended into the log
    const auto start = static_cast<qsizetype>(html.indexOf("<body", 0, Qt::CaseInsensitive));
    if (start < 0) {
        return text.toHtmlEscaped().replace('\n', QStringLiteral("<br>"));
    }
    start = static_cast<qsizetype>(html.indexOf(u'>', start)) + 1;
    const auto end = static_cast<qsizetype>(html.lastIndexOf("</body>", -1, Qt::CaseInsensitive));
    if (end < start) {
        return text.toHtmlEscaped().replace('\n', QStringLiteral("<br>"));
    }
    return html.mid(start, end - start);
}

}  // namespace

AiAssistantDialog::AiAssistantDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("AI Assistant"));
    resize(680, 580);

    auto* layout = new QVBoxLayout(this);

    // conversation log
    m_log = new QTextBrowser(this);
    m_log->setOpenExternalLinks(true);
    layout->addWidget(m_log, 1);

    // status line (tool activity)
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: gray;");
    layout->addWidget(m_statusLabel);

    // settings
    auto* providerRow = new QHBoxLayout();
    providerRow->addWidget(new QLabel(tr("Provider:")));
    m_providerCombo = new QComboBox(this);
    m_providerCombo->addItem("OpenRouter", "openrouter");
    m_providerCombo->addItem("OpenAI", "openai");
    m_providerCombo->addItem("Ollama (local)", "ollama");
    m_providerCombo->addItem("LM Studio (local)", "lmstudio");
    m_providerCombo->addItem("Groq", "groq");
    m_providerCombo->addItem(tr("Custom"), "custom");
    providerRow->addWidget(m_providerCombo, 1);
    layout->addLayout(providerRow);

    auto* settingsRow = new QHBoxLayout();
    settingsRow->addWidget(new QLabel(tr("API URL:")));
    m_baseUrlEdit = new QLineEdit(this);
    settingsRow->addWidget(m_baseUrlEdit, 1);
    settingsRow->addWidget(new QLabel(tr("Key:")));
    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText(tr("API key (sk-or-… / sk-…)"));
    settingsRow->addWidget(m_apiKeyEdit, 1);
    layout->addLayout(settingsRow);

    auto* modelRow = new QHBoxLayout();
    modelRow->addWidget(new QLabel(tr("Model:")));
    m_modelCombo = new QComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    modelRow->addWidget(m_modelCombo, 1);
    m_fetchModelsButton = new QToolButton(this);
    m_fetchModelsButton->setText(tr("Fetch"));
    m_fetchModelsButton->setToolTip(
        tr("Download the model list from the API.\nWith the model \"openrouter/free\" the best free model is picked automatically."));
    modelRow->addWidget(m_fetchModelsButton);
    layout->addLayout(modelRow);

    // input row
    auto* inputRow = new QHBoxLayout();
    m_formatCombo = new QComboBox(this);
    m_formatCombo->setToolTip(tr("How to render assistant replies"));
    m_formatCombo->addItem(tr("Auto"), "auto");
    m_formatCombo->addItem(tr("Markdown"), "markdown");
    m_formatCombo->addItem(tr("MarkdownV2"), "markdownv2");
    m_formatCombo->addItem(tr("Plain"), "plain");
    inputRow->addWidget(m_formatCombo);
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("Ask the assistant… (Enter to send)"));
    inputRow->addWidget(m_input, 1);
    m_sendButton = new QPushButton(tr("Send"), this);
    inputRow->addWidget(m_sendButton);
    m_stopButton = new QPushButton(tr("Stop"), this);
    m_stopButton->setEnabled(false);
    connect(m_stopButton, &QPushButton::clicked, this, [this] { m_assistant.abortGeneration(); });
    inputRow->addWidget(m_stopButton);
    auto* resetButton = new QPushButton(tr("New chat"), this);
    inputRow->addWidget(resetButton);
    layout->addLayout(inputRow);

    // load settings
    auto* settings = APPLICATION->settings();
    m_baseUrlEdit->setText(settings->get("AiApiBaseUrl").toString());
    m_apiKeyEdit->setText(settings->get("AiApiKey").toString());
    const QString savedModel = settings->get("AiModel").toString();
    m_modelCombo->addItem(AiAssistant::autoFreeModelName());
    m_modelCombo->addItem("gpt-4o-mini");
    if (!savedModel.isEmpty()) {
        m_modelCombo->addItem(savedModel);
    }
    int idx = m_modelCombo->findText(savedModel);
    m_modelCombo->setCurrentIndex(idx < 0 ? 0 : idx);

    // select the provider preset matching the saved base URL
    const QString savedUrl = settings->get("AiApiBaseUrl").toString();
    int providerIdx = m_providerCombo->count() - 1;  // Custom
    for (int i = 0; i < m_providerCombo->count() - 1; ++i) {
        if (m_providerCombo->itemData(i).toString() == savedUrl) {
            providerIdx = i;
            break;
        }
    }
    m_providerCombo->setCurrentIndex(providerIdx);
    connect(m_providerCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString id = m_providerCombo->itemData(index).toString();
        applyProviderPreset(id);
    });

    // load the persisted conversation
    m_assistant.loadHistory();
    reloadHistoryDisplay();

    connect(m_sendButton, &QPushButton::clicked, this, &AiAssistantDialog::send);
    connect(resetButton, &QPushButton::clicked, this, [this] {
        m_assistant.resetConversation();
        m_log->clear();
        appendMessage("system", tr("New conversation."));
    });
    connect(m_fetchModelsButton, &QToolButton::clicked, this, &AiAssistantDialog::fetchModels);
    connect(&m_assistant, &AiAssistant::streamStarted, this, [this] {
        appendHtml(QStringLiteral("<b style='color:#9ece6a'>AI:</b>"));
        auto cursor = m_log->textCursor();
        cursor.movePosition(QTextCursor::End);
        m_streamAnchor = cursor.position();
        m_streaming = true;
    });
    connect(&m_assistant, &AiAssistant::streamDelta, this, [this](const QString& delta) {
        auto cursor = m_log->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(delta);
        m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
    });
    connect(&m_assistant, &AiAssistant::streamFinished, this, [this](bool hadToolCalls, const QString& fullText) {
        if (!m_streaming) {
            return;
        }
        // remove the raw streamed block
        auto cursor = m_log->textCursor();
        cursor.setPosition(m_streamAnchor);
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        m_streaming = false;
        if (!hadToolCalls && !fullText.isEmpty()) {
            appendHtml(QStringLiteral("<b style='color:#9ece6a'>AI:</b> %1").arg(renderAssistantText(fullText)));
        }
    });
    connect(&m_assistant, &AiAssistant::toolStarted, this, [this](const QString& tool, const QString& args) {
        m_statusLabel->setText(QStringLiteral("🔧 %1 %2…").arg(tool, args));
    });
    connect(&m_assistant, &AiAssistant::toolFinished, this,
            [this](const QString& tool, const QString& result, bool ok) {
                appendMessage("tool", QStringLiteral("%1 → %2%3").arg(tool, ok ? QString() : tr("FAILED: "), result));
            });
    connect(&m_assistant, &AiAssistant::errorOccurred, this, [this](const QString& error) {
        m_statusLabel->clear();
        appendMessage("error", error);
    });
    connect(&m_assistant, &AiAssistant::busyChanged, this, [this](bool busy) {
        m_sendButton->setEnabled(!busy);
        m_input->setEnabled(!busy);
        m_stopButton->setEnabled(busy);
    });
}

void AiAssistantDialog::appendHtml(const QString& html)
{
    m_log->append(html);
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

void AiAssistantDialog::appendMessage(const QString& role, const QString& text)
{
    QString html = text.toHtmlEscaped().replace('\n', QStringLiteral("<br>"));
    if (role == "user") {
        appendHtml(QStringLiteral("<b style='color:#7aa2f7'>%1:</b> %2").arg(tr("You"), html));
    } else if (role == "assistant") {
        appendHtml(QStringLiteral("<b style='color:#9ece6a'>AI:</b> %1").arg(renderAssistantText(text)));
    } else if (role == "tool") {
        appendHtml(QStringLiteral("<i style='color:gray'>🔧 %1</i>").arg(html));
    } else if (role == "error") {
        appendHtml(QStringLiteral("<b style='color:#f7768e'>⚠ %1</b>").arg(html));
    } else {
        appendHtml(QStringLiteral("<i style='color:gray'>%1</i>").arg(html));
    }
}

QString AiAssistantDialog::renderAssistantText(const QString& text) const
{
    const int idx = m_formatCombo->currentIndex();
    const QString mode = m_formatCombo->itemData(idx).isValid() ? m_formatCombo->itemData(idx).toString()
                                                                 : QStringLiteral("auto");
    if (mode == "plain") {
        return text.toHtmlEscaped().replace('\n', QStringLiteral("<br>"));
    }
    if (mode == "markdownv2") {
        return markdownToHtml(unescapeMarkdownV2(text));
    }
    if (mode == "markdown") {
        return markdownToHtml(text);
    }
    // auto: MarkdownV2-escaped text is unescaped first, then rendered
    if (countMarkdownV2Escapes(text) >= 2) {
        return markdownToHtml(unescapeMarkdownV2(text));
    }
    return markdownToHtml(text);
}

void AiAssistantDialog::reloadHistoryDisplay()
{
    const auto& history = m_assistant.history();
    const auto start = static_cast<int>(qMax<qsizetype>(0, history.size() - 40));
    for (int i = start; i < history.size(); ++i) {
        const auto& msg = history.at(i);
        const QString role = msg["role"].toString();
        const QString content = msg["content"].toString();
        if (role == "user" && !content.isEmpty()) {
            appendMessage("user", content);
        } else if (role == "assistant" && !content.isEmpty()) {
            appendMessage("assistant", renderAssistantText(content));
        }
    }
}

void AiAssistantDialog::applyProviderPreset(const QString& presetId)
{
    struct Preset {
        const char* id;
        const char* url;
        const char* model;
    };
    static const Preset presets[] = {
        { "openrouter", "https://openrouter.ai/api/v1", "openrouter/free" },
        { "openai", "https://api.openai.com/v1", "gpt-4o-mini" },
        { "ollama", "http://localhost:11434/v1", "llama3.1" },
        { "lmstudio", "http://localhost:1234/v1", "" },
        { "groq", "https://api.groq.com/openai/v1", "llama-3.3-70b-versatile" },
    };
    for (const auto& p : presets) {
        if (presetId == p.id) {
            m_baseUrlEdit->setText(p.url);
            if (QString(p.model).isEmpty()) {
                return;  // keep the user's model
            }
            int idx = m_modelCombo->findText(p.model);
            if (idx < 0) {
                m_modelCombo->addItem(p.model);
                idx = m_modelCombo->count() - 1;
            }
            m_modelCombo->setCurrentIndex(idx);
            return;
        }
    }
    // custom: keep everything as-is
}

void AiAssistantDialog::saveSettings()
{
    auto* settings = APPLICATION->settings();
    settings->set("AiApiBaseUrl", m_baseUrlEdit->text().trimmed());
    settings->set("AiApiKey", m_apiKeyEdit->text().trimmed());
    const QString model = m_modelCombo->currentData().isValid() ? m_modelCombo->currentData().toString()
                                                                 : m_modelCombo->currentText().trimmed();
    settings->set("AiModel", model);
}

void AiAssistantDialog::fetchModels()
{
    saveSettings();
    m_fetchModelsButton->setEnabled(false);
    m_fetchModelsButton->setText("…");

    QStringList free, paid;
    QString error;
    const bool ok =
        m_assistant.fetchModelIds(m_baseUrlEdit->text().trimmed(), m_apiKeyEdit->text().trimmed(), &free, &paid, &error);

    const QString current = m_modelCombo->currentData().isValid() ? m_modelCombo->currentData().toString()
                                                                   : m_modelCombo->currentText();
    m_modelCombo->clear();
    m_modelCombo->addItem(AiAssistant::autoFreeModelName());

    if (ok) {
        for (const auto& id : free) {
            m_modelCombo->addItem(QStringLiteral("%1  [free]").arg(id), id);
        }
        for (const auto& id : paid) {
            m_modelCombo->addItem(id, id);
        }
        m_statusLabel->setText(tr("Models loaded: %1 free, %2 paid").arg(free.size()).arg(paid.size()));
    } else {
        m_statusLabel->setText(tr("Failed to fetch models: %1").arg(error));
    }

    // restore the previous selection if it still exists
    int idx = m_modelCombo->findData(current);
    if (idx < 0) {
        idx = m_modelCombo->findText(current);
    }
    m_modelCombo->setCurrentIndex(idx < 0 ? 0 : idx);

    m_fetchModelsButton->setEnabled(true);
    m_fetchModelsButton->setText(tr("Fetch"));
}

void AiAssistantDialog::send()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    saveSettings();
    m_input->clear();
    appendMessage("user", text);
    m_assistant.sendUserMessage(text);
}

void AiAssistantDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_input->hasFocus()) {
            send();
            return;
        }
    }
    QDialog::keyPressEvent(event);
}
