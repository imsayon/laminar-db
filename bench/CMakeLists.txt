add_executable(laminar_storage_bench storage.cpp)
target_link_libraries(laminar_storage_bench PRIVATE laminar::core)
target_include_directories(laminar_storage_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
laminar_configure_target(laminar_storage_bench)

if(BUILD_TESTING)
  add_executable(laminar_projection_demo projection.cpp)
  target_link_libraries(laminar_projection_demo PRIVATE laminar::core)
  target_include_directories(laminar_projection_demo PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
  target_compile_definitions(laminar_projection_demo PRIVATE LAMINAR_REDIS_SERVER="${REDIS_SERVER}")
  laminar_configure_target(laminar_projection_demo)
  add_test(NAME laminar_projection_demo COMMAND laminar_projection_demo)
  set_tests_properties(laminar_projection_demo PROPERTIES TIMEOUT 120)
endif()
