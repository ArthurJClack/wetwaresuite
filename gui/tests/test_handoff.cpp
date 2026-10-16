// The window's handoff model (Send to..., inbox banner, Open in <app>) from
// made-up app.* results: no window and no other app needed.
#include "check.hpp"
#include "wetwaresuite/gui/handoff.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::gui;

namespace {

Result targets(const std::string& list, const std::string& installed, const std::string& programs) {
    Result r = Result::success("targets");
    r.with("targets", list).with("installed", installed).with("programs", programs);
    return r;
}

void testLists() {
    CHECK((splitList("a, b,,c ,") == std::vector<std::string>{"a", "b", "c"}));
    CHECK(splitList("").empty());
    CHECK(appTitle("model") == "Model" && appTitle("cad") == "CAD" && appTitle("suite") == "wetwaresuite");
    Result r = Result::success("x");
    r.with("n", std::int64_t{3}).with("b", true);
    CHECK(fieldText(r, "n") == "3" && fieldText(r, "b") == "true" && fieldText(r, "missing").empty());
}

void testSendChoices() {
    auto c = sendChoices(targets("model,game,video,image", "game,image", "game"), "image");
    CHECK(c.size() == 3);  // never the app itself
    CHECK(c[0].app == "game" && c[0].installed && c[0].program);  // installed first
    CHECK(c[1].app == "model" && !c[1].installed && c[2].app == "video");
    CHECK(c[1].label == "Model");
    CHECK(sendChoices(Result::failure("no doc"), "image").empty());
    CHECK(sendChoices(targets("", "", ""), "image").empty());

    Result sent = Result::success("sent");
    sent.with("to", std::string("game")).with("opened", false);
    CHECK(launchAfterSend(sent, "image", c) == "game");
    Result openedHere = Result::success("sent");
    openedHere.with("to", std::string("game")).with("opened", true);
    CHECK(launchAfterSend(openedHere, "suite", c).empty());
    Result toModel = Result::success("sent");
    toModel.with("to", std::string("model")).with("opened", false);
    CHECK(launchAfterSend(toModel, "image", c).empty());  // no model program to start
    CHECK(launchAfterSend(Result::failure("x"), "image", c).empty());
}

void testInbox() {
    CHECK(!inboxBanner(Result::success("nothing", std::int64_t{0}).with("count", std::int64_t{0})).show);
    CHECK(!inboxBanner(Result::failure("broken")).show);
    Result one = Result::success("1");
    one.with("count", std::int64_t{1}).with("file", std::string("/h/handoff/brick.png")).with("from", std::string("image"));
    one.with("kind", std::string("texture"));
    auto b = inboxBanner(one);
    CHECK(b.show && b.text == "A texture from Image is waiting: brick.png." && b.button == "Open it");
    Result three = Result::success("3");
    three.with("count", std::int64_t{3}).with("file", std::string("C:\\h\\song.wav")).with("from", std::string("sound"));
    three.with("kind", std::string("audio"));
    CHECK(inboxBanner(three).text == "An audio file from Sound is waiting: song.wav (and 2 more).");
    Result plain = Result::success("1");
    plain.with("count", std::int64_t{1}).with("file", std::string("notes.txt")).with("kind", std::string("file"));
    CHECK(inboxBanner(plain).text == "A file is waiting: notes.txt.");
}

void testSuggestion() {
    Result w = Result::success("opens in model");
    w.with("app", std::string("model")).with("program", true);
    auto s = openSuggestion(w, "image", "/x/head.obj", false);
    CHECK(s.show && s.app == "model" && s.installed && s.button == "Open in Model");
    CHECK(s.text == "This app can't open head.obj: it's a file for the Model app.");
    CHECK(openSuggestion(w, "image", "/x/head.obj", true).text == "head.obj usually belongs in the Model app.");
    CHECK(!openSuggestion(w, "model", "/x/head.obj", true).show);  // already the right app
    CHECK(!openSuggestion(w, "suite", "/x/head.obj", false).show);  // the all-in-one has every app
    Result files = Result::success("files");
    files.with("app", std::string("files")).with("program", true);
    CHECK(!openSuggestion(files, "image", "a.bin", false).show);
    Result missing = Result::success("cad");
    missing.with("app", std::string("cad")).with("program", false);
    auto m = openSuggestion(missing, "image", "part.stl", false);
    CHECK(m.show && !m.installed && m.text.find("CAD isn't installed") != std::string::npos);
}

}  // namespace

int main() {
    testLists();
    testSendChoices();
    testInbox();
    testSuggestion();
    return check::summary("gui_handoff");
}
