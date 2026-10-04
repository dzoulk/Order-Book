// pybind11 bindings exposing both engines to Python, primarily so a future
// RL market-making agent (see orderbook_gym/) can drive the book without
// writing C++. Both OrderBook (v1) and FastOrderBook (v2) are exposed with
// the same method set, since they share the same public interface.
//
// Exceptions: pybind11's default translation turns std::invalid_argument
// and std::out_of_range (thrown by both engines for bad input) into
// Python ValueError and IndexError respectively, no custom translator
// needed.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "orderbook/fast_order_book.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/types.hpp"

namespace py = pybind11;
using namespace orderbook;

namespace {

template <typename Book>
void bindBook(py::module_& m, const char* name) {
    py::class_<Book>(m, name)
        .def(py::init<>())
        .def("add_limit", &Book::addLimit, py::arg("id"), py::arg("side"), py::arg("price"), py::arg("qty"))
        .def("add_market", &Book::addMarket, py::arg("id"), py::arg("side"), py::arg("qty"))
        .def("cancel", &Book::cancel, py::arg("id"))
        .def("reduce_qty", &Book::reduceQty, py::arg("id"), py::arg("new_qty"))
        .def("replace_price", &Book::replacePrice, py::arg("id"), py::arg("new_price"))
        .def("best_bid", &Book::bestBid)
        .def("best_ask", &Book::bestAsk)
        .def("depth_at", &Book::depthAt, py::arg("side"), py::arg("price"));
}

} // namespace

PYBIND11_MODULE(orderbook_native, m) {
    m.doc() = "Python bindings for the C++ limit order book engine";

    py::enum_<Side>(m, "Side").value("Buy", Side::Buy).value("Sell", Side::Sell);

    py::class_<Trade>(m, "Trade")
        .def_readonly("buy_order_id", &Trade::buyOrderId)
        .def_readonly("sell_order_id", &Trade::sellOrderId)
        .def_readonly("price", &Trade::price)
        .def_readonly("qty", &Trade::qty)
        .def_readonly("seq", &Trade::seq);

    bindBook<OrderBook>(m, "OrderBook");
    bindBook<FastOrderBook>(m, "FastOrderBook");
}
