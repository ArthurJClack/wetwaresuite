add_library(wetwaresuite_audio
  src/buffer.cpp src/io.cpp src/decode_dr.cpp src/decode_vorbis.cpp src/internal.cpp src/dsp.cpp src/dynamics.cpp
  src/spectral.cpp src/effects.cpp src/analysis.cpp src/draw.cpp src/project.cpp src/document.cpp
  src/rack.cpp src/repair.cpp)
add_library(wetwaresuite::audio ALIAS wetwaresuite_audio)
target_include_directories(wetwaresuite_audio PUBLIC include
  PRIVATE ${PROJECT_SOURCE_DIR}/third_party/dr_libs ${PROJECT_SOURCE_DIR}/third_party/stb_vorbis)
target_link_libraries(wetwaresuite_audio PUBLIC wetwaresuite::core wetwaresuite::raster PRIVATE nlohmann_json::nlohmann_json)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_audio_lib tests/test_audio_lib.cpp)
  target_link_libraries(test_audio_lib PRIVATE wetwaresuite::audio wetwaresuite_check)
  add_test(NAME audio_lib COMMAND test_audio_lib)
  add_executable(test_audio_rack tests/test_rack.cpp)
  target_link_libraries(test_audio_rack PRIVATE wetwaresuite::audio wetwaresuite_check)
  add_test(NAME audio_rack COMMAND test_audio_rack)
endif()
