include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  google_benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(google_benchmark)

add_executable(tinylsm_bench db_benchmark.cpp)
target_link_libraries(tinylsm_bench PRIVATE tinylsm::tinylsm benchmark::benchmark)
target_compile_features(tinylsm_bench PRIVATE cxx_std_20)
tinylsm_set_warnings(tinylsm_bench)

if(TINYLSM_BENCHMARK_LEVELDB)
  set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    leveldb
    GIT_REPOSITORY https://github.com/google/leveldb.git
    GIT_TAG 1.23
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(leveldb)
  target_link_libraries(tinylsm_bench PRIVATE leveldb)
  target_compile_definitions(tinylsm_bench PRIVATE TINYLSM_HAVE_LEVELDB=1)
endif()
