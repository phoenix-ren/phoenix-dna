// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "conversationmodel.h"

#include <QDateTime>

namespace
{
/*! \brief The name of a step's kind, as the pane knows it. */
QString kindName(ConversationModel::StepKind kind)
{
    switch (kind) {
    case ConversationModel::StepKind::Thought:
        return QStringLiteral("thought");
    case ConversationModel::StepKind::Tool:
        return QStringLiteral("tool");
    case ConversationModel::StepKind::Compaction:
        return QStringLiteral("compaction");
    case ConversationModel::StepKind::Text:
        break;
    }
    return QStringLiteral("text");
}

/*! \brief The current time, in milliseconds since the epoch. */
qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}
} // namespace

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
    case RunningRole:
        return exchange.running;
    case StepsRole:
        return stepsFor(exchange);
    case ExchangeRole:
        return m_exchangeNumbers.value(index.row(), 0);
    default:
        return {};
    }
}

QHash<int, QByteArray> ConversationModel::roleNames() const
{
    return {
        {PromptRole, "prompt"},   {SenderRole, "sender"}, {ResultRole, "result"},
        {RunningRole, "running"}, {StepsRole, "steps"},   {ExchangeRole, "exchangeNumber"},
    };
}

void ConversationModel::reset()
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    beginResetModel();
    m_exchanges.clear();
    m_exchangeNumbers.clear();
    m_promptCount = 0;
    endResetModel();
    Q_EMIT countChanged();
}

void ConversationModel::truncate(int count)
{
    if (count < 0) {
        count = 0;
    }
    if (count >= m_exchanges.size()) {
        return;
    }
    beginRemoveRows(QModelIndex(), count, m_exchanges.size() - 1);
    m_exchanges.resize(count);
    m_exchangeNumbers.resize(count);
    // The queries are counted again: the exchanges that are kept keep
    // their numbers (the history only ever loses its tail), but the
    // count of them is the one of the shorter history now.
    m_promptCount = 0;
    for (int row = 0; row < count; ++row) {
        m_exchangeNumbers[row] = m_exchanges.at(row).prompt.isEmpty() ? 0 : ++m_promptCount;
    }
    endRemoveRows();
    Q_EMIT countChanged();
}

int ConversationModel::rowOfExchange(int number) const
{
    if (number < 1) {
        return -1;
    }
    for (int row = 0; row < m_exchangeNumbers.size(); ++row) {
        if (m_exchangeNumbers.at(row) == number) {
            return row;
        }
    }
    return -1;
}

void ConversationModel::beginExchange(const QString &prompt, const QString &sender)
{
    Exchange exchange;
    exchange.prompt = prompt;
    exchange.sender = sender;
    beginInsertRows(QModelIndex(), m_exchanges.size(), m_exchanges.size());
    m_exchanges.append(exchange);
    m_exchangeNumbers.append(prompt.isEmpty() ? 0 : ++m_promptCount);
    endInsertRows();
    Q_EMIT countChanged();
}

