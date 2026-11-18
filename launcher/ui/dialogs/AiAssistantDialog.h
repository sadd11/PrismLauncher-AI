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

#include <QDialog>

#include "ai/AiAssistant.h"

class QTextBrowser;
class QLineEdit;
class QPushButton;
class QComboBox;
class QLabel;
class QToolButton;

/** The AI assistant chat window (toolbar → "AI Assistant"). */
class AiAssistantDialog : public QDialog {
    Q_OBJECT

   public:
    explicit AiAssistantDialog(QWidget* parent = nullptr);
    ~AiAssistantDialog() override = default;

   protected:
    void keyPressEvent(QKeyEvent* event) override;

   private slots:
    void send();
    void saveSettings();
    void fetchModels();

   private:
    void appendHtml(const QString& html);
    void appendMessage(const QString& role, const QString& text);
    QString renderAssistantText(const QString& text) const;
    void reloadHistoryDisplay();
    void applyProviderPreset(const QString& presetId);

    AiAssistant m_assistant;
    QTextBrowser* m_log = nullptr;
    QLineEdit* m_input = nullptr;
    QPushButton* m_sendButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QComboBox* m_providerCombo = nullptr;
    QLineEdit* m_baseUrlEdit = nullptr;
    QLineEdit* m_apiKeyEdit = nullptr;
    QComboBox* m_modelCombo = nullptr;
    QToolButton* m_fetchModelsButton = nullptr;
    QComboBox* m_formatCombo = nullptr;
    QLabel* m_statusLabel = nullptr;

    // streaming state
    bool m_streaming = false;
    int m_streamAnchor = 0;
};
