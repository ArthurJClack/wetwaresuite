cmake_minimum_required(VERSION 3.21)
project(achclwss VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACHCLWSS_BUILD_TESTS "Build unit tests" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

if(ACHCLWSS_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)
add_subdirectory(mcp)
add_subdirectory(libs/raster)
add_subdirectory(libs/mesh)
add_subdirectory(libs/audio)
add_subdirectory(tools/toolkit)
add_subdirectory(tools/vector)
add_subdirectory(tools/blueprint)
add_subdirectory(tools/photo)
add_subdirectory(tools/paint)
add_subdirectory(tools/convert)
add_subdirectory(tools/forge)
add_subdirectory(tools/slicer)
add_subdirectory(tools/cad)
add_subdirectory(tools/audio)
add_subdirectory(apps/achclwss)
