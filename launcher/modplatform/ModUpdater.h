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

#include <QString>
#include <QStringList>

#include <functional>

class MinecraftInstance;

/**
 * "Update all mods" for an instance: scans <gameRoot>/mods for packwiz
 * metadata (*.pw.toml) and updates Modrinth-hosted mods to the latest
 * version compatible with the instance's Minecraft version.
 *
 * Only Modrinth-hosted mods are updated (CurseForge metadata is skipped).
 * The result is a human-readable summary suitable for dialogs and the AI
 * assistant.
 */
namespace ModUpdater {

struct UpdateSummary {
    QStringList updated;
    QStringList upToDate;
    QStringList failed;
    QStringList skippedNoMetadata;
};

UpdateSummary updateInstanceMods(MinecraftInstance* instance,
                                 const std::function<void(int, int, const QString&)>& progress = {});

/// Same, but downloads happen synchronously; suitable for AI tools.
UpdateSummary updateInstanceModsSync(MinecraftInstance* instance,
                                     const std::function<void(int, int, const QString&)>& progress = {});

/// Human-readable summary suitable for dialogs and the AI assistant.
QString summarize(const UpdateSummary& summary);

}  // namespace ModUpdater
