cmake_minimum_required(VERSION 3.24)

project(TinyLSM VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

option(TINYLSM_BUILD_TOOLS "Build TinyLSM command-line tools" ON)
option(TINYLSM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TINYLSM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(TINYLSM_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(TINYLSM_FETCH_TEST_DEPS "Fetch GoogleTest when unavailable" OFF)
option(TINYLSM_FETCH_PROTOBUF "Fetch pinned Protobuf when unavailable" ON)

include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
  message(VERBOSE "TinyLSM will export compile commands when compilation targets exist")
endif()
if(TINYLSM_ENABLE_ASAN OR TINYLSM_ENABLE_UBSAN)
  message(VERBOSE "TinyLSM will apply requested sanitizers when compilation targets exist")
endif()

add_subdirectory(src)

if(TINYLSM_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
