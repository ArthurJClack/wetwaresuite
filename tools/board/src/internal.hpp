// Helpers shared by the board's source files (not part of the public API).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/board/board.hpp"
#include "wetwaresuite/pdf/fonts.hpp"

namespace wetwaresuite::board::detail {

constexpr double kLineHeight = 1.25;
constexpr double kPi = 3.14159265358979323846;

pdf::Font pdfFont(const std::string& font, bool bold, bool italic);

// Where an element's label goes: the inner box text is wrapped and aligned in.
Box textArea(const Element& e, const Resolved& style);

// One line of label text placed on the canvas (y = baseline).
struct PlacedLine {
    double x = 0, y = 0;
    std::string text;
};
std::vector<PlacedLine> placeText(const Element& e, const Resolved& style, const Box& area, bool wrap = true);

// Small deterministic random numbers (identical on every compiler).
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double uniform() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }  // [0,1)
    double range(double a, double b) { return a + (b - a) * uniform(); }
};
std::uint64_t hashOf(const std::string& s);

std::string lower(std::string s);
std::string trim(const std::string& s);
std::vector<std::string> splitLines(const std::string& text);
std::string fmtNum(double v);  // compact: no trailing zeros

// Polyline helpers.
double length(const std::vector<Pt>& pts);
Pt pointAt(const std::vector<Pt>& pts, double distance, Pt* direction = nullptr);
std::vector<Pt> flattenCubic(Pt a, Pt b, Pt c, Pt d, int steps);

// Command helpers shared by commands.cpp and commands_more.cpp.
ActionFactory boardCommandFactory(Session& session, std::function<Result(BoardDoc&, const Params&)> edit, bool undoable);
bool resolveIdList(const Board& b, const std::string& list, std::vector<std::string>& out, std::string& error);
bool unlockedElement(const Board& b, const Element& e, std::string& error);
std::string describeElement(const Board& b, const Element& e);
std::vector<ParamSpec> renderParamSpecs();
RenderOptions renderOptionsFrom(const Params& p);
std::unique_ptr<Action> createBoardAction(Session& s, std::function<Result(std::shared_ptr<BoardDoc>&)> make);

}  // namespace wetwaresuite::board::detail
