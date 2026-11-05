# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The context view: what a session spent of the model's context window.

The numbers the view shows come from two places: the session store
(how many queries and answers the session holds, and what its runs
spent) and the configuration (which model is in force, how much that
model holds at once, what a million of its tokens cost). Both halves
are checked here, and the arithmetic that joins them.
"""

from __future__ import annotations

from conftest import wait_for

from phoenix_dna.config import Config, load_config
from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.provider import FakeProvider
from phoenix_dna.sessions import SessionStore


def _events_of(events, kind):
    return [e for e in events if e["kind"] == kind]


def test_store_reports_the_shape_of_a_session(tmp_path):
    store = SessionStore(tmp_path / "users")
    user = store.local_user

    store.append_message(user, "user", "s1", {"role": "user", "content": "hi"})
    store.append_message(user, "user", "s1", {"role": "assistant", "content": "yo"})
    store.append_message(user, "user", "s1", {"role": "tool", "content": "output"})
    store.record_run(
        user,
        "user",
        "s1",
        "run_finished",
        {"status": "completed", "tokens_in": 100, "tokens_out": 20},
    )

    context = store.session_context(user, "s1")

    # The messages: the two the conversation is made of, and the tool
    # result in between (which is a message too).
    assert context["userMessages"] == 1
    assert context["assistantMessages"] == 1
    assert context["messageCount"] == 3
    assert context["runs"] == 1

    # The tokens of the run, as the view shows them.
    assert context["tokensIn"] == 100
    assert context["tokensOut"] == 20
    assert context["totalTokens"] == 120

    # A session that is not there has no context to report.
    assert store.session_context(user, "missing") == {}
    assert store.session_context(user, "") == {}


def test_store_splits_the_cached_part_of_the_prompt(tmp_path):
    store = SessionStore(tmp_path / "users")
    user = store.local_user

    store.record_run(
        user,
        "user",
        "s1",
        "run_finished",
        {
            "status": "completed",
            "tokens_in": 220_392,
            "tokens_out": 152,
            "cache_tokens": 220_000,
            "reasoning_tokens": 64,
        },
    )

    context = store.session_context(user, "s1")

    # The cached part of the prompt is not fresh input: the fresh
    # input is what is left of the prompt tokens.
    assert context["cacheTokens"] == 220_000
    assert context["tokensIn"] == 392
    assert context["tokensOut"] == 152
    assert context["reasoningTokens"] == 64
    # The total is the sum of the three - the cached tokens are
    # counted once, not twice.
    assert context["totalTokens"] == 220_544


def _daemon_with(config, answer="an answer"):
    """A daemon over the fake provider, with the events it emits."""
    events: list = []
    daemon = AgentDaemon(
        config,
        provider=FakeProvider([answer]),
        emitter=lambda event: events.append(event),
    )
    return daemon, events


def test_daemon_answers_with_the_model_and_the_share(tmp_path):
    config = Config(
        provider="fake",
        config_dir=tmp_path / "config",
        data_dir=tmp_path / "state",
        llm_provider="minimax",
        model="MiniMax-M3",
        context_window=1000,
    )
    config.config_dir.mkdir(parents=True, exist_ok=True)
    config.data_dir.mkdir(parents=True, exist_ok=True)

    daemon, events = _daemon_with(config)
    try:
        daemon.submit("a question")
        assert wait_for(lambda: _events_of(events, "run_finished"), timeout=5)
        session_id = _events_of(events, "run_started")[0]["session_id"]
        user = daemon._sessions.store.local_user

        context = daemon.session_context(user, session_id)

        # The model in force, and the window it holds.
        assert context["provider"] == "minimax"
        assert context["model"] == "MiniMax-M3"
        assert context["contextLimit"] == 1000

        # The fake provider spends 11 input and 7 output tokens per
        # turn; the share of the window is what that is.
        assert context["totalTokens"] == 18
        assert context["usage"] == 1.8

        # No price configured: the view says so rather than quoting a
        # price of nothing.
        assert context["priced"] is False
        assert context["cost"] == 0.0
    finally:
        daemon.shutdown()


def test_daemon_prices_the_session_when_the_model_is_priced(tmp_path):
    config = Config(
        provider="fake",
        config_dir=tmp_path / "config",
        data_dir=tmp_path / "state",
        llm_provider="minimax",
        model="MiniMax-M3",
        context_window=1000,
        price_input=0.30,
        price_output=1.20,
    )
    config.config_dir.mkdir(parents=True, exist_ok=True)
    config.data_dir.mkdir(parents=True, exist_ok=True)

    daemon, events = _daemon_with(config)
    try:
        daemon.submit("a question")
        assert wait_for(lambda: _events_of(events, "run_finished"), timeout=5)
        session_id = _events_of(events, "run_started")[0]["session_id"]
        user = daemon._sessions.store.local_user

        context = daemon.session_context(user, session_id)

        assert context["priced"] is True
        # 11 input tokens at $0.30 per million, 7 output tokens at
        # $1.20 per million.
        expected = (11 * 0.30 + 7 * 1.20) / 1_000_000.0
        assert abs(context["cost"] - expected) < 1e-9
    finally:
        daemon.shutdown()


def test_the_provider_supplies_the_context_window(tmp_path):
    config_dir = tmp_path / "config"
    config_dir.mkdir(parents=True, exist_ok=True)

    # MiniMax's models hold a million tokens: the provider knows it.
    config = load_config(config_dir, tmp_path / "state", env={})
    assert config.llm_provider == "minimax"
    assert config.context_window == 1_000_000

    # A server of the user's own knows nothing: the number is theirs.
    (config_dir / "agent.toml").write_text(
        '[agent]\nllm_base_url = "http://localhost:8000/v1"\nmodel = "my-model"\n'
    )
    config = load_config(config_dir, tmp_path / "state", env={})
    assert config.llm_provider == "custom"
    assert config.context_window == 0

    # The user's own number wins over the provider's default.
    (config_dir / "agent.toml").write_text(
        '[agent]\nllm_base_url = "http://localhost:8000/v1"\n'
        'model = "my-model"\ncontext_window = 32000\n'
    )
    config = load_config(config_dir, tmp_path / "state", env={})
    assert config.context_window == 32000

    # And so does the environment.
    config = load_config(
        config_dir,
        tmp_path / "state",
        env={"PHOENIX_CONTEXT_WINDOW": "64000"},
    )
    assert config.context_window == 64000
