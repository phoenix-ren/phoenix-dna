// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "sessiontranscriptmodel.h"

namespace {
//: How many messages are handed over at a time. Few enough that the
//: view fills in a few steps — the bar at the top of the window has
//: something to show — and many enough that a long transcript is not
//: a long wait.
constexpr int kChunkSize = 24;
} // namespace

SessionTranscriptModel::SessionTranscriptModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_chunkTimer.setSingleShot(true);
    // A chunk per frame: the view fills in steps, and the window
    // paints in between.
    m_chunkTimer.setInterval(16);
    connect(&m_chunkTimer, &QTimer::timeout,
            this, &SessionTranscriptModel::appendChunk);
}

int SessionTranscriptModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_handed;
}

QVariant SessionTranscriptModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_handed) {
        return {};
    }
    const QVariantMap &message = m_messages.at(index.row());
    switch (role) {
    case RoleRole:
        return message.value(QStringLiteral("role"));
    case RoleContent:
        return message.value(QStringLiteral("content"));
    case RoleTs:
        return message.value(QStringLiteral("ts"));
    default:
        return {};
    }
}

QHash<int, QByteArray> SessionTranscriptModel::roleNames() const
{
    return {
        {RoleRole, QByteArrayLiteral("role")},
        {RoleContent, QByteArrayLiteral("content")},
        {RoleTs, QByteArrayLiteral("ts")},
    };
}

void SessionTranscriptModel::load(const QVariantList &messages)
{
    QList<QVariantMap> kept;
    kept.reserve(messages.size());
    for (const QVariant &entry : messages) {
        const QVariantMap message = entry.toMap();
        const QString content = message.value(QStringLiteral("content")).toString();
        if (content.trimmed().isEmpty()) {
            // A tool call is stored as an assistant turn without
            // content: a row with nothing in it is neither a query nor
            // an answer.
            continue;
        }
        kept.append(message);
    }

    beginResetModel();
    m_messages = kept;
    m_handed = 0;
    m_total = m_messages.size();
    endResetModel();

    Q_EMIT totalCountChanged();

    if (m_total == 0) {
        setLoading(false);
        return;
    }
    setLoading(true);
    m_chunkTimer.start();
}

void SessionTranscriptModel::clear()
{
    m_chunkTimer.stop();

    beginResetModel();
    m_messages.clear();
    m_handed = 0;
    const bool hadMessages = m_total != 0;
    m_total = 0;
    endResetModel();

    if (hadMessages) {
        Q_EMIT totalCountChanged();
    }
    setLoading(false);
}

void SessionTranscriptModel::appendChunk()
{
    if (m_handed >= m_total) {
        setLoading(false);
        return;
    }

    const int count = qMin(kChunkSize, m_total - m_handed);
    beginInsertRows(QModelIndex(), m_handed, m_handed + count - 1);
    m_handed += count;
    endInsertRows();

    if (m_handed < m_total) {
        m_chunkTimer.start();
    } else {
        setLoading(false);
    }
}

void SessionTranscriptModel::setLoading(bool loading)
{
    if (m_loading == loading) {
        return;
    }
    m_loading = loading;
    Q_EMIT loadingChanged();
}
