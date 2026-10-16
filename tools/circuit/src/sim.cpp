// SPICE-lite: modified nodal analysis with Newton-Raphson for diodes, a DC operating
// point and a fixed-step backward-Euler transient.
#include <optional>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::circuit {

namespace {

constexpr double kVt = 0.025852;
constexpr double kPi = 3.14159265358979323846;

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

struct Waveform {
    enum Kind { Dc, Sin, Pulse } kind = Dc;
    std::vector<double> a;
    double at(double t) const {
        if (kind == Dc) return a.empty() ? 0 : a[0];
        if (kind == Sin) {
            const double off = a.size() > 0 ? a[0] : 0, amp = a.size() > 1 ? a[1] : 0, f = a.size() > 2 ? a[2] : 0, td = a.size() > 3 ? a[3] : 0;
            return t < td ? off : off + amp * std::sin(2 * kPi * f * (t - td));
        }
        const double v1 = a.size() > 0 ? a[0] : 0, v2 = a.size() > 1 ? a[1] : 0, td = a.size() > 2 ? a[2] : 0;
        const double tr = std::max(a.size() > 3 ? a[3] : 0, 1e-12), tf = std::max(a.size() > 4 ? a[4] : 0, 1e-12);
        const double pw = a.size() > 5 ? a[5] : 1e30, per = a.size() > 6 ? a[6] : 1e30;
        if (t < td) return v1;
        double u = t - td;
        if (per > 0 && per < 1e29) u = std::fmod(u, per);
        if (u < tr) return v1 + (v2 - v1) * u / tr;
        if (u < tr + pw) return v2;
        if (u < tr + pw + tf) return v2 + (v1 - v2) * (u - tr - pw) / tf;
        return v1;
    }
};

Waveform parseWave(const std::string& text, const std::string& who) {
    Waveform w;
    const std::string u = upper(text);
    auto args = [&](std::size_t open) {
        const auto close = text.find(')', open);
        std::string inner = text.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
        std::replace(inner.begin(), inner.end(), ',', ' ');
        std::stringstream ss(inner);
        std::string tok;
        std::vector<double> out;
        while (ss >> tok) {
            auto v = parseValue(tok);
            if (!v) throw std::runtime_error(who + ": can't read '" + tok + "' in " + text);
            out.push_back(*v);
        }
        return out;
    };
    if (u.rfind("SIN(", 0) == 0 || u.rfind("SIN (", 0) == 0) {
        w.kind = Waveform::Sin;
        w.a = args(text.find('('));
    } else if (u.rfind("PULSE", 0) == 0) {
        w.kind = Waveform::Pulse;
        w.a = args(text.find('('));
    } else {
        std::string t = text;
        if (u.rfind("DC ", 0) == 0) t = text.substr(3);
        auto v = parseValue(t);
        if (!v) throw std::runtime_error(who + ": value '" + text + "' isn't a number, SIN(...) or PULSE(...)");
        w.a = {*v};
    }
    return w;
}

// "+5V" -> 5, "+3V3" -> 3.3, "-12V" -> -12, "12V" -> 12
std::optional<double> railVoltage(const std::string& name) {
    std::string s = upper(name);
    double sign = 1;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        if (s[0] == '-') sign = -1;
        s.erase(0, 1);
    }
    const auto v = s.find('V');
    if (v == std::string::npos || v == 0) return std::nullopt;
    const std::string a = s.substr(0, v), b = s.substr(v + 1);
    if (!std::all_of(a.begin(), a.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; })) return std::nullopt;
    if (!std::all_of(b.begin(), b.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) return std::nullopt;
    try {
        return sign * std::stod(b.empty() ? a : a + "." + b);
    } catch (...) {
        return std::nullopt;
    }
}

struct Elem {
    enum Kind { R, C, L, D, V, I, Opamp } kind = R;
    std::string name;
    int a = -1, b = -1, c = -1;  // nodes (-1 = ground); Opamp: a = +, b = -, c = out
    double value = 0;
    double is = 1e-14, n = 1;
    Waveform wave;
    int branch = -1;
    double state = 0;  // previous voltage (C) / current (L) / junction voltage (D)
};

