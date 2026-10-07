cmake_minimum_required(VERSION 3.25)
project(grw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /Ob2 /Zi /DNDEBUG")

add_compile_options(/W4 /permissive- /EHsc /utf-8)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_link_options(
  "$<$<CONFIG:RelWithDebInfo>:/OPT:REF>"
  "$<$<CONFIG:RelWithDebInfo>:/OPT:ICF>"
  "$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>")

enable_testing()
add_subdirectory(tests)
