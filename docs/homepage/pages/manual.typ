// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "Manual — Phoenix DNA")
#import "/layout.typ": *
#show: page.with(title: "Manual", subtitle: "Every surface, every button, every shortcut — what the agent does, and where you change it.", active: "manual")

Phoenix DNA is made of six surfaces. Only one of them has to be added by
hand; the rest open by themselves when there is something to say. This
manual walks each of them in the order a conversation goes, names the
thing you are looking at, and says where to change it.

= The six surfaces

#table(
  columns: (1.2fr, 1.5fr, 2.2fr),
  table.header([Surface], [Opened by], [What it is]),
  [Panel widget], [Right-click the panel → *Add widgets* → *Phoenix DNA*], [Five buttons and a live waveform, in your panel],
  [Dictation overlay], [The record button, or *Type a query*], [The transcript surface: what was heard, what you typed, what is attached],
  [Conversation window], [By itself, when the agent starts working], [The answer, the trail of tools and thoughts, the follow-up bar],
  [Wizard window], [By itself, when the agent needs an answer], [One question at a time, with a progress bar and a Back button],
  [Session search], [The search button], [Your past conversations, by name and full text],
  [Settings window], [The gear button], [Appearance, API keys, the voice, the global shortcuts],
)

Everything but the panel is themed by the app itself and follows the
colour scheme you pick in the settings; the panel widget always keeps
matching the panel it lives in.

= The panel widget

Five icon buttons, with the live waveform drawn between the first two of
them. There is no text in the panel: each button names itself in its
tooltip, and the tooltip carries the *live* keybinding — rebind it and
the tooltip follows.

#table(
  columns: (1fr, 1.1fr, 1fr, 2fr),
  table.header([Button], [Icon], [Default shortcut], [What it does]),
  [Type a query], [`keyboard`], [`Meta+Alt+Return`], [Opens the typed-prompt window. It is a toggle: press it again and the window goes away — and your draft goes with it, unless you press *Esc* first],
  [Record / Stop], [`mic-audio-lines`, then `circle-stop`], [`Meta+Alt+Space`], [Starts a dictation. Pressing it again stops the recording and sends the clip to the speech-to-text endpoint. This button starts a *new* session; the record button in the conversation window continues the one on screen],
  [Screenshot a window], [`camera`], [`Meta+Alt`], [Starts KWin's window picker: the pointer is followed, the window under it is named in the panel's status line, and the window you click is photographed — the click never reaches the window itself],
  [Search sessions], [`search`], [`Meta+Alt+grave`], [Opens the session search. A toggle: press it again and the search goes away],
  [Settings], [`settings`], [—], [Opens the settings window. A toggle, too. The tooltip is *Settings — API keys and global shortcuts*],
)

The default search binding is `Meta+Alt` followed by the grave accent key.

A few details worth knowing:

- *The record button is the only one that opens the microphone.* Nothing
  else listens, and nothing listens while the agent is talking — pressing
  record while it reads an answer aloud stops the voice first, so the two
  never talk over each other.
- *The keyboard button greys out* while the microphone is open: the
  recording owns the prompt.
- *The camera button brightens* while the picker waits for a click.
- *The widget never shrinks below about 1.5 cm of real panel width*, so
  the waveform and the record button are never clipped at the panel's
  edge.

== The full representation

Clicking the widget's handle (or *Show Full Representation* in Plasma's
own widget menu) opens a popup with the very same row — the same five
buttons, the same waveform, the same tooltips. The only difference is
room: the popup is wider, so the waveform has space to breathe.

There is no Phoenix DNA context menu of its own. Plasma's stock widget
menu is what you get.

== The waveform

The waveform is the audio envelope of the last three quarters of a
second — a scrolling bar chart, the way a sound editor draws it. It is
dim while idle and picks up the Plasma highlight colour while recording.
Only the envelope is drawn, never the raw samples, and the bars are
scaled against the loudest moment of the recording, so quiet speech is
still visible.

The waveform is part of the record button: hovering it lights the button
frame, and clicking it starts the recording exactly as clicking the
button does.

= Asking the agent

== Dictation

Press the record button (or its shortcut) and the dictation overlay comes
up under the panel widget: a frameless window with the heading
*Listening…*, a spinner while the clip is being transcribed, and the
transcript in a field.

