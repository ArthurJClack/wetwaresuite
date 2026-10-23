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
  src/fillseries.cpp
  src/sheetio.cpp
  src/xlsx_common.cpp
  src/xlsx_read.cpp
  src/xlsx_write.cpp
  src/ods.cpp
  src/doc.cpp
  src/docx.cpp
  src/docx_write.cpp
  src/docmodel.cpp
  src/odt.cpp
  src/doclayout.cpp
  src/deck.cpp
  src/deck_show.cpp
  src/chart.cpp
  src/office.cpp
  src/cmd_sheet.cpp
  src/cmd_sheet_style.cpp
  src/cmd_sheet_rules.cpp
  src/cmd_sheet_objects.cpp
  src/cmd_sheet_data.cpp
  src/cmd_doc.cpp
  src/cmd_doc_format.cpp
  src/cmd_doc_table.cpp
  src/cmd_doc_review.cpp
  src/cmd_deck.cpp
  src/cmd_deck_objects.cpp
  src/cmd_deck_show.cpp
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
  add_executable(test_office_files tests/test_office_files.cpp)
  target_include_directories(test_office_files PRIVATE src)
  target_link_libraries(test_office_files PRIVATE wetwaresuite::office wetwaresuite_check)
  add_test(NAME office_files COMMAND test_office_files)
  add_executable(test_office_sheetcmds tests/test_office_sheetcmds.cpp)
  target_include_directories(test_office_sheetcmds PRIVATE src)
  target_link_libraries(test_office_sheetcmds PRIVATE wetwaresuite::office wetwaresuite_check)
  add_test(NAME office_sheetcmds COMMAND test_office_sheetcmds)
  add_executable(test_office_doccmds tests/test_office_doccmds.cpp)
  target_include_directories(test_office_doccmds PRIVATE src)
  target_link_libraries(test_office_doccmds PRIVATE wetwaresuite::office wetwaresuite_check)
  add_test(NAME office_doccmds COMMAND test_office_doccmds)
  add_executable(test_office_deckcmds tests/test_office_deckcmds.cpp)
  target_include_directories(test_office_deckcmds PRIVATE src)
  target_link_libraries(test_office_deckcmds PRIVATE wetwaresuite::office wetwaresuite_check)
  add_test(NAME office_deckcmds COMMAND test_office_deckcmds)
endif()
