add_library(achclwss_audio
  src/buffer.cpp src/io.cpp src/decode_dr.cpp src/decode_vorbis.cpp src/internal.cpp src/dsp.cpp src/dynamics.cpp
  src/spectral.cpp src/effects.cpp src/analysis.cpp src/draw.cpp src/project.cpp src/document.cpp)
add_library(achclwss::audio ALIAS achclwss_audio)
target_include_directories(achclwss_audio PUBLIC include
  PRIVATE ${PROJECT_SOURCE_DIR}/third_party/dr_libs ${PROJECT_SOURCE_DIR}/third_party/stb_vorbis)
target_link_libraries(achclwss_audio PUBLIC achclwss::core achclwss::raster PRIVATE nlohmann_json::nlohmann_json)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_audio_lib tests/test_audio_lib.cpp)
  target_link_libraries(test_audio_lib PRIVATE achclwss::audio achclwss_check)
  add_test(NAME audio_lib COMMAND test_audio_lib)
endif()