void solve(std::vector<double>& A, std::vector<double>& x, int n) {
    // Gaussian elimination with partial pivoting; A is n x n row-major, x holds the RHS
    for (int col = 0; col < n; ++col) {
        int piv = col;
        for (int r = col + 1; r < n; ++r) {
            if (std::abs(A[static_cast<std::size_t>(r) * n + col]) > std::abs(A[static_cast<std::size_t>(piv) * n + col])) piv = r;
        }
        if (std::abs(A[static_cast<std::size_t>(piv) * n + col]) < 1e-300) throw std::runtime_error("the circuit matrix is singular (a floating node or a loop of voltage sources?)");
        if (piv != col) {
            for (int k = 0; k < n; ++k) std::swap(A[static_cast<std::size_t>(col) * n + k], A[static_cast<std::size_t>(piv) * n + k]);
            std::swap(x[static_cast<std::size_t>(col)], x[static_cast<std::size_t>(piv)]);
        }
        const double d = A[static_cast<std::size_t>(col) * n + col];
        for (int r = col + 1; r < n; ++r) {
            const double f = A[static_cast<std::size_t>(r) * n + col] / d;
            if (f == 0) continue;
            for (int k = col; k < n; ++k) A[static_cast<std::size_t>(r) * n + k] -= f * A[static_cast<std::size_t>(col) * n + k];
            x[static_cast<std::size_t>(r)] -= f * x[static_cast<std::size_t>(col)];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = x[static_cast<std::size_t>(r)];
        for (int k = r + 1; k < n; ++k) s -= A[static_cast<std::size_t>(r) * n + k] * x[static_cast<std::size_t>(k)];
        x[static_cast<std::size_t>(r)] = s / A[static_cast<std::size_t>(r) * n + r];
    }
}

}  // namespace

