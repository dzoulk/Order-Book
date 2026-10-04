# Minimal Gymnasium environment wrapping FastOrderBook. This is the
# stretch goal from the project plan: "expose the book to Python and wrap
# it as a Gymnasium environment", scaffolding for a market-making RL agent
# to be built later, not a tuned RL problem in itself. See docs/HISTORY.md
# for what's deliberately left simple here and what a real version would
# need (risk limits, adverse-selection modeling, reward shaping, episode
# termination beyond a fixed step count, etc).
from __future__ import annotations

from typing import Any, Optional

import gymnasium as gym
import numpy as np
from gymnasium import spaces

import orderbook_native as ob

# Background synthetic order flow uses ids starting at 1; keeping the
# agent's own order ids far away avoids any chance of collision (duplicate
# ids raise ValueError, by design, see docs/DESIGN.md).
_AGENT_ID_BASE = 10**15


class OrderBookEnv(gym.Env):
    """A single agent quotes both sides of FastOrderBook against randomly
    generated background order flow.

    Observation: [best_bid, best_ask, spread, inventory] as float32.
    best_bid/best_ask use -1.0 as a sentinel when that side is empty.

    Action (Discrete(3)):
      0 = hold (do nothing)
      1 = quote both sides: cancel any existing quotes, then place a
          fresh bid at mid - quote_offset and ask at mid + quote_offset,
          each for quote_qty
      2 = cancel all of the agent's resting quotes

    Reward: change in mark-to-market net worth (cash + inventory * mid)
    this step, minus a small quadratic inventory penalty. Marking
    inventory to market (rather than only counting realized cash flow)
    matters here: a market maker holding inventory isn't "losing" purely
    because it spent cash to acquire it.
    """

    metadata = {"render_modes": []}

    def __init__(
        self,
        *,
        max_steps: int = 500,
        background_ops_per_step: int = 10,
        mid_price: int = 10_000,
        price_band: int = 10,
        quote_offset: int = 1,
        quote_qty: int = 5,
        inventory_penalty_coeff: float = 1e-4,
    ):
        super().__init__()
        self.max_steps = max_steps
        self.background_ops_per_step = background_ops_per_step
        self.initial_mid_price = mid_price
        self.price_band = price_band
        self.quote_offset = quote_offset
        self.quote_qty = quote_qty
        self.inventory_penalty_coeff = inventory_penalty_coeff

        self.observation_space = spaces.Box(
            low=-1.0, high=np.finfo(np.float32).max, shape=(4,), dtype=np.float32
        )
        self.action_space = spaces.Discrete(3)

        self._rng: np.random.Generator = np.random.default_rng()
        self._book: Optional[ob.FastOrderBook] = None
        self._next_bg_id = 1
        self._next_agent_id = _AGENT_ID_BASE
        self._bid_order_id: Optional[int] = None
        self._ask_order_id: Optional[int] = None
        self._bid_remaining = 0
        self._ask_remaining = 0
        self._inventory = 0
        self._cash = 0.0
        self._mid = float(mid_price)
        self._prior_net_worth = 0.0
        self._step_count = 0

    def reset(self, *, seed: Optional[int] = None, options: Optional[dict] = None):
        super().reset(seed=seed)
        self._rng = np.random.default_rng(seed)

        self._book = ob.FastOrderBook()
        self._next_bg_id = 1
        self._next_agent_id = _AGENT_ID_BASE
        self._bid_order_id = None
        self._ask_order_id = None
        self._bid_remaining = 0
        self._ask_remaining = 0
        self._inventory = 0
        self._cash = 0.0
        self._mid = float(self.initial_mid_price)
        self._prior_net_worth = 0.0
        self._step_count = 0

        return self._observation(), {}

    def step(self, action: int):
        assert self._book is not None, "call reset() before step()"

        if action == 1:
            self._place_quotes()
        elif action == 2:
            self._cancel_quotes()
        # action == 0: hold

        self._run_background_flow()

        net_worth = self._cash + self._inventory * self._mid
        inventory_penalty = self.inventory_penalty_coeff * (self._inventory**2)
        reward = (net_worth - self._prior_net_worth) - inventory_penalty
        self._prior_net_worth = net_worth

        self._step_count += 1
        terminated = False
        truncated = self._step_count >= self.max_steps

        info: dict[str, Any] = {"inventory": self._inventory, "cash": self._cash, "mid": self._mid}
        return self._observation(), reward, terminated, truncated, info

    def _place_quotes(self) -> None:
        self._cancel_quotes()
        mid = int(round(self._mid))

        bid_id = self._next_agent_id
        self._next_agent_id += 1
        self._book.add_limit(bid_id, ob.Side.Buy, mid - self.quote_offset, self.quote_qty)
        self._bid_order_id = bid_id
        self._bid_remaining = self.quote_qty

        ask_id = self._next_agent_id
        self._next_agent_id += 1
        self._book.add_limit(ask_id, ob.Side.Sell, mid + self.quote_offset, self.quote_qty)
        self._ask_order_id = ask_id
        self._ask_remaining = self.quote_qty

    def _cancel_quotes(self) -> None:
        if self._bid_order_id is not None:
            self._book.cancel(self._bid_order_id)
            self._bid_order_id = None
            self._bid_remaining = 0
        if self._ask_order_id is not None:
            self._book.cancel(self._ask_order_id)
            self._ask_order_id = None
            self._ask_remaining = 0

    def _run_background_flow(self) -> None:
        for _ in range(self.background_ops_per_step):
            if self._rng.random() < 0.05:
                self._mid += int(self._rng.choice([-1, 1]))

            side = ob.Side.Buy if self._rng.random() < 0.5 else ob.Side.Sell
            bg_id = self._next_bg_id
            self._next_bg_id += 1

            if self._rng.integers(0, 100) < 90:
                offset = int(self._rng.integers(-self.price_band, self.price_band + 1))
                price = int(round(self._mid)) + offset
                qty = int(self._rng.integers(1, 20))
                trades = self._book.add_limit(bg_id, side, price, qty)
            else:
                qty = int(self._rng.integers(1, 20))
                trades = self._book.add_market(bg_id, side, qty)

            for t in trades:
                self._apply_fill_if_agent(t)

    def _apply_fill_if_agent(self, t) -> None:
        if t.buy_order_id == self._bid_order_id:
            self._inventory += t.qty
            self._cash -= t.price * t.qty
            self._bid_remaining -= t.qty
            if self._bid_remaining <= 0:
                self._bid_order_id = None
        elif t.sell_order_id == self._ask_order_id:
            self._inventory -= t.qty
            self._cash += t.price * t.qty
            self._ask_remaining -= t.qty
            if self._ask_remaining <= 0:
                self._ask_order_id = None

    def _observation(self) -> np.ndarray:
        best_bid = self._book.best_bid()
        best_ask = self._book.best_ask()
        bb = float(best_bid) if best_bid is not None else -1.0
        ba = float(best_ask) if best_ask is not None else -1.0
        spread = (ba - bb) if (best_bid is not None and best_ask is not None) else -1.0
        return np.array([bb, ba, spread, float(self._inventory)], dtype=np.float32)
