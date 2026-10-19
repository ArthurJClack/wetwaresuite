add_library(wetwaresuite_office
  src/xml.cpp
  src/formula.cpp
  src/fn_math.cpp
  src/fn_stat.cpp
  src/fn_text.cpp
  src/fn_lookup.cpp
  src/fn_date.cpp
  src/sheetfmt.cpp
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
add_library(wetwaresuite::office ALIAS wetwaresuite_office)
target_include_directories(wetwaresuite_office PUBLIC include)
target_link_libraries(wetwaresuite_office PUBLIC wetwaresuite::core wetwaresuite::raster wetwaresuite::pdf)

if(WETWARESUITE_BUILD_TESTS)
  add_executable(test_office tests/test_office.cpp)
  target_include_directories(test_office PRIVATE src)
  target_link_libraries(test_office PRIVATE wetwaresuite::office wetwaresuite::mcp nlohmann_json::nlohmann_json wetwaresuite_check)
  add_test(NAME office COMMAND test_office)
  add_executable(test_office_calc tests/test_office_calc.cpp)
  target_include_directories(test_office_calc PRIVATE src)
  target_link_libraries(test_office_calc PRIVATE wetwaresuite::office wetwaresuite_check)
  add_test(NAME office_calc COMMAND test_office_calc)
endif()
