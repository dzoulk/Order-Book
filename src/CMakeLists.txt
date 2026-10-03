add_library(orderbook_core
    order_book.cpp
    reference_book.cpp
    fast_order_book.cpp
)

target_include_directories(orderbook_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(orderbook_core PUBLIC cxx_std_20)

# Required so this static library can also be linked into the Python
# extension module (python/), which is a shared object.
set_target_properties(orderbook_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(orderbook_core PRIVATE /W4)
else()
    target_compile_options(orderbook_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
