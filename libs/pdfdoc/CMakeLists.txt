# Roboto Medium, built in for text that looks the same everywhere (vector text, unicode PDF text)
set(_ww_sans_cpp ${CMAKE_CURRENT_BINARY_DIR}/builtin_sans.cpp)
set(_ww_sans_ttf ${PROJECT_SOURCE_DIR}/third_party/imgui/fonts/Roboto-Medium.ttf)
add_custom_command(OUTPUT ${_ww_sans_cpp}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${_ww_sans_ttf} -DOUTPUT=${_ww_sans_cpp} -DNAME=kBuiltinSans
          -DNAMESPACE=wetwaresuite::pdfdoc -P ${PROJECT_SOURCE_DIR}/gui/embed.cmake
  DEPENDS ${_ww_sans_ttf} ${PROJECT_SOURCE_DIR}/gui/embed.cmake)
add_library(wetwaresuite_pdfdoc src/writer.cpp src/fonts.cpp ${_ww_sans_cpp})
add_library(wetwaresuite::pdfdoc ALIAS wetwaresuite_pdfdoc)
target_include_directories(wetwaresuite_pdfdoc PUBLIC include PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
target_link_libraries(wetwaresuite_pdfdoc PUBLIC wetwaresuite::raster)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_pdfdoc tests/test_pdfdoc.cpp)
  target_link_libraries(test_pdfdoc PRIVATE wetwaresuite::pdfdoc wetwaresuite_check)
  add_test(NAME pdfdoc COMMAND test_pdfdoc)
endif()
