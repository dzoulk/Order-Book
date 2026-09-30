include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(orderbook_tests
    book_test.cpp
)

target_link_libraries(orderbook_tests PRIVATE
    orderbook_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(orderbook_tests)
