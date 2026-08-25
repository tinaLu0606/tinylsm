include_guard(GLOBAL)

find_package(Protobuf CONFIG QUIET)

if(NOT Protobuf_FOUND)
  if(NOT TINYLSM_FETCH_PROTOBUF)
    message(FATAL_ERROR "Protobuf not found; install it or enable TINYLSM_FETCH_PROTOBUF")
  endif()
  include(FetchContent)
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_PROTOBUF_BINARIES ON CACHE BOOL "" FORCE)
  set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    protobuf
    GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
    GIT_TAG v29.3
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(protobuf)
  if(APPLE AND TARGET absl_random_internal_randen_hwaes_impl)
    # AppleClang 21 rejects Abseil's x86-only flag even behind -Xarch_x86_64
    # when the active target is arm64. The portable implementation remains.
    set_property(TARGET absl_random_internal_randen_hwaes_impl
                 PROPERTY COMPILE_OPTIONS "")
    set_property(TARGET absl_random_internal_randen_hwaes
                 PROPERTY COMPILE_OPTIONS "")
  endif()
  include("${protobuf_SOURCE_DIR}/cmake/protobuf-generate.cmake")
endif()
