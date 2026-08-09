add_library(laminar_core
  core/memtable.cpp
  util/crc32c.cpp
  util/status.cpp
)

add_library(laminar::core ALIAS laminar_core)

target_include_directories(laminar_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(laminar_core PUBLIC Threads::Threads)
laminar_configure_target(laminar_core)
