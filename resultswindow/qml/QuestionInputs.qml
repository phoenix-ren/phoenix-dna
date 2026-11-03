// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The inputs of an ask_user question: one choice per option (radio
 * buttons, or checkboxes when several may be picked), a field for the
 * user's own words, and the send that submits either.
 *
 * Shared by the results window's inline question card and its wizard
 * view. The state — picked options, typed words — belongs to the
 * question, not the component: the next question starts with a clean
 * slate.
 */
ColumnLayout {
    id: inputs

    // The question map whose options are rendered: at least
    // {question, options: [{label, description?}]}; a true `multiple`
    // chooses checkboxes over radio buttons.
    property var question: ({})

    // Whether the "skip the rest" escape hatch shows. The wizard offers
    // it; a lone question times out instead.
    property bool showSkip: false

    // Emitted when the user answers: the 0-based indices of the chosen
    // options (may be empty) and their own words (may be empty). The
    // skip button submits with a text that tells the model to proceed
    // on its best judgment — in English on purpose, since the model
    // reads it, not the user.
    signal submitted(var selected, string text)

    // The options picked so far, as 0-based indices.
    property var checkedOptions: []

    onQuestionChanged: {
        checkedOptions = [];
        ownAnswer.text = "";
    }

    readonly property bool multiple: question.multiple === true
    readonly property bool canSend: checkedOptions.length > 0
        || ownAnswer.text.trim().length > 0

    function pick(index) {
        checkedOptions = [index];
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

    function submit() {
        submitted(checkedOptions, ownAnswer.text);
    }

    function skip() {
        submitted([], "skip the remaining questions and proceed with your best judgment");
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
            readonly property bool multiple: inputs.multiple

            function activate() {
                if (optionRow.multiple) {
                    inputs.toggle(optionRow.index);
                } else {
                    inputs.pick(optionRow.index);
                }
            }

            // Bare indicators: the label beside them carries the
            // window's fixed palette (theme label colors would be
            // unreadable on the dark backdrop under a light theme). Both
            // controls reflect the card's state only — Qt's own button
            // grouping would not reach across the delegates.
            PC3.RadioButton {
                visible: !optionRow.multiple
                checked: optionRow.chosen
                onClicked: inputs.pick(optionRow.index)
            }

            PC3.CheckBox {
                visible: optionRow.multiple
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

                    // The label chooses like the indicator does: a
                    // bigger tap target.
                    TapHandler {
                        onTapped: optionRow.activate()
                    }
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

    RowLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing

        // The user's own words: always offered (the model is told never
        // to add an "other" option), and enough on their own. Enter
        // sends, like Shift+Enter sends a follow-up.
        PC3.TextField {
            id: ownAnswer

            objectName: "ownAnswerField"

            Layout.fillWidth: true

            placeholderText: i18n("…or write your own answer")

            onAccepted: if (inputs.canSend) {
                inputs.submit();
            }
        }

        PC3.Button {
            objectName: "answerButton"

            Layout.alignment: Qt.AlignRight
            icon.name: "document-send"
            text: i18n("Send answer")
            enabled: inputs.canSend
            onClicked: inputs.submit()
        }

        // The escape hatch: answers with a skip text the model reads,
        // so it stops asking and proceeds with its best judgment.
        PC3.Button {
            objectName: "skipButton"

            visible: inputs.showSkip
            icon.name: "media-skip-forward"
            text: i18n("Skip the rest")
            onClicked: inputs.skip()
        }
    }
}
