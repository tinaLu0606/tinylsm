cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "usage: cmake -DINPUT=<benchmark.json> -P scripts/verify_compaction_benchmark_json.cmake")
endif()
if(NOT EXISTS "${INPUT}")
  message(FATAL_ERROR "benchmark JSON does not exist: ${INPUT}")
endif()

file(READ "${INPUT}" benchmark_json)
string(JSON benchmark_count ERROR_VARIABLE json_error LENGTH "${benchmark_json}" benchmarks)
if(json_error)
  message(FATAL_ERROR "invalid Google Benchmark JSON: ${json_error}")
endif()

set(expected_names TinyLSM/CompactionMixedManual TinyLSM/CompactionMixedSizeTiered)
set(found_names "")
math(EXPR last_index "${benchmark_count} - 1")
foreach(index RANGE "${last_index}")
  string(JSON aggregate_error ERROR_VARIABLE aggregate_status GET
         "${benchmark_json}" benchmarks "${index}" aggregate_name)
  if(aggregate_status OR NOT aggregate_error STREQUAL "median")
    continue()
  endif()
  string(JSON name GET "${benchmark_json}" benchmarks "${index}" name)
  string(REGEX REPLACE "/iterations:1/repeats:5/manual_time_median$" "" base_name "${name}")
  list(APPEND found_names "${base_name}")
  foreach(counter IN ITEMS point_lookups table_probes logical_write_bytes logical_live_bytes
                         flush_output_bytes compaction_output_bytes table_count
                         read_amplification write_amplification space_amplification
                         put_p99_us get_p99_us scan_p99_us)
    string(JSON counter_error ERROR_VARIABLE counter_status GET
           "${benchmark_json}" benchmarks "${index}" "${counter}")
    if(counter_status)
      message(FATAL_ERROR "benchmark median ${base_name} is missing ${counter}")
    endif()
  endforeach()
endforeach()

foreach(expected IN LISTS expected_names)
  if(NOT expected IN_LIST found_names)
    message(FATAL_ERROR "benchmark JSON is missing median result: ${expected}")
  endif()
endforeach()

message(STATUS "verified ${INPUT}: compaction medians and raw amplification counters")
