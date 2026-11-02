// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QString>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief The privilege mode the agent operates in.
 *
 * The mode is a security boundary: it decides both whether the agent may
 * escalate privileges and which directory it treats as its working context.
 *
 * \list
 * \li \c Root — escalates through the agent daemon (KDE password dialog,
 *     sudo); working directory is /.
 * \li \c User — no escalation, working directory is the user's home.
 * \endlist
 *
 * The mode values are exposed to QML through this Q_NAMESPACE: QML reads
 * them as \c AgentMode.Root and \c AgentMode.User. The enum here stays
 * the single source of truth for both C++ and QML.
 *
 * \note This must be a namespace, not a QObject class: a QML_ANONYMOUS or
 * QML_UNCREATABLE type cannot resolve \c Type.Value enum lookups from QML,
 * which produced "Cannot read property 'Root' of undefined".
 */
namespace AgentMode
{

Q_NAMESPACE
QML_NAMED_ELEMENT(AgentMode)

enum Mode
{
    Root = 0, ///< Elevated via KAuth; working directory "/".
    User = 1, ///< Unprivileged; working directory "~".
};
Q_ENUM_NS(Mode)

/*! \brief The working directory implied by the given mode. */
QString workingDirectory(Mode mode);

/*! \brief Whether the given mode may escalate privileges. */
bool isElevated(Mode mode);

/*! \brief The icon representing the given mode. */
QString iconName(Mode mode);

/*! \brief The machine name the agent daemon uses for the given mode. */
QString modeName(Mode mode);

/*! \brief A human-readable name for the given mode. */
QString displayName(Mode mode);

} // namespace AgentMode