- *The transcript is editable.* The recognition is a first draft, not a
  verdict — fix the wording before you send it.
- *Emptying the field means "throw the recording away".* What you type
  in its place is a fresh query, not the recording's prompt.
- *The finished transcript is copied to the clipboard*, so you can paste
  it into the prompt box of another agent. Phoenix DNA does not mind
  being the typist for a lesser rival.
- *The waveform is in the panel, not in the overlay.* The overlay shows
  the words.

The clip is transcribed by whichever endpoint you named in the settings —
the same multipart request for MiniMax, OpenAI, Groq, OpenRouter or your
own server. The transcriber re-reads its configuration for every clip,
so a changed endpoint or key applies to the very next word you say.

If the conversation window is already open, no overlay appears: the
window's own follow-up bar becomes the transcript surface, and the
recording continues the conversation you are looking at.

== The typed prompt

The same overlay in keyboard mode, with the heading *Type a query* and
the placeholder *Type your query…*. It opens with the keyboard button, or
with `Meta+Alt+Return` from anywhere.

- *Return sends*, `Shift+Return` is a line break.
- *Esc throws the text away.* It is the only way to lose it: hiding the
  overlay with the toggle keeps the draft, so a mis-click costs nothing.
- A typed prompt *continues the conversation on screen* when the
  conversation window is up, and starts a new session when it is not.

== Files

The *+* button opens a file picker (*Attach files to the prompt*). The
picker offers *Images and documents* — PNG, JPEG, GIF, WebP, BMP, TIFF,
PDF, TXT, Markdown, CSV, log, JSON, XML, YAML — and *All files*.

Every picked file becomes a chip above the prompt: a paperclip, the file
name, and a small *x* to remove it again. The files travel with the
prompt to the daemon, which copies them into its own attachment
directory, where the agent's tools can read them.

Images are shown to the model directly — when the model can see them.
A text-only model is told the image's name and path instead, which is
the truth rather than a broken picture. A PDF's text is extracted with
`pdftotext` when it is installed, and the extracted text is what the
model reads.

== Screenshots

The camera button (or `Meta+Alt`) starts KWin's window picker. The
pointer is taken over, the window under it is named in the panel's status
line, and the window you click is photographed. The click never reaches
the window itself, so you do not have to be careful where you aim.

The picture is attached to the prompt you were writing, like any other
file, and named after the window it shows. If the picker fails or you
change your mind, the reason comes back in the overlay's status line.

Screenshots need KWin — it is where the pointer is followed and the
picture is taken.

= The conversation window

This is the window the agent works in, and it opens by itself the moment
a run starts. It is a real window: you can move it, resize it, and leave
it open on a second monitor.

- *The title* is the session's name — *Fix the wifi driver — Phoenix
  DNA* — or *Conversation — Phoenix DNA* while the session has none yet.
- *It stays on top of other windows*, because an answer that announces
  itself is worth the interruption.
- *Esc hides it*; it does not destroy it. The next run brings it back
  where the compositor had it. Its size is remembered between sessions.
- *The window is themed by the app*, not by Plasma: the agent's answer
  is set in a face of your choosing (see the settings), and the chrome
  around it keeps the system's font.

== Your message

Your prompt sits on the right, in a bubble with a tail, and your login
name is under the tail. The bubble wraps at four fifths of the width, so
a long question never runs the width of the window.

== Forking the conversation

Sometimes the answer is a dead end, and the way out is the same question
asked differently — or asked earlier, before the conversation went off
the rails.

Move the pointer over one of your own messages, and a little branch
button appears beside your name. It forks the conversation there: the
fork holds everything up to that message — the message itself and the
answer that followed it — and what came after stays where it was. The
session it was forked from is not touched — the two grow apart, and the
original keeps its own history.

- *The fork is a session of its own.* The follow-ups typed after it
  continue the fork, not the conversation it came from. The window says
  so, under the input bar.
- *The fork is listed in the session search* under its own name: the
  name of the session it was forked from, marked as a fork, until the
  daemon names it after what was said in it.
- *While the agent works*, the button is greyed out: a conversation
  that is still being written cannot be forked yet.

== The trail: what the agent did

