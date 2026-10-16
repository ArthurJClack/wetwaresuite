// calc: exact numbers, the parser, units, statistics, solving, graphs, the commands and MCP.
#include <cmath>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "wetwaresuite/calc/calc.hpp"
#include "wetwaresuite/mcp/server.hpp"

using namespace wetwaresuite;
using nlohmann::json;

namespace {

std::string ev(calc::State& st, const std::string& e) {
    try {
        return calc::evaluate(st, e).text;
    } catch (const std::exception& ex) {
        return std::string("error: ") + ex.what();
    }
}

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

std::string field(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

}  // namespace

int main() {
    using calc::BigInt;
    // ---- big whole numbers ----
    {
        BigInt a = BigInt::parse("123456789012345678901234567890");
        BigInt b = BigInt::parse("987654321098765432109876543210");
        CHECK((a * b).toString() == "121932631137021795226185032733622923332237463801111263526900");
        CHECK((b / a).toString() == "8");
        CHECK((b % a).toString() == "9000000000900000000090");
        CHECK((a - b).toString() == "-864197532086419753208641975320");
        CHECK(BigInt::pow(BigInt(2), 100).toString() == "1267650600228229401496703205376");
        CHECK(BigInt::parse("ff", 16).toString() == "255");
        CHECK(BigInt(255).toString(2) == "11111111");
        CHECK(BigInt(-1).lowBits(8).toString() == "255");
        CHECK(BigInt::bitAnd(BigInt(12), BigInt(10)).toString() == "8");
        CHECK(BigInt::bitXor(BigInt(-1), BigInt(5)).toString() == "-6");
        CHECK(BigInt(-7).shiftRight(1).toString() == "-4");
        CHECK(BigInt::gcd(BigInt(48), BigInt(-18)).toString() == "6");
        // Knuth division with multi-limb divisors
        BigInt big = BigInt::pow(BigInt(3), 200), div = BigInt::pow(BigInt(7), 40) + BigInt(12345);
        BigInt q, r;
        BigInt::divmod(big, div, q, r);
        CHECK(q * div + r == big);
        CHECK(r < div);
    }

    // ---- expressions ----
    {
        calc::State st;
        CHECK(ev(st, "2 + 2") == "4");
        CHECK(ev(st, "1/3 + 1/6") == "0.5");
        CHECK(ev(st, "1/3") == "0.333333333333");
        CHECK(ev(st, "1/3 to frac") == "1/3");
        CHECK(ev(st, "0.1 + 0.2") == "0.3");  // exact decimals: no 0.30000000000000004
        CHECK(ev(st, "2^100") == "1267650600228229401496703205376");
        CHECK(ev(st, "20!") == "2432902008176640000");
        CHECK(ev(st, "-2^2") == "-4");
        CHECK(ev(st, "2^3^2") == "512");
        CHECK(ev(st, "(3 + 4) * 2") == "14");
        CHECK(ev(st, "2(3 + 4)") == "14");
        CHECK(ev(st, "7 mod 3") == "1");
        CHECK(ev(st, "-7 % 3") == "2");
        CHECK(ev(st, "7 // 2") == "3");
        CHECK(ev(st, "sqrt(16/9)") == "4/3 to frac" || ev(st, "sqrt(16/9) to frac") == "4/3");
        CHECK(ev(st, "sqrt(2)") == "1.41421356237");
        CHECK(ev(st, "sin(30)") == "0.5");  // degrees by default
        CHECK(ev(st, "sin 90") == "1");
        CHECK(ev(st, "cos(pi rad)") == "-1");
        CHECK(ev(st, "asin(1)") == "90");
        CHECK(ev(st, "log(1000)") == "3");
        CHECK(ev(st, "ln(e)") == "1");
        CHECK(ev(st, "200 + 10%") == "220");
        CHECK(ev(st, "50% * 30") == "15");
        CHECK(ev(st, "x = 5") == "x = 5");
        CHECK(ev(st, "2x + 1") == "11");
        CHECK(ev(st, "ans * 2") == "22");
        CHECK(ev(st, "f(a, b) = a^2 + b") == "f(a, b) = a^2 + b");
        CHECK(ev(st, "f(3, 1)") == "10");
        CHECK(ev(st, "data = [4, 8, 15, 16, 23, 42]") == "data = [4, 8, 15, 16, 23, 42]");
        CHECK(ev(st, "mean(data)") == "18");
        CHECK(ev(st, "median(data)") == "15.5");
        CHECK(ev(st, "data * 2") == "[8, 16, 30, 32, 46, 84]");
        CHECK(ev(st, "gcd(12, 18, 24)") == "6");
        CHECK(ev(st, "ncr(52, 5)") == "2598960");
        CHECK(ev(st, "isprime(2^61 - 1)") == "1");
        CHECK(ev(st, "3 < 4") == "1");
        CHECK(ev(st, "a = 1; b = 2; a + b") == "3");
        CHECK(ev(st, "2 π") == "6.28318530718");
        CHECK(ev(st, "6 × 7 ÷ 2") == "21");
        CHECK(ev(st, "1e3 + 1_000") == "2000");
        CHECK(ev(st, "nosuch + 1").rfind("error: I don't know 'nosuch'", 0) == 0);
        CHECK(ev(st, "1/0").rfind("error: can't divide by zero", 0) == 0);
        CHECK(ev(st, "(1 + 2").rfind("error:", 0) == 0);
        CHECK(ev(st, "sin(").rfind("error:", 0) == 0);
        CHECK(st.history.size() >= 20);
    }

    // ---- programmer mode ----
    {
        calc::State st;
        CHECK(ev(st, "0xff + 0b1") == "256");
        CHECK(ev(st, "255 to hex") == "0xff");
        CHECK(ev(st, "10 to bin") == "0b1010");
        CHECK(ev(st, "0o17") == "15");
        CHECK(ev(st, "0xf0 | 0x0f") == "255");
        CHECK(ev(st, "0xff & 0b1010") == "10");
        CHECK(ev(st, "6 xor 3") == "5");
        CHECK(ev(st, "1 << 10") == "1024");
        CHECK(ev(st, "~0") == "-1");
        CHECK(ev(st, "-1 to hex") == "0xffffffffffffffff");
        st.settings.bits = 8;
        CHECK(ev(st, "-2 to hex") == "0xfe");
        st.settings.base = "hex";
        CHECK(ev(st, "200 + 55") == "0xff");
    }

    // ---- units ----
    {
        calc::State st;
        CHECK(ev(st, "5 km to m") == "5000 m");
        CHECK(ev(st, "1 mi to km") == "1.609344 km");
        CHECK(ev(st, "60 mph to km/h") == "96.56064 km/h");
        CHECK(ev(st, "100 degC to degF") == "212 degF");
        CHECK(ev(st, "32 °F to °C") == "0 °C");
        CHECK(ev(st, "0 degC to K") == "273.15 K");
        CHECK(ev(st, "3 ft + 2 in to cm") == "96.52 cm");
        CHECK(ev(st, "1 GiB to MB") == "1073.741824 MB");
        CHECK(ev(st, "2 h + 30 min to min") == "150 min");
        CHECK(ev(st, "1 acre to m^2") == "4046.8564224 m^2");
        CHECK(ev(st, "10 kilometres to miles") == "6.21371192237 miles");
        CHECK(ev(st, "180 deg to rad") == "3.14159265359 rad");
        CHECK(ev(st, "5 km + 3 s").rfind("error: can't add a time to a length", 0) == 0);
        CHECK(ev(st, "5 kg to m").rfind("error: can't convert a mass to a length", 0) == 0);
        CHECK(near(calc::convertUnits(1, "lb", "kg"), 0.45359237));
        calc::UnitInfo u;
        CHECK(calc::findUnit("mA", u) && near(u.scale, 1e-3));
        CHECK(calc::findUnit("KiB", u) && near(u.scale, 8192));
        CHECK(!calc::findUnit("blorp", u));
    }

    // ---- statistics, solving, calculus, primes ----
    {
        calc::State st;
        auto s = calc::describe({2, 4, 4, 4, 5, 5, 7, 9});
        CHECK(near(s.mean, 5) && near(s.pstdev, 2) && near(s.median, 4.5));
        CHECK(s.modes.size() == 1 && s.modes[0] == 4);
        auto r = calc::linearRegression({1, 2, 3, 4}, {3, 5, 7, 9});
        CHECK(near(r.slope, 2) && near(r.intercept, 1) && near(r.r2, 1));
        auto sol = calc::solve(st, "x^2 = 2");
        CHECK(sol.roots.size() == 2 && near(sol.roots[0][0], -std::sqrt(2.0)) && near(sol.roots[1][0], std::sqrt(2.0)));
        auto lin = calc::solve(st, "2x + 3 = 11");
        CHECK(lin.roots.size() == 1 && lin.roots[0][0] == 4);
        auto touch = calc::solve(st, "(x - 3)^2");
        CHECK(touch.roots.size() == 1 && near(touch.roots[0][0], 3, 1e-6));
        auto sys = calc::solve(st, "2x + y = 5; x - y = 1");
        CHECK(sys.roots.size() == 1 && sys.vars[0] == "x" && near(sys.roots[0][0], 2) && near(sys.roots[0][1], 1));
        calc::State rad;
        rad.settings.angle = calc::Angle::Radians;
        auto tanRoots = calc::solve(rad, "tan(x)", {}, -4, 4);  // poles at ±pi/2 are not roots
        CHECK(tanRoots.roots.size() == 3);
        CHECK(near(calc::integrate(st, "x^2", "x", 0, 3), 9));
        CHECK(near(calc::integrate(rad, "sin(x)", "x", 0, 3.14159265358979323846), 2, 1e-8));
        CHECK(near(calc::derivative(st, "x^3", "x", 2), 12, 1e-6));
        auto f = calc::factorize(BigInt(360));
        CHECK(f.size() == 3 && f[0].second == 3 && f[1].second == 2);
        auto f2 = calc::factorize(BigInt::parse("18446744073709551617"));  // 2^64 + 1 = 274177 * 67280421310721
        CHECK(f2.size() == 2 && f2[0].first.toString() == "274177");
        CHECK(calc::parseNumbers("1, 2 3\n4").size() == 4);
    }

    // ---- graphs ----
    {
        calc::State st;
        st.settings.angle = calc::Angle::Radians;
        calc::GraphOptions o;
        auto curves = calc::sampleCurves(st, calc::splitExpressions("y = sin(x); x^2/10"), o, 200);
        CHECK(curves.size() == 2 && curves[0].ys.size() == 200);
        std::string png = calc::graphPng(curves, o);
        CHECK(png.size() > 1000 && png.compare(1, 3, "PNG") == 0);
    }

    // ---- commands, undo and MCP ----
    {
        Session s;
        calc::registerCommands(s);
        Result r = s.run("calc.eval", {{"expression", std::string("3/4 + 1/6")}});
        CHECK(r.ok && r.message == "0.916666666667" && field(r, "exact") == "11/12");
        r = s.run("calc.eval", {{"expression", std::string("255")}});
        CHECK(field(r, "hex") == "0xff" && field(r, "bin") == "0b11111111");
        CHECK(s.run("calc.let", {{"name", std::string("rate")}, {"value", std::string("0.2")}}).ok);
        CHECK(s.run("calc.eval", {{"expression", std::string("100 * rate")}}).message == "20");
        CHECK(s.history().undo());  // the calculation
        CHECK(s.history().undo());  // the variable
        CHECK(!s.run("calc.eval", {{"expression", std::string("rate")}}).ok);
        CHECK(s.run("calc.memory", {{"action", std::string("store")}, {"value", std::string("42")}}).ok);
        CHECK(s.run("calc.memory", {{"action", std::string("add")}, {"value", std::string("8")}}).message == "memory = 50");
        CHECK(s.run("calc.eval", {{"expression", std::string("mem / 2")}}).message == "25");
        CHECK(s.run("calc.settings", {{"angle", std::string("rad")}}).ok);
        CHECK(s.run("calc.eval", {{"expression", std::string("sin(pi/2)")}}).message == "1");
        CHECK(s.run("calc.convert", {{"value", std::string("26.2")}, {"from", std::string("mi")}, {"to", std::string("km")}}).message == "26.2 mi = 42.1648128 km");
        Result st = s.run("calc.stats", {{"data", std::string("2 4 4 4 5 5 7 9")}});
        CHECK(st.ok && field(st, "pstdev") == "2");
        Result sv = s.run("calc.solve", {{"equation", std::string("x^2 - 5x + 6 = 0")}});
        CHECK(sv.ok && sv.message == "x = 2\nx = 3");
        Result fa = s.run("calc.factor", {{"value", std::string("97")}});
        CHECK(fa.ok && fa.message == "97 = 97 (prime)");
        Result b = s.run("calc.base", {{"value", std::string("-1")}, {"bits", std::int64_t{16}}});
        CHECK(b.ok && field(b, "hex") == "0xffff");
        Result g = s.run("calc.graph", {{"expressions", std::string("sin(x)")}});
        CHECK(g.ok && g.attachments.size() == 1);
        auto out = std::filesystem::temp_directory_path() / "wetwaresuite_calc_graph.png";
        CHECK(s.run("calc.graph", {{"expressions", std::string("x^2")}, {"output", out.string()}}).ok);
        CHECK(std::filesystem::file_size(out) > 1000);
        std::filesystem::remove(out);
        Result smp = s.run("calc.sample", {{"expressions", std::string("x; -x")}, {"samples", std::int64_t{11}}});
        CHECK(smp.ok && std::get<NumberList>(*smp.field("ys")).size() == 22);
        Result h = s.run("calc.history", {});
        CHECK(h.ok && h.message.find("mem / 2  =  25") != std::string::npos);

        mcp::Server server(s, {"wetwaresuite", "test", "calc"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        int seen = 0;
        for (const auto& t : list) {
            if (std::string(t["name"]).rfind("calc_", 0) == 0) ++seen;
        }
        CHECK(seen == 22);
        json call = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                     {"params", {{"name", "calc_eval"}, {"arguments", {{"expression", "12 * 12"}}}}}};
        json res = json::parse(*server.handle(call.dump()))["result"];
        CHECK(res["isError"] == false && res["structuredContent"]["text"] == "144");
    }
    return check::summary("calc");
}
