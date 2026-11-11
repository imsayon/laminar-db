function(laminar_add_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE laminar::core)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  laminar_configure_target(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

laminar_add_test(laminar_memtable_tests test_memtable.cpp)
laminar_add_test(laminar_primitives_tests test_primitives.cpp)
laminar_add_test(laminar_format_tests test_format.cpp)
laminar_add_test(laminar_db_tests test_db.cpp)
laminar_add_test(laminar_changes_tests test_changes.cpp)
laminar_add_test(laminar_compaction_tests test_compaction.cpp)
laminar_add_test(laminar_storage_failure_tests test_storage_failures.cpp)
add_test(NAME laminar_cli_tests
  COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:laminar> -P ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.cmake)
add_test(NAME laminar_demo_tests
  COMMAND bash ${PROJECT_SOURCE_DIR}/examples/storage_demo.sh $<TARGET_FILE:laminar>)
set_tests_properties(laminar_storage_failure_tests PROPERTIES TIMEOUT 300)

add_executable(laminar_fuzz_seeds fuzz_seeds.cpp)
target_link_libraries(laminar_fuzz_seeds PRIVATE laminar::core)
target_include_directories(laminar_fuzz_seeds PRIVATE ${PROJECT_SOURCE_DIR}/src)
laminar_configure_target(laminar_fuzz_seeds)
add_test(NAME laminar_fuzz_seed_tests
  COMMAND laminar_fuzz_seeds ${CMAKE_CURRENT_BINARY_DIR}/fuzz-seeds)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND CMAKE_BUILD_TYPE STREQUAL "Release")
  add_test(NAME laminar_bench_smoke
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/bench/run.py
      $<TARGET_FILE:laminar_storage_bench> ${CMAKE_CURRENT_BINARY_DIR}/bench-smoke --smoke)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT LAMINAR_ENABLE_THREAD_SANITIZER)
  add_executable(laminar_format_fuzz EXCLUDE_FROM_ALL
    fuzz_format.cpp
    ${PROJECT_SOURCE_DIR}/src/storage/format.cpp
    ${PROJECT_SOURCE_DIR}/src/util/crc32c.cpp
    ${PROJECT_SOURCE_DIR}/src/util/status.cpp
  )
  target_include_directories(laminar_format_fuzz PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(laminar_format_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(laminar_format_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  # Ubuntu's libFuzzer uses libstdc++ even when the decoder uses libc++.
  target_link_libraries(laminar_format_fuzz PRIVATE -Wl,-lstdc++)
  laminar_configure_target(laminar_format_fuzz)
endif()
