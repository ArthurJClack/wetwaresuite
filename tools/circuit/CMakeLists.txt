add_library(achclwss_circuit src/library.cpp src/footprints.cpp src/schematic.cpp src/pcb.cpp src/route.cpp src/gerber.cpp
            src/render.cpp src/io.cpp src/sim.cpp src/cmd_file.cpp src/cmd_sch.cpp src/cmd_pcb.cpp)
add_library(achclwss::circuit ALIAS achclwss_circuit)
target_include_directories(achclwss_circuit PUBLIC include)
target_link_libraries(achclwss_circuit PUBLIC achclwss::core achclwss::raster achclwss::pdf PRIVATE nlohmann_json::nlohmann_json)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_circuit tests/test_circuit.cpp)
  target_link_libraries(test_circuit PRIVATE achclwss::circuit achclwss::mcp achclwss_check)
  add_test(NAME circuit COMMAND test_circuit)
endif()
