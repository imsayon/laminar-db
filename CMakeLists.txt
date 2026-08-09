cmake_minimum_required(VERSION 3.25)

project(laminar VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LAMINAR_WARNINGS_AS_ERRORS "Treat Laminar warnings as errors" OFF)
option(LAMINAR_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LAMINAR_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)

if(LAMINAR_ENABLE_SANITIZERS AND LAMINAR_ENABLE_THREAD_SANITIZER)
  message(FATAL_ERROR "Address/undefined sanitizers cannot be combined with ThreadSanitizer")
endif()

find_package(Threads REQUIRED)

function(laminar_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_23)
  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wformat=2
    -Wundef
    $<$<BOOL:${LAMINAR_WARNINGS_AS_ERRORS}>:-Werror>
  )

  if(LAMINAR_ENABLE_SANITIZERS)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()

  if(LAMINAR_ENABLE_THREAD_SANITIZER)
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
  endif()
endfunction()

add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

add_subdirectory(bench)
