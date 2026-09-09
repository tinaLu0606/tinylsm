cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "usage: cmake -DINPUT=<benchmark.json> -P scripts/verify_benchmark_json.cmake")
endif()
if(NOT EXISTS "${INPUT}")
  message(FATAL_ERROR "benchmark JSON does not exist: ${INPUT}")
endif()

file(READ "${INPUT}" benchmark_json)
string(JSON benchmark_count ERROR_VARIABLE json_error LENGTH "${benchmark_json}"
       benchmarks)
if(json_error)
  message(FATAL_ERROR "invalid Google Benchmark JSON: ${json_error}")
endif()

set(expected_names
    TinyLSM/FillSequentialAsync
    TinyLSM/FillSequentialSync
    TinyLSM/ReadRandomHit
    TinyLSM/ReadRandomMiss
    TinyLSM/ScanTail10000Repeated
    TinyLSM/FillBatch1
    TinyLSM/FillBatch10
    TinyLSM/FillBatch100
    TinyLSM/ConcurrentFill1
    TinyLSM/ConcurrentFill2
    TinyLSM/ConcurrentFill4
    TinyLSM/ConcurrentFill8
    TinyLSM/CompactManyTables
    LevelDB/FillSequentialAsync
    LevelDB/FillSequentialSync
    LevelDB/ReadRandomHit
    LevelDB/ReadRandomMiss
    LevelDB/ScanTail10000Repeated
    LevelDB/FillBatch1
    LevelDB/FillBatch10
    LevelDB/FillBatch100
    LevelDB/ConcurrentFill1
    LevelDB/ConcurrentFill2
    LevelDB/ConcurrentFill4
    LevelDB/ConcurrentFill8)

set(found_names "")
math(EXPR last_index "${benchmark_count} - 1")
foreach(index RANGE "${last_index}")
  string(JSON name GET "${benchmark_json}" benchmarks "${index}" name)
  string(JSON aggregate_error ERROR_VARIABLE aggregate_status GET
         "${benchmark_json}" benchmarks "${index}" aggregate_name)
  if(NOT aggregate_status AND aggregate_error STREQUAL "median")
    string(REGEX REPLACE "/iterations:1/repeats:5/manual_time_median$" "" base_name
                         "${name}")
    list(APPEND found_names "${base_name}")
  endif()
endforeach()

foreach(expected IN LISTS expected_names)
  if(NOT expected IN_LIST found_names)
    message(FATAL_ERROR "benchmark JSON is missing median result: ${expected}")
  endif()
endforeach()

list(LENGTH found_names median_count)
list(LENGTH expected_names expected_count)
if(median_count LESS expected_count)
  message(FATAL_ERROR
          "expected at least ${expected_count} median results, found ${median_count}")
endif()

message(STATUS "verified ${INPUT}: ${median_count} complete median results")
