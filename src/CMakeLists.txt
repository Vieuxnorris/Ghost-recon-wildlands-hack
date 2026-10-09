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
  aim/target.cpp
  aim/ballistics.cpp
  aim/trajectory.cpp
  scan/matrix_detect.cpp
  scan/candidates.cpp
  scan/regions.cpp
  scan/ray_filter.cpp
  game/model.cpp
  game/registry.cpp
  game/world.cpp)
target_include_directories(grw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(testhost testhost/main.cpp)
target_link_libraries(testhost PRIVATE d3d11 dxgi)

add_test(NAME testhost_smoke COMMAND testhost --exit-after 2000 --resize-every 300)
add_test(NAME testhost_present1 COMMAND testhost --present1 --exit-after 1000)

find_package(minhook CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
add_library(grw SHARED dll/dllmain.cpp dll/hooks.cpp dll/runtime.cpp dll/overlay.cpp dll/panel.cpp dll/esp.cpp dll/aimbot.cpp dll/hwbp.cpp
  dll/bullet_probe.cpp dll/raycast.cpp dll/los.cpp)
target_link_libraries(grw PRIVATE grw_core minhook::minhook imgui::imgui d3d11 dxgi)

add_executable(probe probe/main.cpp)
target_link_libraries(probe PRIVATE d3d11 dxgi psapi)

add_executable(injector injector/main.cpp)
target_include_directories(injector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
