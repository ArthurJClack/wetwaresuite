find_package(Threads REQUIRED)

add_library(wetwaresuite_automate src/script.cpp src/commands.cpp)
add_library(wetwaresuite::automate ALIAS wetwaresuite_automate)
target_include_directories(wetwaresuite_automate PUBLIC include)
target_link_libraries(wetwaresuite_automate PUBLIC wetwaresuite::core PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_automate tests/test_automate.cpp)
  target_link_libraries(test_automate PRIVATE wetwaresuite::automate wetwaresuite::photo wetwaresuite::vector wetwaresuite::convert wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME automate COMMAND test_automate)

  # the Python client's tests, against the real wetwaresuite binary
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME automate_python
             COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${PROJECT_SOURCE_DIR}/python/tests -t ${PROJECT_SOURCE_DIR}/python -v)
    set_tests_properties(automate_python PROPERTIES
      ENVIRONMENT "WETWARESUITE_BIN=$<TARGET_FILE:wetwaresuite>;PYTHONPATH=${PROJECT_SOURCE_DIR}/python;PYTHONDONTWRITEBYTECODE=1")
  else()
    message(STATUS "python3 not found: skipping the Python client tests")
  endif()
endif()
