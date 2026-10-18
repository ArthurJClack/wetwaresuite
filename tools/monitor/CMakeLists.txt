add_library(wetwaresuite_monitor
  src/linux.cpp
  src/macos.cpp
  src/windows.cpp
  src/monitor.cpp
  src/commands.cpp)
add_library(wetwaresuite::monitor ALIAS wetwaresuite_monitor)
target_include_directories(wetwaresuite_monitor PUBLIC include)
target_link_libraries(wetwaresuite_monitor PUBLIC wetwaresuite::core
  PRIVATE wetwaresuite::sysutil wetwaresuite::raster wetwaresuite::pdf)
if(WIN32)
  target_link_libraries(wetwaresuite_monitor PRIVATE iphlpapi advapi32)
endif()

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_monitor tests/test_monitor.cpp)
  target_link_libraries(test_monitor PRIVATE wetwaresuite::monitor wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME monitor COMMAND test_monitor)
endif()
