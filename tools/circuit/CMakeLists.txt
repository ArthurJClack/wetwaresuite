add_library(wetwaresuite_circuit src/library.cpp src/footprints.cpp src/schematic.cpp src/pcb.cpp src/route.cpp src/gerber.cpp
            src/render.cpp src/io.cpp src/sim.cpp src/cmd_file.cpp src/cmd_sch.cpp src/cmd_pcb.cpp)
add_library(wetwaresuite::circuit ALIAS wetwaresuite_circuit)
target_include_directories(wetwaresuite_circuit PUBLIC include)
target_link_libraries(wetwaresuite_circuit PUBLIC wetwaresuite::core wetwaresuite::raster wetwaresuite::pdf PRIVATE nlohmann_json::nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_circuit tests/test_circuit.cpp)
  target_link_libraries(test_circuit PRIVATE wetwaresuite::circuit wetwaresuite::mcp wetwaresuite_check)
  add_test(NAME circuit COMMAND test_circuit)
endif()
