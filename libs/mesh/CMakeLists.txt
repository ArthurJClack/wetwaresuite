add_library(achclwss_mesh
  src/math.cpp src/mesh.cpp src/triangulate.cpp src/primitives.cpp src/csg.cpp src/ops.cpp
  src/io.cpp src/gltf.cpp src/threemf.cpp src/render.cpp src/document.cpp)
add_library(achclwss::mesh ALIAS achclwss_mesh)
target_sources(achclwss_mesh PRIVATE src/topology.cpp)
target_include_directories(achclwss_mesh PUBLIC include)
target_link_libraries(achclwss_mesh PUBLIC achclwss::core achclwss::raster PRIVATE nlohmann_json::nlohmann_json)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_mesh tests/test_mesh.cpp)
  target_link_libraries(test_mesh PRIVATE achclwss::mesh achclwss_check)
  add_test(NAME mesh COMMAND test_mesh)
endif()
