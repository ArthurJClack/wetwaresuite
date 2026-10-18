add_library(wetwaresuite_code
  src/languages.cpp
  src/highlight.cpp
  src/outline.cpp
  src/runners.cpp
  src/common.cpp
  src/cmd_edit.cpp
  src/cmd_project.cpp
  src/cmd_run.cpp
  src/cmd_git.cpp)
add_library(wetwaresuite::code ALIAS wetwaresuite_code)
target_include_directories(wetwaresuite_code PUBLIC include)
target_link_libraries(wetwaresuite_code PUBLIC wetwaresuite::core PRIVATE wetwaresuite::sysutil)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_code tests/test_code.cpp)
  target_link_libraries(test_code PRIVATE wetwaresuite::code wetwaresuite::sysutil wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME code COMMAND test_code)
endif()