Between your message and the answer, the window shows the trail: every
step the agent took, each one a header you can click.

- *A thought* — *Used 3 Thought* — the model's own reasoning, folded by
  default, in a dimmer face. Open it and you read what it was thinking;
  folded, it costs nothing to display.
- *A tool* — *Used 2 bash*, *Used 1 read* — the commands it ran, folded
  by default too. A step that failed turns its title red.
- *A transfer* — *Downloading…* while it runs, *Downloaded* when it is
  over (and *Uploading…* / *Uploaded* the other way round) — the files
  it moved over the network, titled by what it is doing rather than by
  the tool's name.
- *A spinner* while the step is still running, and *the elapsed time*
  once it is done. A step that took less than a second shows no time at
  all: it was quick, and it is over.

Opening a tool step shows the command that was run and its output, with
a tick for success and a cross for an error. Very long output is elided
in the middle, and the elision says how much of it there was.

Everything the agent says — the answer, the reasoning notes, the tool
log, your own prompt — is selectable, copyable, and scanned for
addresses: files, folders, web pages, mailboxes and phone numbers all
become links that the desktop opens the right way.

== The answer

The answer is plain text on the window's own background — there is no
card around it, because the answer *is* the window's text.

- *Markdown* is rendered: headings, lists, tables, code.
- *LaTeX formulas* are rendered to PDF pages and shown as pictures, when
  a TeX engine is installed (`texlive-xetex`). Without one they stay
  verbatim — deliberately: a mangled formula is worse than an honest
  one. If the compile fails, a line under the answer says *Math not
  rendered: …* and the verbatim formula stays.
- *The reading font is yours to choose* — a family and a size for the
  light mode and another pair for the dark one, set in the settings.

Under the answer sits a small attribution row: *Agent*, and a
megaphone button that reads that answer aloud.

== The meters

Three meters tell you what the agent is doing, drawn as rows of LEDs
that fill up like a level meter in a sound editor.

- The *CPU meter* sits on the running tool: *CPU*, the bar, the
  percentage and the number of cores it uses (`×2.4`). It is the
  answer to "is that command still alive?" — a command that has been
  going for five minutes proves it with a meter.
- The *wire meter* is two bars, *OUT* and *IN*, in bytes per second: the
  pace of the conversation with the model, each way.
- The *transfer meter* sits on the running download or upload: a bar
  labelled *DL* or *UL* for the pace of the bytes, and a progress bar
  with the count — `1.2 GB of 4.4 GB (27%)` — for how much of the thing
  is here. When the far end does not say how big the thing is, the bar
  travels instead of filling, and the count stands alone. A download
  the agent runs is a download you can watch.

The meters fade in while a step runs and fade out when it is done. The
CPU meter turns amber past 60 % and red past 85 %.

== Follow-ups

At the bottom of the window is the follow-up bar — the same field the
dictation overlay uses, so a conversation can be continued by voice or
by keyboard without changing windows.

- *Your files* sit above it as chips.
- *The field* is one rounded box holding the text and the controls
  inside it: the record button (which continues *this* session), the
  *+* to attach a file, the camera, and the send arrow at the right.
- *Return sends*, `Shift+Return` is a line break.
- *While the agent works*, the send button becomes a stop button — one
  control, two meanings, always in the same place.
- *A red line under the field* says why a clip could not be
  transcribed: no key, no network, no quota.

== Reading it aloud

The megaphone button under an answer reads that answer aloud. Read-aloud
is off by default and configured in the settings.

- The answer is *normalized for the ear*: mathematics is spoken as
  mathematics, code fences and URLs are skipped.
- *You can interrupt it* at any moment: the stop button in the follow-up
  bar, closing the window, `Esc`, or pressing record — the microphone
  barges in over the voice.
- *Two read modes.* *After the answer is complete* (the default) speaks
  only the final answer, so a multi-turn run never reads its own
  preambles aloud. *While it is generated* starts sooner, holding back
  one sentence — but a sentence that is later retracted cannot be
  unsaid. That is the trade the mode makes.
- *The speed slider* time-stretches the audio with the pitch kept, so
  the voice stays the voice at every speed.

= The wizard

