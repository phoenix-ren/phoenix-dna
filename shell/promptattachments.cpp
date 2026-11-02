// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "promptattachments.h"

#include <QFileInfo>
#include <QMimeDatabase>
#include <QVariantMap>

PromptAttachments::PromptAttachments(QObject *parent) : QObject(parent) {}

QVariantList PromptAttachments::items() const
{
    QVariantList result;
    result.reserve(m_paths.size());
    for (int i = 0; i < m_paths.size(); ++i) {
        QVariantMap item;
        item.insert(QStringLiteral("name"), m_names.at(i));
        item.insert(QStringLiteral("path"), m_paths.at(i));
        item.insert(QStringLiteral("mimeType"), m_mimeTypes.at(i));
        item.insert(QStringLiteral("size"), m_sizes.at(i));
        result.append(item);
    }
    return result;
}

bool PromptAttachments::addFile(const QUrl &url)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (path.isEmpty()) {
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        return false;
    }

    // The same file twice would only confuse the agent (and the chip list).
    if (m_paths.contains(info.absoluteFilePath())) {
        return false;
    }

    if (info.size() > MaxFileSize) {
        return false;
    }

    const QMimeDatabase database;
    const QString mimeType = database.mimeTypeForFile(info).name();

    m_paths.append(info.absoluteFilePath());
    m_names.append(info.fileName());
    m_mimeTypes.append(mimeType);
    m_sizes.append(info.size());

    Q_EMIT changed();
    return true;
}

void PromptAttachments::remove(int index)
{
    if (index < 0 || index >= m_paths.size()) {
        return;
    }
    m_paths.removeAt(index);
    m_names.removeAt(index);
    m_mimeTypes.removeAt(index);
    m_sizes.removeAt(index);
    Q_EMIT changed();
}

void PromptAttachments::clear()
{
    if (m_paths.isEmpty()) {
        return;
    }
    m_paths.clear();
    m_names.clear();
    m_mimeTypes.clear();
    m_sizes.clear();
    Q_EMIT changed();
}

QVariantList PromptAttachments::toPayload() const
{
    QVariantList payload;
    payload.reserve(m_paths.size());
    for (int i = 0; i < m_paths.size(); ++i) {
        QVariantMap item;
        item.insert(QStringLiteral("name"), m_names.at(i));
        item.insert(QStringLiteral("path"), m_paths.at(i));
        item.insert(QStringLiteral("mimeType"), m_mimeTypes.at(i));
        payload.append(item);
    }
    return payload;
}