SimResult simulate(const Circuit& c, const Netlist& nl, const SimOptions& opt) {
    SimResult res;
    // nodes
    std::map<std::string, int> node;
    std::string ground;
    for (const auto& n : nl.nets) {
        const std::string u = upper(n.name);
        if (u == "GND" || u == "0" || u == "GNDA" || u == "AGND") {
            ground = n.name;
            break;
        }
    }
    std::vector<Elem> elems;
    auto nodeOf = [&](const std::string& ref, const std::string& pin) -> int {
        const std::string net = nl.netOf(ref, pin);
        if (net.empty()) return -2;  // unconnected
        if (net == ground) return -1;
        auto it = node.find(net);
        if (it != node.end()) return it->second;
        const int id = static_cast<int>(node.size());
        node[net] = id;
        res.nets.push_back(net);
        return id;
    };
    auto unsupported = [&](const Part& p, const std::string& what) {
        if (!opt.ignoreUnsupported) throw std::runtime_error(p.ref + " (" + what + ") can't be simulated; pass ignore_unsupported=true to leave it out");
        res.warnings.push_back(p.ref + " (" + what + ") left out of the simulation");
    };
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (!sym) continue;
        const std::string s = sym->name;
        if (sym->power) continue;
        auto two = [&](Elem::Kind k, const std::string& pa, const std::string& pb, double value) {
            Elem e;
            e.kind = k;
            e.name = p.ref;
            e.a = nodeOf(p.ref, pa);
            e.b = nodeOf(p.ref, pb);
            e.value = value;
            if (e.a == -2 || e.b == -2) {
                res.warnings.push_back(p.ref + " has an unconnected pin; left out");
                return;
            }
            elems.push_back(e);
        };
        auto val = [&](const std::string& fallback) {
            auto v = parseValue(p.value.empty() ? fallback : p.value);
            if (!v) throw std::runtime_error(p.ref + ": value '" + p.value + "' isn't a number");
            return *v;
        };
        if (s == "resistor") two(Elem::R, "1", "2", std::max(1e-6, val("1k")));
        else if (s == "potentiometer") {
            double w = 0.5;
            if (auto it = p.fields.find("wiper"); it != p.fields.end()) w = std::clamp(parseValue(it->second).value_or(0.5), 0.0, 1.0);
            const double r = val("10k");
            two(Elem::R, "1", "2", std::max(1e-3, r * w));
            two(Elem::R, "2", "3", std::max(1e-3, r * (1 - w)));
        } else if (s == "capacitor" || s == "capacitor_polarized") two(Elem::C, "1", "2", val("100n"));
        else if (s == "inductor") two(Elem::L, "1", "2", val("10u"));
        else if (s == "diode" || s == "led") {
            Elem e;
            e.kind = Elem::D;
            e.name = p.ref;
            e.a = nodeOf(p.ref, "2");
            e.b = nodeOf(p.ref, "1");
            if (s == "led") {
                const std::string v = upper(p.value);
                double vf = 2.0;
                if (v.find("GREEN") != std::string::npos) vf = 2.2;
                if (v.find("BLUE") != std::string::npos || v.find("WHITE") != std::string::npos || v.find("UV") != std::string::npos) vf = 3.0;
                if (auto it = p.fields.find("vf"); it != p.fields.end()) vf = parseValue(it->second).value_or(vf);
                e.n = 2;
                e.is = 0.01 / std::exp(vf / (e.n * kVt));  // vf at 10 mA
            }
            if (auto it = p.fields.find("is"); it != p.fields.end()) e.is = parseValue(it->second).value_or(e.is);
            if (auto it = p.fields.find("n"); it != p.fields.end()) e.n = parseValue(it->second).value_or(e.n);
            if (e.a == -2 || e.b == -2) res.warnings.push_back(p.ref + " has an unconnected pin; left out");
            else elems.push_back(e);
        } else if (s == "vsource" || s == "battery") {
            Elem e;
            e.kind = Elem::V;
            e.name = p.ref;
            e.a = nodeOf(p.ref, "1");
            e.b = nodeOf(p.ref, "2");
            e.wave = parseWave(p.value.empty() ? "5" : p.value, p.ref);
            if (e.a == -2 || e.b == -2) res.warnings.push_back(p.ref + " has an unconnected pin; left out");
            else elems.push_back(e);
        } else if (s == "isource") {
            Elem e;
            e.kind = Elem::I;
            e.name = p.ref;
            e.a = nodeOf(p.ref, "1");
            e.b = nodeOf(p.ref, "2");
            e.wave = parseWave(p.value.empty() ? "1m" : p.value, p.ref);
            if (e.a == -2 || e.b == -2) res.warnings.push_back(p.ref + " has an unconnected pin; left out");
            else elems.push_back(e);
        } else if (s == "switch") {
            const std::string st = upper(p.fields.count("state") ? p.fields.at("state") : "open");
            const bool closed = st == "ON" || st == "CLOSED" || st == "1" || st == "PRESSED" || st == "TRUE";
            two(Elem::R, "1", "2", closed ? 0.01 : 1e12);
        } else if (s == "switch_spdt") {
            const std::string st = upper(p.fields.count("state") ? p.fields.at("state") : "a");
            const bool a = st != "B" && st != "3";
            two(Elem::R, "2", "1", a ? 0.01 : 1e12);
            two(Elem::R, "2", "3", a ? 1e12 : 0.01);
        } else if (s == "opamp") {
            Elem e;
            e.kind = Elem::Opamp;
            e.name = p.ref;
            e.a = nodeOf(p.ref, "3");
            e.b = nodeOf(p.ref, "2");
            e.c = nodeOf(p.ref, "6");
            e.value = 1e5;
            if (e.a == -2 || e.b == -2 || e.c == -2) res.warnings.push_back(p.ref + " has an unconnected input or output; left out");
            else {
                res.warnings.push_back(p.ref + " is modelled as an ideal amplifier (gain 1e5, no supply limits)");
                elems.push_back(e);
            }
        } else if (s == "header" || s == "header2" || sym->module || s == "crystal") {
            res.warnings.push_back(p.ref + " (" + s + ") doesn't conduct in the simulation; left out");
        } else {
            unsupported(p, s);
        }
    }
    // power rails become sources to ground
    std::set<std::string> rails;
    for (const auto& p : c.parts) {
        auto sym = symbolOf(p);
        if (!sym || !sym->power) continue;
        const std::string net = p.value;
        if (net == ground || rails.count(net)) continue;
        if (!nl.net(net)) continue;
        std::optional<double> v = railVoltage(net);
        if (auto it = p.fields.find("voltage"); it != p.fields.end()) v = parseValue(it->second);
        if (!v) throw std::runtime_error("power net " + net + " has no voltage: name it like +5V or add field voltage=5 to " + p.ref);
        rails.insert(net);
        Elem e;
        e.kind = Elem::V;
        e.name = net;
        const int id = [&] {
            auto it = node.find(net);
            if (it != node.end()) return it->second;
            const int k = static_cast<int>(node.size());
            node[net] = k;
            res.nets.push_back(net);
            return k;
        }();
        e.a = id;
        e.b = -1;
        e.wave.a = {*v};
        elems.push_back(e);
    }
    if (ground.empty()) {
        if (!elems.empty()) throw std::runtime_error("no ground: name a net GND (add a gnd symbol) so voltages have a reference");
    }
    if (elems.empty()) throw std::runtime_error("nothing to simulate");
    const int nn = static_cast<int>(node.size());
    int nb = 0;
    for (auto& e : elems) {
        if (e.kind == Elem::V || e.kind == Elem::L || e.kind == Elem::Opamp) e.branch = nn + nb++;
    }
    const int N = nn + nb;
    std::vector<double> x(static_cast<std::size_t>(N), 0.0);

    auto step = [&](double t, double dt, bool dc) {
        for (int iter = 0; iter < 300; ++iter) {
            std::vector<double> A(static_cast<std::size_t>(N) * N, 0.0), rhs(static_cast<std::size_t>(N), 0.0);
            auto at = [&](int r, int c2) -> double& { return A[static_cast<std::size_t>(r) * N + c2]; };
            auto g = [&](int a, int b, double G) {
                if (a >= 0) at(a, a) += G;
                if (b >= 0) at(b, b) += G;
                if (a >= 0 && b >= 0) at(a, b) -= G, at(b, a) -= G;
            };
            auto inj = [&](int a, int b, double I) {  // current I flowing from a to b through the element
                if (a >= 0) rhs[static_cast<std::size_t>(a)] -= I;
                if (b >= 0) rhs[static_cast<std::size_t>(b)] += I;
            };
            auto v = [&](int k) { return k >= 0 ? x[static_cast<std::size_t>(k)] : 0.0; };
            for (int k = 0; k < nn; ++k) at(k, k) += 1e-12;
            for (auto& e : elems) {
                switch (e.kind) {
                    case Elem::R: g(e.a, e.b, 1.0 / e.value); break;
                    case Elem::C:
                        if (!dc) {
                            const double G = e.value / dt;
                            g(e.a, e.b, G);
                            inj(e.a, e.b, -G * e.state);
                        }
                        break;
                    case Elem::L: {
                        const int b = e.branch;
                        if (e.a >= 0) at(e.a, b) += 1, at(b, e.a) += 1;
                        if (e.b >= 0) at(e.b, b) -= 1, at(b, e.b) -= 1;
                        if (!dc) {
                            at(b, b) -= e.value / dt;
                            rhs[static_cast<std::size_t>(b)] = -e.value / dt * e.state;
                        }
                        break;
                    }
                    case Elem::D: {
                        const double nvt = e.n * kVt;
                        double vd = v(e.a) - v(e.b);
                        // limit the step so exp() stays sane
                        const double vcrit = nvt * std::log(nvt / (std::sqrt(2.0) * e.is));
                        if (vd > vcrit && std::abs(vd - e.state) > 2 * nvt) {
                            vd = e.state > 0 ? e.state + nvt * std::log(1 + (vd - e.state) / nvt) : nvt * std::log(std::max(vd / nvt, 1e-30));
                        }
                        vd = std::min(vd, 5.0);
                        e.state = vd;
                        const double ex = std::exp(vd / nvt);
                        const double id = e.is * (ex - 1);
                        const double gd = e.is / nvt * ex + 1e-12;
                        g(e.a, e.b, gd);
                        inj(e.a, e.b, id - gd * vd);
                        break;
                    }
                    case Elem::V: {
                        const int b = e.branch;
                        if (e.a >= 0) at(e.a, b) += 1, at(b, e.a) += 1;
                        if (e.b >= 0) at(e.b, b) -= 1, at(b, e.b) -= 1;
                        rhs[static_cast<std::size_t>(b)] = e.wave.at(t);
                        break;
                    }
                    case Elem::I: inj(e.b, e.a, e.wave.at(t)); break;
                    case Elem::Opamp: {
                        const int b = e.branch;
                        if (e.c >= 0) at(e.c, b) += 1, at(b, e.c) += 1;
                        if (e.a >= 0) at(b, e.a) -= e.value;
                        if (e.b >= 0) at(b, e.b) += e.value;
                        break;
                    }
                }
            }
            std::vector<double> nx = rhs;
            solve(A, nx, N);
            double change = 0;
            for (int k = 0; k < N; ++k) change = std::max(change, std::abs(nx[static_cast<std::size_t>(k)] - x[static_cast<std::size_t>(k)]) / (1 + std::abs(nx[static_cast<std::size_t>(k)])));
            x = nx;
            ++res.iterations;
            bool nonlinear = false;
            double junction = 0;  // how far the diodes' assumed voltages are from the new solution
            for (const auto& e : elems) {
                if (e.kind != Elem::D) continue;
                nonlinear = true;
                junction = std::max(junction, std::abs(v(e.a) - v(e.b) - e.state));
            }
            if (!nonlinear || (iter > 0 && change < 1e-9 && junction < 1e-6)) return;
        }
        res.warnings.push_back("Newton iterations did not fully converge at t=" + formatValue(t) + "s");
    };
    step(0, 0, true);
    auto keepState = [&] {
        for (auto& e : elems) {
            if (e.kind == Elem::C) e.state = (e.a >= 0 ? x[static_cast<std::size_t>(e.a)] : 0) - (e.b >= 0 ? x[static_cast<std::size_t>(e.b)] : 0);
            if (e.kind == Elem::L) e.state = x[static_cast<std::size_t>(e.branch)];
        }
    };
    keepState();
    if (opt.tstop > 0) {
        double dt = opt.step > 0 ? opt.step : opt.tstop / 500;
        const double steps = opt.tstop / dt;
        if (steps > 200000) throw std::runtime_error("too many time steps (" + std::to_string(static_cast<long long>(steps)) + "); use a larger step");
        std::vector<int> probeIdx;
        if (opt.probes.empty()) {
            for (int k = 0; k < nn && k < 8; ++k) {
                res.probes.push_back(res.nets[static_cast<std::size_t>(k)]);
                probeIdx.push_back(k);
            }
        } else {
            for (const auto& pr : opt.probes) {
                const Net* n = nl.net(pr);
                if (!n) throw std::runtime_error("no net called " + pr + " to probe");
                if (n->name == ground) {
                    res.probes.push_back(n->name);
                    probeIdx.push_back(-1);
                    continue;
                }
                auto it = node.find(n->name);
                if (it == node.end()) throw std::runtime_error("net " + pr + " has no simulated parts on it");
                res.probes.push_back(n->name);
                probeIdx.push_back(it->second);
            }
        }
        res.series.assign(res.probes.size(), {});
        auto record = [&](double t) {
            res.times.push_back(t);
            for (std::size_t i = 0; i < probeIdx.size(); ++i) res.series[i].push_back(probeIdx[i] >= 0 ? x[static_cast<std::size_t>(probeIdx[i])] : 0.0);
        };
        record(0);
        const long long n = static_cast<long long>(std::ceil(steps - 1e-9));
        for (long long i = 1; i <= n; ++i) {
            const double t = std::min(opt.tstop, static_cast<double>(i) * dt);
            step(t, dt, false);
            keepState();
            record(t);
        }
    }
    res.voltages.assign(static_cast<std::size_t>(nn), 0);
    for (int k = 0; k < nn; ++k) res.voltages[static_cast<std::size_t>(k)] = x[static_cast<std::size_t>(k)];
    for (const auto& e : elems) {
        if (e.kind == Elem::V) res.currents.push_back({e.name, -x[static_cast<std::size_t>(e.branch)]});
    }
    return res;
}