Sometimes the agent cannot decide for you: which disk, which format,
which of two accounts. Then it asks — and the question arrives as a
*wizard*: a window of its own, centred on the screen, taking the
keyboard while you answer.

== One question

A single question does not need a wizard. It arrives as a card inside
the conversation window, under the conversation: the question in bold, a
row of radio buttons with the options the model chose, a
*…or write your own answer* field, and a *Send answer* button.

Nothing is sent until you press *Send answer*, and the agent waits: the
card says *The agent is waiting for this answer before it continues.*

== Several questions

Two or more questions open the wizard window — a large, frameless,
always-on-top window, centred, at least 700 by 560 pixels.

- *The title* is the form's goal — what the questions collect. Without
  one, the wizard is titled *A few questions first*.
- *The progress bar* is exact: the whole form arrived at once, so the
  count is real. *Question 2 of 3*, and the bar stands full on the last
  question.
- *The answered questions stay on screen*, dimmed, with a tick and your
  answer beside them.
- *Next is disabled until the question is answered.* A question cannot
  be left behind.
- *Back revisits.* Your answers are kept: going back and forward never
  eats one.
- *Submit answers sends every answer at once.* Nothing reaches the
  agent before that.

Underneath, the wizard says where you are: *Nothing is sent until you
submit; Back revisits your answers.* — and on the last question,
*Everything is sent together when you submit; the agent is waiting.*

== Esc, and getting the wizard back

`Esc` puts the wizard away. It does not throw the questions away: the
agent keeps waiting, and the conversation window shows a card with a
*Show questions* button that brings the wizard straight back, with every
answer still in place. The wizard's own footer says so:
*Esc hides this window; the agent keeps waiting for your answers.*

A refused submission — one the daemon does not accept — hands the very
same form back, answers and all, rather than starting over.

== The questions themselves

The options are radio buttons, one per choice, and the whole row of an
option is its tap target: the indicator, the label and the description
under it. When a question allows several answers, the indicators become
checkboxes.

Every question ends with *…or write your own answer* — a radio button
of its own and a text field beside it. Typing in the field picks it, so
the shape of your answer is visible while you write it. Picking a listed
option puts the own-answer radio down again.

The model is told to put every question it already knows into a single
call, to give each question two to six options, and — when it has a
recommendation — to put that option first and mark it *(recommended)*. It
is also told never to add an *other* option of its own: you can always
write your own answer.

== When you do not answer

A question waits five minutes. After that the agent is told that nobody
answered, and it carries on with its best judgment — saying in its final
answer which assumption it made. An unanswered question is not a hung
run.

A destructive command's confirmation works the other way round: it fails
closed, so a timeout *denies* the command. Nothing destructive happens
while you are away.

== Allow or deny

Not every question is the model's. A *destructive root command* — a
recursive forced delete, a filesystem format, a raw write to a device, a
package removal — raises a confirmation card that the daemon owns, not
the model: the command and the reason it was flagged are shown, the
options are fixed (*Allow*, *Deny*), and the card is titled *System
confirmation — required before the command runs*.

The reason is that consent to run a destructive command as root must not
come from a model that a web page might have talked into. And it fails
closed: a timeout or a cancellation denies the command, where a timeout
on an ordinary question only tells the agent to carry on with its best
judgment.

Before that, the password itself comes from *KDE's own password dialog*,
not a terminal nobody is watching. The password lives in memory only,
is validated immediately, and is reused for a while — as `sudo` does —
before it is asked again.

= The session archive

The search button opens a two-pane window over the desktop: your
sessions on the left, the selected session's conversation on the right.

- *Typing narrows the list.* The match is fuzzy and in order: every
  character of your query has to occur in the session's name, so *dt*
  finds *Disk tuning*, and every further character drops the sessions
  that no longer match. The best match sorts to the top.
- *A session's name* is written by the daemon after the conversation,
  from what it was about, and rewritten as the session grows. A session
  it never named is listed under its first question, so nothing is ever
  unfindable. You can name a session yourself, and your name stands.
- *The right pane* shows the conversation as it was: your messages as
  bubbles, the agent's answers as Markdown, formulas rendered, addresses
  clickable.
- *Click* a session to look at it; *double-click* it, or select it and
  press *Return*, to open it in the conversation window — with its
  history loaded, so you can carry the conversation on.
