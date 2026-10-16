add_library(achclwss_flash src/firmware.cpp src/serial.cpp src/sim.cpp src/avr.cpp src/esp.cpp src/stm32.cpp src/flash.cpp src/commands.cpp)
add_library(achclwss::flash ALIAS achclwss_flash)
target_include_directories(achclwss_flash PUBLIC include)
target_link_libraries(achclwss_flash PUBLIC achclwss::core PRIVATE achclwss::raster)
if(WIN32)
  target_link_libraries(achclwss_flash PRIVATE advapi32)  # registry: COM port names and USB ids
endif()

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_flash tests/test_flash.cpp)
  target_link_libraries(test_flash PRIVATE achclwss::flash achclwss::mcp nlohmann_json::nlohmann_json achclwss_check)
  add_test(NAME flash COMMAND test_flash)
endif()
