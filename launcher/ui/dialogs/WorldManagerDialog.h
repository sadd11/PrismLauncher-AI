// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher AI fork
 *  Copyright (C) 2026 Prism Launcher Contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 */

#pragma once

#include <QDialog>

#include "ai/AiAssistant.h"

class QListWidget;
class QPushButton;
class QLabel;
class MinecraftInstance;

/** Simple world manager: list, backup, delete, open folder. */
class WorldManagerDialog : public QDialog {
    Q_OBJECT

   public:
    explicit WorldManagerDialog(QWidget* parent, MinecraftInstance* instance);

   private slots:
    void refresh();
    void backupSelected();
    void deleteSelected();
    void openSelectedFolder();

   private:
    QString selectedWorldFolder() const;

    QListWidget* m_list = nullptr;
    QLabel* m_infoLabel = nullptr;
    QPushButton* m_backupButton = nullptr;
    QPushButton* m_deleteButton = nullptr;
    QPushButton* m_openFolderButton = nullptr;
    MinecraftInstance* m_instance = nullptr;
};
