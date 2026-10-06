# Direct tests of the pybind11 bindings (python/bindings.cpp) against
# both engines, as opposed to test_env.py, which only exercises
# OrderBook/FastOrderBook indirectly through the Gymnasium wrapper.
# reduce_qty/replace_price in particular were added to the C++ engines
# after the Python bindings existed, got bound, and had never been
# called from Python by any test until this file.
import pytest

import orderbook_native as ob

BOOKS = [ob.OrderBook, ob.FastOrderBook]


@pytest.mark.parametrize("Book", BOOKS)
def test_resting_limit_order_shows_up_in_best_bid(Book):
    book = Book()
    book.add_limit(1, ob.Side.Buy, 100, 10)
    assert book.best_bid() == 100
    assert book.depth_at(ob.Side.Buy, 100) == 10


@pytest.mark.parametrize("Book", BOOKS)
def test_crossing_limit_orders_produce_a_trade(Book):
    book = Book()
    book.add_limit(1, ob.Side.Buy, 100, 10)
    trades = book.add_limit(2, ob.Side.Sell, 100, 10)

    assert len(trades) == 1
    trade = trades[0]
    assert trade.buy_order_id == 1
    assert trade.sell_order_id == 2
    assert trade.price == 100
    assert trade.qty == 10


@pytest.mark.parametrize("Book", BOOKS)
def test_add_market_matches_against_resting_limit(Book):
    book = Book()
    book.add_limit(1, ob.Side.Sell, 100, 5)
    trades = book.add_market(2, ob.Side.Buy, 5)

    assert len(trades) == 1
    assert trades[0].qty == 5
    assert book.best_ask() is None


@pytest.mark.parametrize("Book", BOOKS)
def test_cancel_known_id_returns_true_and_removes_order(Book):
    book = Book()
    book.add_limit(1, ob.Side.Buy, 100, 10)
    assert book.cancel(1) is True
    assert book.best_bid() is None


@pytest.mark.parametrize("Book", BOOKS)
def test_cancel_unknown_id_returns_false(Book):
    book = Book()
    assert book.cancel(999) is False


@pytest.mark.parametrize("Book", BOOKS)
def test_reduce_qty_keeps_priority_and_shrinks_quantity(Book):
    book = Book()
    book.add_limit(1, ob.Side.Buy, 100, 10)
    assert book.reduce_qty(1, 4) is True
    assert book.depth_at(ob.Side.Buy, 100) == 4


@pytest.mark.parametrize("Book", BOOKS)
def test_reduce_qty_unknown_id_returns_false(Book):
    book = Book()
    assert book.reduce_qty(999, 1) is False


@pytest.mark.parametrize("Book", BOOKS)
def test_reduce_qty_to_zero_raises_value_error(Book):
    book = Book()
    book.add_limit(1, ob.Side.Buy, 100, 10)
    with pytest.raises(ValueError):
        book.reduce_qty(1, 0)


@pytest.mark.parametrize("Book", BOOKS)
def test_replace_price_moves_resting_order(Book):
    book = Book()
    book.add_limit(1, ob.Side.Buy, 100, 10)
    book.replace_price(1, 105)
    assert book.best_bid() == 105
    assert book.depth_at(ob.Side.Buy, 100) == 0


@pytest.mark.parametrize("Book", BOOKS)
def test_replace_price_unknown_id_raises_value_error(Book):
    book = Book()
    with pytest.raises(ValueError):
        book.replace_price(999, 100)


@pytest.mark.parametrize("Book", BOOKS)
def test_duplicate_resting_id_raises_value_error(Book):
    book = Book()
    book.add_limit(1, ob.Side.Buy, 100, 10)
    with pytest.raises(ValueError):
        book.add_limit(1, ob.Side.Buy, 101, 5)


def test_fast_order_book_rejects_price_outside_supported_band():
    book = ob.FastOrderBook()
    with pytest.raises(IndexError):
        book.add_limit(1, ob.Side.Buy, 2_000_000, 10)
