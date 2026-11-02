// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief The files the user attached to the prompt that is being composed.
 *
 * The dictation overlay, the results pane's follow-up field and the panel
 * widget all edit the *same* prompt: the next thing the user sends to the
 * agent. The attachments therefore live in one place - here - and every
 * surface shows the same list.
 *
 * An attachment is a local file the user picked (an image, a PDF, a log
 * file...). The widget does not copy or read it: the agent daemon receives
 * the path and does the copying, so the file lands in the daemon's own
 * attachment directory and stays readable for the whole run.
 */
class PromptAttachments : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    /*! \brief The attachments, oldest first, as a list of maps.
     *
     * Each entry has \c name, \c path, \c mimeType and \c size keys - the
     * shape the prompt surfaces render as a chip. */
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)

    /*! \brief How many files are attached. */
    Q_PROPERTY(int count READ count NOTIFY changed)

public:
    /*! \brief The largest file that may be attached (32 MiB).
     *
     * The file is copied into the agent's attachment directory and, for
     * images, inlined into the prompt as base64 - which multiplies its
     * size. The limit keeps a stray video file from bloating the request. */
    static constexpr qint64 MaxFileSize = 32LL * 1024 * 1024;

    explicit PromptAttachments(QObject *parent = nullptr);

    QVariantList items() const;
    int count() const { return static_cast<int>(m_paths.size()); }

    /*! \brief Attaches the file behind \a url.
     *
     * Returns false (and changes nothing) when the URL does not point at a
     * readable local file, when the file is already attached, or when it is
     * larger than MaxFileSize. */
    Q_INVOKABLE bool addFile(const QUrl &url);

    /*! \brief Removes the attachment at \a index. */
    Q_INVOKABLE void remove(int index);

    /*! \brief Removes every attachment. */
    Q_INVOKABLE void clear();

    /*! \brief The attachments as a JSON payload for the agent daemon.
     *
     * A list of \c {"name", "path", "mimeType"} objects - the contract the
     * daemon's SubmitPromptWithAttachments() expects. */
    QVariantList toPayload() const;

Q_SIGNALS:
    /*! \brief Emitted whenever the list of attachments changed. */
    void changed();

private:
    QStringList m_paths;
    QStringList m_names;
    QStringList m_mimeTypes;
    QList<qint64> m_sizes;
};
