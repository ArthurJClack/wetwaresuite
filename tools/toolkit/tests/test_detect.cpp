#include <filesystem>
#include <fstream>

#include "achclwss/toolkit/detect.hpp"
#include "check.hpp"

using namespace achclwss;
using namespace std::string_view_literals;
using toolkit::detect;

int main() {
    // content wins over a wrong extension
    CHECK(detect("\x89PNG\r\n\x1a\n...."sv, "photo.jpg").format == "PNG image");
    CHECK(detect("\xff\xd8\xff\xe0"sv, "a.bin").tool == "photo");
    CHECK(detect("8BPS\0\x01"sv, "x").format == "Photoshop document");
    CHECK(detect("RIFF\0\0\0\0WAVEfmt "sv, "x").category == "audio");
    CHECK(detect("RIFF\0\0\0\0WEBPVP8 "sv, "x").format == "WebP image");
    CHECK(detect("\0\0\0\x18" "ftypheic"sv, "x").format == "HEIC image");
    CHECK(detect("\0\0\0\x18" "ftypisom"sv, "x").tool == "cut");
    CHECK(detect("\x1a\x45\xdf\xa3....webm"sv, "x").format == "WebM video");
    CHECK(detect("fLaC"sv, "x").format == "FLAC audio");
    CHECK(detect("ID3\x04"sv, "x").format == "MP3 audio");
    CHECK(detect("%PDF-1.7"sv, "x").tool == "office");
    CHECK(detect("ISO-10303-21;\nHEADER;"sv, "x").tool == "cad");
    CHECK(detect("solid cube\n facet normal 0 0 1"sv, "cube.stl").format == "STL mesh");
    CHECK(detect("glTF\x02\0\0\0"sv, "x").tool == "asset forge");
    CHECK(detect("SQLite format 3\0"sv, "x").category == "data");
    CHECK(detect("7z\xbc\xaf\x27\x1c"sv, "x").category == "archive");

    // containers are told apart by extension
    CHECK(detect("PK\x03\x04"sv, "report.docx").format == "Word document");
    CHECK(detect("PK\x03\x04"sv, "budget.xlsx").category == "spreadsheet");
    CHECK(detect("PK\x03\x04"sv, "print.3mf").tool == "cad");
    CHECK(detect("PK\x03\x04"sv, "stuff.zip").format == "ZIP archive");
    CHECK(detect("II*\0"sv, "IMG_0001.CR2").category == "raw");
    CHECK(detect("II*\0"sv, "scan.tif").format == "TIFF image");
    CHECK(detect("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1"sv, "old.xls").category == "spreadsheet");

    // text and unknowns
    CHECK(detect("<?xml version=\"1.0\"?>\n<svg xmlns=\"...\">"sv, "icon").format == "SVG drawing");
    CHECK(detect("{\"a\": 1}"sv, "config.json").category == "data");
    CHECK(detect("hello there\n"sv, "notes").format == "Plain text");
    CHECK(detect("\x01\x02\x03\0\x05"sv, "mystery").category == "binary");

    // file-based commands through a session
    auto dir = std::filesystem::temp_directory_path() / "achclwss_test_detect";
    std::filesystem::create_directories(dir);
    auto file = dir / "sample.flac";
    {
        std::ofstream out(file, std::ios::binary);
        out << "fLaC" << std::string(60, 'x');
    }
    Session session;
    toolkit::registerCommands(session.registry());

    Result inspect = session.run("toolkit.inspect", {{"path", file.string()}});
    CHECK(inspect.ok);
    CHECK(inspect.message.find("FLAC audio") != std::string::npos);
    CHECK(inspect.message.find("64 bytes") != std::string::npos);

    Result hex = session.run("toolkit.hex", {{"path", file.string()}, {"length", std::int64_t{16}}});
    CHECK(hex.ok);
    CHECK(hex.message.find("66 4c 61 43") != std::string::npos);
    CHECK(hex.message.find("|fLaCxxxxxxxxxxxx|") != std::string::npos);

    CHECK(!session.run("toolkit.hex", {{"path", file.string()}, {"length", std::int64_t{0}}}).ok);
    CHECK(!session.run("toolkit.inspect", {{"path", (dir / "nope").string()}}).ok);
    CHECK(session.history().labels().empty());  // read-only commands stay out of undo

    std::filesystem::remove_all(dir);
    return check::summary("toolkit");
}