void ConversationModel::appendResult(const QString &delta)
{
    if (delta.isEmpty() || m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    // The words the model says: they join the answer's step, or start
    // one. A step that is not the answer's (the preamble of an earlier
    // stretch) is left alone: the answer is always its own step.
    Group *group = lastGroupOfKind(exchange, StepKind::Text);
    if (!group || !group->answer) {
        group = &pushGroup(exchange, StepKind::Text);
        group->answer = true;
    }
    group->body += delta;
    exchange.result += delta;
    touchCurrent({ResultRole, StepsRole});
}

void ConversationModel::setResult(const QString &result)
{
    if (result.isEmpty() || m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    if (exchange.result == result) {
        return;
    }
    // The finished text is the answer. The step that carries these words
    // — the one that was streaming, or the one that already holds
    // exactly them — becomes the answer; the pane shows it below the
    // trail, so it leaves the trail and is shown only once.
    Group *group = nullptr;
    if (!exchange.groups.isEmpty() && exchange.groups.last().kind == StepKind::Text) {
        group = &exchange.groups.last();
    } else {
        for (int i = exchange.groups.size() - 1; i >= 0; --i) {
            if (exchange.groups.at(i).kind == StepKind::Text
                && exchange.groups.at(i).body == result) {
                group = &exchange.groups[i];
                break;
            }
        }
    }
    if (!group) {
        group = &pushGroup(exchange, StepKind::Text);
    }
    for (Group &other : exchange.groups) {
        other.answer = false;
    }
    group->answer = true;
    group->body = result;
    exchange.result = result;
    touchCurrent({ResultRole, StepsRole});
}

void ConversationModel::demoteResult()
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    // The agent reached for a tool: the words so far are not the answer
    // any more but the preamble to the work. They stay in the trail, in
    // the order they were said.
    bool changed = !exchange.result.isEmpty();
    exchange.result.clear();
    for (Group &group : exchange.groups) {
        if (group.answer) {
            group.answer = false;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    touchCurrent({ResultRole, StepsRole});
}

void ConversationModel::appendThinking(const QString &delta)
{
    if (delta.isEmpty() || m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    // The notes the model thinks in: they join the thought step that is
    // running, or start one.
    Group *group = lastGroupOfKind(exchange, StepKind::Thought);
    if (!group) {
        group = &pushGroup(exchange, StepKind::Thought);
    }
    group->body += delta;
    touchCurrent({StepsRole});
}

void ConversationModel::appendTrailText(const QString &text)
{
    if (text.isEmpty() || m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    Group &group = pushGroup(exchange, StepKind::Text);
    group.body = text;
    touchCurrent({StepsRole});
}

void ConversationModel::beginToolCall(const QString &tool, const QString &markdown,
                                      const QString &direction)
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    // The calls of one stretch belong together: they are one step of the
    // trail, not one step each. The transfer tool is listed once per
    // direction, so a stretch that pulls a file and pushes another shows
    // both words without repeating the tool's name.
    Group *group = lastGroupOfKind(exchange, StepKind::Tool);
    const bool fresh = !group;
    if (fresh) {
        group = &pushGroup(exchange, StepKind::Tool);
    }
    const bool listed = group->tools.contains(tool)
        && !(tool == QLatin1String("transfer")
             && !group->directions.contains(direction));
    if (!listed) {
        group->tools.append(tool);
        group->directions.append(direction);
    }
    if (!group->body.isEmpty()) {
        group->body += QStringLiteral("\n\n");
    }
    group->body += markdown;
    if (!fresh) {
        ++group->count;
    }
    // The call is running: the step is open again, and its clock runs
    // on until the call comes back.
    group->running = true;
    group->endedAt = 0;
    touchCurrent({StepsRole});
}

void ConversationModel::endToolCall(const QString &status, const QString &markdown)
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    if (exchange.groups.isEmpty()) {
        return;
    }
    Group &group = exchange.groups.last();
    if (group.kind != StepKind::Tool) {
        return;
    }
    group.body += QStringLiteral("\n\n") + markdown;
    group.status = status;
    group.running = false;
    group.endedAt = now();
    touchCurrent({StepsRole});
}

void ConversationModel::appendCompaction(int summarised)
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    // The step stands alone: it says what happened to the conversation,
    // not what the agent said.
    Group &group = pushGroup(exchange, StepKind::Compaction);
    group.body = summarised > 0
        ? QStringLiteral("The conversation was summarised to make room — %1 earlier "
                         "messages became a summary.")
              .arg(summarised)
        : QStringLiteral("The conversation was summarised to make room.");
    group.running = false;
    group.endedAt = now();
    touchCurrent({StepsRole});
}

void ConversationModel::finishExchange()
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    Exchange &exchange = m_exchanges.last();
    if (!exchange.running) {
        return;
    }
    exchange.running = false;
    if (!exchange.groups.isEmpty()) {
        Group &group = exchange.groups.last();
        if (group.running) {
            group.running = false;
            group.endedAt = now();
        }
    }
    touchCurrent({RunningRole, StepsRole});
}

void ConversationModel::touchCurrent(const QList<int> &roles)
{
    if (m_exchanges.isEmpty()) {
        return;
    }
    const QModelIndex index = this->index(m_exchanges.size() - 1);
    Q_EMIT dataChanged(index, index, roles);
}

QVariantList ConversationModel::stepsFor(const Exchange &exchange) const
{
    QVariantList trail;
    for (const Group &group : exchange.groups) {
        // The answer has its own place in the pane, below the trail.
        if (group.answer) {
            continue;
        }
        QVariantMap step;
        step.insert(QStringLiteral("id"), group.id);
        step.insert(QStringLiteral("kind"), kindName(group.kind));
        step.insert(QStringLiteral("body"), group.body);
        step.insert(QStringLiteral("count"), group.count);
        step.insert(QStringLiteral("tools"), group.tools);
        step.insert(QStringLiteral("directions"), group.directions);
        step.insert(QStringLiteral("status"), group.status);
        step.insert(QStringLiteral("running"), group.running);
        step.insert(QStringLiteral("startedAt"), group.startedAt);
        step.insert(QStringLiteral("endedAt"), group.endedAt);
        trail.append(step);
    }
    return trail;
}

ConversationModel::Group *ConversationModel::lastGroupOfKind(Exchange &exchange, StepKind kind)
{
    if (exchange.groups.isEmpty()) {
        return nullptr;
    }
    Group &group = exchange.groups.last();
    return group.kind == kind ? &group : nullptr;
}

ConversationModel::Group &ConversationModel::pushGroup(Exchange &exchange, StepKind kind)
{
    // Whatever ran until now is over.
    if (!exchange.groups.isEmpty()) {
        Group &previous = exchange.groups.last();
        if (previous.running) {
            previous.running = false;
            previous.endedAt = now();
        }
    }
    Group group;
    group.kind = kind;
    group.id = ++m_groupId;
    group.count = 1;
    group.startedAt = now();
    group.running = true;
    exchange.groups.append(group);
    return exchange.groups.last();
}
