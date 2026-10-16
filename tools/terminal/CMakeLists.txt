add_library(wetwaresuite_terminal
  src/terminal.cpp
  src/commands.cpp)
add_library(wetwaresuite::terminal ALIAS wetwaresuite_terminal)
target_include_directories(wetwaresuite_terminal PUBLIC include)
target_link_libraries(wetwaresuite_terminal PUBLIC wetwaresuite::core PRIVATE wetwaresuite::sysutil)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_terminal tests/test_terminal.cpp)
  target_link_libraries(test_terminal PRIVATE wetwaresuite::terminal wetwaresuite::sysutil wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME terminal COMMAND test_terminal)
endif()
