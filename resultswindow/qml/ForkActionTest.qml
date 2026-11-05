// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import org.phoenixdna.resultswindow

/*
 * The results window's fork affordance, exercised on the real window.
 *
 * The window under test is the real one, loaded from the results-window
 * QML module (see ForkActionTest.qml) — not a copy of it — so what the
 * test measures is what the user sees.
 *
 * A fork branches the conversation into a new session at one of the
 * user's queries: every query carries the button that asks for it, and
 * the button asks for the query it stands next to. A run that carries no
 * query has nothing to fork at, and so no button.
 */
Window {
    id: root

    visible: true
    width: 800
    height: 600

    // The window under test. It is a toplevel, so it is shown offscreen:
    // the offscreen platform plugin renders it without a display.
    ResultsWindow {
        id: window

        visible: true
        width: 600
        height: 400

        // The conversation the window shows — the model the C++ side of
        // this test provides, in the roles the window's delegate reads.
        conversation: testConversation

        // The forks the window asks for: the exchange each one is taken
        // at.
        property var forks: []
        onForkRequested: (exchange) => root.forks.push(exchange)
    }

    // The forks the window asked for.
    property var forks: []

    // Walks the item tree for every item with the given name, in the
    // order they stand in.
    function findAllByName(item, name) {
        const found = []
        if (!item) {
            return found
        }
        if (item.objectName === name) {
            found.push(item)
            return found
        }
        const children = item.children
        for (let i = 0; i < children.length; i += 1) {
            found.push.apply(found, root.findAllByName(children[i], name))
        }
        return found
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
    // loop to turn so that the delegates exist before the next step
    // looks at them.
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
        steps = [stepEveryQueryCarriesAForkButton,
                 stepTheButtonAsksForItsOwnExchange,
                 stepTheNoticeIsShown]
        step = 0
        done = false
    }

    Component.onCompleted: start()

    // ---- The steps -----------------------------------------------------

    // Every query of the conversation carries the button that forks it.
    // The run that carries no query has none that works: there is no
    // query of the user's to fork at, so its button stays disabled.
    function stepEveryQueryCarriesAForkButton() {
        const buttons = root.findAllByName(window.contentItem, "forkButton")
        root.check(buttons.length === 3,
                   "every exchange carries a fork button (" + buttons.length + ")")
        if (buttons.length < 3) {
            return
        }
        root.check(buttons[0].enabled, "the first query can be forked at")
        root.check(buttons[1].enabled, "the second query can be forked at")
        root.check(!buttons[2].enabled,
                   "the run without a query cannot be forked at")
    }

    // The button asks for its own exchange, and not another: the fork is
    // taken at the query the button stands next to.
    function stepTheButtonAsksForItsOwnExchange() {
        const buttons = root.findAllByName(window.contentItem, "forkButton")
        if (buttons.length < 2) {
            return
        }
        root.check(root.forks.length === 0, "no fork is asked for before the click")
        buttons[0].clicked()
        root.check(root.forks.length === 1 && root.forks[0] === 1,
                   "the first button asks for the first exchange ("
                   + root.forks.join(",") + ")")
        buttons[1].clicked()
        root.check(root.forks.length === 2 && root.forks[1] === 2,
                   "the second button asks for the second exchange ("
                   + root.forks.join(",") + ")")
    }

    // The window can say what happened: the notice is what the shell
    // fills in when the daemon has made the fork.
    function stepTheNoticeIsShown() {
        window.showNotice("Forked.")
        root.check(window.notice === "Forked.", "the notice says what happened")
    }
}
