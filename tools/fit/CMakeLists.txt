add_library(wetwaresuite_fit
  src/bvh.cpp src/scan.cpp src/fitting.cpp src/build.cpp src/doc.cpp
  src/cmd_scan.cpp src/cmd_measure.cpp src/cmd_refs.cpp src/cmd_parts.cpp src/cmd_presets.cpp src/cmd_output.cpp)
add_library(wetwaresuite::fit ALIAS wetwaresuite_fit)
target_include_directories(wetwaresuite_fit PUBLIC include)
target_link_libraries(wetwaresuite_fit PUBLIC wetwaresuite::mesh wetwaresuite::raster wetwaresuite::sculpt PRIVATE nlohmann_json::nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_fit tests/test_fit.cpp)
  target_link_libraries(test_fit PRIVATE wetwaresuite::fit wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME fit COMMAND test_fit)
endif()
