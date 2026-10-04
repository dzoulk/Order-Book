# Open-addressing hash map, drop-in replacement for std::unordered_map.
# Profiling found index_ (an unordered_map<OrderId, ...>) costing ~13.5%
# of engine time, node-based maps allocate a separate node per entry and
# chase pointers to find them; this stores entries contiguously instead.
# See docs/DESIGN.md for the before/after.
include(FetchContent)
FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG        v5.3.0
)
FetchContent_MakeAvailable(unordered_dense)

add_library(orderbook_core
    order_book.cpp
    reference_book.cpp
    fast_order_book.cpp
    occupancy_bitmap.cpp
)

target_include_directories(orderbook_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(orderbook_core PUBLIC unordered_dense::unordered_dense)

target_compile_features(orderbook_core PUBLIC cxx_std_20)

# Required so this static library can also be linked into the Python
# extension module (python/), which is a shared object.
set_target_properties(orderbook_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(orderbook_core PRIVATE /W4)
else()
    target_compile_options(orderbook_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
