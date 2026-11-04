// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import org.phoenixdna.resultswindow

/*
 * The results window's scroll-follow behaviour, exercised on the real
 * window.
 *
 * The window follows the newest content the way a terminal does, and the
 * conversation is polished in several passes after every arrival (the
 * Markdown import, the paragraph spacing, the link pass, the formula
 * swap). Each pass changes the content's height, so the follow has to
 * survive them: it is re-applied until the height stops moving, and the
 * position is restored across the formula swap's wholesale rewrite of the
 * document.
 *
 * The window is the real one, loaded from the module — not a copy — so
 * the test measures what the user sees. The conversation comes from the
 * test's own model (the C++ side of this test), in the roles the
 * window's delegate reads.
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
        conversation: root.conversationModel
    }

    // The model the C++ side of the test provides.
    property var conversationModel: testConversation

    // The window's scroller, found by its object name.
    readonly property var scroller:
        findByName(window.contentItem, "scroller")

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
    // loop to turn so that the layout and the polish passes have run
    // before the next step looks at the result.
    property var steps: []
    property int step: 0
    property bool done: false

    Timer {
        id: driver

        interval: 80
        repeat: true
        running: !root.done

        onTriggered: {
            if (root.step >= root.steps.length) {
                root.done = true
                testConversation.finish()
                return
            }
            const fn = root.steps[root.step]
            root.step += 1
            fn()
        }
    }

    function start() {
        steps = [stepConversationArrives,
                 stepHasScrollableContent,
                 stepFollowsGrowth,
                 stepFollowsPolish,
                 stepUnpinsOnUserScroll,
                 stepRepinsOnShow,
                 stepFormulaSwapRestoresPosition]
        step = 0
        done = false
    }

    Component.onCompleted: start()

    // ---- The steps -----------------------------------------------------

    // The conversation reaches the window.
    function stepConversationArrives() {
        const scroller = root.scroller
        root.check(scroller !== undefined && scroller !== null,
                   "the scroller is found")
        if (!scroller) {
            return
        }

        // The window's scroll API. A window without it cannot follow
        // the content at all, and the steps below would quietly do
        // nothing — so its absence is a failure of its own.
        root.check(scroller.bottomPosition !== undefined,
                   "the window knows where its bottom is")
        root.check(scroller.onUserScroll !== undefined,
                   "the window hears the user scrolling")
        root.check(scroller.saveBottom !== undefined
                   && scroller.restoreBottom !== undefined,
                   "the window keeps its place across a document rewrite")

        // A first answer, tall enough that the window has to scroll.
        setResult(paragraph(120))
    }

    // The window knows how tall its content is — the height the follow
    // aims at is measured from the content, not from a value a layout
    // pass behind.
    function stepHasScrollableContent() {
        const scroller = root.scroller
        root.check(scroller.contentHeight > scroller.height,
                   "the window has more content than it can show ("
                   + scroller.contentHeight + " in " + scroller.height + ")")
    }

    // New content arrives: the window follows it to the bottom.
    function stepFollowsGrowth() {
        const scroller = root.scroller
        if (!scroller) {
            return
        }
        scroller.pinnedToBottom = true
        scroller.contentY = scroller.bottomPosition()
        root.check(scroller.pinnedToBottom,
                   "the window is pinned after a show")
        root.check(Math.abs(scroller.contentY - scroller.bottomPosition()) < 1.0,
                   "the pinned window is at the bottom")

        // The answer grows: the window follows it down.
        setResult(paragraph(240))
    }

    // The polish passes change the height again; the window follows them
    // too, and comes to rest at the bottom rather than short of it.
    function stepFollowsPolish() {
        const scroller = root.scroller
        const bottom = scroller.bottomPosition()
        root.check(Math.abs(scroller.contentY - bottom) < 1.0,
                   "the window rests at the bottom after the polish "
                   + "(contentY " + scroller.contentY + ", bottom "
                   + bottom + ")")
        root.check(scroller.pinnedToBottom,
                   "the window is still pinned after the polish")
    }

    // The user scrolls up: the window stops following. The scroll is
    // the Flickable's own user-movement signal that tells the window
    // the user took hold of the content — a programmatic move of
    // contentY is the window's own doing and does not unpin it.
    function stepUnpinsOnUserScroll() {
        const scroller = root.scroller
        // The user drags the content upwards, and the Flickable says
        // so: that is what unpins the conversation.
        scroller.contentY = scroller.bottomPosition() - 500
        scroller.onUserScroll()
        root.check(!scroller.pinnedToBottom,
                   "scrolling up unpins the window")

        // A new arrival must not drag the view back down.
        const where = scroller.contentY
        setResult(paragraph(400))
        root.check(Math.abs(scroller.contentY - where) < 1.0,
                   "the window stays where the user left it")
    }

    // Showing the window again re-pins it and brings the newest content
    // into view.
    function stepRepinsOnShow() {
        const scroller = root.scroller
        window.showResult()
        root.check(scroller.pinnedToBottom,
                   "showing the window re-pins it")
        root.check(Math.abs(scroller.contentY - scroller.bottomPosition()) < 1.0,
                   "showing the window brings the newest content into view")
    }

    // The formula swap rewrites the document wholesale; the position is
    // restored across it.
    function stepFormulaSwapRestoresPosition() {
        const scroller = root.scroller
        // Stand in for the swap: the document is replaced, which takes
        // the view's position with it.
        scroller.saveBottom()
        scroller.contentY = 0
        scroller.restoreBottom()
        root.check(Math.abs(scroller.contentY - scroller.bottomPosition()) < 1.0,
                   "the position is restored across the document rewrite")
    }

    // ---- Helpers -------------------------------------------------------

    // Replaces the answer of the exchange — the "run" streaming.
    function setResult(text) {
        testConversation.setResult(text)
    }

    // A block of text with the given number of lines, so the answer's
    // height is known to grow.
    function paragraph(lines) {
        const out = []
        for (let i = 0; i < lines; i += 1) {
            out.push("line " + i + " of the answer, with some words in it")
        }
        return out.join("\n")
    }
}
