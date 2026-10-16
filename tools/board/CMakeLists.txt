add_library(wetwaresuite_board src/model.cpp src/geometry.cpp src/layout.cpp src/mermaid.cpp src/render.cpp src/interop.cpp src/commands.cpp
            src/commands_more.cpp)
add_library(wetwaresuite::board ALIAS wetwaresuite_board)
target_include_directories(wetwaresuite_board PUBLIC include)
target_link_libraries(wetwaresuite_board PUBLIC wetwaresuite::core wetwaresuite::raster PRIVATE wetwaresuite::vector wetwaresuite::pdf wetwaresuite::pdfdoc
                      nlohmann_json::nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_board tests/test_board.cpp)
  target_link_libraries(test_board PRIVATE wetwaresuite::board wetwaresuite::mcp wetwaresuite::raster nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME board COMMAND test_board)
endif()
