// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "speaker.h"
#include "keysfile.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonValue>

namespace
{
const QString kDaemonService = QStringLiteral("dna.phoenix.Agent");
const QString kDaemonPath = QStringLiteral("/dna/phoenix/Agent");
const QString kDaemonInterface = QStringLiteral("dna.phoenix.Agent");

// "true"/"1"/"on" — the same spellings config.py accepts.
bool truthy(const QString &value)
{
    const QString lowered = value.trimmed().toLower();
    return lowered == QLatin1String("true") || lowered == QLatin1String("1")
        || lowered == QLatin1String("yes") || lowered == QLatin1String("on");
}

// keys.json values arrive as JSON booleans or strings; accept both.
QString keysString(const QString &key)
{
    const QJsonValue value = PhoenixKeys::readKeys().value(key);
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return value.toString();
}
} // namespace

Speaker::Speaker(QObject *parent)
    : QObject(parent)
{
    // The match rule binds to the well-known name, so it can be
    // installed before the daemon is running — delivery starts as soon
    // as the name is acquired (the same reasoning as AgentBridge's).
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface,
        QStringLiteral("SpeechStateChanged"), this,
        SLOT(onSpeechStateChanged(QString)));

    // The static config decides the initial availability; the daemon's
    // own answer (authoritative, sees agent.toml) refines it right away.
    setAvailable(resolveConfig().enabled);
    probeState();
}

SpeechConfig Speaker::resolveConfig()
{
    SpeechConfig config;

    // Environment beats keys.json ("tts_enabled"); without either, the
    // affordance stays hidden until GetSpeechState says otherwise.
    QString enabled = qEnvironmentVariable("PHOENIX_TTS_ENABLED");
    if (enabled.isEmpty()) {
        enabled = keysString(QStringLiteral("tts_enabled"));
    }
    config.enabled = truthy(enabled);

    QString voice = qEnvironmentVariable("PHOENIX_TTS_VOICE");
    if (voice.isEmpty()) {
        voice = keysString(QStringLiteral("tts_voice"));
    }
    config.voice = voice;
    return config;
}

void Speaker::speak(const QString &text)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("SpeakText"));
    message.setArguments({text});
    QDBusConnection::sessionBus().asyncCall(message);
}

void Speaker::stop()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("StopSpeaking"));
    QDBusConnection::sessionBus().asyncCall(message);
}

void Speaker::reload()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("ReloadSpeech"));
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        if (!call->error().isValid()) {
            probeState();
        }
    });
}

void Speaker::onSpeechStateChanged(const QString &state)
{
    setSpeaking(state == QLatin1String("speaking"));
}

void Speaker::probeState()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("GetSpeechState"));
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        QDBusPendingReply<bool, QString> reply = *call;
        call->deleteLater();
        if (reply.isError()) {
            return; // no daemon: the static config stands
        }
        setAvailable(reply.argumentAt<0>());
        setSpeaking(reply.argumentAt<1>() == QLatin1String("speaking"));
    });
}

void Speaker::setSpeaking(bool speaking)
{
    if (m_speaking == speaking) {
        return;
    }
    m_speaking = speaking;
    Q_EMIT speakingChanged();
}

void Speaker::setAvailable(bool available)
{
    if (m_available == available) {
        return;
    }
    m_available = available;
    Q_EMIT availableChanged();
}
