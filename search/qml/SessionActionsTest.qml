// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import org.phoenixdna.search

/*
 * The session search's session actions, exercised on the real overlay.
 *
 * The overlay under test is the real one, loaded from the session-search
 * QML module (see SessionActionsTest.qml) — not a copy of it — so what the
 * test measures is what the user sees.
 *
 * A session that is thrown away cannot be brought back, so the button
 * only asks: the confirmation names the session, and only the
 * confirmation hands the session to the daemon. A session can also be
 * named the user's way, and the field for it comes up with the name the
 * session has. The test walks both round trips.
 */
Window {
    id: root

    visible: true
    width: 800
    height: 600

    // The overlay under test. It is a Plasma Dialog, so it is a window
    // of its own; the offscreen platform shows it without a display.
    SessionSearchOverlay {
        id: overlay

        visible: true
        width: 800
        height: 600

        // The sessions the overlay lists: the daemon's answers, in the
        // shape the overlay reads them.
        summaries: [
            {
                "id": "s1",
                "mode": "user",
                "title": "Disk tuning",
                "summary": "Disk tuning",
                "preview": "how much disk is left?",
                "createdAt": "2026-01-01T00:00:00+00:00",
                "updatedAt": "2026-01-01T00:00:00+00:00",
                "messageCount": 2
            },
            {
                "id": "s2",
                "mode": "user",
                "title": "",
                "summary": "Weather report",
                "preview": "what is the weather like?",
                "createdAt": "2026-01-02T00:00:00+00:00",
                "updatedAt": "2026-01-02T00:00:00+00:00",
                "messageCount": 2
            }
        ]

        // The sessions the user threw away, and the ones they named.
        property var deleted: []
        property var renamed: []
        onSessionDeleteRequested: (sessionId) => root.deleted.push(sessionId)
        onSessionRenameRequested: (sessionId, title) =>
            root.renamed.push({ "id": sessionId, "title": title })
    }

    // The sessions the user threw away, and the ones they named.
    property var deleted: []
    property var renamed: []

    // Walks the item tree for the named object.
    function findByName(item, name) {
        if (!item) {
            return null
        }
        if (item.objectName === name) {
            return item
        }
        const children = item.children
        for (let i = 0; i < children.length; i += 1) {
            const found = findByName(children[i], name)
            if (found) {
                return found
            }
        }
        return null
    }

    // The session list of the overlay, and the button of the row at
    // \a index that is named \a button.
    function listOf(overlayItem) {
        return findByName(overlayItem.contentItem, "sessionList")
    }

    function buttonOf(overlayItem, index, button) {
        const list = listOf(overlayItem)
        if (!list) {
            return null
        }
        const row = list.itemAtIndex(index)
        if (!row) {
            return null
        }
        return findByName(row, button)
    }

    // The name field of the rename question.
    function renameFieldOf(overlayItem) {
        return findByName(overlayItem.contentItem, "renameField")
    }

    property int failures: 0
    property string failure: ""

    function check(condition, message) {
        if (!condition) {
            failures += 1
            if (failure.length === 0) {
                failure = message
            }
            console.warn("FAIL: " + message)
        } else {
            console.warn("ok: " + message)
        }
    }

    // The steps, run one after the other, each waiting for the event
    // loop to turn so that the layout has run before the next step
    // looks at the result.
    property var steps: []
    property int step: 0
    property bool done: false

    Timer {
        id: driver

        interval: 60
        repeat: true
        running: !root.done

        onTriggered: {
            if (root.step >= root.steps.length) {
                root.done = true
                testControl.finish(root.failures > 0 ? 1 : 0)
                return
            }
            const fn = root.steps[root.step]
            root.step += 1
            fn()
        }
    }

    function start() {
        steps = [stepEveryRowCanBeThrownAway,
                 stepTheClickOnlyAsks,
                 stepTheQuestionNamesTheSession,
                 stepDecliningLeavesEverythingAlone,
                 stepAcceptingAsksForThatSession,
                 stepEveryRowCanBeNamed,
                 stepTheFieldComesUpWithTheName,
                 stepDecliningANameChangesNothing,
                 stepAcceptingANameAsksForIt,
                 stepAnEmptyNameIsAnAnswerToo]
        step = 0
        done = false
    }

    Component.onCompleted: start()

    // ---- The steps -----------------------------------------------------

    // Every row of the list carries a delete button and a rename
    // button: the session's way out of the history, and its own name.
    function stepEveryRowCanBeThrownAway() {
        root.check(root.listOf(overlay) !== null, "the session list is found")
        root.check(root.buttonOf(overlay, 0, "deleteButton") !== null,
                   "the first row carries a delete button")
        root.check(root.buttonOf(overlay, 1, "deleteButton") !== null,
                   "the second row carries a delete button")
    }

    // The click does not delete anything: it asks.
    function stepTheClickOnlyAsks() {
        const button = root.buttonOf(overlay, 0, "deleteButton")
        if (!button) {
            return
        }
        root.check(overlay.deleting === false,
                   "no confirmation is up before the click")
        button.clicked()
        root.check(overlay.deleting === true,
                   "the click opens the confirmation")
        root.check(root.deleted.length === 0,
                   "the click deletes nothing on its own")
    }

    // The question names the session it is about: the list is full of
    // similar names, and the wrong one must not go.
    function stepTheQuestionNamesTheSession() {
        root.check(overlay.pendingDeleteId === "s1",
                   "the confirmation is about the clicked session ("
                   + overlay.pendingDeleteId + ")")
        root.check(overlay.pendingDeleteName === "Disk tuning",
                   "the confirmation names the session ("
                   + overlay.pendingDeleteName + ")")
        const scrim = root.findByName(overlay.contentItem, "deleteScrim")
        root.check(scrim !== null && scrim.visible,
                   "the confirmation covers the window")
    }

    // Declining leaves the session where it is.
    function stepDecliningLeavesEverythingAlone() {
        overlay.cancelDelete()
        root.check(overlay.deleting === false,
                   "the confirmation goes away when it is declined")
        root.check(overlay.pendingDeleteId === "",
                   "no session is pending any more")
        root.check(root.deleted.length === 0,
                   "declining deletes nothing")
    }

    // Accepting asks for exactly that session.
    function stepAcceptingAsksForThatSession() {
        const button = root.buttonOf(overlay, 1, "deleteButton")
        if (!button) {
            return
        }
        button.clicked()
        root.check(overlay.pendingDeleteId === "s2",
                   "the second row asks about the second session")
        overlay.confirmDelete()
        root.check(root.deleted.length === 1,
                   "the confirmation asks for one session to be deleted")
        root.check(root.deleted.length === 1 && root.deleted[0] === "s2",
                   "the session asked for is the one that was clicked ("
                   + root.deleted.join(",") + ")")
        root.check(overlay.deleting === false,
                   "the confirmation goes away once it is accepted")
    }

    // Every row carries a rename button as well.
    function stepEveryRowCanBeNamed() {
        root.check(root.buttonOf(overlay, 0, "renameButton") !== null,
                   "the first row carries a rename button")
        root.check(root.buttonOf(overlay, 1, "renameButton") !== null,
                   "the second row carries a rename button")
    }

    // The field comes up with the name the session has now, so the
    // user can change it rather than write it from scratch.
    function stepTheFieldComesUpWithTheName() {
        const button = root.buttonOf(overlay, 0, "renameButton")
        if (!button) {
            return
        }
        root.check(overlay.renaming === false,
                   "no name is asked for before the click")
        button.clicked()
        root.check(overlay.renaming === true,
                   "the click asks for a name")
        root.check(overlay.pendingRenameId === "s1",
                   "the name is asked for the clicked session ("
                   + overlay.pendingRenameId + ")")
        const field = root.renameFieldOf(overlay)
        root.check(field !== null, "the name field is found")
        root.check(field !== null && field.text === "Disk tuning",
                   "the field comes up with the name the session has ("
                   + (field ? field.text : "") + ")")
    }

    // Declining a name changes nothing.
    function stepDecliningANameChangesNothing() {
        overlay.cancelRename()
        root.check(overlay.renaming === false,
                   "the question goes away when it is declined")
        root.check(root.renamed.length === 0,
                   "declining names nothing")
    }

    // Accepting asks for exactly that session, with the name that was
    // typed.
    function stepAcceptingANameAsksForIt() {
        const button = root.buttonOf(overlay, 1, "renameButton")
        const field = root.renameFieldOf(overlay)
        if (!button || !field) {
            return
        }
        button.clicked()
        root.check(overlay.pendingRenameId === "s2",
                   "the second row asks about the second session")
        // What the user types is what the overlay knows.
        field.text = "My own name"
        root.check(overlay.pendingRenameText === "My own name",
                   "the typed name is the one the overlay holds ("
                   + overlay.pendingRenameText + ")")
        overlay.confirmRename()
        root.check(root.renamed.length === 1,
                   "the question asks for one session to be named")
        root.check(root.renamed.length === 1
                   && root.renamed[0].id === "s2"
                   && root.renamed[0].title === "My own name",
                   "the session and the name asked for are the ones that "
                   + "were typed")
        root.check(overlay.renaming === false,
                   "the question goes away once it is accepted")
    }

    // An empty name is an answer too: the session goes back to the
    // name the model gives it.
    function stepAnEmptyNameIsAnAnswerToo() {
        const button = root.buttonOf(overlay, 0, "renameButton")
        const field = root.renameFieldOf(overlay)
        if (!button || !field) {
            return
        }
        button.clicked()
        field.text = ""
        root.check(overlay.pendingRenameText === "",
                   "the empty name is what the overlay holds")
        overlay.confirmRename()
        root.check(root.renamed.length === 2,
                   "the empty name is asked for as well")
        root.check(root.renamed.length === 2
                   && root.renamed[1].id === "s1"
                   && root.renamed[1].title === "",
                   "the session asked for is the one that was clicked, and "
                   + "the name asked for is the empty one")
    }
}
