include_guard(GLOBAL)

function(tinylsm_enable_sanitizers target)
  if(NOT TINYLSM_ENABLE_ASAN AND NOT TINYLSM_ENABLE_UBSAN AND
     NOT TINYLSM_ENABLE_TSAN)
    return()
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    message(FATAL_ERROR "TinyLSM sanitizers currently support AppleClang, Clang, and GCC")
  endif()

  set(sanitizers "")
  if(TINYLSM_ENABLE_TSAN AND (TINYLSM_ENABLE_ASAN OR TINYLSM_ENABLE_UBSAN))
    message(FATAL_ERROR "ThreadSanitizer must use a dedicated TinyLSM build")
  endif()
  if(TINYLSM_ENABLE_ASAN)
    list(APPEND sanitizers address)
  endif()
  if(TINYLSM_ENABLE_UBSAN)
    list(APPEND sanitizers undefined)
  endif()
  if(TINYLSM_ENABLE_TSAN)
    list(APPEND sanitizers thread)
  endif()
  list(JOIN sanitizers "," sanitizer_flags)

  target_compile_options(${target} PRIVATE
    "-fsanitize=${sanitizer_flags}"
    -fno-omit-frame-pointer
  )
  target_link_options(${target} PRIVATE "-fsanitize=${sanitizer_flags}")
endfunction()
