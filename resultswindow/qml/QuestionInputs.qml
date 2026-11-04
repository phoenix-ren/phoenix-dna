// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The inputs of an ask_user question: one choice per option (radio
 * buttons, or checkboxes when several may be picked), plus a final
 * "write your own answer" choice that carries a text field and behaves
 * like any other option - a radio of its own, picked by clicking it or
 * simply by typing. The whole row of an option is its tap target; the
 * choices are exclusive (single-choice questions).
 *
 * Shared by the results window's inline question card and the wizard.
 * The state - picked options, own-words - belongs to the question, not
 * the component: the next question starts with a clean slate, and a
 * walker (the wizard) snapshots and restores it with currentState() /
 * setState() as it moves between questions.
 */
ColumnLayout {
    id: inputs

    // The question map whose options are rendered: at least
    // {question, options: [{label, description?}]}; a true `multiple`
    // chooses checkboxes over radio buttons.
    property var question: ({})

    // Whether the own submit row shows. The inline card uses it; the
    // wizard hides it and provides Back / Next / Submit chrome instead
    // (its submit sends every question's answers at once).
    property bool showSubmit: true

    // Emitted when the user answers: the 0-based indices of the chosen
    // options (may be empty) and their own words (may be empty).
    signal submitted(var selected, string text)

    // The options picked so far, as 0-based indices, and whether the
    // own-answer choice is the active one.
    property var checkedOptions: []
    property bool ownSelected: false

    onQuestionChanged: {
        checkedOptions = [];
        ownSelected = false;
        ownAnswer.text = "";
    }

    readonly property bool multiple: question.multiple === true

    // An answer exists: an option is picked, or the own-answer choice
    // carries words. The walker keeps Next/Submit disabled until this
    // is true, so every question leaves with an answer.
    readonly property bool canSend: checkedOptions.length > 0
        || (ownSelected && ownAnswer.text.trim().length > 0)

    function pick(index) {
        checkedOptions = [index];
        ownSelected = false;
    }

    function toggle(index) {
        const next = checkedOptions.slice();
        const at = next.indexOf(index);
        if (at === -1) {
            next.push(index);
        } else {
            next.splice(at, 1);
        }
        checkedOptions = next;
    }

    function pickOwn() {
        ownSelected = true;
        checkedOptions = [];
        ownAnswer.forceActiveFocus();
    }

    function submit() {
        submitted(ownSelected ? [] : checkedOptions, ownAnswer.text);
    }

    // The wizard navigates between questions: it snapshots the inputs
    // when leaving a question (picked options, own-answer choice, own
    // words) and restores them when coming back - the back button must
    // not eat an answer the user already gave.
    function currentState() {
        return {
            "selected": checkedOptions,
            "ownSelected": ownSelected,
            "text": ownAnswer.text,
        };
    }

    function setState(state) {
        checkedOptions = (state && state.selected) || [];
        ownSelected = (state && state.ownSelected) || false;
        ownAnswer.text = (state && state.text) || "";
    }

    Repeater {
        id: optionsRepeater

        model: inputs.question.options ?? []

        delegate: RowLayout {
            id: optionRow

            required property var modelData
            required property int index

            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            readonly property bool chosen:
                inputs.checkedOptions.includes(optionRow.index)

            function activate() {
                if (inputs.multiple) {
                    inputs.toggle(optionRow.index);
                } else {
                    inputs.pick(optionRow.index);
                }
            }

            // The whole row is the choice's tap target - indicator,
            // label and description alike: an option is a row in the
            // implicit grid the radios form, not a small circle to hit.
            TapHandler {
                onTapped: optionRow.activate()
            }

            // Bare indicators: the label beside them carries the
            // window's fixed palette (theme label colors would be
            // unreadable on the dark backdrop under a light theme). Both
            // controls reflect the card's state only - Qt's own button
            // grouping would not reach across the delegates.
            PC3.RadioButton {
                visible: !inputs.multiple
                checked: optionRow.chosen
                onClicked: inputs.pick(optionRow.index)
            }

            PC3.CheckBox {
                visible: inputs.multiple
                checked: optionRow.chosen
                onClicked: inputs.toggle(optionRow.index)
            }

            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true

                Text {
                    Layout.fillWidth: true

                    text: optionRow.modelData.label
                    wrapMode: Text.WordWrap
                    color: PhoenixTheme.textColor
                }

                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin:
                        Kirigami.Units.gridUnit + Kirigami.Units.smallSpacing

                    visible: (optionRow.modelData.description ?? "").length > 0
                    text: optionRow.modelData.description ?? ""
                    wrapMode: Text.WordWrap
                    color: PhoenixTheme.secondaryTextColor
                }
            }
        }
    }

    // The own-answer choice: a full option row of its own - radio plus
    // the words - picked by clicking it or simply by typing. Picking a
    // listed option puts it down again.
    RowLayout {
        id: ownRow

        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        PC3.RadioButton {
            objectName: "ownAnswerRadio"

            checked: inputs.ownSelected
            onClicked: inputs.pickOwn()

            PC3.ToolTip {
                text: i18n("Write your own answer")
            }
        }

        PC3.TextField {
            id: ownAnswer

            objectName: "ownAnswerField"

            Layout.fillWidth: true

            placeholderText: i18n("…or write your own answer")

            // Typing is choosing: the radio goes on the moment words
            // arrive, so the answer's shape is visible while it is
            // written.
            onTextChanged: if (text.trim().length > 0 && !inputs.ownSelected) {
                inputs.pickOwn();
            }

            onAccepted: if (inputs.canSend) {
                inputs.submit();
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing
        visible: inputs.showSubmit

        PC3.Button {
            objectName: "answerButton"

            Layout.alignment: Qt.AlignRight
            icon.name: "document-send"
            text: i18n("Send answer")
            enabled: inputs.canSend
            onClicked: inputs.submit()
        }
    }
}
