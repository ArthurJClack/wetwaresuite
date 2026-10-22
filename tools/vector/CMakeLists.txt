add_library(wetwaresuite_vector src/geometry.cpp src/bezier.cpp src/stroke.cpp src/doc.cpp src/render.cpp src/serial.cpp src/svg.cpp src/svg_write.cpp
            src/export.cpp src/import.cpp src/trace.cpp src/commands.cpp src/text.cpp src/boolean.cpp src/cmd_text.cpp src/cmd_util.cpp src/cmd_paths.cpp src/cmd_pathops.cpp src/cmd_regions.cpp src/cmd_appearance.cpp src/cmd_organize.cpp src/cmd_boards.cpp)
add_library(wetwaresuite::vector ALIAS wetwaresuite_vector)
target_include_directories(wetwaresuite_vector PUBLIC include)
target_link_libraries(wetwaresuite_vector PUBLIC wetwaresuite::core wetwaresuite::raster PRIVATE wetwaresuite::pdfdoc wetwaresuite::pdf)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_vector tests/test_vector.cpp tests/test_vector_tools.cpp)
  target_link_libraries(test_vector PRIVATE wetwaresuite::vector wetwaresuite::mcp wetwaresuite_check)
  add_test(NAME vector COMMAND test_vector)
endif()
