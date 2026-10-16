add_library(achclwss_notes
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
add_library(achclwss::notes ALIAS achclwss_notes)
target_include_directories(achclwss_notes PUBLIC include)
target_link_libraries(achclwss_notes PUBLIC achclwss::core achclwss::raster
  PRIVATE achclwss::pdf achclwss::office nlohmann_json::nlohmann_json)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_notes tests/test_notes.cpp)
  target_link_libraries(test_notes PRIVATE achclwss::notes achclwss::mcp nlohmann_json::nlohmann_json achclwss_check)
  add_test(NAME notes COMMAND test_notes)
endif()
