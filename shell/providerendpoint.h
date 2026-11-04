// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QHash>
#include <QString>

/*! \brief The provider an endpoint URL belongs to, or "" when it is none
 *  of the known ones.
 *
 *  The endpoint is the whole of the configuration: a user who typed
 *  "https://api.openai.com/v1" means OpenAI, whatever a remembered
 *  provider name says. Matched on the host, so a proxy in front of a
 *  service does not change the answer.
 *
 *  The table is the same as the daemon's (see
 *  agent/phoenix_dna/config.py, _ENDPOINT_HOSTS): the window and the
 *  daemon must read the same provider out of the same settings, or the
 *  key the window shows would not be the key the daemon uses.
 */
inline QString providerForEndpoint(const QString &endpoint)
{
    if (endpoint.isEmpty()) {
        return QString();
    }
    static const QHash<QString, QString> hosts = {
        {QStringLiteral("api.openai.com"), QStringLiteral("openai")},
        {QStringLiteral("api.groq.com"), QStringLiteral("groq")},
        {QStringLiteral("openrouter.ai"), QStringLiteral("openrouter")},
        {QStringLiteral("api.minimax.io"), QStringLiteral("minimax")},
        {QStringLiteral("api.minimax.chat"), QStringLiteral("minimax")},
    };
    QString text = endpoint.trimmed().toLower();
    // Strip the scheme, then the path and the port: what is left is
    // the host.
    const int scheme = text.indexOf(QStringLiteral("://"));
    if (scheme >= 0) {
        text = text.mid(scheme + 3);
    }
    text = text.section(QLatin1Char('/'), 0, 0).section(QLatin1Char(':'), 0, 0);
    for (auto it = hosts.cbegin(); it != hosts.cend(); ++it) {
        if (text == it.key() || text.endsWith(QLatin1Char('.') + it.key())) {
            return it.value();
        }
    }
    return QString();
}
