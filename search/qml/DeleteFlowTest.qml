// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import org.phoenixdna.search

/*
 * The session search's delete flow, exercised on the real overlay.
 *
 * The overlay under test is the real one, loaded from the session-search
 * QML module (see DeleteFlowTest.qml) — not a copy of it — so what the
 * test measures is what the user sees.
 *
 * A session that is thrown away cannot be brought back, so the button
 * only asks: the confirmation names the session, and only the
 * confirmation hands the session to the daemon. The test walks that
 * round trip: the click opens the question, the question names the
 * session, declining it leaves everything alone, and accepting it asks
 * for exactly that session to be deleted.
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
                "title": "",
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

        // The sessions the user threw away: the overlay hands them to
        // the shell, which asks the daemon to delete them.
        property var deleted: []
        onSessionDeleteRequested: (sessionId) => root.deleted.push(sessionId)
    }

    // The sessions the user threw away.
    property var deleted: []

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

    // The session list of the overlay, and the delete button of the row
    // at \a index.
    function listOf(overlayItem) {
        return findByName(overlayItem.contentItem, "sessionList")
    }

    function deleteButtonOf(overlayItem, index) {
        const list = listOf(overlayItem)
        if (!list) {
            return null
        }
        const row = list.itemAtIndex(index)
        if (!row) {
            return null
        }
        return findByName(row, "deleteButton")
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
                 stepTheSecondSessionToo]
        step = 0
        done = false
    }

    Component.onCompleted: start()

    // ---- The steps -----------------------------------------------------

    // Every row of the list carries a delete button: the session's way
    // out of the history.
    function stepEveryRowCanBeThrownAway() {
        root.check(root.listOf(overlay) !== null, "the session list is found")
        root.check(root.deleteButtonOf(overlay, 0) !== null,
                   "the first row carries a delete button")
        root.check(root.deleteButtonOf(overlay, 1) !== null,
                   "the second row carries a delete button")
    }

    // The click does not delete anything: it asks.
    function stepTheClickOnlyAsks() {
        const button = root.deleteButtonOf(overlay, 0)
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
        const button = root.deleteButtonOf(overlay, 1)
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

    // The flow works for every row, not just the first two.
    function stepTheSecondSessionToo() {
        root.check(root.deleted.length === 1 && root.deleted[0] === "s2",
                   "exactly one session was thrown away")
    }
}
