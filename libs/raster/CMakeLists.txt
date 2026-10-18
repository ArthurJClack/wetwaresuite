find_package(Threads REQUIRED)
add_library(wetwaresuite_raster src/image.cpp src/codec.cpp src/ops.cpp src/paint.cpp src/ora.cpp src/document.cpp src/parallel.cpp
            src/pyramid.cpp src/selection.cpp)
add_library(wetwaresuite::raster ALIAS wetwaresuite_raster)
target_include_directories(wetwaresuite_raster PUBLIC include PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
target_link_libraries(wetwaresuite_raster PUBLIC wetwaresuite::core PRIVATE Threads::Threads)
# no fused multiply-adds behind our back: the SIMD and scalar loops (and every platform) give the same bytes
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(wetwaresuite_raster PRIVATE -ffp-contract=off)
elseif(MSVC)
  target_compile_options(wetwaresuite_raster PRIVATE /fp:precise)
endif()

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_raster tests/test_raster.cpp)
  target_link_libraries(test_raster PRIVATE wetwaresuite::raster wetwaresuite_check)
  add_test(NAME raster COMMAND test_raster)
endif()