- *Up* and *Down* move the selection, *Return* opens, *Esc* closes.

== Throwing a session away

Every entry in the list carries a trash button — *Delete this session* —
and the click only asks: a session that is gone cannot be brought back,
so the confirmation names the session it is about. The list is full of
similar names, and the wrong one must not go.

The question is modal: the window behind it is dimmed, the query field
and the list are out of reach, and *Esc* declines it rather than closing
the search. *Cancel* declines it too; *Delete* throws the session away —
the conversation, and the audit trail of its runs, with it. What is left
behind would be an orphan nobody ever sees again.

The archive is a SQLite database on your own disk, one per user. There
is no account to make and nothing to import: the conversations are
already yours, and the search is the daemon's own fuzzy matcher
answering over them.

== Naming a session yourself

Every entry of the list carries a pencil button — *Rename this session* —
and the field that comes up holds the name the session has now, so you
change it rather than write it from scratch.

The name you type stands: the daemon is not asked for a name for that
session again, and its own naming never overwrites yours. An empty name
is an answer too — the session goes back to the name the daemon gives
it, and the next run names it afresh.

The question is modal, like the delete's: the window behind it is dimmed
and *Esc* declines it rather than closing the search. The name is the
daemon's to compute, so the list shows the name you typed and the search
matches it right away.

= The settings window

The gear button opens it. Four sections, one scrolling page, no tabs.

== Appearance

*Colour scheme* — *Follow system* (the default), *Light* or *Dark*. The
choice applies at once, to the conversation window, the session search,
the settings window and the dictation overlay. The panel widget keeps
matching the panel it lives on.

*Answer fonts* — a family and a size for the light mode and another pair
for the dark one, chosen independently of each other and of the colour
scheme; the scheme decides which pair applies. Every font the system has
is on offer, a line of sample text shows the face as the answers would
be set in it, and the size is in pixels (8 to 48).

The app's own defaults are Literata for the light mode — a face drawn for
long reading on a screen — and Inter for the dark one, a screen face. A
system with neither keeps its own font. Only the agent's own words are
set in these fonts; the labels, buttons and tooltips around them always
keep the system's.

== API keys

Two roles, each with an endpoint, a model and a key.

*LLM*

- *Endpoint* — any OpenAI-compatible URL. The host in the URL decides
  the service; an unknown host is treated as your own server.
- *Model* — leave it empty for the service's default.
- *Key* — stored in `~/.config/phoenix-dna/keys.json`, mode 0600. The
  eye button shows it.

*Speech-to-text*

- The same three fields, for the endpoint that transcribes your
  dictations.

Both sections tell you the name the key is stored under, so you can find
it in the file. The LLM key is read by the daemon at startup — so when
you save it, Phoenix DNA restarts the daemon for you and tells you so.
The speech-to-text key needs no such thing: the transcriber re-reads
its configuration for every clip.

== Read aloud

- *The agent reads its answers aloud* — the master switch, off by
  default. The label is clickable.
- *Test* — says the fixed sentence *This is how the agent's answers will
  sound.* through the pipeline you have configured.
- *Endpoint* and *Model* — for the speech endpoint. MiniMax speaks a
  different protocol, so it is picked from the list below rather than
  by URL; OpenAI, Groq and OpenRouter are named by the endpoint above.
- *Voice provider* — *MiniMax* or *Local*. Local speaks on this
  computer and needs no key at all (Kokoro or Piper; see the
  #html.elem("a", attrs: (href: "features.html"), [features page])).
- *When* — *After the answer is complete* or *While it is generated*.
- *Voice* — the voice id. The field is editable, for the voices the
  service offers.
- *Speed* — 0.75× to 2×, the pitch kept.

== Global shortcuts

Four rows: *Toggle recording*, *Screenshot a window*, *Search sessions*
and *Type a query*. Click a row's button and press the new combination;
the button shows *Type the new shortcut…* while it listens.

- *Escape cancels* the capture, *Backspace* clears the binding, and the
  small *x* beside the row unbinds it outright.
- A combination is committed as soon as the key goes down. A
  modifiers-only combination — like the default `Meta+Alt` for the
  screenshot — is committed when the last modifier is released.
- Only the screenshot may be bound to modifiers alone; the others need
  a regular key too, and the window says so when they do not.
- Combinations the system has already taken never reach the window.

The bindings are the ones *System Settings → Shortcuts* edits, so they
survive restarts and can be rebound there as well.

== When a change takes effect

#table(
  columns: (2fr, 1.4fr),
  table.header([Setting], [Applies]),
  [Colour scheme, answer fonts and sizes], [At once, on the spot],
  [Read-aloud: the switch, the mode, the voice, the provider, the speed], [At once — the speech pipeline is rebuilt for you],
  [Speech-to-text: endpoint, model, key], [On the next dictation],
  [LLM: the key], [At once — the window restarts `phoenix.service` for you, and says so],
  [LLM: the endpoint and the model], [After `systemctl --user restart phoenix.service` — the daemon reads them at startup, and the window says so when you save them],
  [Global shortcuts], [At once, through KGlobalAccel],
)

