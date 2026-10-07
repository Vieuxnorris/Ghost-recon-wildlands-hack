add_library(grw_core STATIC
  math/project.cpp
  math/angles.cpp
  core/clock.cpp
  core/stats.cpp
  core/mem.cpp
  telemetry/json_line.cpp
  telemetry/telemetry.cpp
  config/config.cpp
  aim/controller.cpp
  aim/sway.cpp
  aim/recoil.cpp
  aim/target.cpp)
target_include_directories(grw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(testhost testhost/main.cpp)
target_link_libraries(testhost PRIVATE d3d11 dxgi)

add_test(NAME testhost_smoke COMMAND testhost --exit-after 2000 --resize-every 300)
add_test(NAME testhost_present1 COMMAND testhost --present1 --exit-after 1000)