raster::Image plotSeries(const SimResult& r, int width, int height) {
    raster::Image img(width, height, {255, 255, 255, 255});
    if (r.times.size() < 2 || r.series.empty()) return img;
    double vmin = 1e18, vmax = -1e18;
    for (const auto& s : r.series) {
        for (double v : s) vmin = std::min(vmin, v), vmax = std::max(vmax, v);
    }
    if (vmax - vmin < 1e-9) vmax += 0.5, vmin -= 0.5;
    const double pad = (vmax - vmin) * 0.08;
    vmin -= pad, vmax += pad;
    const double t0 = r.times.front(), t1 = r.times.back();
    const double L = 64, R = width - 16.0, T = 16, B = height - 40.0;
    auto X = [&](double t) { return L + (t - t0) / (t1 - t0) * (R - L); };
    auto Y = [&](double v) { return B - (v - vmin) / (vmax - vmin) * (B - T); };
    const raster::Color grid{225, 225, 232, 255}, axis{90, 90, 100, 255};
    for (int i = 0; i <= 5; ++i) {
        const double v = vmin + (vmax - vmin) * i / 5, t = t0 + (t1 - t0) * i / 5;
        raster::drawLine(img, L, Y(v), R, Y(v), grid, 1);
        raster::drawLine(img, X(t), T, X(t), B, grid, 1);
        const std::string vl = formatValue(v) + "V", tl = formatValue(t) + "s";
        pdf::drawText(img, L - 6 - pdf::textWidthPx(vl, {}, 11), Y(v) + 4, vl, {}, 11, axis);
        pdf::drawText(img, X(t) - pdf::textWidthPx(tl, {}, 11) / 2, B + 16, tl, {}, 11, axis);
    }
    raster::strokeRect(img, L, T, R - L, B - T, axis, 1);
    static const raster::Color colors[] = {{31, 119, 180, 255}, {214, 39, 40, 255}, {44, 160, 44, 255}, {255, 127, 14, 255},
                                           {148, 103, 189, 255}, {140, 86, 75, 255}, {227, 119, 194, 255}, {23, 190, 207, 255}};
    double lx = L + 8;
    for (std::size_t s = 0; s < r.series.size(); ++s) {
        const auto col = colors[s % 8];
        const auto& v = r.series[s];
        // decimate to about two points per pixel column
        const std::size_t stride = std::max<std::size_t>(1, v.size() / static_cast<std::size_t>(std::max(1.0, (R - L) * 2)));
        for (std::size_t i = stride; i < v.size(); i += stride) {
            raster::drawLine(img, X(r.times[i - stride]), Y(v[i - stride]), X(r.times[i]), Y(v[i]), col, 1.6);
        }
        raster::drawLine(img, lx, T + 12, lx + 18, T + 12, col, 3);
        pdf::drawText(img, lx + 22, T + 16, r.probes[s], {}, 12, {40, 40, 50, 255});
        lx += 34 + pdf::textWidthPx(r.probes[s], {}, 12);
    }
    pdf::drawText(img, (L + R) / 2 - 14, height - 6.0, "time", {}, 11, axis);
    return img;
}

}  // namespace wetwaresuite::circuit
