add_library(wetwaresuite_mesh
  src/math.cpp src/mesh.cpp src/triangulate.cpp src/primitives.cpp src/csg.cpp src/ops.cpp
  src/io.cpp src/gltf.cpp src/fbx.cpp src/step.cpp src/threemf.cpp src/render.cpp src/document.cpp)
add_library(wetwaresuite::mesh ALIAS wetwaresuite_mesh)
target_sources(wetwaresuite_mesh PRIVATE src/topology.cpp)
target_include_directories(wetwaresuite_mesh PUBLIC include)
target_link_libraries(wetwaresuite_mesh PUBLIC wetwaresuite::core wetwaresuite::raster PRIVATE nlohmann_json::nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_mesh tests/test_mesh.cpp)
  target_link_libraries(test_mesh PRIVATE wetwaresuite::mesh wetwaresuite_check)
  target_compile_definitions(test_mesh PRIVATE WETWARESUITE_MESH_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME mesh COMMAND test_mesh)
endif()
