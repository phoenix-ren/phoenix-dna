// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "agentmode.h"

#include <QDir>

namespace AgentMode
{

QString workingDirectory(Mode mode)
{
    switch (mode) {
    case Root:
        // Administrative work operates on the whole filesystem.
        return QStringLiteral("/");
    case User:
        // Everyday personal work operates in the user's home.
        return QDir::homePath();
    case Web:
        // General queries have no filesystem context at all.
        return QString();
    }
    return QString();
}

bool isElevated(Mode mode)
{
    return mode == Root;
}

QString iconName(Mode mode)
{
    switch (mode) {
    case Root:
        // Root work means commands run via sudo: the terminal says "shell",
        // where the sliders of preferences-system said "settings".
        return QStringLiteral("utilities-terminal-symbolic");
    case User:
        // The home icon is the universal shorthand for the user's context.
        return QStringLiteral("go-home-symbolic");
    case Web:
        return QStringLiteral("globe-symbolic");
    }
    return QString();
}

QString modeName(Mode mode)
{
    // Matches the daemon's Mode enum values (agent/phoenix_ai/modes.py),
    // which is what travels over D-Bus.
    switch (mode) {
    case Root:
        return QStringLiteral("root");
    case User:
        return QStringLiteral("user");
    case Web:
        return QStringLiteral("web");
    }
    return QString();
}

QString displayName(Mode mode)
{
    switch (mode) {
    case Root:
        return QStringLiteral("Root");
    case User:
        return QStringLiteral("User");
    case Web:
        return QStringLiteral("Web");
    }
    return QString();
}

} // namespace AgentMode
