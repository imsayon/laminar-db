add_executable(laminar_storage_bench storage.cpp)
target_link_libraries(laminar_storage_bench PRIVATE laminar::core)
target_include_directories(laminar_storage_bench PRIVATE
  ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
laminar_configure_target(laminar_storage_bench)
