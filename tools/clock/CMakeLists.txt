add_library(wetwaresuite_clock
  src/time.cpp
  src/commands.cpp)
add_library(wetwaresuite::clock ALIAS wetwaresuite_clock)
target_include_directories(wetwaresuite_clock PUBLIC include)
target_link_libraries(wetwaresuite_clock PUBLIC wetwaresuite::core PRIVATE wetwaresuite::sysutil)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_clock tests/test_clock.cpp)
  target_link_libraries(test_clock PRIVATE wetwaresuite::clock wetwaresuite::sysutil wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME clock COMMAND test_clock)
endif()
