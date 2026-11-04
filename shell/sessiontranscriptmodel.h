// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief The transcript of one session, as a list model.
 *
 * The session search shows the queries and responses of the selected
 * session on the right. The transcript arrives as one JSON blob and is
 * handed over in chunks: the rows appear a chunk at a time, so the
 * view fills while the window stays responsive — a long session holds
 * hundreds of messages, and building all of them in one go is what
 * made the window freeze for a moment on every click.
 *
 * The empty turns are dropped on the way in: a tool call is stored as
 * an assistant turn without content, and a row with nothing in it is
 * neither a query nor an answer — it would only take a delegate of
 * its own.
 */
class SessionTranscriptModel : public QAbstractListModel
{
    Q_OBJECT
    // The type is known to QML, so the view can read the counters off
    // it (totalCount, loading); it is never created from QML — the
    // shell owns the one the search shows.
    QML_ELEMENT
    QML_UNCREATABLE("The transcript is made by the shell, not by QML.")

    /*! \brief How many messages the transcript holds. */
    Q_PROPERTY(int totalCount READ totalCount NOTIFY totalCountChanged)

    /*! \brief Whether the transcript is still being handed over. */
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)

public:
    enum Roles
    {
        RoleRole = Qt::UserRole + 1,
        RoleContent,
        RoleTs,
    };

    explicit SessionTranscriptModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /*! \brief Takes the transcript and hands it over, a chunk at a time.
     *
     * Replaces whatever was shown before: the rows of the old
     * transcript are gone when the call is made, not when the new
     * ones arrive. */
    void load(const QVariantList &messages);

    /*! \brief Empties the transcript — a new session was picked. */
    void clear();

    int totalCount() const { return m_total; }
    bool isLoading() const { return m_loading; }

Q_SIGNALS:
    void totalCountChanged();
    void loadingChanged();

private:
    void appendChunk();
    void setLoading(bool loading);

    // The messages of the transcript, in the order they were stored.
    QList<QVariantMap> m_messages;
    // The rows handed to the view so far; the rest are still to come.
    int m_handed = 0;
    // How many messages the transcript holds in all.
    int m_total = 0;
    bool m_loading = false;
    // The timer that hands over the next chunk.
    QTimer m_chunkTimer;
};