Environment variables beat the file. When one is set, the window says so
in orange — *An API key environment variable is set and takes precedence
over the stored key.*

= Keyboard reference

#table(
  columns: (1.6fr, 1.5fr, 2.6fr),
  table.header([Where], [Keys], [What it does]),
  [Anywhere], [`Meta+Alt+Space`], [Start or stop the recording],
  [Anywhere], [`Meta+Alt`], [Photograph the window you point at],
  [Anywhere], [`Meta+Alt+grave`], [Open the session search],
  [Anywhere], [`Meta+Alt+Return`], [Open the typed-prompt window],
  [Dictation overlay], [`Return`], [Send the prompt],
  [Dictation overlay], [`Shift+Return`], [A line break],
  [Dictation overlay], [`Esc`], [Throw the typed text away — typing mode only],
  [Conversation window], [`Return`], [Send the follow-up],
  [Conversation window], [`Shift+Return`], [A line break],
  [Conversation window], [`Esc`], [Hide the window, and stop the reading],
  [Wizard], [`Esc`], [Hide the wizard; the agent keeps waiting],
  [Session search], [`Up` / `Down`], [Move the selection],
  [Session search], [`Return`], [Open the selected session],
  [Session search], [`Esc`], [Declines the question that is up first; closes the search otherwise],
  [Settings], [`Esc`], [Close the window (unless a shortcut is being recorded)],
)

= Files on disk

#table(
  columns: (2fr, 2.4fr),
  table.header([Path], [Contents]),
  [`~/.config/phoenix-dna/keys.json`], [The API keys (mode 0600) and the appearance settings],
  [`~/.config/phoenix-dna/agent.toml`], [The daemon's own configuration. It overrides the settings window, and is where the local voice is named],
  [`~/.local/state/phoenix-dna/users/<user>/sessions.db`], [The session archive — one SQLite database per user],
  [`~/.local/state/phoenix-dna/attachments/`], [The files that travelled with a prompt, copied per run],
  [`~/.local/state/phoenix-dna/settings-window.ini`], [The remembered size of the settings window — and of the conversation window, in `results-window.ini` beside it],
  [`~/.local/share/phoenix-dna/tts-venv`], [The optional local voice engine's own virtualenv],
)

= When something does not work

*The agent never starts.* The daemon is a user service; look at it with

#cmd([the daemon's status and log], ```bash
systemctl --user status phoenix.service
journalctl --user -u phoenix.service -f
```)

*A new key does not take effect.* Saving the LLM key restarts the daemon
for you; if that restart fails, the window says so and tells you to run
it by hand. The *endpoint* and the *model* still need you to do it:

#cmd([restart the daemon], ```bash
systemctl --user restart phoenix.service
```)

*The panel still shows the old widget* after an upgrade — Plasma keeps
the old plugin until it is restarted:

#cmd([restart the panel], ```bash
systemctl --user restart plasma-plasmashell
```)

*The formulas look like source code.* A TeX engine is missing; install
`texlive-xetex` (or lualatex, or pdflatex) and they render. Without one
they stay verbatim on purpose.

*A shortcut does not fire.* Combinations already taken by the system
never reach the window. Check *System Settings → Shortcuts* for a
conflict, and rebind here or there.

*QML errors after an install.*

#cmd([the shell's own log], ```bash
journalctl --user -b | grep -i phoenix
```)
