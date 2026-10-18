add_library(wetwaresuite_calc
  src/bigint.cpp
  src/units.cpp
  src/parse.cpp
  src/eval.cpp
  src/functions.cpp
  src/numeric.cpp
  src/graph.cpp
  src/commands.cpp)
add_library(wetwaresuite::calc ALIAS wetwaresuite_calc)
target_include_directories(wetwaresuite_calc PUBLIC include)
target_link_libraries(wetwaresuite_calc PUBLIC wetwaresuite::core PRIVATE wetwaresuite::sysutil wetwaresuite::raster wetwaresuite::pdf)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_calc tests/test_calc.cpp)
  target_link_libraries(test_calc PRIVATE wetwaresuite::calc wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME calc COMMAND test_calc)
endif()
