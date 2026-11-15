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

#include "AiAssistant.h"
#include "Application.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

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
    auto* settingsRow = new QHBoxLayout();
    settingsRow->addWidget(new QLabel(tr("API URL:")));
    m_baseUrlEdit = new QLineEdit(this);
    m_baseUrlEdit->setPlaceholderText("https://openrouter.ai/api/v1");
    settingsRow->addWidget(m_baseUrlEdit, 1);
    settingsRow->addWidget(new QLabel(tr("Key:")));
    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText("sk-or-…");
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
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("Ask the assistant… (Enter to send)"));
    inputRow->addWidget(m_input, 1);
    m_sendButton = new QPushButton(tr("Send"), this);
    inputRow->addWidget(m_sendButton);
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

    connect(m_sendButton, &QPushButton::clicked, this, &AiAssistantDialog::send);
    connect(resetButton, &QPushButton::clicked, this, [this] {
        m_assistant.resetConversation();
        m_log->clear();
        appendMessage("system", tr("New conversation."));
    });
    connect(m_fetchModelsButton, &QToolButton::clicked, this, &AiAssistantDialog::fetchModels);
    connect(&m_assistant, &AiAssistant::assistantReply, this, [this](const QString& text) {
        m_statusLabel->clear();
        appendMessage("assistant", text);
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
    });
}

void AiAssistantDialog::appendHtml(const QString& html)
{
    m_log->append(html);
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

void AiAssistantDialog::appendMessage(const QString& role, const QString& text)
{
    QString html = text.toHtmlEscaped().replace('\n', "<br>");
    if (role == "user") {
        appendHtml(QStringLiteral("<b style='color:#7aa2f7'>%1:</b> %2").arg(tr("You"), html));
    } else if (role == "assistant") {
        appendHtml(QStringLiteral("<b style='color:#9ece6a'>AI:</b> %1").arg(html));
    } else if (role == "tool") {
        appendHtml(QStringLiteral("<i style='color:gray'>🔧 %1</i>").arg(html));
    } else if (role == "error") {
        appendHtml(QStringLiteral("<b style='color:#f7768e'>⚠ %1</b>").arg(html));
    } else {
        appendHtml(QStringLiteral("<i style='color:gray'>%1</i>").arg(html));
    }
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
