// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*!
 * \brief Shared access to ~/.config/phoenix-dna/keys.json.
 *
 * The key file is read by several parties — the agent daemon (Python,
 * agent/phoenix_dna/config.py), the transcriber (per request) and the
 * settings store (read/write). These header-only helpers keep the path
 * resolution in one place; the semantics match config.py:
 *
 *   * the directory is $PHOENIX_CONFIG_DIR when set,
 *     ~/.config/phoenix-dna otherwise;
 *   * a missing or malformed file simply means "no keys".
 */

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace PhoenixKeys {

/*! \brief The configuration directory, honouring PHOENIX_CONFIG_DIR. */
inline QString configDir()
{
    QString dir = qEnvironmentVariable("PHOENIX_CONFIG_DIR");
    if (dir.isEmpty()) {
        dir = QDir::homePath() + QStringLiteral("/.config/phoenix-dna");
    }
    return dir;
}

/*! \brief The path of keys.json inside the configuration directory. */
inline QString keysFilePath()
{
    return QDir(configDir()).filePath(QStringLiteral("keys.json"));
}

/*! \brief The parsed keys.json; an empty object when there is none. */
inline QJsonObject readKeys()
{
    QFile file(keysFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace PhoenixKeys
