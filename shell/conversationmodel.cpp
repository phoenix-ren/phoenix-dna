// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "conversationmodel.h"

ConversationModel::ConversationModel(QObject *parent) : QAbstractListModel(parent) {}

int ConversationModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_exchanges.size();
}

QVariant ConversationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_exchanges.size()) {
        return {};
    }
    const Exchange &exchange = m_exchanges.at(index.row());
    switch (role) {
    case PromptRole:
        return exchange.prompt;
    case SenderRole:
        return exchange.sender;
    case ResultRole:
        return exchange.result;
    case ThinkingRole:
        return exchange.thinking;
    case ToolLogRole:
        return exchange.toolLog;
    case ToolCountRole:
        return exchange.toolCount;
    case RunningRole:
        return exchange.running;
    default:
        return {};
    }
}

QHash<int, QByteArray> ConversationModel::roleNames() const
{
    return {
        {PromptRole, "prompt"},   {SenderRole, "sender"},   {ResultRole, "result"},
        {ToolLogRole, "toolLog"}, {RunningRole, "running"}, {ThinkingRole, "thinking"},
        {ToolCountRole, "toolCount"},
    };
}

void ConversationModel::reset()
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    beginResetModel();
    m_exchanges.clear();
    endResetModel();
    Q_EMIT countChanged();
}

void ConversationModel::beginExchange(const QString &prompt, const QString &sender)
{
    Exchange exchange;
    exchange.prompt = prompt;
    exchange.sender = sender;
    beginInsertRows(QModelIndex(), m_exchanges.size(), m_exchanges.size());
    m_exchanges.append(exchange);
    endInsertRows();
    Q_EMIT countChanged();
}

void ConversationModel::appendResult(const QString &delta)
{
    if (delta.isEmpty() || m_exchanges.isEmpty()) {
        return;
    }
    m_exchanges.last().result += delta;
    touchCurrent({ResultRole});
}

void ConversationModel::setResult(const QString &result)
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    if (m_exchanges.last().result == result) {
        return;
    }
    m_exchanges.last().result = result;
    if (dropLastThinkingIf(result)) {
        touchCurrent({ResultRole, ThinkingRole});
        return;
    }
    touchCurrent({ResultRole});
}

void ConversationModel::demoteResultToThinking()
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    if (exchange.result.isEmpty()) {
        return;
    }
    // The notes are one chunk per turn, separated by a blank line.
    if (!exchange.thinking.isEmpty()) {
        exchange.thinking += QStringLiteral("\n\n");
    }
    exchange.thinking += exchange.result;
    exchange.result.clear();
    touchCurrent({ThinkingRole, ResultRole});
}

void ConversationModel::setThinking(const QString &thinking)
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    if (m_exchanges.last().thinking == thinking) {
        return;
    }
    m_exchanges.last().thinking = thinking;
    touchCurrent({ThinkingRole});
}

void ConversationModel::appendToolLog(const QString &markdown)
{
    if (markdown.isEmpty() || m_exchanges.isEmpty()) {
        return;
    }
    m_exchanges.last().toolLog += markdown;
    touchCurrent({ToolLogRole});
}

void ConversationModel::noteToolCall()
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    ++m_exchanges.last().toolCount;
    touchCurrent({ToolCountRole});
}

void ConversationModel::finishExchange()
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    if (!m_exchanges.last().running) {
        return;
    }
    m_exchanges.last().running = false;
    touchCurrent({RunningRole});
}

void ConversationModel::touchCurrent(const QList<int> &roles)
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    const QModelIndex index = this->index(m_exchanges.size() - 1);
    Q_EMIT dataChanged(index, index, roles);
}

bool ConversationModel::dropLastThinkingIf(const QString &text)
{
    Exchange &exchange = m_exchanges.last();
    if (exchange.thinking.isEmpty() || text.isEmpty()) {
        return false;
    }
    // The notes are one chunk per turn, separated by a blank line: only
    // a whole chunk is dropped, never a part of one.
    const int separator = exchange.thinking.lastIndexOf(QStringLiteral("\n\n"));
    const QString lastChunk = separator < 0
        ? exchange.thinking
        : exchange.thinking.mid(separator + 2);
    if (lastChunk != text) {
        return false;
    }
    if (separator < 0) {
        exchange.thinking.clear();
    } else {
        exchange.thinking.truncate(separator);
    }
    return true;
}
