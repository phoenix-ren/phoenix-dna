# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Tests for the speech half of the D-Bus interface.

Two layers, like the existing service tests: the pure event→signal
mapping (no D-Bus), and a real integration run on a private D-Bus
daemon with a fake engine and player — no network, no audio devices
(the player is fake), just the wire.
"""

from __future__ import annotations

import threading

import pytest

from phoenix_dna import events
from phoenix_dna.service import SignalBridge

# -- pure mapping ---------------------------------------------------------------


class FakeService:
    def __init__(self):
        self.calls = []

    def SpeechStateChanged(self, *args):
        self.calls.append(("SpeechStateChanged", args))


def test_speech_state_event_maps_to_signal():
    service = FakeService()
    bridge = SignalBridge({"speech_state_changed": service.SpeechStateChanged})
    bridge.dispatch(events.make(events.SPEECH_STATE_CHANGED, state="speaking"))
    assert service.calls == [("SpeechStateChanged", ("speaking",))]


def test_speech_state_event_defaults_to_idle_string():
    service = FakeService()
    bridge = SignalBridge({"speech_state_changed": service.SpeechStateChanged})
    bridge.dispatch({"kind": "speech_state_changed"})
    assert service.calls == [("SpeechStateChanged", ("",))]


# -- over the wire ---------------------------------------------------------------

pytest.importorskip("dbus", reason="python3-dbus not installed")


@pytest.fixture(scope="module")
def bus_address():
    import os
    import signal
    import subprocess

    try:
        result = subprocess.run(
            ["dbus-daemon", "--session", "--print-address", "--print-pid", "--fork"],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        pytest.skip(f"cannot start a private dbus-daemon: {exc}")
    if result.returncode != 0:
        pytest.skip(f"cannot start a private dbus-daemon: {result.stderr.strip()}")
    address, _, pid = result.stdout.strip().partition("\n")
    yield address.strip()
    try:
        os.kill(int(pid.strip()), signal.SIGTERM)
    except (OSError, ValueError):
        pass


class FakeEngine:
    sample_rate = 32000

    def __init__(self):
        self.synthesized = []

    def synthesize(self, text):
        self.synthesized.append(text)
        yield b"\x01\x00"

    def voices(self):
        return ["fake"]

    def stop(self):
        pass

    def close(self):
        pass


class FakePlayer:
    def start(self, rate):
        pass

    def write(self, pcm):
        pass

    def stop(self):
        pass

    def close(self):
        pass


@pytest.fixture(scope="module")
def service(bus_address, tmp_path_factory):
    from dbus.mainloop.glib import DBusGMainLoop

    DBusGMainLoop(set_as_default=True)
    import dbus
    import dbus.bus
    from gi.repository import GLib

    from phoenix_dna.config import Config
    from phoenix_dna.daemon import AgentDaemon
    from phoenix_dna.provider import FakeProvider
    from phoenix_dna.service import BUS_NAME, PhoenixService
    from phoenix_dna.speech.speaker import Speaker

    tmp = tmp_path_factory.mktemp("dbus-speech")
    config = Config(provider="fake", config_dir=tmp / "config", data_dir=tmp / "state")
    engine = FakeEngine()
    speaker = Speaker(
        engine=engine,
        player=FakePlayer(),
        enabled=True,
        read_mode="final",
        holdback_sentences=1,
    )
    daemon = AgentDaemon(
        config,
        provider=FakeProvider(["Spoken over the bus."]),
        speaker=speaker,
    )
    speaker.set_emitter(daemon._emit)
    bus = dbus.bus.BusConnection(bus_address)
    assert bus.request_name(BUS_NAME) == dbus.bus.REQUEST_NAME_REPLY_PRIMARY_OWNER
    svc = PhoenixService(daemon, bus)
    loop = GLib.MainLoop()
    thread = threading.Thread(target=loop.run, daemon=True)
    thread.start()
    svc._engine = engine  # for the tests' assertions
    yield svc
    loop.quit()
    daemon.shutdown()
    bus.release_name(BUS_NAME)


@pytest.fixture()
def client(service, bus_address):
    import dbus
    import dbus.bus

    from phoenix_dna.service import BUS_NAME, INTERFACE, OBJECT_PATH

    bus = dbus.bus.BusConnection(bus_address)
    proxy = bus.get_object(BUS_NAME, OBJECT_PATH)
    return bus, dbus.Interface(proxy, INTERFACE)


def test_get_speech_state(client):
    _bus, iface = client
    enabled, state = iface.GetSpeechState()
    assert bool(enabled) is True
    assert str(state) in ("idle", "speaking")


def test_speak_text_and_signal(client, service):
    bus, iface = client
    from phoenix_dna.service import INTERFACE

    received = []
    done = threading.Event()

    def on_state(state):
        received.append(str(state))
        if received and received[-1] == "idle" and "speaking" in received:
            done.set()

    bus.add_signal_receiver(
        on_state, signal_name="SpeechStateChanged", dbus_interface=INTERFACE
    )
    iface.SpeakText("Hello from the test harness, spoken over the private bus.")
    assert done.wait(10), "SpeechStateChanged never returned to idle"
    assert "speaking" in received
    assert service._engine.synthesized


def test_stop_speaking_is_safe_when_idle(client):
    _bus, iface = client
    iface.StopSpeaking()
    enabled, state = iface.GetSpeechState()
    assert str(state) == "idle"


def test_reload_speech_reflects_keys_json_changes(client, service, tmp_path):
    """The settings window's flow: write keys.json, call ReloadSpeech."""
    import json as _json

    bus, iface = client
    keys_file = service._daemon.config.config_dir / "keys.json"
    keys_file.parent.mkdir(parents=True, exist_ok=True)

    def write_keys(enabled: bool) -> None:
        keys_file.write_text(
            _json.dumps({"tts_enabled": enabled, "minimax_api_key": "k"})
        )

    try:
        write_keys(True)
        iface.ReloadSpeech()
        enabled, _state = iface.GetSpeechState()
        assert bool(enabled) is True
    finally:
        write_keys(False)
        iface.ReloadSpeech()
    enabled, _state = iface.GetSpeechState()
    assert bool(enabled) is False
