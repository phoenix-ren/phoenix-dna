// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

pragma Singleton

import QtQuick
import org.kde.kirigami as Kirigami

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
 *
 * The font of the agent's answers lives here too, for the same reason:
 * one place decides what the agent's own words look like in either
 * mode, and every window that shows an answer reads it from here.
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

    // The outline a prompt field draws while the keyboard is in it: one
    // accent, keyed to the mode so it reads on either backdrop (the
    // fields paint their own themed ground, so the desktop's highlight
    // colour is not consulted).
    readonly property color focusBorderColor: isDark ? "#3daee9" : "#1793d1"

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

    // ---- The agent's font --------------------------------------------------
    // The font the agent's own words are set in — the answers and the
    // notes it thinks in. The chrome around them (labels, buttons,
    // tooltips) keeps the system's font: the voice of the agent is the
    // one thing in the window that is set apart.
    //
    // Two families, one per mode: the light mode is meant to be read
    // like a news article, the dark one like a screen that never sleeps.
    // The two are chosen independently of each other and of the scheme
    // above — whether the desktop is dark or light decides which of
    // them applies, nothing else does.
    //
    // Written by the shell (the settings store owns the persisted
    // values). A family is one of three things: the empty string, the
    // app's own default for the mode; the word "system", the system's
    // own font; or a family by name. A size of zero is the app's own
    // size for the mode.
    property string lightFont: ""
    property string darkFont: ""
    property int lightFontSize: 0
    property int darkFontSize: 0

    // The word that stands for the system's own font among the three a
    // family can be: the settings window offers it as one row of the
    // list, the store writes it into keys.json, and the resolution
    // below reads it back.
    readonly property string systemFontChoice: "system"

    // The app's own defaults for the answers: the face and the size
    // they are set in when the user has chosen nothing for the mode.
    //
    // The light mode gets a reading face and the size such a face
    // needs — Literata was drawn for long-form reading on a screen,
    // and a serif at the app's own size is too small to read. The dark
    // one gets a screen face at the app's own size, which is what a
    // screen face is drawn for; on a dark ground a serif is tiring.
    //
    // Both are fallbacks: a system without them installed keeps the
    // system's own font.
    readonly property string defaultLightFont: "Literata"
    readonly property string defaultDarkFont: "Inter"
    readonly property int defaultLightFontSize:
        Math.max(17, Math.round(appTextSize * 1.35))
    readonly property int defaultDarkFontSize:
        Math.round(appTextSize * 1.12)

    // The system's own font family, as the desktop's theme names it.
    // Kirigami's theme follows the Plasma theme, so this changes with
    // it; the generic fallback covers a desktop that names no family
    // of its own.
    readonly property string systemFontFamily: {
        const family = Kirigami.Theme.defaultFont.family
        return family.length > 0 ? family : "Sans Serif"
    }

    // The families this system has, as the font database names them.
    readonly property var installedFamilies: Qt.fontFamilies()

    // The app's own text size, in pixels, as the app font names it.
    // The app font carries either a pixel size or a point size, and
    // only the first of the two can be used as one.
    readonly property int appFontSize: Qt.application.font.pixelSize

    // The app's own text size in pixels, whether the app font names it
    // in pixels or in points: the desktop's theme is the fallback for
    // the latter, and a plain 12 the last one. This is what the
    // settings window shows as the size the answers would be set in.
    readonly property int appTextSize: {
        if (appFontSize > 0) {
            return appFontSize
        }
        const themed = Kirigami.Theme.defaultFont.pixelSize
        return themed > 0 ? themed : 12
    }

    // The family the agent's answers are set in right now: the one for
    // the mode the windows currently run in.
    //
    // The family a mode's answers are set in is one of three things:
    // the one chosen for it, the system's own font when that is what
    // was chosen, or the app's own default for the mode when nothing
    // was chosen.
    readonly property string answerFontFamily: isDark
        ? darkAnswerFontFamily
        : lightAnswerFontFamily

    // The same family for each mode, for the settings window to show
    // what the answers would be set in.
    readonly property string lightAnswerFontFamily:
        resolveAnswerFont(lightFont, defaultLightFont)
    readonly property string darkAnswerFontFamily:
        resolveAnswerFont(darkFont, defaultDarkFont)

    // The family a mode's answers are set in, given what was chosen
    // for the mode and the app's default for it.
    function resolveAnswerFont(chosen, fallback) {
        if (chosen === systemFontChoice) {
            return systemFontFamily
        }
        if (chosen.length > 0) {
            return chosen
        }
        // The app's default, if this system has it; the system's own
        // font, which every system has, if it does not.
        return installedFamilies.indexOf(fallback) >= 0
            ? fallback
            : systemFontFamily
    }

    // The size the answers are set in right now, in pixels: the one
    // chosen for the mode the windows currently run in, or the app's
    // own size for it.
    readonly property int answerFontSize: isDark
        ? darkAnswerFontSize
        : lightAnswerFontSize

    // The same size for a mode, for the settings window to show what
    // the answers would be set in.
    readonly property int lightAnswerFontSize: lightFontSize > 0
        ? lightFontSize
        : defaultLightFontSize
    readonly property int darkAnswerFontSize: darkFontSize > 0
        ? darkFontSize
        : defaultDarkFontSize

    // The notes the agent thinks in: smaller than the answer, since
    // they are context and not the result.
    readonly property int notesFontSize:
        Math.round(answerFontSize * 0.76)

    // Whether the answers are set in a serif face.
    //
    // Qt's font database names no style hint for any family on Linux:
    // every one of them comes back as "any style". So the theme keeps
    // its own list of the reading faces — the families whose very name
    // says so, and the ones whose name does not.
    readonly property bool answerIsSerif: isSerifFamily(answerFontFamily)

    function isSerifFamily(family) {
        if (family.length === 0) {
            return false
        }
        // A name that says "Sans Serif" says the opposite of what the
        // name "Serif" says — and the generic family is named exactly
        // that.
        if (family.indexOf("Sans Serif") >= 0
                || family.indexOf("Sans-Serif") >= 0) {
            return false
        }
        if (family.indexOf("Serif") >= 0) {
            return true
        }
        return serifFamilies.indexOf(family) >= 0
    }

    readonly property var serifFamilies: [
        "Bitstream Charter", "Charis SIL", "Charter", "Crimson Pro",
        "Crimson Text", "EB Garamond", "Gentium Book Basic",
        "IBM Plex Serif", "Libertinus Serif", "Libre Baskerville",
        "Linux Libertine", "Literata", "Newsreader", "Newsreader 16pt",
        "P052", "Spectral", "TeX Gyre Pagella", "TeX Gyre Schola",
        "TeX Gyre Termes", "URW Bookman", "Z003"
    ]

    // The gap between two paragraphs of the answer: a line and a
    // quarter of the answer's own size for a sans body, a line and a
    // half for a serif one — a serif has taller ascenders and
    // descenders, and the eye needs the extra room to find the line
    // below.
    readonly property int answerParagraphSpacing:
        Math.round(answerFontSize * (answerIsSerif ? 1.45 : 1.25))
}
