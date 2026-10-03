find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import pybind11; print(pybind11.get_cmake_dir())"
    OUTPUT_VARIABLE pybind11_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE pybind11_FIND_RESULT
)
if(NOT pybind11_FIND_RESULT EQUAL 0)
    message(FATAL_ERROR "pybind11 not found for ${Python3_EXECUTABLE}. "
                         "Install it in that Python's environment first, e.g.: "
                         "${Python3_EXECUTABLE} -m pip install pybind11")
endif()

find_package(pybind11 REQUIRED CONFIG)

pybind11_add_module(orderbook_native bindings.cpp)
target_link_libraries(orderbook_native PRIVATE orderbook_core)
