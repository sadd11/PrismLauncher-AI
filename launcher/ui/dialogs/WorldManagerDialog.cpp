// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher AI fork
 *  Copyright (C) 2026 Prism Launcher Contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 */

#include "WorldManagerDialog.h"

#include "minecraft/MinecraftInstance.h"
#include "minecraft/WorldList.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "FileSystem.h"

WorldManagerDialog::WorldManagerDialog(QWidget* parent, MinecraftInstance* instance) : QDialog(parent), m_instance(instance)
{
    setWindowTitle(tr("Worlds — %1").arg(instance->name()));
    resize(560, 420);

    auto* layout = new QVBoxLayout(this);

    m_infoLabel = new QLabel(this);
    layout->addWidget(m_infoLabel);

    m_list = new QListWidget(this);
    layout->addWidget(m_list, 1);

    auto* buttons = new QHBoxLayout();
    m_backupButton = new QPushButton(tr("Backup"), this);
    m_deleteButton = new QPushButton(tr("Delete"), this);
    m_openFolderButton = new QPushButton(tr("Open folder"), this);
    buttons->addWidget(m_backupButton);
    buttons->addWidget(m_deleteButton);
    buttons->addWidget(m_openFolderButton);
    layout->addLayout(buttons);

    auto* closeRow = new QHBoxLayout();
    closeRow->addStretch(1);
    auto* close = new QPushButton(tr("Close"), this);
    closeRow->addWidget(close);
    layout->addLayout(closeRow);

    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        const bool has = !m_list->selectedItems().isEmpty();
        m_backupButton->setEnabled(has);
        m_deleteButton->setEnabled(has);
        m_openFolderButton->setEnabled(has);
    });
    connect(m_backupButton, &QPushButton::clicked, this, &WorldManagerDialog::backupSelected);
    connect(m_deleteButton, &QPushButton::clicked, this, &WorldManagerDialog::deleteSelected);
    connect(m_openFolderButton, &QPushButton::clicked, this, &WorldManagerDialog::openSelectedFolder);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);

    refresh();
}

void WorldManagerDialog::refresh()
{
    m_list->clear();
    WorldList worldList(QDir(m_instance->gameRoot()).filePath("saves"), m_instance);
    worldList.update();
    for (const auto& world : worldList.allWorlds()) {
        const QDateTime lastPlayed = world.lastPlayed();
        m_list->addItem(QStringLiteral("%1  (%2, %3)")
                            .arg(world.name(),
                                 world.gameType().toLogString(),
                                 lastPlayed.isValid() ? lastPlayed.toString("yyyy-MM-dd HH:mm") : tr("never played")));
    }
    if (worldList.allWorlds().isEmpty()) {
        m_list->addItem(tr("No worlds found"));
    }
}

QString WorldManagerDialog::selectedWorldFolder() const
{
    const auto items = m_list->selectedItems();
    if (items.isEmpty()) {
        return {};
    }
    // find the world folder matching the selected display name
    WorldList worldList(QDir(m_instance->gameRoot()).filePath("saves"), m_instance);
    worldList.update();
    const QString displayName = items.first()->text().section(QStringLiteral("  ("), 0, 0);
    for (const auto& world : worldList.allWorlds()) {
        if (world.name() == displayName) {
            return world.container().absoluteFilePath();
        }
    }
    return {};
}

void WorldManagerDialog::backupSelected()
{
    const QString folder = selectedWorldFolder();
    if (folder.isEmpty()) {
        return;
    }
    const QString worldName = QFileInfo(folder).fileName();
    const QString backupRoot = FS::PathCombine(m_instance->gameRoot(), "backups");
    FS::ensureFolderPathExists(backupRoot);
    const QString backupPath = FS::PathCombine(
        backupRoot, QStringLiteral("%1-%2").arg(worldName, QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
    FS::copy folderCopy(folder, backupPath);
    folderCopy.followSymlinks(false).copyDirectories(true);
    if (folderCopy()) {
        QMessageBox::information(this, tr("Backup"), tr("Backup created: %1").arg(backupPath));
    } else {
        QMessageBox::warning(this, tr("Backup"), tr("Backup failed"));
    }
}

void WorldManagerDialog::deleteSelected()
{
    const auto items = m_list->selectedItems();
    if (items.isEmpty()) {
        return;
    }
    const QString displayName = items.first()->text().section(QStringLiteral("  ("), 0, 0);
    const QString folder = selectedWorldFolder();
    if (folder.isEmpty()) {
        return;
    }
    const auto reply = QMessageBox::question(this, tr("Delete world"),
                                             tr("Delete world '%1' permanently?").arg(displayName),
                                             QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }
    WorldList worldList(QDir(m_instance->gameRoot()).filePath("saves"), m_instance);
    worldList.update();
    for (World world : worldList.allWorlds()) {
        if (world.name() == displayName) {
            world.destroy();
            break;
        }
    }
    refresh();
}

void WorldManagerDialog::openSelectedFolder()
{
    const QString folder = selectedWorldFolder();
    if (!folder.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(folder).absolutePath()));
    }
}
