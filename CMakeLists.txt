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

# Configure targets: libdewolf, libdewolf-logic, libdewolf-idioms

add_library(dewolf_logic STATIC src/dewolf_logic/logic.cpp src/dewolf_logic/dag.cpp src/dewolf_logic/world.cpp src/dewolf_logic/range_simplifier.cpp src/dewolf_logic/bitwise_simplifier.cpp)
target_link_libraries(dewolf_logic PUBLIC idax::idax)
target_include_directories(dewolf_logic PUBLIC src/dewolf_logic)

add_library(dewolf_idioms STATIC src/dewolf_idioms/idioms.cpp)
target_link_libraries(dewolf_idioms PUBLIC idax::idax)
target_include_directories(dewolf_idioms PUBLIC src/dewolf_idioms)

add_library(dewolf STATIC src/dewolf/dewolf.cpp src/common/types.cpp src/common/arena.cpp src/dewolf/structures/cfg.cpp src/dewolf/lifter.cpp src/dewolf/ssa/dominators.cpp src/dewolf/ssa/ssa_constructor.cpp src/dewolf/ssa/ssa_destructor.cpp src/dewolf/pipeline/pipeline.cpp src/dewolf/pipeline/preprocessing_stages.cpp src/dewolf/pipeline/optimization_stages.cpp src/dewolf/structuring/structurer.cpp src/dewolf/structuring/structuring_stage.cpp src/dewolf/codegen/codegen.cpp)
target_link_libraries(dewolf PUBLIC idax::idax dewolf_logic dewolf_idioms)
target_include_directories(dewolf PUBLIC src/dewolf src/common)

# IDA Plugin target
ida_add_plugin(dewolf_ida
    SOURCES src/plugin/plugin.cpp
    LIBRARIES dewolf dewolf_logic dewolf_idioms idax::idax
)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
