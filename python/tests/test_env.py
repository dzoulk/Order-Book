import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # python/ for the orderbook_gym package

from orderbook_gym import OrderBookEnv


def test_reset_returns_valid_observation():
    env = OrderBookEnv(max_steps=10)
    obs, info = env.reset(seed=42)
    assert env.observation_space.contains(obs)
    assert info == {}


def test_step_returns_valid_types():
    env = OrderBookEnv(max_steps=10)
    env.reset(seed=42)
    obs, reward, terminated, truncated, info = env.step(1)
    assert env.observation_space.contains(obs)
    assert isinstance(reward, float)
    assert isinstance(terminated, bool)
    assert isinstance(truncated, bool)
    assert "inventory" in info


def test_truncates_after_max_steps():
    env = OrderBookEnv(max_steps=5)
    env.reset(seed=1)
    truncated = False
    steps = 0
    while not truncated:
        _, _, _, truncated, _ = env.step(0)
        steps += 1
        assert steps <= 5
    assert steps == 5


def test_quoting_against_background_flow_eventually_fills():
    env = OrderBookEnv(max_steps=50, background_ops_per_step=20, quote_qty=5)
    env.reset(seed=7)
    for _ in range(50):
        _, _, _, truncated, _ = env.step(1)  # requote every step
        if truncated:
            break
    # 1000 background ops against a resting best-priced quote should
    # produce at least one fill; a fill is the only thing that moves cash
    # or inventory away from their initial values.
    assert env._cash != 0.0 or env._inventory != 0


def test_gymnasium_api_compliance():
    checker = pytest.importorskip("gymnasium.utils.env_checker")
    env = OrderBookEnv(max_steps=20)
    checker.check_env(env, skip_render_check=True)
