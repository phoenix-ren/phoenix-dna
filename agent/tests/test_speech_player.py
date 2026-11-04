# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Playback tests: the PCM must reach the sink, and the speed must not
touch the pitch.

The sink is a stub that counts the bytes as they arrive — the real
``pw-cat`` plays what it reads, so bytes that never reach the pipe are
never heard. The speed is checked with a sine wave through the real
ffmpeg: the wave gets shorter or longer, and its pitch stays.
"""

from __future__ import annotations

import array
import math
import os
import shutil
import stat
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

from phoenix_dna.speech import player as player_module
from phoenix_dna.speech.player import Player, atempo_filters

#: A stand-in for pw-cat: counts the bytes that arrive and writes the
#: running total to a file, so the test can see what the sink has *now*
#: (not what it gets when the pipe closes).
_SINK = """\
#!/usr/bin/env python3
import sys

total = 0
path = sys.argv[1]
while True:
    data = sys.stdin.buffer.read(4096)
    if not data:
        break
    total += len(data)
    with open(path, "w") as handle:
        handle.write(str(total))
"""

#: A stand-in for pw-cat that keeps the audio it is fed.
_SINK_PCM = """\
#!/usr/bin/env python3
import os
import sys

with open(sys.argv[1], "wb", buffering=0) as handle:
    while True:
        data = os.read(0, 65536)
        if not data:
            break
        handle.write(data)
"""


def _sink(tmp_path: Path, body: str = _SINK) -> tuple[Path, Path]:
    """A counting sink and the file it counts into."""
    counter = tmp_path / "count.txt"
    script = tmp_path / "sink.py"
    script.write_text(body.replace("sys.argv[1]", repr(str(counter))))
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    wrapper = tmp_path / "sink.sh"
    wrapper.write_text(f"#!/bin/sh\nexec {sys.executable} {script} \"$@\"\n")
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)
    return wrapper, counter


def _wait_for(counter: Path, wanted: int, timeout: float = 5.0) -> int:
    deadline = time.monotonic() + timeout
    seen = 0
    while time.monotonic() < deadline:
        if counter.exists():
            try:
                seen = int(counter.read_text().strip() or 0)
            except ValueError:
                seen = 0
            if seen >= wanted:
                return seen
        time.sleep(0.02)
    return seen


def test_written_pcm_reaches_the_sink_at_once(tmp_path):
    """The audio must not wait in Python's pipe buffer.

    Python buffers a pipe in 128 KB blocks: without a flush the PCM
    would sit there until the buffer overflowed, so the tail of every
    utterance — up to a buffer's worth — would never be played.
    """
    wrapper, counter = _sink(tmp_path)

    player = Player(rate=32000, pwcat_bin=str(wrapper))
    try:
        player.start(32000)
        player.write(b"\x00\x01" * 2048)  # 4096 bytes, as the engine yields
        # The sink has the bytes while the player is still open: nothing
        # is waiting in a buffer.
        assert _wait_for(counter, 4096) == 4096
    finally:
        player.close()


def test_the_tail_of_an_utterance_is_not_left_behind(tmp_path):
    """Everything written is played — the last chunk included."""
    wrapper, counter = _sink(tmp_path)

    player = Player(rate=32000, pwcat_bin=str(wrapper))
    try:
        player.start(32000)
        chunk = b"\x00\x01" * 2048
        for _ in range(3):
            player.write(chunk)
        assert _wait_for(counter, 3 * 4096) == 3 * 4096
    finally:
        player.close()


# -- the speed ------------------------------------------------------------------


def test_atempo_filters():
    # 1.0 is the speed the audio was synthesized at: no filter at all.
    assert atempo_filters(1.0) == []
    assert atempo_filters(1.25) == ["atempo=1.25"]
    assert atempo_filters(0.75) == ["atempo=0.75"]
    # outside atempo's own 0.5–2.0 the factors are chained
    assert atempo_filters(3.0) == ["atempo=2.0", "atempo=1.5"]
    assert atempo_filters(0.25) == ["atempo=0.5", "atempo=0.5"]
    # nonsense is ignored, not obeyed
    assert atempo_filters("fast") == []
    assert atempo_filters(float("nan")) == []


def test_the_speed_reaches_the_decoder(monkeypatch, tmp_path):
    """The chain carries the speed as an atempo filter.

    A speed other than 1.0 needs the ffmpeg stage even when the rates
    match — and the filter is what stretches the audio.
    """
    spawned: list[list[str]] = []

    class FakeProcess:
        def __init__(self, argv, **kwargs):
            self.argv = argv
            self.stdin = None
            self.stdout = None
            self.stderr = None
            self.pid = 1

        def poll(self):
            return None

        def kill(self):
            return None

        def wait(self, timeout=None):
            return 0

    def fake_popen(argv, **kwargs):
        spawned.append(list(argv))
        return FakeProcess(argv, **kwargs)

    monkeypatch.setattr(player_module.subprocess, "Popen", fake_popen)

    player = Player(rate=32000, speed=1.5, pwcat_bin="/bin/true")
    try:
        player.start(32000)
    finally:
        player.close()
    assert any("atempo=1.5" in arg for arg in spawned[1])

    # At the speed the audio was made at, the stage stays out of the way.
    spawned.clear()
    player = Player(rate=32000, speed=1.0, pwcat_bin="/bin/true")
    try:
        player.start(32000)
    finally:
        player.close()
    assert len(spawned) == 1  # only the sink


@pytest.mark.skipif(
    shutil.which("ffmpeg") is None, reason="ffmpeg is not installed"
)
def test_the_speed_stretches_the_audio_without_touching_the_pitch(tmp_path):
    """A sine wave comes out shorter or longer — at the same pitch."""
    rate, seconds, freq = 32000, 2.0, 440.0
    pcm = b"".join(
        struct.pack("<h", int(20000 * math.sin(2 * math.pi * freq * i / rate)))
        for i in range(int(rate * seconds))
    )
    out = tmp_path / "out.pcm"

    for speed, expected in ((1.5, 1.0 / 1.5), (0.75, 1.0 / 0.75)):
        if out.exists():
            out.unlink()
        wrapper = tmp_path / "sink.sh"
        script = tmp_path / "sink.py"
        script.write_text(_SINK_PCM.replace("sys.argv[1]", repr(str(out))))
        script.chmod(script.stat().st_mode | stat.S_IXUSR)
        wrapper.write_text(f"#!/bin/sh\nexec {sys.executable} {script} \"$@\"\n")
        wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)

        player = Player(rate=32000, speed=speed, pwcat_bin=str(wrapper))
        try:
            player.start(rate)
            player.write(pcm)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if out.exists() and out.stat().st_size > 0:
                    break
                time.sleep(0.05)
        finally:
            player.close()

        samples = array.array("h")
        samples.frombytes(out.read_bytes())
        duration = len(samples) / rate
        # The length follows the speed (atempo's own look-ahead makes
        # it a touch shorter, so the check is a range, not an equality).
        assert 0.9 * expected * seconds < duration < 1.05 * expected * seconds
        # The pitch does not move: a sine stays at its frequency.
        crossings = sum(
            1
            for i in range(1, len(samples))
            if (samples[i - 1] < 0) != (samples[i] < 0)
        )
        measured = crossings / 2 / duration
        assert abs(measured - freq) < 5, f"the pitch moved to {measured:.1f} Hz"
