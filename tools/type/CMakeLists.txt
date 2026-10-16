add_library(achclwss_type
  src/names.cpp src/geometry.cpp src/outline_ops.cpp src/font.cpp
  src/sfnt_read.cpp src/sfnt_write.cpp src/stbtt.cpp src/formats.cpp
  src/render.cpp src/trace.cpp src/doc.cpp
  src/cmdutil.cpp src/cmd_file.cpp src/cmd_glyph.cpp src/cmd_shape.cpp src/cmd_spacing.cpp src/cmd_output.cpp src/cmd_trace.cpp)
add_library(achclwss::type ALIAS achclwss_type)
target_include_directories(achclwss_type PUBLIC include PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
target_link_libraries(achclwss_type PUBLIC achclwss::core achclwss::raster
  PRIVATE achclwss::vector achclwss::pdf achclwss::pdfdoc nlohmann_json::nlohmann_json)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_type tests/test_type.cpp)
  target_include_directories(test_type PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
  target_link_libraries(test_type PRIVATE achclwss::type achclwss::mcp achclwss_check)
  add_test(NAME type COMMAND test_type)
endif()
