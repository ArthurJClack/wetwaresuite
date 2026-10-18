// The offline unit tables: SI and everyday units, SI and binary prefixes, plurals.
#include <cmath>
#include <set>

#include "wetwaresuite/calc/calc.hpp"

namespace wetwaresuite::calc {

namespace {

Dims D(int m, int kg = 0, int s = 0, int A = 0, int K = 0, int mol = 0, int cd = 0, int bit = 0) {
    Dims d;
    const int v[kDims] = {m, kg, s, A, K, mol, cd, bit};
    for (int i = 0; i < kDims; ++i) d.e[i] = static_cast<std::int8_t>(v[i]);
    return d;
}

struct Row {
    const char* names;  // first is the symbol, then aliases, comma separated
    const char* category;
    const char* title;
    double scale;
    Dims dims;
    bool prefix = false;  // takes SI prefixes (km, mA)
    bool binary = false;  // takes binary prefixes too (KiB)
    double offset = 0;
    bool angle = false;
};

constexpr double kPi = 3.14159265358979323846;

const std::vector<Row>& rows() {
    static const std::vector<Row> table = {
        // length
        {"m,metre,meter", "length", "metre", 1, D(1), true},
        {"inch,inches,\"", "length", "inch", 0.0254, D(1)},
        {"ft,foot,feet,'", "length", "foot", 0.3048, D(1)},
        {"yd,yard", "length", "yard", 0.9144, D(1)},
        {"mi,mile", "length", "mile", 1609.344, D(1)},
        {"nmi,nauticalmile", "length", "nautical mile", 1852, D(1)},
        {"au,astronomicalunit", "length", "astronomical unit", 149597870700.0, D(1)},
        {"ly,lightyear", "length", "light year", 9460730472580800.0, D(1)},
        {"pc,parsec", "length", "parsec", 3.0856775814913673e16, D(1), true},
        {"angstrom,\xC3\x85", "length", "angstrom", 1e-10, D(1)},
        {"thou,mil", "length", "thou (1/1000 inch)", 0.0000254, D(1)},
        {"furlong", "length", "furlong", 201.168, D(1)},
        // mass
        {"g,gram,gramme", "mass", "gram", 0.001, D(0, 1), true},
        {"t,tonne,ton,metricton", "mass", "tonne", 1000, D(0, 1), true},
        {"lb,lbs,pound", "mass", "pound", 0.45359237, D(0, 1)},
        {"oz,ounce", "mass", "ounce", 0.028349523125, D(0, 1)},
        {"st,stone", "mass", "stone", 6.35029318, D(0, 1)},
        {"ct,carat", "mass", "carat", 0.0002, D(0, 1)},
        {"gr,grain", "mass", "grain", 0.00006479891, D(0, 1)},
        {"shortton,uston", "mass", "short ton (US)", 907.18474, D(0, 1)},
        {"longton,ukton", "mass", "long ton (UK)", 1016.0469088, D(0, 1)},
        {"Da,dalton", "mass", "dalton", 1.66053906660e-27, D(0, 1), true},
        // time
        {"s,sec,second", "time", "second", 1, D(0, 0, 1), true},
        {"min,minute", "time", "minute", 60, D(0, 0, 1)},
        {"h,hr,hour", "time", "hour", 3600, D(0, 0, 1)},
        {"day,days,d", "time", "day", 86400, D(0, 0, 1)},
        {"week,wk", "time", "week", 604800, D(0, 0, 1)},
        {"fortnight", "time", "fortnight", 1209600, D(0, 0, 1)},
        {"month,mo", "time", "month (average)", 2629746, D(0, 0, 1)},
        {"year,yr", "time", "year (average)", 31556952, D(0, 0, 1)},
        {"decade", "time", "decade", 315569520, D(0, 0, 1)},
        {"century", "time", "century", 3155695200.0, D(0, 0, 1)},
        // frequency
        {"Hz,hertz", "frequency", "hertz", 1, D(0, 0, -1), true},
        {"rpm", "frequency", "revolutions per minute", 1.0 / 60, D(0, 0, -1)},
        {"bpm", "frequency", "beats per minute", 1.0 / 60, D(0, 0, -1)},
        // area
        {"ha,hectare", "area", "hectare", 10000, D(2)},
        {"acre,ac", "area", "acre", 4046.8564224, D(2)},
        {"are", "area", "are", 100, D(2)},
        {"sqm,m2", "area", "square metre", 1, D(2)},
        {"km2,sqkm", "area", "square kilometre", 1e6, D(2)},
        {"cm2", "area", "square centimetre", 1e-4, D(2)},
        {"sqft,ft2", "area", "square foot", 0.09290304, D(2)},
        {"sqin,in2", "area", "square inch", 0.00064516, D(2)},
        {"sqmi,mi2", "area", "square mile", 2589988.110336, D(2)},
        // volume
        {"L,l,litre,liter", "volume", "litre", 0.001, D(3), true},
        {"m3,cbm", "volume", "cubic metre", 1, D(3)},
        {"cm3,cc", "volume", "cubic centimetre", 1e-6, D(3)},
        {"ft3,cuft", "volume", "cubic foot", 0.028316846592, D(3)},
        {"in3,cuin", "volume", "cubic inch", 0.000016387064, D(3)},
        {"gal,gallon", "volume", "gallon (US)", 0.003785411784, D(3)},
        {"ukgal,impgal", "volume", "gallon (UK)", 0.00454609, D(3)},
        {"qt,quart", "volume", "quart (US)", 0.000946352946, D(3)},
        {"pt,pint", "volume", "pint (US)", 0.000473176473, D(3)},
        {"ukpt,imppint", "volume", "pint (UK)", 0.00056826125, D(3)},
        {"cup", "volume", "cup (US)", 0.0002365882365, D(3)},
        {"floz,fluidounce", "volume", "fluid ounce (US)", 0.0000295735295625, D(3)},
        {"tbsp,tablespoon", "volume", "tablespoon (US)", 0.00001478676478125, D(3)},
        {"tsp,teaspoon", "volume", "teaspoon (US)", 0.00000492892159375, D(3)},
        {"bbl,barrel", "volume", "barrel (oil)", 0.158987294928, D(3)},
        // speed
        {"mph", "speed", "miles per hour", 0.44704, D(1, 0, -1)},
        {"kph,kmh", "speed", "kilometres per hour", 1 / 3.6, D(1, 0, -1)},
        {"mps", "speed", "metres per second", 1, D(1, 0, -1)},
        {"kn,knot", "speed", "knot", 1852.0 / 3600, D(1, 0, -1)},
        {"fps", "speed", "feet per second", 0.3048, D(1, 0, -1)},
        {"mach", "speed", "mach (sea level)", 340.29, D(1, 0, -1)},
        // temperature
        {"K,kelvin", "temperature", "kelvin", 1, D(0, 0, 0, 0, 1), true},
        {"degC,\xC2\xB0" "C,celsius", "temperature", "degree Celsius", 1, D(0, 0, 0, 0, 1), false, false, 273.15},
        {"degF,\xC2\xB0" "F,fahrenheit", "temperature", "degree Fahrenheit", 5.0 / 9, D(0, 0, 0, 0, 1), false, false, 459.67},
        {"degR,rankine", "temperature", "degree Rankine", 5.0 / 9, D(0, 0, 0, 0, 1)},
        // angle
        {"rad,radian", "angle", "radian", 1, Dims{}, true, false, 0, true},
        {"deg,\xC2\xB0,degree", "angle", "degree", kPi / 180, Dims{}, false, false, 0, true},
        {"grad,gon", "angle", "gradian", kPi / 200, Dims{}, false, false, 0, true},
        {"turn,rev,revolution", "angle", "turn", 2 * kPi, Dims{}, false, false, 0, true},
        {"arcmin", "angle", "arcminute", kPi / 10800, Dims{}, false, false, 0, true},
        {"arcsec", "angle", "arcsecond", kPi / 648000, Dims{}, true, false, 0, true},
        // force, pressure
        {"N,newton", "force", "newton", 1, D(1, 1, -2), true},
        {"lbf", "force", "pound-force", 4.4482216152605, D(1, 1, -2)},
        {"kgf", "force", "kilogram-force", 9.80665, D(1, 1, -2)},
        {"dyn,dyne", "force", "dyne", 1e-5, D(1, 1, -2)},
        {"Pa,pascal", "pressure", "pascal", 1, D(-1, 1, -2), true},
        {"bar", "pressure", "bar", 100000, D(-1, 1, -2), true},
        {"atm,atmosphere", "pressure", "atmosphere", 101325, D(-1, 1, -2)},
        {"psi", "pressure", "pound per square inch", 6894.757293168, D(-1, 1, -2)},
        {"mmHg", "pressure", "millimetre of mercury", 133.322387415, D(-1, 1, -2)},
        {"torr", "pressure", "torr", 101325.0 / 760, D(-1, 1, -2)},
        {"inHg", "pressure", "inch of mercury", 3386.389, D(-1, 1, -2)},
        // energy, power
        {"J,joule", "energy", "joule", 1, D(2, 1, -2), true},
        {"cal,calorie", "energy", "calorie", 4.184, D(2, 1, -2), true},
        {"Cal,foodcalorie", "energy", "food calorie (kcal)", 4184, D(2, 1, -2)},
        {"Wh", "energy", "watt hour", 3600, D(2, 1, -2), true},
        {"eV,electronvolt", "energy", "electronvolt", 1.602176634e-19, D(2, 1, -2), true},
        {"BTU,btu", "energy", "British thermal unit", 1055.05585262, D(2, 1, -2)},
        {"erg", "energy", "erg", 1e-7, D(2, 1, -2)},
        {"therm", "energy", "therm", 105505585.262, D(2, 1, -2)},
        {"W,watt", "power", "watt", 1, D(2, 1, -3), true},
        {"hp,horsepower", "power", "horsepower", 745.69987158227022, D(2, 1, -3)},
        // electricity
        {"A,amp,ampere", "electric current", "ampere", 1, D(0, 0, 0, 1), true},
        {"V,volt", "voltage", "volt", 1, D(2, 1, -3, -1), true},
        {"ohm,\xCE\xA9", "resistance", "ohm", 1, D(2, 1, -3, -2), true},
        {"Ah", "electric charge", "ampere hour", 3600, D(0, 0, 1, 1), true},
        {"F,farad", "capacitance", "farad", 1, D(-2, -1, 4, 2), true},
        // amount, light
        {"mol,mole", "amount", "mole", 1, D(0, 0, 0, 0, 0, 1), true},
        {"cd,candela", "light", "candela", 1, D(0, 0, 0, 0, 0, 0, 1), true},
        {"lm,lumen", "light", "lumen", 1, D(0, 0, 0, 0, 0, 0, 1), true},
        {"lx,lux", "light", "lux", 1, D(-2, 0, 0, 0, 0, 0, 1), true},
        // data
        {"bit,b", "data", "bit", 1, D(0, 0, 0, 0, 0, 0, 0, 1), true, true},
        {"B,byte", "data", "byte", 8, D(0, 0, 0, 0, 0, 0, 0, 1), true, true},
        {"bps", "data rate", "bit per second", 1, D(0, 0, -1, 0, 0, 0, 0, 1), true},
        {"kbps", "data rate", "kilobit per second", 1e3, D(0, 0, -1, 0, 0, 0, 0, 1)},
        {"Mbps", "data rate", "megabit per second", 1e6, D(0, 0, -1, 0, 0, 0, 0, 1)},
        {"Gbps", "data rate", "gigabit per second", 1e9, D(0, 0, -1, 0, 0, 0, 0, 1)},
    };
    return table;
}

struct Prefix {
    const char* text;
    double factor;
};

const std::vector<Prefix>& siPrefixes() {
    static const std::vector<Prefix> p = {
        {"Y", 1e24}, {"Z", 1e21}, {"E", 1e18}, {"P", 1e15}, {"T", 1e12}, {"G", 1e9}, {"M", 1e6}, {"k", 1e3},
        {"h", 1e2}, {"da", 1e1}, {"d", 1e-1}, {"c", 1e-2}, {"m", 1e-3}, {"u", 1e-6}, {"\xC2\xB5", 1e-6},
        {"\xCE\xBC", 1e-6}, {"n", 1e-9}, {"p", 1e-12}, {"f", 1e-15}, {"a", 1e-18},
    };
    return p;
}

const std::vector<Prefix>& binaryPrefixes() {
    static const std::vector<Prefix> p = {
        {"Ki", 1024.0}, {"Mi", 1048576.0}, {"Gi", 1073741824.0}, {"Ti", 1099511627776.0},
        {"Pi", 1125899906842624.0}, {"Ei", 1152921504606846976.0}, {"K", 1e3},  // KB = kB, as people write it
    };
    return p;
}

std::vector<std::string> splitNames(const char* names) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* c = names; *c; ++c) {
        if (*c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += *c;
        }
    }
    out.push_back(cur);
    return out;
}

