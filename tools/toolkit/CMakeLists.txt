add_library(achclwss_toolkit src/detect.cpp)
target_include_directories(achclwss_toolkit PUBLIC include)
target_link_libraries(achclwss_toolkit PUBLIC achclwss::core)

add_executable(achclwss-toolkit src/main.cpp)
target_link_libraries(achclwss-toolkit PRIVATE achclwss_toolkit achclwss::mcp)
install(TARGETS achclwss-toolkit RUNTIME DESTINATION bin)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_toolkit tests/test_detect.cpp)
  target_link_libraries(test_toolkit PRIVATE achclwss_toolkit achclwss_check)
  add_test(NAME toolkit COMMAND test_toolkit)
endif()
