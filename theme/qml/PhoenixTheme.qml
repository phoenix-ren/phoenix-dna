// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

pragma Singleton

import QtQuick

/*
 * The shared colour palette of the app's own windows.
 *
 * One singleton serves every component — the dictation overlay, the
 * results window, the session search, the settings window — so the
 * colour scheme chosen in the settings recolours them all at once,
 * live. The applet's shell pushes the inputs in (two Bindings in
 * main.qml): the persisted preference from the settings store, and
 * whether the desktop currently runs dark, read off the applet's own
 * Kirigami theme — inside plasmashell that is the Plasma theme, and it
 * updates live. Everything below is derived from those two.
 *
 * Three modes:
 *
 *   * "system" (the default) — follow the desktop. The Plasma theme is
 *     the judge of that, deliberately: the PC3 controls inside the
 *     windows are themed by it (Kirigami offers no per-window scheme
 *     override in the KF6 versions targeted), so resolving "system"
 *     from the same source keeps the app's own surfaces and the
 *     controls in agreement.
 *
 *   * "light" / "dark" — forced. The windows paint their own backdrops
 *     and texts, so they honour the choice whatever the desktop runs;
 *     the PC3 controls among them keep following the Plasma theme, the
 *     way they always have.
 *
 * The dark values are the ones the windows used to hardcode; the light
 * ones are their counterparts, built from black overlays the way the
 * dark ones are built from white ones.
 */
QtObject {
    // ---- The mode ---------------------------------------------------------
    // "system", "light" or "dark". Written by the shell (the settings
    // store owns the persisted value); read by everything else.
    property string preference: "system"

    // Whether the desktop side of the resolution is the dark one.
    // Written by the shell, from the applet's Kirigami (Plasma) theme.
    property bool systemPrefersDark: false

    // The resolved mode every colour below keys off.
    readonly property bool isDark: preference === "dark"
        || (preference !== "light" && systemPrefersDark)

    // ---- Backdrops and surfaces -------------------------------------------
    // The semi-transparent backdrop the windows paint themselves on.
    // Mostly opaque either way: readable over busy desktops, with just
    // a hint of the desktop through it.
    readonly property color backgroundColor: isDark
        ? Qt.rgba(0.07, 0.07, 0.09, 0.93)
        : Qt.rgba(0.97, 0.97, 0.98, 0.96)

    // The backdrop's own outline, as the session search draws one.
    readonly property color backdropBorderColor: isDark
        ? Qt.rgba(1, 1, 1, 0.18)
        : Qt.rgba(0, 0, 0, 0.12)

    // The fill of a sub-pane on the backdrop (the search's two panes).
    readonly property color paneFill: isDark
        ? Qt.rgba(1, 1, 1, 0.04)
        : Qt.rgba(0, 0, 0, 0.03)

    // The fill of the selected row in such a pane.
    readonly property color selectionFill: isDark
        ? Qt.rgba(1, 1, 1, 0.12)
        : Qt.rgba(0, 0, 0, 0.08)

    // A user prompt's chat bubble.
    readonly property color bubbleFill: isDark
        ? Qt.rgba(1, 1, 1, 0.08)
        : Qt.rgba(0, 0, 0, 0.05)
    readonly property color bubbleBorderColor: isDark
        ? Qt.rgba(1, 1, 1, 0.14)
        : Qt.rgba(0, 0, 0, 0.10)

    // The agent's answer card.
    readonly property color cardFill: isDark
        ? Qt.rgba(1, 1, 1, 0.06)
        : Qt.rgba(0, 0, 0, 0.035)
    readonly property color cardBorderColor: isDark
        ? Qt.rgba(1, 1, 1, 0.10)
        : Qt.rgba(0, 0, 0, 0.08)

    // An attachment chip (a file on the prompt being composed).
    readonly property color chipFill: isDark
        ? Qt.rgba(1, 1, 1, 0.08)
        : Qt.rgba(0, 0, 0, 0.05)
    readonly property color chipBorderColor: isDark
        ? Qt.rgba(1, 1, 1, 0.16)
        : Qt.rgba(0, 0, 0, 0.12)

    // ---- Text -------------------------------------------------------------
    // Section headings ("API keys"), the brightest regular text.
    readonly property color headingTextColor: isDark ? "#f2f2f2" : "#1a1a21"

    // Sub-headings inside a section ("LLM (MiniMax)").
    readonly property color subheadingTextColor: isDark ? "#cfcfcf" : "#3f3f49"

    // Body text (labels, chat-bubble text).
    readonly property color textColor: isDark ? "#e8e8e8" : "#25252d"

    // The agent's answer — the one text the windows exist to show, a
    // step brighter than the rest.
    readonly property color primaryTextColor: isDark ? "#f7f7f7" : "#131318"

    // An attention state, brighter than the body text (the camera
    // button while the window picker waits).
    readonly property color strongTextColor: isDark ? "#fcfcfc" : "#0b0b0e"

    // A hovered section header, between body and secondary text.
    readonly property color hoveredTextColor: isDark ? "#d0d0d0" : "#2f2f39"

    // The small grey attributions and footnotes.
    readonly property color secondaryTextColor: isDark ? "#9a9a9a" : "#5f5f6a"

    // ---- Semantics ---------------------------------------------------------
    // Complaints, successes and warnings. The dark values are Breeze's
    // dark-theme text colours; the light ones are the darker shades a
    // light backdrop needs for the same contrast.
    readonly property color errorColor: isDark ? "#f66151" : "#c01c28"
    readonly property color successColor: isDark ? "#57e389" : "#15803d"
    readonly property color warningColor: isDark ? "#f67400" : "#b45309"
}
