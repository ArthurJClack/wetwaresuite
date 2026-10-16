add_library(achclwss_office
  src/xml.cpp
  src/formula.cpp
  src/sheet.cpp
  src/sheetio.cpp
  src/doc.cpp
  src/docx.cpp
  src/doclayout.cpp
  src/deck.cpp
  src/chart.cpp
  src/office.cpp
  src/cmd_sheet.cpp
  src/cmd_doc.cpp
  src/cmd_deck.cpp
  src/cmd_pdf.cpp)
add_library(achclwss::office ALIAS achclwss_office)
target_include_directories(achclwss_office PUBLIC include)
target_link_libraries(achclwss_office PUBLIC achclwss::core achclwss::raster achclwss::pdf)

if(ACHCLWSS_BUILD_TESTS)
  add_executable(test_office tests/test_office.cpp)
  target_include_directories(test_office PRIVATE src)
  target_link_libraries(test_office PRIVATE achclwss::office achclwss::mcp nlohmann_json::nlohmann_json achclwss_check)
  add_test(NAME office COMMAND test_office)
endif()
