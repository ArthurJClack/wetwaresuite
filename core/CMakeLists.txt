add_library(achclwss_core src/command.cpp)
add_library(achclwss::core ALIAS achclwss_core)
target_include_directories(achclwss_core PUBLIC include)

if(ACHCLWSS_BUILD_TESTS)
  add_library(achclwss_check INTERFACE)
  target_include_directories(achclwss_check INTERFACE tests)

  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE achclwss::core achclwss_check)
  add_test(NAME core COMMAND test_core)
endif()
