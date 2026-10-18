// The GUI's window-free logic, tested against made-up and real (photo, paint) commands.
#include <algorithm>

#include "check.hpp"
#include "wetwaresuite/gui/logic.hpp"
#include "wetwaresuite/paint/paint.hpp"
#include "wetwaresuite/photo/photo.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::gui;

namespace {

CommandInfo cmd(std::string id, std::string title, std::string description, std::vector<ParamSpec> params = {}) {
    return {std::move(id), std::move(title), std::move(description), std::move(params), true, false};
}

Field* fieldNamed(Form& f, const std::string& name) {
    for (auto& x : f.fields) {
        if (x.spec.name == name) return &x;
    }
    return nullptr;
}

void testSearch() {
    std::vector<CommandInfo> cmds = {
        cmd("photo.resize", "Resize image", "Scale an image to a new size."),
        cmd("photo.crop", "Crop", "Keep a rectangle of the image."),
        cmd("paint.stroke", "Brush stroke", "Paint a brush stroke through a list of points."),
        cmd("photo.adjust", "Adjust", "Brightness, contrast, saturation and more."),
    };
    auto hits = search("resize", cmds);
    CHECK(!hits.empty() && hits[0].command->id == "photo.resize");
    // a word only in the description still finds it
    hits = search("brightness", cmds);
    CHECK(hits.size() == 1 && hits[0].command->id == "photo.adjust");
    // letters in order: "brsh" finds "Brush stroke"
    hits = search("brsh", cmds);
    CHECK(!hits.empty() && hits[0].command->id == "paint.stroke");
    // every word must match
    CHECK(search("crop brightness", cmds).empty());
    CHECK(matchScore("zzz", cmds[0]) == 0);
    // case does not matter, title beats description
    CHECK(matchScore("CROP", cmds[1]) > matchScore("rectangle", cmds[1]));
    // empty query lists everything in order
    hits = search("", cmds);
    CHECK(hits.size() == cmds.size() && hits[0].command->id == "photo.resize");
    CHECK(search("", cmds, 2).size() == 2);
    // tool prefix finds the tool's commands
    hits = search("paint", cmds);
    CHECK(!hits.empty() && hits[0].command->id == "paint.stroke");
}

void testLabelsAndRanges() {
    CHECK(labelFor("max_size") == "Max size");
    CHECK(labelFor("doc") == "Document");
    CHECK(labelFor("output_dir") == "Output dir");
    CHECK(labelFor("dpi") == "DPI");
    CHECK(labelFor("svg_path") == "SVG path");
    double lo = 0, hi = 0;
    CHECK(parseRange("edge hardness 0..100 (overrides the preset)", lo, hi) && lo == 0 && hi == 100);
    CHECK(parseRange("JPEG quality 1..100", lo, hi) && lo == 1 && hi == 100);
    CHECK(parseRange("tilt -45.5..45.5 degrees", lo, hi) && lo == -45.5 && hi == 45.5);
    CHECK(!parseRange("width in px", lo, hi));
    CHECK(!parseRange("weird 5..1", lo, hi));
}

void testKinds() {
    CHECK(kindFor({"doc", ParamType::String, "id of an open image, e.g. img1"}, "photo.crop") == FieldKind::Document);
    CHECK(kindFor({"color", ParamType::String, "paint colour (#rrggbb, #rrggbbaa, rgb(r,g,b) or a name)"}, "paint.fill") == FieldKind::Color);
    CHECK(kindFor({"path", ParamType::String, "path to the image file"}, "photo.open") == FieldKind::OpenFile);
    CHECK(kindFor({"path", ParamType::String, "file to write, e.g. out.png"}, "photo.save") == FieldKind::SaveFile);
    CHECK(kindFor({"output", ParamType::String, "GIF file to write"}, "animate.export_gif") == FieldKind::SaveFile);
    CHECK(kindFor({"input_dir", ParamType::String, "folder with the source images"}, "photo.batch") == FieldKind::Folder);
    CHECK(kindFor({"filter", ParamType::String, "resampling filter", false, {"bilinear", "nearest"}}, "photo.resize") == FieldKind::Choice);
    CHECK(kindFor({"name", ParamType::String, "layer name"}, "paint.layer_add") == FieldKind::Text);
    CHECK(kindFor({"auto", ParamType::Bool, "auto contrast"}, "photo.levels") == FieldKind::Bool);
    CHECK(kindFor({"points", ParamType::NumberList, "x,y pairs"}, "paint.stroke") == FieldKind::NumberList);
    CHECK(kindFor({"width", ParamType::Int, "width"}, "photo.new") == FieldKind::Integer);
}

void testForms() {
    CommandInfo info = cmd("demo.go", "Go", "Does a thing.",
                           {{"doc", ParamType::String, "id of an open image, e.g. img1", true},
                            {"width", ParamType::Int, "width in px", true},
                            {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{90}},
                            {"amount", ParamType::Number, "strength 0..1", false},
                            {"mode", ParamType::String, "how", false, {"fast", "good"}, std::string("good")},
                            {"color", ParamType::String, "colour (#rrggbb)", false, {}, std::string("#ff0000")},
                            {"flip", ParamType::Bool, "mirror it", false, {}, false},
                            {"points", ParamType::NumberList, "x,y pairs", false},
                            {"preview", ParamType::Bool, "attach a PNG preview", false, {}, false}});
    Form form = buildForm(info, {"img1", "img2"}, "img2");
    CHECK(form.fields.size() == 9);
    CHECK(fieldNamed(form, "doc")->text == "img2");
    CHECK(fieldNamed(form, "width")->number == 1920);  // a sensible start for a required size
    CHECK(fieldNamed(form, "quality")->number == 90 && fieldNamed(form, "quality")->min == 1.0);
    CHECK(!fieldNamed(form, "amount")->set);  // optional, no default: the command decides
    CHECK(fieldNamed(form, "mode")->text == "good");
    CHECK(fieldNamed(form, "color")->kind == FieldKind::Color);
    CHECK(fieldNamed(form, "preview")->hidden);

    Params p;
    std::string error;
    CHECK(toParams(form, p, error));
    CHECK(std::get<std::string>(p.at("doc")) == "img2");
    CHECK(std::get<std::int64_t>(p.at("width")) == 1920);
    CHECK(std::get<std::int64_t>(p.at("quality")) == 90);
    CHECK(!p.count("amount") && !p.count("points") && !p.count("preview"));
    CHECK(std::get<bool>(p.at("flip")) == false);

    CHECK(setFieldText(*fieldNamed(form, "amount"), "0.5"));
    CHECK(setFieldText(*fieldNamed(form, "flip"), "yes"));
    CHECK(!setFieldText(*fieldNamed(form, "width"), "wide"));
    CHECK(!setFieldText(*fieldNamed(form, "mode"), "slow"));
    CHECK(setFieldText(*fieldNamed(form, "points"), "[10, 20; 30 40]"));
    CHECK(toParams(form, p, error));
    CHECK(std::get<double>(p.at("amount")) == 0.5);
    CHECK(std::get<bool>(p.at("flip")));
    CHECK((std::get<NumberList>(p.at("points")) == NumberList{10, 20, 30, 40}));

    fieldNamed(form, "points")->text = "10, twenty";
    CHECK(!toParams(form, p, error) && error.find("Points") != std::string::npos);
    fieldNamed(form, "points")->text.clear();
    fieldNamed(form, "doc")->text.clear();
    CHECK(!toParams(form, p, error) && error == "Please fill in \"Document\".");

    // no documents open: the document field starts empty
    Form empty = buildForm(info);
    CHECK(fieldNamed(empty, "doc")->text.empty() && !fieldNamed(empty, "doc")->set);
}

void testRealCommands() {
    Session session;
    photo::registerCommands(session);
    paint::registerCommands(session);
    const auto all = session.registry().list();

    auto photoPicks = beginnerCommands("photo", all);
    CHECK(photoPicks.size() == 8);
    CHECK(photoPicks[0] == "photo.open" && photoPicks[1] == "photo.new" && photoPicks[2] == "photo.save");
    CHECK(std::find(photoPicks.begin(), photoPicks.end(), "photo.resize") != photoPicks.end());
    auto paintPicks = beginnerCommands("paint", all);
    CHECK(!paintPicks.empty() && paintPicks.size() <= 8);
    CHECK(std::find(paintPicks.begin(), paintPicks.end(), "paint.stroke") != paintPicks.end());
    for (const auto& id : paintPicks) CHECK(id.rfind("paint.", 0) == 0);
    CHECK(toolCommands("paint", all).size() > paintPicks.size());

    // a form built from a real command runs through Session::run
    const CommandInfo* newInfo = session.registry().find("photo.new");
    Form f = buildForm(*newInfo);
    setFieldText(*fieldNamed(f, "width"), "64");
    setFieldText(*fieldNamed(f, "height"), "32");
    Params p;
    std::string error;
    CHECK(toParams(f, p, error));
    Result r = session.run("photo.new", p);
    CHECK(r.ok);
    const std::string doc = r.field("doc") ? toText(*r.field("doc")) : "";
    CHECK(doc == "img1");

    // the stroke form picks the open image and passes colour as hex
    Form stroke = buildForm(*session.registry().find("paint.stroke"), session.workspace().ids());
    CHECK(fieldNamed(stroke, "doc")->text == "img1");
    CHECK(fieldNamed(stroke, "color")->kind == FieldKind::Color);
    CHECK(fieldNamed(stroke, "hardness")->max == 100.0);
    setFieldText(*fieldNamed(stroke, "points"), "2,2, 30,20");
    CHECK(toParams(stroke, p, error));
    CHECK(session.run("paint.stroke", p).ok);
    CHECK(session.history().canUndo());

    // errors from the core read as plain words
    Result bad = session.run("photo.new", {{"width", std::int64_t{10}}});
    CHECK(!bad.ok && friendlyError(bad.message) == "Please fill in \"Height\".");

    // opening a .png tries photo first
    auto cands = openCandidates("/tmp/cat.PNG", {"paint", "photo"}, all);
    CHECK(!cands.empty() && cands[0] == "photo.open");
    CHECK(openCandidates("x.png", {"paint"}, all).empty());
}

void testColoursRecentMessages() {
    auto c = parseHex("#ff8000");
    CHECK(c && c->r == 1.0f && c->b == 0.0f && c->a == 1.0f);
    CHECK(parseHex("#f80") && toHex(*parseHex("#f80")) == "#ff8800");
    CHECK(toHex(*parseHex("#11223344")) == "#11223344");
    CHECK(parseHex("transparent")->a == 0.0f);
    CHECK(!parseHex("red") && !parseHex("#12345") && !parseHex("#gg0000"));
    CHECK(toHex(fromRgb24(0x3d7eff)) == "#3d7eff");

    std::vector<std::string> recent = {"a", "b", "c"};
    recent = pushRecent(recent, "b");
    CHECK((recent == std::vector<std::string>{"b", "a", "c"}));
    recent = pushRecent(recent, "d", 3);
    CHECK((recent == std::vector<std::string>{"d", "b", "a"}));
    CHECK((parseRecent("x\r\ny\n\nx\n") == std::vector<std::string>{"x", "y"}));
    CHECK(parseRecent(formatRecent(recent)) == recent);

    CHECK(friendlyError("missing parameter 'max_size' for photo.batch") == "Please fill in \"Max size\".");
    CHECK(friendlyError("parameter 'filter' for photo.resize must be one of: bilinear, nearest") ==
          "\"Filter\" has to be one of: bilinear, nearest.");
    CHECK(friendlyError("parameter 'width' for photo.new must be integer") == "\"Width\" needs a whole number.");
    CHECK(friendlyError("point is outside the image") == "Point is outside the image.");
    CHECK(sentence("grayscale applied") == "Grayscale applied." && sentence("Done!") == "Done!");
    Result r = Result::success("ok");
    r.with("doc", std::string("img1")).with("max_size", std::int64_t{5});
    CHECK((resultLines(r) == std::vector<std::string>{"Document: img1", "Max size: 5"}));
    CHECK(toolTitle("forge") == "Asset forge" && toolTitle("photo") == "Photo");
    CHECK(!toolBlurb("paint").empty() && !toolBlurb("unknown").empty());
}

}  // namespace

int main() {
    testSearch();
    testLabelsAndRanges();
    testKinds();
    testForms();
    testRealCommands();
    testColoursRecentMessages();
    return check::summary("gui_logic");
}