UnitInfo infoOf(const Row& r, const std::string& name) {
    UnitInfo u;
    u.name = name;
    u.category = r.category;
    u.title = r.title;
    u.scale = r.scale;
    u.offset = r.offset;
    u.dims = r.dims;
    u.angle = r.angle;
    return u;
}

bool exactLookup(const std::string& name, UnitInfo& out, const Row** row) {
    for (const auto& r : rows()) {
        for (const auto& n : splitNames(r.names)) {
            if (n == name) {
                out = infoOf(r, name);
                if (row) *row = &r;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

bool Dims::none() const {
    for (auto v : e) {
        if (v) return false;
    }
    return true;
}

bool Dims::operator==(const Dims& o) const {
    for (int i = 0; i < kDims; ++i) {
        if (e[i] != o.e[i]) return false;
    }
    return true;
}

bool findUnit(const std::string& name, UnitInfo& out) {
    if (name.empty()) return false;
    if (exactLookup(name, out, nullptr)) return true;
    // plurals of long names: "metres", "inches", "feet" is listed
    if (name.size() > 3) {
        if (name.back() == 's' && exactLookup(name.substr(0, name.size() - 1), out, nullptr) && out.name.size() > 2) {
            out.name = name;
            return true;
        }
        if (name.size() > 4 && name.compare(name.size() - 2, 2, "es") == 0 && exactLookup(name.substr(0, name.size() - 2), out, nullptr)) {
            out.name = name;
            return true;
        }
    }
    // long prefixed names: "kilometre", "milliseconds", "megabytes"
    static const std::vector<Prefix> words = {{"yotta", 1e24}, {"zetta", 1e21}, {"exa", 1e18}, {"peta", 1e15}, {"tera", 1e12},
                                              {"giga", 1e9}, {"mega", 1e6}, {"kilo", 1e3}, {"hecto", 1e2}, {"deca", 1e1},
                                              {"deci", 1e-1}, {"centi", 1e-2}, {"milli", 1e-3}, {"micro", 1e-6}, {"nano", 1e-9},
                                              {"pico", 1e-12}, {"femto", 1e-15}, {"atto", 1e-18}, {"kibi", 1024.0},
                                              {"mebi", 1048576.0}, {"gibi", 1073741824.0}, {"tebi", 1099511627776.0}};
    for (const auto* list : {&words, &siPrefixes(), &binaryPrefixes()}) {
        for (const auto& p : *list) {
            const std::string pre = p.text;
            if (name.size() <= pre.size() || name.compare(0, pre.size(), pre) != 0) continue;
            UnitInfo base;
            const Row* row = nullptr;
            std::string rest = name.substr(pre.size());
            bool found = exactLookup(rest, base, &row);
            if (!found && rest.size() > 3 && rest.back() == 's') found = exactLookup(rest.substr(0, rest.size() - 1), base, &row);
            if (!found || !row) continue;
            const bool isBinary = list == &binaryPrefixes() || p.factor == 1024.0 || p.factor == 1048576.0 ||
                                  p.factor == 1073741824.0 || p.factor == 1099511627776.0;
            if (isBinary ? !row->binary : !row->prefix) continue;
            out = base;
            out.name = name;
            out.scale = base.scale * p.factor;
            out.title = std::string(list == &words ? p.text : "") + base.title;
            if (list != &words) out.title = pre + " " + base.title;
            return true;
        }
    }
    return false;
}

std::vector<UnitInfo> unitList(const std::string& category) {
    std::vector<UnitInfo> out;
    for (const auto& r : rows()) {
        if (!category.empty() && category != r.category) continue;
        out.push_back(infoOf(r, splitNames(r.names).front()));
    }
    return out;
}

std::vector<std::string> unitCategories() {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& r : rows()) {
        if (seen.insert(r.category).second) out.push_back(r.category);
    }
    return out;
}

}  // namespace wetwaresuite::calc
