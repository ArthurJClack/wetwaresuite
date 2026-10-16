add_library(wetwaresuite_toolkit src/detect.cpp)
target_include_directories(wetwaresuite_toolkit PUBLIC include)
target_link_libraries(wetwaresuite_toolkit PUBLIC wetwaresuite::core)

add_executable(wetwaresuite-toolkit src/main.cpp)
target_link_libraries(wetwaresuite-toolkit PRIVATE wetwaresuite_toolkit wetwaresuite::mcp)
install(TARGETS wetwaresuite-toolkit RUNTIME DESTINATION bin)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_toolkit tests/test_detect.cpp)
  target_link_libraries(test_toolkit PRIVATE wetwaresuite_toolkit wetwaresuite_check)
  add_test(NAME toolkit COMMAND test_toolkit)
endif()
