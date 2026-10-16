add_library(wetwaresuite_notes
  src/text.cpp
  src/parse.cpp
  src/vault.cpp
  src/render.cpp
  src/graph.cpp
  src/common.cpp
  src/cmd_vault.cpp
  src/cmd_notes.cpp
  src/cmd_meta.cpp
  src/cmd_tasks.cpp
  src/cmd_links.cpp
  src/cmd_export.cpp)
add_library(wetwaresuite::notes ALIAS wetwaresuite_notes)
target_include_directories(wetwaresuite_notes PUBLIC include)
target_link_libraries(wetwaresuite_notes PUBLIC wetwaresuite::core wetwaresuite::raster
  PRIVATE wetwaresuite::pdf wetwaresuite::office nlohmann_json::nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_notes tests/test_notes.cpp)
  target_link_libraries(test_notes PRIVATE wetwaresuite::notes wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME notes COMMAND test_notes)
endif()
