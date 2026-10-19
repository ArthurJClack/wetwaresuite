// The sound app's own panels, for audio projects and songs:
//   - Effects: the rack of the picked track or the master, like the device chain
//     in Live or the effects rack in Audition: add, reorder, bypass, remove, and
//     a slider (or a list) per setting.
//   - Envelope: the automation of volume, pan or any effect setting as points on
//     a graph: click to add, drag to move, right-click to delete.
//   - Piano roll (songs): the notes of a pattern on a pitch/beat grid: double-click
//     to add, drag to move, drag the right edge to resize, right-click to delete.
// The panels only read through read-only list commands and change things through
// audio.* / beats.* commands, so everything undoes, scripts and works over MCP.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "shell.hpp"
#include "tool_panels.hpp"

namespace wetwaresuite::gui {

namespace {

double toNum(const std::string& s, double fallback = 0) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
std::string noteText(int midi) { return std::string(kNoteNames[((midi % 12) + 12) % 12]) + std::to_string(midi / 12 - 1); }

struct FxSetting {
    std::string name, unit;
    double value = 0, min = 0, max = 1;
    std::vector<std::string> choices;
};

struct FxSlot {
    int index = 0;
    std::string type;
    bool bypass = false;
    std::vector<FxSetting> settings;
};

struct EnvPoint {
    double time = 0, value = 0;
};

struct SoundState {
    std::string doc;
    std::uint64_t seen = ~0ull;
    std::string owner = "master";  // "master" or a track number
    std::vector<std::pair<std::string, std::string>> owners;  // id, label
    std::vector<FxSlot> rack;
    std::map<std::string, std::vector<EnvPoint>> lanes;  // target -> points
    double length = 10;  // seconds (projects) or beats (songs) shown on the envelope
    std::string target = "volume";
    std::vector<EnvPoint> editing;  // the lane while a point is dragged
    int dragPoint = -1;
    bool dragMoved = false;
    int addType = 0;
    std::map<std::string, float> sliders;  // a slider's value while it's dragged
};

struct Roll {
    std::string doc, pattern;
    std::uint64_t seen = ~0ull;
    std::vector<std::string> patterns;
    struct N {
        int pitch = 60;
        double start = 0, length = 1;
        int velocity = 100;
    };
    std::vector<N> notes;
    int bars = 1;
    double beatsPerBar = 4;
    int selected = -1;
    enum class Drag { None, Move, Resize } drag = Drag::None;
    int dragNote = -1;
    double grabBeat = 0, newStart = 0, newLength = 1;
    int grabPitch = 0, newPitch = 0;
    double lastLength = 1;
    float pxPerBeat = 48;
    float velocity = 100;
};

SoundState& sound() {
    static SoundState s;
    return s;
}

Roll& roll() {
    static Roll r;
    return r;
}

double snapTo(double v, double grid) { return std::round(v / grid) * grid; }

}  // namespace

// ---- effects rack and envelope (right panel) ----------------------------------------

void Shell::soundPanel() {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return;
    const bool song = doc->kind() == "song";
    if (!song && doc->kind() != "audio project") return;
    const std::string tool = song ? "beats" : "audio";
    if (!find(tool + ".fx_list") || !find(tool + ".fx_add")) return;
    const std::string docKey = song ? "song" : "project";
    SoundState& st = sound();
    if (st.doc != currentDoc_) {
        st = SoundState{};
        st.doc = currentDoc_;
    }

    // which rack: the master or a track
    auto ownerParams = [&](Params p) {
        p[docKey] = currentDoc_;
        if (st.owner == "master") {
            p["master"] = true;
        } else if (song) {
            p["track"] = st.owner;
        } else {
            p["track"] = static_cast<std::int64_t>(std::atoi(st.owner.c_str()));
        }
        return p;
    };
    auto act = [&](const std::string& command, Params p, const char* title) {
        if (!find(command)) return false;
        Result r = run(command, ownerParams(std::move(p)));
        if (!r.ok) {
            resultFor_ = title;
            r.message = friendlyError(r.message);
            result_ = std::move(r);
            return false;
        }
        canvas_.dirty = true;
        return true;
    };

    if (st.seen != changes_) {
        st.seen = changes_;
        st.owners.clear();
        st.owners.emplace_back("master", "Master");
        TimelineLayout layout;
        double end = 0;
        if (doc->timeline(layout)) {
            for (const auto& t : layout.tracks) {
                st.owners.emplace_back(t.id, t.id + ": " + t.name);
                for (const auto& c : t.clips) end = std::max(end, c.start + c.length);
            }
        }
        if (std::none_of(st.owners.begin(), st.owners.end(), [&](const auto& o) { return o.first == st.owner; })) st.owner = "master";
        st.rack.clear();
        for (const auto& row : tp::rows(query(tool + ".fx_list", ownerParams({})))) {
            if (row.size() < 9) continue;
            const int i = std::atoi(row[1].c_str());
            if (st.rack.empty() || st.rack.back().index != i) {
                FxSlot slot;
                slot.index = i;
                slot.type = row[2];
                slot.bypass = row[3] == "1";
                st.rack.push_back(slot);
            }
            FxSetting s;
            s.name = row[4];
            s.value = toNum(row[5]);
            s.min = toNum(row[6]);
            s.max = toNum(row[7], 1);
            s.unit = row[8];
            if (row.size() > 9 && !row[9].empty()) {
                std::size_t pos = 0;
                while (pos <= row[9].size()) {
                    const std::size_t bar = std::min(row[9].find('|', pos), row[9].size());
                    s.choices.push_back(row[9].substr(pos, bar - pos));
                    pos = bar + 1;
                }
            }
            st.rack.back().settings.push_back(std::move(s));
        }
        st.lanes.clear();
        Result lanes = query(tool + ".automation", ownerParams({}));
        for (const auto& row : tp::rows(lanes))
            if (row.size() >= 4) st.lanes[row[1]].push_back({toNum(row[2]), toNum(row[3])});
        st.length = song ? std::max(4.0, tp::number(lanes, "length", 16)) : std::max(1.0, end);
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Effects", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::PushID("sound");
    const float unit = ImGui::GetFontSize();

    std::string ownerLabel = "Master";
    for (const auto& o : st.owners)
        if (o.first == st.owner) ownerLabel = o.second;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##owner", ownerLabel.c_str())) {
        for (const auto& o : st.owners) {
            if (ImGui::Selectable(o.second.c_str(), o.first == st.owner)) {
                st.owner = o.first;
                st.seen = ~0ull;
                st.target = "volume";
            }
        }
        ImGui::EndCombo();
    }

    // ---- the rack ----
    if (st.rack.empty()) tp::muted("No effects yet: add one below.");
    for (std::size_t k = 0; k < st.rack.size(); ++k) {
        const FxSlot& fx = st.rack[k];
        ImGui::PushID(fx.index);
        const std::string label = std::to_string(fx.index) + "  " + fx.type + (fx.bypass ? "  (off)" : "");
        const bool open = ImGui::TreeNodeEx("##fx", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap, "%s", label.c_str());
        ImGui::SameLine(std::max(ImGui::GetContentRegionAvail().x - unit * 7.5f, unit * 6));
        bool on = !fx.bypass;
        if (ImGui::Checkbox("##on", &on)) act(tool + ".fx_set", {{"fx", static_cast<std::int64_t>(fx.index)}, {"bypass", !on}}, "Effect on/off");
        ImGui::SetItemTooltip("On (unticked: bypassed)");
        ImGui::SameLine();
        ImGui::BeginDisabled(k == 0 || !find(tool + ".fx_move"));
        if (ImGui::ArrowButton("##up", ImGuiDir_Up))
            act(tool + ".fx_move", {{"fx", static_cast<std::int64_t>(fx.index)}, {"to", static_cast<std::int64_t>(fx.index - 1)}}, "Move effect");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(k + 1 >= st.rack.size() || !find(tool + ".fx_move"));
        if (ImGui::ArrowButton("##down", ImGuiDir_Down))
            act(tool + ".fx_move", {{"fx", static_cast<std::int64_t>(fx.index)}, {"to", static_cast<std::int64_t>(fx.index + 1)}}, "Move effect");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) act(tool + ".fx_remove", {{"fx", static_cast<std::int64_t>(fx.index)}}, "Remove effect");
        ImGui::SetItemTooltip("Remove this effect");
        if (open) {
            for (const auto& s : fx.settings) {
                ImGui::PushID(s.name.c_str());
                const std::string key = st.owner + "/" + std::to_string(fx.index) + "/" + s.name;
                auto set = [&](double v) { act(tool + ".fx_set", {{"fx", static_cast<std::int64_t>(fx.index)}, {"values", s.name + "=" + fmt(v)}}, "Effect setting"); };
                ImGui::SetNextItemWidth(-unit * 5.5f);
                if (!s.choices.empty()) {
                    const int cur = std::clamp(static_cast<int>(std::lround(s.value - s.min)), 0, static_cast<int>(s.choices.size()) - 1);
                    if (ImGui::BeginCombo(s.name.c_str(), s.choices[static_cast<std::size_t>(cur)].c_str())) {
                        for (std::size_t c = 0; c < s.choices.size(); ++c)
                            if (ImGui::Selectable(s.choices[c].c_str(), static_cast<int>(c) == cur)) set(s.min + static_cast<double>(c));
                        ImGui::EndCombo();
                    }
                } else {
                    auto it = st.sliders.find(key);
                    float v = it != st.sliders.end() ? it->second : static_cast<float>(s.value);
                    const bool wide = s.min > 0 && s.max / s.min >= 50;  // Hz, ms: drag on a log scale
                    const std::string format = "%.4g" + (s.unit.empty() ? std::string() : " " + s.unit);
                    if (ImGui::SliderFloat(s.name.c_str(), &v, static_cast<float>(s.min), static_cast<float>(s.max), format.c_str(),
                                           wide ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None))
                        st.sliders[key] = v;
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        st.sliders.erase(key);
                        set(v);
                    } else if (!ImGui::IsItemActive()) {
                        st.sliders.erase(key);
                    }
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (const CommandInfo* add = find(tool + ".fx_add")) {
        std::vector<std::string> types;
        for (const auto& p : add->params)
            if (p.name == "type") types = p.choices;
        if (!types.empty()) {
            st.addType = std::clamp(st.addType, 0, static_cast<int>(types.size()) - 1);
            ImGui::SetNextItemWidth(-unit * 4.5f);
            if (ImGui::BeginCombo("##addtype", types[static_cast<std::size_t>(st.addType)].c_str())) {
                for (std::size_t i = 0; i < types.size(); ++i)
                    if (ImGui::Selectable(types[i].c_str(), static_cast<int>(i) == st.addType)) st.addType = static_cast<int>(i);
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Add", {-1, 0})) act(tool + ".fx_add", {{"type", types[static_cast<std::size_t>(st.addType)]}}, "Add effect");
        }
    }

    // ---- envelope ----
    if (find(tool + ".automate")) {
        ImGui::Spacing();
        tp::title("Envelope", 1.0f);
        struct Target {
            std::string id;
            double lo, hi;
        };
        std::vector<Target> targets = {{"volume", -60, 12}};
        if (st.owner != "master" || !song) targets.push_back({"pan", -1, 1});
        for (const auto& fx : st.rack)
            for (const auto& s : fx.settings)
                if (s.choices.empty()) targets.push_back({"fx" + std::to_string(fx.index) + "." + s.name, s.min, s.max});
        auto tgt = std::find_if(targets.begin(), targets.end(), [&](const Target& t) { return t.id == st.target; });
        if (tgt == targets.end()) {
            st.target = "volume";
            tgt = targets.begin();
        }
        ImGui::SetNextItemWidth(-unit * 4.5f);
        if (ImGui::BeginCombo("##target", (st.target + (st.lanes.count(st.target) ? "  *" : "")).c_str())) {
            for (const auto& t : targets)
                if (ImGui::Selectable((t.id + (st.lanes.count(t.id) ? "  *" : "")).c_str(), t.id == st.target)) st.target = t.id, st.dragPoint = -1;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!st.lanes.count(st.target));
        if (ImGui::Button("Clear", {-1, 0})) act(tool + ".automate", {{"target", st.target}, {"mode", std::string("clear")}}, "Clear automation");
        ImGui::EndDisabled();

        const double lo = tgt->lo, hi = tgt->hi;
        std::vector<EnvPoint> pts = st.dragPoint >= 0 ? st.editing : (st.lanes.count(st.target) ? st.lanes[st.target] : std::vector<EnvPoint>{});
        const ImVec2 size{std::max(unit * 8, ImGui::GetContentRegionAvail().x), unit * 7};
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##env", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg), line = ImGui::GetColorU32(ImGuiCol_PlotLines), dot = ImGui::GetColorU32(ImGuiCol_SliderGrabActive),
                    grid = ImGui::GetColorU32(ImGuiCol_Border);
        dl->AddRectFilled(o, {o.x + size.x, o.y + size.y}, bg, 3);
        auto X = [&](double t) { return o.x + static_cast<float>(t / st.length) * size.x; };
        auto Y = [&](double v) { return o.y + size.y - static_cast<float>((v - lo) / std::max(1e-9, hi - lo)) * size.y; };
        auto toTime = [&](float x) { return std::clamp(static_cast<double>(x - o.x) / size.x * st.length, 0.0, st.length); };
        auto toValue = [&](float y) { return std::clamp(lo + (hi - lo) * static_cast<double>(o.y + size.y - y) / size.y, lo, hi); };
        if (song)
            for (double b = 4; b < st.length; b += 4) dl->AddLine({X(b), o.y}, {X(b), o.y + size.y}, grid);
        if (st.target == "volume" || st.target == "pan") dl->AddLine({o.x, Y(0)}, {o.x + size.x, Y(0)}, grid);
        if (pts.empty()) {
            dl->AddText({o.x + 6, o.y + 4}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "click to add a point");
        } else {
            dl->AddLine({o.x, Y(pts.front().value)}, {X(pts.front().time), Y(pts.front().value)}, line, 2);
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) dl->AddLine({X(pts[i].time), Y(pts[i].value)}, {X(pts[i + 1].time), Y(pts[i + 1].value)}, line, 2);
            dl->AddLine({X(pts.back().time), Y(pts.back().value)}, {o.x + size.x, Y(pts.back().value)}, line, 2);
            for (const auto& p : pts) dl->AddCircleFilled({X(p.time), Y(p.value)}, unit * 0.3f, dot);
        }
        const ImVec2 m = ImGui::GetIO().MousePos;
        int near = -1;
        for (std::size_t i = 0; i < pts.size(); ++i)
            if (std::fabs(X(pts[i].time) - m.x) < unit * 0.5f && std::fabs(Y(pts[i].value) - m.y) < unit * 0.5f) near = static_cast<int>(i);
        auto commit = [&](std::vector<EnvPoint> v) {
            std::sort(v.begin(), v.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.time < b.time; });
            if (v.empty()) {
                act(tool + ".automate", {{"target", st.target}, {"mode", std::string("clear")}}, "Automation");
                return;
            }
            NumberList flat;
            for (const auto& p : v) flat.push_back(p.time), flat.push_back(p.value);
            act(tool + ".automate", {{"target", st.target}, {"points", flat}}, "Automation");
        };
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (near < 0) {
                pts.push_back({toTime(m.x), toValue(m.y)});
                commit(pts);
            } else {
                st.editing = pts;
                st.dragPoint = near;
                st.dragMoved = false;
            }
        }
        if (st.dragPoint >= 0 && st.dragPoint < static_cast<int>(st.editing.size())) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                st.editing[static_cast<std::size_t>(st.dragPoint)] = {toTime(m.x), toValue(m.y)};
                st.dragMoved = true;
            } else {
                if (st.dragMoved) commit(st.editing);
                st.dragPoint = -1;
            }
        }
        if (hovered && near >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            pts.erase(pts.begin() + near);
            commit(pts);
        }
        if (hovered) {
            const double t = near >= 0 ? pts[static_cast<std::size_t>(near)].time : toTime(m.x);
            const double v = near >= 0 ? pts[static_cast<std::size_t>(near)].value : toValue(m.y);
            ImGui::SetTooltip("%s %.3g, %s %.3g%s", song ? "beat" : "time", t, st.target.c_str(), v, near >= 0 ? "\nright-click to delete" : "");
        }
        tp::muted(song ? "Across the song, in beats." : "Across the project, in seconds.");
    }
    ImGui::PopID();
}

