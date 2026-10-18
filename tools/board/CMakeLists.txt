add_library(achclwss_board src/model.cpp src/geometry.cpp src/layout.cpp src/mermaid.cpp src/render.cpp src/interop.cpp src/commands.cpp
            src/commands_more.cpp)
add_library(achclwss::board ALIAS achclwss_board)
target_include_directories(achclwss_board PUBLIC include)
target_link_libraries(achclwss_board PUBLIC achclwss::core achclwss::raster PRIVATE achclwss::vector achclwss::pdf achclwss::pdfdoc
                      nlohmann_json::nlohmann_json)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_board tests/test_board.cpp)
  target_link_libraries(test_board PRIVATE achclwss::board achclwss::mcp achclwss::raster nlohmann_json::nlohmann_json achclwss_check)
  add_test(NAME board COMMAND test_board)
endif()
