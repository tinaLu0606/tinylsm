find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(tinylsm_tests
  unit/core_test.cpp
  integration/db_test.cpp
  recovery/flush_recovery_test.cpp
  test_support/fault_injection_fs.cpp
  test_support/temp_dir.cpp
)
target_link_libraries(tinylsm_tests PRIVATE tinylsm::tinylsm GTest::gtest_main)
target_include_directories(tinylsm_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/tests
)
target_compile_features(tinylsm_tests PRIVATE cxx_std_20)
tinylsm_set_warnings(tinylsm_tests)
tinylsm_enable_sanitizers(tinylsm_tests)
gtest_discover_tests(tinylsm_tests)
