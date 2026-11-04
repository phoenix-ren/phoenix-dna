// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami

// The app's own icon set: Lucide's drawings, inked in the colour of the
// window they stand in.
import org.phoenixdna.theme

/*
 * The shortcut capture button of the settings window.
 *
 * Click it and press the combination that should own the hotkey: a combo
 * with a regular key is committed the moment the key goes down; a
 * modifiers-only combination (the screenshot picker's "Meta+Alt" style)
 * is committed when the last modifier comes back up without a regular key
 * in between — held modifiers alone can never decide the binding, or the
 * first one pressed would.
 *
 * Escape cancels the capture; Backspace or Delete clears the binding.
 * Combinations the system grabs globally (already-bound ones, plain Meta
 * on Plasma) never reach the window, so nothing happens for them — the
 * button keeps listening until Escape.
 *
 * The button is purely presentational plus capture: what to do with a
 * captured combination is the window's business (sequenceCaptured), as is
 * the binding it shows (sequenceText).
 */
PC3.Button {
    id: control

    // The binding to show, in native notation ("" when none is bound).
    property string sequenceText: ""

    // Emitted with the captured combination; key 0 means modifiers-only.
    signal sequenceCaptured(int key, int modifiers)

    // Emitted when the binding should be removed.
    signal sequenceCleared()

    readonly property bool listening: internal.listening

    // Wide enough for "Type the new shortcut…" plus the icon.
    implicitWidth: Math.max(implicitContentWidth + 2 * Kirigami.Units.gridUnit,
                            Kirigami.Units.gridUnit * 12)

    text: internal.listening
          ? i18n("Type the new shortcut…")
          : (sequenceText.length > 0 ? sequenceText : i18n("None"))
    icon.source: LucideIcons.url("keyboard", PhoenixTheme.textColor)

    // Recording state: the highlight asks for the keys.
    highlighted: internal.listening

    onClicked: startListening()

    function startListening() {
        internal.listening = true
        internal.heldModifiers = 0
        internal.maxModifiers = 0
        forceActiveFocus()
    }

    function stopListening() {
        internal.listening = false
    }

    QtObject {
        id: internal

        property bool listening: false

        // The modifier keys currently held, and the widest set held since
        // the capture began — the latter is what a modifiers-only
        // combination is built from.
        property int heldModifiers: 0
        property int maxModifiers: 0

        // Only the four modifiers KGlobalAccel serializes; keypad and
        // group-switch bits would corrupt the sequence.
        function relevant(modifiers) {
            return modifiers & (Qt.ShiftModifier | Qt.ControlModifier
                                | Qt.AltModifier | Qt.MetaModifier)
        }

        // The modifier bit a pressed modifier key contributes.
        function modifierBit(key) {
            switch (key) {
                case Qt.Key_Shift: return Qt.ShiftModifier
                case Qt.Key_Control: return Qt.ControlModifier
                case Qt.Key_Alt: return Qt.AltModifier
                case Qt.Key_Meta: return Qt.MetaModifier
                default: return 0
            }
        }
    }

    Keys.onPressed: (event) => {
        if (!internal.listening) {
            return
        }
        // Everything is ours while listening: the window's Escape-to-close
        // must not fire, and Space must not "click" the focused button.
        event.accepted = true

        if (event.key === Qt.Key_Escape) {
            stopListening()
            return
        }
        if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete) {
            stopListening()
            control.sequenceCleared()
            return
        }
        // Tab stays keyboard navigation — binding it would trap it.
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            return
        }
        // A held key repeating is not a fresh combination.
        if (event.isAutoRepeat) {
            return
        }

        const bit = internal.modifierBit(event.key)
        if (bit !== 0) {
            // A modifier went down: remember it and wait — either a
            // regular key completes the combination, or the release of
            // the last modifier commits the modifiers-only one (below).
            internal.heldModifiers |= bit
            internal.maxModifiers |= internal.relevant(event.modifiers)
            return
        }
        if (event.key === 0 || event.key === Qt.Key_unknown) {
            return
        }

        stopListening()
        control.sequenceCaptured(event.key, internal.relevant(event.modifiers))
    }

    Keys.onReleased: (event) => {
        if (!internal.listening) {
            event.accepted = false
            return
        }
        event.accepted = true

        const bit = internal.modifierBit(event.key)
        if (bit === 0) {
            return
        }
        internal.heldModifiers &= ~bit
        if (internal.heldModifiers === 0 && internal.maxModifiers !== 0) {
            // The last modifier came up without a regular key in between:
            // a modifiers-only combination, wide as the most that was
            // held (pressing both, releasing both spells "Meta+Alt").
            stopListening()
            control.sequenceCaptured(0, internal.maxModifiers)
        }
    }

    // Losing the focus (window deactivated, another control clicked) ends
    // the capture without a binding rather than leaving the button in its
    // recording state forever.
    onActiveFocusChanged: if (!activeFocus && internal.listening) {
        stopListening()
    }
}
