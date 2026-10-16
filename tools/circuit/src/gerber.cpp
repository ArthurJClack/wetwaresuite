// Fabrication output: Gerber RS-274X with X2 file attributes, and an Excellon drill file.
// Board y runs down; Gerber y runs up, so every y is negated (the picture is unchanged).
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

#include "achclwss/circuit/circuit.hpp"
#include "geom.hpp"

namespace achclwss::circuit {

namespace {

std::string coord(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld", static_cast<long long>(std::llround(v * 1e6)));
    return b;
}

std::string dec(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}

class Gerber {
public:
    Gerber(const std::string& function, const std::string& polarityNote = "Positive") {
        head_ << "%TF.GenerationSoftware,achclwss,circuit,1.0*%\n";
        head_ << "%TF.FileFunction," << function << "*%\n";
        head_ << "%TF.FilePolarity," << polarityNote << "*%\n";
        head_ << "%FSLAX46Y46*%\n%MOMM*%\n%LPD*%\nG01*\n";
    }
    int aperture(const std::string& def) {
        auto it = apertures_.find(def);
        if (it != apertures_.end()) return it->second;
        const int code = 10 + static_cast<int>(apertures_.size());
        apertures_[def] = code;
        order_.push_back(def);
        return code;
    }
    void use(int code) {
        if (code != current_) {
            body_ << "D" << code << "*\n";
            current_ = code;
        }
    }
    void line(Pt a, Pt b, double width) {
        use(aperture("C," + dec(width)));
        body_ << "X" << coord(a.x) << "Y" << coord(-a.y) << "D02*\n";
        body_ << "X" << coord(b.x) << "Y" << coord(-b.y) << "D01*\n";
    }
    void flash(Pt p, const std::string& def) {
        use(aperture(def));
        body_ << "X" << coord(p.x) << "Y" << coord(-p.y) << "D03*\n";
    }
    void pad(const WorldPad& p, double grow) {
        const double w = p.w + 2 * grow, h = p.h + 2 * grow;
        if (p.shape == PadShape::Circle) flash(p.pos, "C," + dec(std::min(w, h)));
        else if (p.shape == PadShape::Oval) flash(p.pos, std::abs(w - h) < 1e-9 ? "C," + dec(w) : "O," + dec(w) + "X" + dec(h));
        else flash(p.pos, "R," + dec(w) + "X" + dec(h));
    }
    void rect(double x0, double y0, double x1, double y1) {
        body_ << "G36*\n";
        body_ << "X" << coord(x0) << "Y" << coord(-y0) << "D02*\n";
        body_ << "X" << coord(x1) << "Y" << coord(-y0) << "D01*\n";
        body_ << "X" << coord(x1) << "Y" << coord(-y1) << "D01*\n";
        body_ << "X" << coord(x0) << "Y" << coord(-y1) << "D01*\n";
        body_ << "X" << coord(x0) << "Y" << coord(-y0) << "D01*\n";
        body_ << "G37*\n";
    }
    std::string str() const {
        std::ostringstream o;
        o << head_.str();
        for (const auto& def : order_) o << "%ADD" << apertures_.at(def) << def << "*%\n";
        o << body_.str() << "M02*\n";
        return o.str();
    }

private:
    std::ostringstream head_, body_;
    std::map<std::string, int> apertures_;
    std::vector<std::string> order_;
    int current_ = -1;
};

void zoneRegions(Gerber& g, const ZoneFill& f) {
    // horizontal runs merged into rectangles while they repeat on following rows
    std::map<std::pair<int, int>, int> open;  // (xa, xb) -> start row
    auto emit = [&](int xa, int xb, int ya, int yb) {
        g.rect(f.x0 + xa * f.cell, f.y0 + ya * f.cell, f.x0 + (xb + 1) * f.cell, f.y0 + (yb + 1) * f.cell);
    };
    for (int y = 0; y <= f.ny; ++y) {
        std::map<std::pair<int, int>, int> next;
        if (y < f.ny) {
            int x = 0;
            while (x < f.nx) {
                if (!f.at(x, y)) {
                    ++x;
                    continue;
                }
                const int xa = x;
                while (x < f.nx && f.at(x, y)) ++x;
                const std::pair<int, int> k{xa, x - 1};
                auto it = open.find(k);
                next[k] = it != open.end() ? it->second : y;
                if (it != open.end()) open.erase(it);
            }
        }
        for (const auto& [k, start] : open) emit(k.first, k.second, start, y - 1);
        open = std::move(next);
    }
}

}  // namespace

std::vector<OutFile> fabFiles(const Circuit& c, const Netlist& nl, const std::string& base) {
    std::vector<OutFile> out;
    const auto pads = worldPads(c, nl);
    const auto fills = fillZones(c, nl);
    const double mask = 0.05;
    for (int layer = 0; layer < 2; ++layer) {
        const int bit = layer == 0 ? 1 : 2;
        const std::string side = layer == 0 ? "Top" : "Bot";
        // copper
        Gerber cu(layer == 0 ? "Copper,L1,Top" : "Copper,L2,Bot");
        for (const auto& f : fills) {
            if (f.layer == layer) zoneRegions(cu, f);
        }
        for (const auto& t : c.board.traces) {
            if (t.layer == layer) cu.line(t.a, t.b, t.width);
        }
        for (const auto& p : pads) {
            if (p.layers & bit) cu.pad(p, 0);
        }
        for (const auto& v : c.board.vias) cu.flash(v.pos, "C," + dec(v.diameter));
        out.push_back({base + (layer == 0 ? "-F_Cu.gtl" : "-B_Cu.gbl"), cu.str()});
        // solder mask openings
        Gerber sm(std::string("Soldermask,") + side, "Negative");
        for (const auto& p : pads) {
            if (p.layers & bit) sm.pad(p, mask);
        }
        out.push_back({base + (layer == 0 ? "-F_Mask.gts" : "-B_Mask.gbs"), sm.str()});
        // paste (SMD pads only)
        Gerber sp(std::string("Paste,") + side);
        for (const auto& p : pads) {
            if (p.drill <= 0 && (p.layers & bit)) sp.pad(p, 0);
        }
        out.push_back({base + (layer == 0 ? "-F_Paste.gtp" : "-B_Paste.gbp"), sp.str()});
        // silkscreen
        Gerber sk(std::string("Legend,") + side);
        for (const auto& part : c.parts) {
            if (!part.pcb.placed || part.pcb.bottom != (layer == 1)) continue;
            auto fp = footprintOf(part);
            if (!fp) continue;
            for (const auto& [a, b] : fp->silk) sk.line(toBoard(part.pcb, a), toBoard(part.pcb, b), 0.15);
            const double h = 1.0;
            const double w = strokeTextWidth(part.ref, h);
            for (const auto& stroke : strokeText(part.ref, -w / 2, fp->y0 - 0.3, h)) {
                for (std::size_t i = 1; i < stroke.size(); ++i) sk.line(toBoard(part.pcb, stroke[i - 1]), toBoard(part.pcb, stroke[i]), 0.15);
            }
        }
        out.push_back({base + (layer == 0 ? "-F_Silkscreen.gto" : "-B_Silkscreen.gbo"), sk.str()});
    }
    Gerber edge("Profile,NP");
    const auto& poly = c.board.outline;
    for (std::size_t i = 0; i < poly.size(); ++i) edge.line(poly[i], poly[(i + 1) % poly.size()], 0.1);
    out.push_back({base + "-Edge_Cuts.gm1", edge.str()});

    // Excellon drill file, plated holes
    std::map<long long, std::vector<Pt>> byTool;  // drill in microns -> holes
    for (const auto& p : pads) {
        if (p.drill > 0) byTool[std::llround(p.drill * 1000)].push_back(p.pos);
    }
    for (const auto& v : c.board.vias) byTool[std::llround(v.drill * 1000)].push_back(v.pos);
    std::ostringstream d;
    d << "M48\n; DRILL file {achclwss circuit} plated through holes\n; FORMAT={-:-/ absolute / metric / decimal}\nFMAT,2\nMETRIC\n";
    int tool = 1;
    std::map<long long, int> toolOf;
    for (const auto& [size, holes] : byTool) {
        toolOf[size] = tool;
        char b[64];
        std::snprintf(b, sizeof b, "T%dC%.3f\n", tool++, static_cast<double>(size) / 1000.0);
        d << b;
    }
    d << "%\nG90\nG05\n";
    for (const auto& [size, holes] : byTool) {
        d << "T" << toolOf[size] << "\n";
        for (const auto& h : holes) {
            char b[64];
            std::snprintf(b, sizeof b, "X%.3fY%.3f\n", h.x, -h.y);
            d << b;
        }
    }
    d << "M30\n";
    out.push_back({base + ".drl", d.str()});
    return out;
}

}  // namespace achclwss::circuit