// ---- piano roll (songs, above the arrangement) ---------------------------------------

void Shell::pianoRollView() {
    Roll& r = roll();
    if (r.doc != currentDoc_) {
        r = Roll{};
        r.doc = currentDoc_;
    }
    const float unit = ImGui::GetFontSize();
    auto act = [&](const std::string& command, Params p, const char* title) {
        if (!find(command)) return false;
        p["song"] = currentDoc_;
        p["pattern"] = r.pattern;
        Result res = run(command, p);
        if (!res.ok) {
            resultFor_ = title;
            res.message = friendlyError(res.message);
            result_ = std::move(res);
            return false;
        }
        return true;
    };
    if (r.seen != changes_) {
        r.seen = changes_;
        r.patterns.clear();
        const std::string names = tp::text(query("beats.info", {{"song", currentDoc_}}), "pattern_names");
        std::size_t pos = 0;
        while (pos < names.size()) {
            std::size_t comma = names.find(", ", pos);
            if (comma == std::string::npos) comma = names.size();
            r.patterns.push_back(names.substr(pos, comma - pos));
            pos = comma + 2;
        }
        if (std::find(r.patterns.begin(), r.patterns.end(), r.pattern) == r.patterns.end()) r.pattern = r.patterns.empty() ? "" : r.patterns.front();
        r.notes.clear();
        if (!r.pattern.empty()) {
            Result q = query("beats.notes", {{"song", currentDoc_}, {"pattern", r.pattern}});
            for (const auto& row : tp::rows(q))
                if (row.size() >= 6) r.notes.push_back({std::atoi(row[1].c_str()), toNum(row[3]), toNum(row[4], 1), std::atoi(row[5].c_str())});
            r.bars = std::max(1, static_cast<int>(tp::number(q, "bars", 1)));
            r.beatsPerBar = std::max(1.0, tp::number(q, "beats_per_bar", 4));
        }
        if (r.selected >= static_cast<int>(r.notes.size())) r.selected = -1;
        if (r.selected >= 0) r.velocity = static_cast<float>(r.notes[static_cast<std::size_t>(r.selected)].velocity);
    }

    // ---- toolbar ----
    ImGui::SetNextItemWidth(unit * 9);
    if (ImGui::BeginCombo("##pattern", r.pattern.empty() ? "(no patterns)" : r.pattern.c_str())) {
        for (const auto& p : r.patterns)
            if (ImGui::Selectable(p.c_str(), p == r.pattern)) r.pattern = p, r.seen = ~0ull, r.selected = -1;
        ImGui::EndCombo();
    }
    if (r.pattern.empty()) {
        ImGui::SameLine();
        tp::muted("Add a pattern (beats.pattern_add) to edit its notes here.");
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Quantize")) act("beats.note_quantize", {{"grid", 0.25}}, "Quantize");
    ImGui::SetItemTooltip("Snap every note start to 16ths");
    ImGui::SameLine();
    if (ImGui::Button("-12")) act("beats.note_transpose", {{"semitones", std::int64_t{-12}}}, "Transpose");
    ImGui::SameLine();
    if (ImGui::Button("+12")) act("beats.note_transpose", {{"semitones", std::int64_t{12}}}, "Transpose");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 5);
    ImGui::SliderFloat("##zoom", &r.pxPerBeat, 16, 160, "zoom %.0f");
    if (r.selected >= 0) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 7);
        ImGui::SliderFloat("##vel", &r.velocity, 1, 127, "velocity %.0f");
        if (ImGui::IsItemDeactivatedAfterEdit())
            act("beats.note_set", {{"notes", NumberList{static_cast<double>(r.selected)}}, {"velocity", static_cast<std::int64_t>(std::lround(r.velocity))}}, "Velocity");
        ImGui::SameLine();
        if (ImGui::Button("Delete") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete))) {
            if (act("beats.note_remove", {{"notes", NumberList{static_cast<double>(r.selected)}}}, "Delete note")) r.selected = -1;
        }
    }

    // ---- the grid ----
    int lo = 48, hi = 84;
    if (!r.notes.empty()) {
        lo = 127, hi = 0;
        for (const auto& n : r.notes) lo = std::min(lo, n.pitch), hi = std::max(hi, n.pitch);
        lo = std::max(0, lo - 7);
        hi = std::min(127, std::max(hi + 7, lo + 24));
    }
    const float rowH = std::max(8.0f, unit * 0.75f), keyW = unit * 3;
    const double beats = r.bars * r.beatsPerBar;
    const ImVec2 size{keyW + static_cast<float>(beats) * r.pxPerBeat + unit, (hi - lo + 1) * rowH};
    ImGui::BeginChild("##rollscroll", {0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##roll", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 white = ImGui::GetColorU32(ImGuiCol_FrameBg), black = ImGui::GetColorU32(ImGuiCol_WindowBg), grid = ImGui::GetColorU32(ImGuiCol_Border),
                bar = ImGui::GetColorU32(ImGuiCol_Text, 0.35f), note = ImGui::GetColorU32(ImGuiCol_PlotHistogram), sel = ImGui::GetColorU32(ImGuiCol_PlotHistogramHovered),
                text = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    auto rowY = [&](int pitch) { return o.y + (hi - pitch) * rowH; };
    auto beatX = [&](double b) { return o.x + keyW + static_cast<float>(b) * r.pxPerBeat; };
    for (int p = lo; p <= hi; ++p) {
        const int pc = p % 12;
        const bool blackKey = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        dl->AddRectFilled({o.x, rowY(p)}, {o.x + size.x, rowY(p) + rowH}, blackKey ? black : white);
        if (pc == 0) dl->AddText({o.x + 2, rowY(p) + (rowH - unit) * 0.5f}, text, noteText(p).c_str());
        dl->AddLine({o.x, rowY(p) + rowH}, {o.x + size.x, rowY(p) + rowH}, grid);
    }
    for (int q = 0; q <= static_cast<int>(beats * 4); ++q) {
        const double b = q * 0.25;
        const bool isBar = std::fabs(std::fmod(b, r.beatsPerBar)) < 1e-9, isBeat = q % 4 == 0;
        if (!isBeat && r.pxPerBeat < 40) continue;
        dl->AddLine({beatX(b), o.y}, {beatX(b), o.y + size.y}, isBar ? bar : grid, isBar ? 2.0f : 1.0f);
    }
    dl->AddLine({o.x + keyW, o.y}, {o.x + keyW, o.y + size.y}, bar);

    const ImVec2 m = ImGui::GetIO().MousePos;
    const double mouseBeat = std::clamp(static_cast<double>((m.x - o.x - keyW) / r.pxPerBeat), 0.0, beats);
    const int mousePitch = std::clamp(hi - static_cast<int>(std::floor((m.y - o.y) / rowH)), lo, hi);
    int hit = -1;
    bool edge = false;
    for (std::size_t i = 0; i < r.notes.size(); ++i) {
        const auto& n = r.notes[i];
        if (n.pitch == mousePitch && m.x >= beatX(n.start) && m.x <= beatX(n.start + n.length) + 2) {
            hit = static_cast<int>(i);
            edge = m.x >= beatX(n.start + n.length) - 6;
        }
    }
    for (std::size_t i = 0; i < r.notes.size(); ++i) {
        auto n = r.notes[i];
        if (static_cast<int>(i) == r.dragNote && r.drag != Roll::Drag::None) n.start = r.newStart, n.pitch = r.newPitch, n.length = r.newLength;
        const ImVec2 a{beatX(n.start) + 1, rowY(n.pitch) + 1}, b{beatX(n.start + n.length) - 1, rowY(n.pitch) + rowH - 1};
        const float shade = 0.45f + 0.55f * n.velocity / 127.0f;
        dl->AddRectFilled(a, b, static_cast<int>(i) == r.selected ? sel : (note & 0x00ffffffu) | (static_cast<ImU32>(255 * shade) << 24), 2);
        dl->AddRect(a, b, text, 2);
    }
    if (hovered) ImGui::SetMouseCursor(edge ? ImGuiMouseCursor_ResizeEW : hit >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hit < 0 && mouseBeat < beats) {
        act("beats.note_add", {{"pitch", std::to_string(mousePitch)}, {"start", std::floor(mouseBeat * 4) / 4}, {"length", r.lastLength}}, "Add note");
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hit >= 0) {
        const auto& n = r.notes[static_cast<std::size_t>(hit)];
        r.selected = r.dragNote = hit;
        r.velocity = static_cast<float>(n.velocity);
        r.drag = edge ? Roll::Drag::Resize : Roll::Drag::Move;
        r.grabBeat = mouseBeat;
        r.grabPitch = mousePitch;
        r.newStart = n.start, r.newPitch = n.pitch, r.newLength = n.length;
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        r.selected = -1;
    }
    if (r.drag != Roll::Drag::None && r.dragNote >= 0 && r.dragNote < static_cast<int>(r.notes.size())) {
        const auto& n = r.notes[static_cast<std::size_t>(r.dragNote)];
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (r.drag == Roll::Drag::Move) {
                r.newStart = std::max(0.0, snapTo(n.start + mouseBeat - r.grabBeat, 0.25));
                r.newPitch = std::clamp(n.pitch + mousePitch - r.grabPitch, 0, 127);
            } else {
                r.newLength = std::max(0.25, snapTo(n.length + mouseBeat - r.grabBeat, 0.25));
            }
        } else {
            const NumberList which{static_cast<double>(r.dragNote)};
            if (r.drag == Roll::Drag::Move && (std::fabs(r.newStart - n.start) > 1e-9 || r.newPitch != n.pitch))
                act("beats.note_set", {{"notes", which}, {"start", r.newStart}, {"pitch", std::to_string(r.newPitch)}}, "Move note");
            if (r.drag == Roll::Drag::Resize && std::fabs(r.newLength - n.length) > 1e-9) {
                if (act("beats.note_set", {{"notes", which}, {"length", r.newLength}}, "Resize note")) r.lastLength = r.newLength;
            }
            r.drag = Roll::Drag::None;
            r.dragNote = -1;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit >= 0) {
        if (act("beats.note_remove", {{"notes", NumberList{static_cast<double>(hit)}}}, "Delete note")) r.selected = -1;
    }
    if (hovered) {
        ImGui::SetTooltip("%s, beat %.2f%s", noteText(mousePitch).c_str(), mouseBeat,
                          hit >= 0 ? "\ndrag to move, drag the end to resize, right-click to delete" : "\ndouble-click to add a note");
    }
    (void)muted;
    ImGui::EndChild();
}

}  // namespace wetwaresuite::gui
