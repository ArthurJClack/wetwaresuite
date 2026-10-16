find_package(Threads REQUIRED)

add_library(achclwss_automate src/script.cpp src/commands.cpp)
add_library(achclwss::automate ALIAS achclwss_automate)
target_include_directories(achclwss_automate PUBLIC include)
target_link_libraries(achclwss_automate PUBLIC achclwss::core PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_automate tests/test_automate.cpp)
  target_link_libraries(test_automate PRIVATE achclwss::automate achclwss::photo achclwss::vector achclwss::convert achclwss::mcp nlohmann_json::nlohmann_json achclwss_check)
  add_test(NAME automate COMMAND test_automate)

  # the Python client's tests, against the real achclwss binary
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME automate_python
             COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${PROJECT_SOURCE_DIR}/python/tests -t ${PROJECT_SOURCE_DIR}/python -v)
    set_tests_properties(automate_python PROPERTIES
      ENVIRONMENT "ACHCLWSS_BIN=$<TARGET_FILE:achclwss>;PYTHONPATH=${PROJECT_SOURCE_DIR}/python;PYTHONDONTWRITEBYTECODE=1")
  else()
    message(STATUS "python3 not found: skipping the Python client tests")
  endif()
endif()
