add_library(achclwss_suite suite.cpp)
target_include_directories(achclwss_suite PUBLIC .)
target_link_libraries(achclwss_suite PUBLIC achclwss_toolkit achclwss::photo achclwss::paint achclwss::vector achclwss::convert)
target_link_libraries(achclwss_suite PUBLIC achclwss::slicer)

add_executable(achclwss main.cpp)
target_link_libraries(achclwss PRIVATE achclwss_suite achclwss::mcp)
install(TARGETS achclwss RUNTIME DESTINATION bin)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_suite tests/test_suite.cpp)
  target_link_libraries(test_suite PRIVATE achclwss_suite achclwss::mcp nlohmann_json::nlohmann_json achclwss_check)
  add_test(NAME suite COMMAND test_suite)
endif()
