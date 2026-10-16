find_package(Threads REQUIRED)

add_library(achclwss_library src/catalog.cpp src/scan.cpp src/query.cpp src/thumbs.cpp src/commands.cpp)
add_library(achclwss::library ALIAS achclwss_library)
target_include_directories(achclwss_library PUBLIC include)
target_link_libraries(achclwss_library PUBLIC achclwss::core achclwss::raster
  PRIVATE achclwss::mesh achclwss::vector achclwss_toolkit nlohmann_json::nlohmann_json Threads::Threads)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_library tests/test_library.cpp)
  target_link_libraries(test_library PRIVATE achclwss::library achclwss::mesh achclwss::vector achclwss::mcp nlohmann_json::nlohmann_json achclwss_check)
  add_test(NAME library COMMAND test_library)
endif()
