# What every app binary shares: the app table, extension routing, the handoff
# folder and the app.* commands, and the common command line. Depends only on
# core and mcp, never on a tool.
add_library(wetwaresuite_apps apps.cpp handoff.cpp app_commands.cpp app_main.cpp)
add_library(wetwaresuite::apps ALIAS wetwaresuite_apps)
target_include_directories(wetwaresuite_apps PUBLIC .)
target_link_libraries(wetwaresuite_apps PUBLIC wetwaresuite::core wetwaresuite::mcp PRIVATE nlohmann_json::nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_apps tests/test_apps.cpp)
  target_link_libraries(test_apps PRIVATE wetwaresuite::apps wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  target_compile_definitions(test_apps PRIVATE WETWARESUITE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  add_test(NAME apps COMMAND test_apps)
endif()
