add_executable(orderbook_itch_replay
    itch_replay_main.cpp
    itch/itch_reader.cpp
)
target_link_libraries(orderbook_itch_replay PRIVATE orderbook_core)
