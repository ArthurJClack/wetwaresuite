#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/pdf/fonts.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "internal.hpp"

namespace wetwaresuite::animate::detail {

namespace fs = std::filesystem;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string num(double v) {
    if (std::fabs(v) < 5e-7) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string base64Encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& in) {
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (char ch : in) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else continue;
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xff);
        }
    }
    return out;
}

namespace {

pdf::Font fontOf(const Element& e) {
    pdf::Font f;
    f.family = e.font == "serif" ? pdf::Family::Serif : e.font == "mono" ? pdf::Family::Mono : pdf::Family::Sans;
    f.bold = e.bold;
    f.italic = e.italic;
    return f;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out(1);
    for (char c : text) {
        if (c == '\n') out.emplace_back();
        else if (c != '\r') out.back() += c;
    }
    return out;
}

constexpr double kLineHeight = 1.2;

}  // namespace

void textBox(const Element& e, double size, double& w, double& h) {
    const auto ls = lines(e.text);
    w = 0;
    for (const auto& l : ls) w = std::max(w, pdf::textWidthPx(l, fontOf(e), size));
    h = static_cast<double>(ls.size()) * size * kLineHeight;
}

Image renderText(const Element& e, double size, Color color, double k) {
    double w = 0, h = 0;
    textBox(e, size, w, h);
    const int iw = std::max(1, static_cast<int>(std::ceil(w * k)) + 2);
    const int ih = std::max(1, static_cast<int>(std::ceil(h * k)) + 1);
    Image img(iw, ih, Color{color.r, color.g, color.b, 0});
    const auto ls = lines(e.text);
    for (std::size_t i = 0; i < ls.size(); ++i) {
        const double baseline = (static_cast<double>(i) * kLineHeight + 0.93) * size * k;
        pdf::drawText(img, 0, baseline, ls[i], fontOf(e), size * k, color);
    }
    return img;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    return static_cast<bool>(out);
}

TempDir::TempDir() {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    for (int attempt = 0; attempt < 100; ++attempt) {
        fs::path p = fs::temp_directory_path() / ("wetwaresuite-animate-" + std::to_string(gen() % 1000000000000ULL));
        std::error_code ec;
        if (fs::create_directory(p, ec)) {
            path_ = p;
            return;
        }
    }
    throw std::runtime_error("can't create a temporary folder");
}

TempDir::~TempDir() {
    std::error_code ec;
    if (!path_.empty()) fs::remove_all(path_, ec);
}

}  // namespace wetwaresuite::animate::detail
