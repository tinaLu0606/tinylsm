if(TINYLSM_FETCH_TEST_DEPS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googletest)
else()
  find_package(GTest CONFIG REQUIRED)
endif()
include(GoogleTest)

add_executable(tinylsm_tests
  unit/core_test.cpp
  unit/filename_test.cpp
  unit/iterator_test.cpp
  integration/db_test.cpp
  recovery/cleanup_recovery_test.cpp
  recovery/compaction_recovery_test.cpp
  recovery/flush_recovery_test.cpp
  test_support/fault_injection_fs.cpp
  test_support/temp_dir.cpp
)
target_link_libraries(tinylsm_tests PRIVATE GTest::gtest_main)

if(TARGET tinylsm_cli_app)
  target_sources(tinylsm_tests PRIVATE integration/cli_test.cpp)
  target_link_libraries(tinylsm_tests PRIVATE tinylsm_cli_app)
else()
  target_link_libraries(tinylsm_tests PRIVATE tinylsm::tinylsm)
endif()

if(TARGET tinylsm_lab_http)
  target_sources(tinylsm_tests PRIVATE integration/lab_server_test.cpp)
  target_link_libraries(tinylsm_tests PRIVATE tinylsm_lab_http)
endif()

target_include_directories(tinylsm_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/tests
  ${PROJECT_SOURCE_DIR}/tools
)
target_compile_features(tinylsm_tests PRIVATE cxx_std_20)
tinylsm_set_warnings(tinylsm_tests)
tinylsm_enable_sanitizers(tinylsm_tests)
gtest_discover_tests(tinylsm_tests)
