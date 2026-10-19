add_library(wetwaresuite_model
  src/poly.cpp src/primitives.cpp src/select.cpp src/extrude.cpp src/bevel.cpp src/cut.cpp src/topo.cpp
  src/subsurf.cpp src/uv.cpp)
add_library(wetwaresuite::model ALIAS wetwaresuite_model)
target_include_directories(wetwaresuite_model PUBLIC include)
target_link_libraries(wetwaresuite_model PUBLIC wetwaresuite::core wetwaresuite::mesh wetwaresuite::raster)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_model_ops tests/test_ops.cpp)
  target_link_libraries(test_model_ops PRIVATE wetwaresuite::model wetwaresuite_check)
  add_test(NAME model_ops COMMAND test_model_ops)
endif()
