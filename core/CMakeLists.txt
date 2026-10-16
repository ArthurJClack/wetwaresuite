add_library(wetwaresuite_core src/command.cpp src/workspace.cpp src/cli.cpp src/process.cpp)
add_library(wetwaresuite::core ALIAS wetwaresuite_core)
target_include_directories(wetwaresuite_core PUBLIC include)

if(WETWARESUITE_BUILD_TESTS)
  add_library(wetwaresuite_check INTERFACE)
  target_include_directories(wetwaresuite_check INTERFACE tests)

  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE wetwaresuite::core wetwaresuite_check)
  add_test(NAME core COMMAND test_core)
endif()
