add_library(grw_core STATIC
  math/project.cpp
  math/angles.cpp
  core/clock.cpp
  core/stats.cpp)
target_include_directories(grw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
