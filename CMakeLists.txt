cmake_minimum_required(VERSION 3.27)
project(dewolf_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    idax
    SOURCE_DIR /Users/int/dev/idax
)

FetchContent_MakeAvailable(idax)

# Include Z3 headers (from Homebrew or system)
find_path(Z3_INCLUDE_DIR z3++.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
if(NOT Z3_INCLUDE_DIR)
    message(FATAL_ERROR "Z3 headers not found. Please install z3 (brew install z3)")
endif()

# Configure targets: libdewolf, libdewolf-logic, libdewolf-idioms

add_library(dewolf_logic STATIC src/dewolf_logic/logic.cpp src/dewolf_logic/dag.cpp src/dewolf_logic/world.cpp src/dewolf_logic/range_simplifier.cpp src/dewolf_logic/bitwise_simplifier.cpp src/dewolf_logic/z3_logic.cpp)
target_link_libraries(dewolf_logic PUBLIC idax::idax)
target_include_directories(dewolf_logic PUBLIC src/dewolf_logic ${Z3_INCLUDE_DIR})

add_library(dewolf_idioms STATIC src/dewolf_idioms/idioms.cpp)
target_link_libraries(dewolf_idioms PUBLIC idax::idax)
target_include_directories(dewolf_idioms PUBLIC src/dewolf_idioms)

add_library(dewolf STATIC src/dewolf/dewolf.cpp src/common/types.cpp src/common/arena.cpp src/dewolf/structures/cfg.cpp src/dewolf/lifter.cpp src/dewolf/ssa/dominators.cpp src/dewolf/ssa/liveness/liveness.cpp src/dewolf/ssa/ssa_constructor.cpp src/dewolf/ssa/ssa_destructor.cpp src/dewolf/pipeline/pipeline.cpp src/dewolf/pipeline/preprocessing_stages.cpp src/dewolf/pipeline/optimization_stages.cpp src/dewolf/structuring/structurer.cpp src/dewolf/structuring/structuring_stage.cpp src/dewolf/codegen/codegen.cpp src/dewolf/structuring/graph_slice/graph_slice.cpp src/dewolf/structuring/reaching_conditions/reaching_conditions.cpp src/dewolf/structuring/cbr/cbr.cpp src/dewolf/structuring/car/car.cpp)
target_link_libraries(dewolf PUBLIC idax::idax dewolf_logic dewolf_idioms)
target_include_directories(dewolf PUBLIC src/dewolf src/common ${Z3_INCLUDE_DIR})

# IDA Plugin target
set(IDA_INSTALL_DIR "/Applications/IDA Professional 9.3.app/Contents/MacOS")
ida_add_plugin(dewolf_ida
    SOURCES src/plugin/plugin.cpp
    LIBRARIES dewolf dewolf_logic dewolf_idioms idax::idax "${IDA_INSTALL_DIR}/libz3.dylib"
)

# Rigorous Headless Tests
add_executable(dewolf_tests tests/test_main.cpp)
target_link_libraries(dewolf_tests dewolf dewolf_logic dewolf_idioms "${IDA_INSTALL_DIR}/libz3.dylib")
if(APPLE)
    set_target_properties(dewolf_tests PROPERTIES LINK_FLAGS "-Wl,-undefined,dynamic_lookup")
endif()

# Real Binary End-to-End Test via idalib
add_executable(dewolf_idalib_test tests/test_idalib.cpp)
target_link_libraries(dewolf_idalib_test
    PRIVATE
        dewolf
        dewolf_logic
        dewolf_idioms
        idax::idax
        "${IDA_INSTALL_DIR}/libidalib.dylib"
        "${IDA_INSTALL_DIR}/libida.dylib"
        "${IDA_INSTALL_DIR}/libz3.dylib"
)
target_compile_definitions(dewolf_idalib_test PRIVATE __EA64__ IDALIB_IMPL)
target_include_directories(dewolf_idalib_test PRIVATE "${IDASDK}/include")
set_target_properties(dewolf_idalib_test PROPERTIES
    BUILD_RPATH "${IDA_INSTALL_DIR}"
    INSTALL_RPATH "${IDA_INSTALL_DIR}"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)



