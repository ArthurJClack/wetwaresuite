add_library(wetwaresuite_flash src/firmware.cpp src/serial.cpp src/sim.cpp src/avr.cpp src/esp.cpp src/stm32.cpp src/flash.cpp src/commands.cpp
            src/build.cpp src/cmd_build.cpp)
add_library(wetwaresuite::flash ALIAS wetwaresuite_flash)
target_include_directories(wetwaresuite_flash PUBLIC include)
target_link_libraries(wetwaresuite_flash PUBLIC wetwaresuite::core PRIVATE wetwaresuite::raster)
if(WIN32)
  target_link_libraries(wetwaresuite_flash PRIVATE advapi32)  # registry: COM port names and USB ids
endif()

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_flash tests/test_flash.cpp)
  target_link_libraries(test_flash PRIVATE wetwaresuite::flash wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME flash COMMAND test_flash)
endif()
