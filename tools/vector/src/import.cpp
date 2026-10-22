// PDF (and Illustrator .ai, which is PDF inside) and simple EPS as editable
// shapes. A small content-stream interpreter follows the graphics state (cm,
// q/Q, colours, line settings, ExtGState alpha), builds paths and turns every
// fill or stroke into a path shape; clipping paths become clip groups and form
// XObjects are followed. Text, images and shadings are skipped. EPS goes
// through a tiny PostScript reader that knows the path, paint and def basics.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "wetwaresuite/pdf/reader.hpp"
#include "wetwaresuite/pdf/view.hpp"
#include "internal.hpp"

namespace wetwaresuite::vector {

namespace {

constexpr double kPx = 4.0 / 3.0;  // points to CSS px

struct Token {
    enum Kind { Num, Name, Op, ArrayStart, ArrayEnd, ProcStart, ProcEnd, Other } kind = Other;
    double num = 0;
    std::string text;
};

// Tokens of a PDF content stream or PostScript program.
class Lexer {
public:
    explicit Lexer(const std::string& s) : s_(s) {}
    bool next(Token& t) {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++i_;
                continue;
            }
            if (c == '%') {
                while (i_ < s_.size() && s_[i_] != '\n' && s_[i_] != '\r') ++i_;
                continue;
            }
            break;
        }
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        t = {};
        if (c == '[' || c == ']' || c == '{' || c == '}') {
            t.kind = c == '[' ? Token::ArrayStart : c == ']' ? Token::ArrayEnd : c == '{' ? Token::ProcStart : Token::ProcEnd;
            ++i_;
            return true;
        }
        if (c == '(') {  // string: skipped as a whole
            int depth = 0;
            for (; i_ < s_.size(); ++i_) {
                if (s_[i_] == '\\') {
                    ++i_;
                    continue;
                }
                if (s_[i_] == '(') ++depth;
                if (s_[i_] == ')' && --depth == 0) {
                    ++i_;
                    break;
                }
            }
            t.kind = Token::Other;
            return true;
        }
        if (c == '<') {
            if (i_ + 1 < s_.size() && s_[i_ + 1] == '<') {
                i_ += 2;
                t.kind = Token::Other;
                t.text = "<<";
                return true;
            }
            const auto end = s_.find('>', i_);
            i_ = end == std::string::npos ? s_.size() : end + 1;
            t.kind = Token::Other;
            return true;
        }
        if (c == '>') {
            i_ += (i_ + 1 < s_.size() && s_[i_ + 1] == '>') ? 2 : 1;
            t.kind = Token::Other;
            t.text = ">>";
            return true;
        }
        if (c == '/') {
            ++i_;
            const std::size_t b = i_;
            while (i_ < s_.size() && !delim(s_[i_])) ++i_;
            t.kind = Token::Name;
            t.text = s_.substr(b, i_ - b);
            return true;
        }
        const std::size_t b = i_;
        while (i_ < s_.size() && !delim(s_[i_])) ++i_;
        if (i_ == b) {
            ++i_;
            t.kind = Token::Other;
            return true;
        }
        t.text = s_.substr(b, i_ - b);
        char* end = nullptr;
        t.num = std::strtod(t.text.c_str(), &end);
        t.kind = (end && *end == 0 && (std::isdigit(static_cast<unsigned char>(t.text[0])) || t.text[0] == '-' || t.text[0] == '+' || t.text[0] == '.'))
                     ? Token::Num
                     : Token::Op;
        if (t.text == "BI") skipInlineImage();
        return true;
    }

private:
    const std::string& s_;
    std::size_t i_ = 0;
    static bool delim(char c) { return std::isspace(static_cast<unsigned char>(c)) || std::strchr("()<>[]{}/%", c); }
    void skipInlineImage() {
        const auto at = s_.find("EI", i_);
        i_ = at == std::string::npos ? s_.size() : at + 2;
    }
};

struct GState {
    Matrix ctm;
    Color fill{0, 0, 0, 255}, stroke{0, 0, 0, 255};
    double fillAlpha = 1, strokeAlpha = 1;
    double width = 1;
    StrokeStyle line;
    int fillComps = 1, strokeComps = 1;  // colour space size: 1 gray, 3 rgb, 4 cmyk
    std::string parent;                  // group the shapes go into (a clip group after W n)
};

Color fromComps(const std::vector<double>& v) {
    auto c8 = [](double x) { return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0, 1.0) * 255)); };
    if (v.size() >= 4) {
        const double k = v[3];
        return {c8((1 - v[0]) * (1 - k)), c8((1 - v[1]) * (1 - k)), c8((1 - v[2]) * (1 - k)), 255};
    }
    if (v.size() == 3) return {c8(v[0]), c8(v[1]), c8(v[2]), 255};
    if (v.size() == 1) return {c8(v[0]), c8(v[0]), c8(v[0]), 255};
    return {128, 128, 128, 255};
}

// Shared by both readers: builds shapes from paths painted in page space.
class Builder {
public:
    Builder(VectorDoc& doc, double pageHeight) : doc_(doc), pageH_(pageHeight) {}

    Point toCanvas(Point p) const { return {p.x * kPx, (pageH_ - p.y) * kPx}; }

    void moveTo(const GState& g, Point p) {
        flushSub();
        cur_.nodes.push_back(node(g.ctm.apply(p)));
        last_ = p;
        start_ = p;
    }
    void lineTo(const GState& g, Point p) {
        if (cur_.nodes.empty()) cur_.nodes.push_back(node(g.ctm.apply(last_)));
        cur_.nodes.push_back(node(g.ctm.apply(p)));
        last_ = p;
    }
    void curveTo(const GState& g, Point c1, Point c2, Point p) {
        if (cur_.nodes.empty()) cur_.nodes.push_back(node(g.ctm.apply(last_)));
        cur_.nodes.back().out = toCanvas(g.ctm.apply(c1));
        Node n = node(g.ctm.apply(p));
        n.in = toCanvas(g.ctm.apply(c2));
        cur_.nodes.push_back(n);
        last_ = p;
    }
    void close() {
        if (!cur_.nodes.empty()) {
            if (cur_.nodes.size() > 1) {
                const Point a = cur_.nodes.front().p, b = cur_.nodes.back().p;
                if (std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6) {
                    cur_.nodes.front().in = cur_.nodes.back().in;
                    cur_.nodes.pop_back();
                }
            }
            cur_.closed = true;
            flushSub();
        }
        last_ = start_;
    }
    void rect(const GState& g, double x, double y, double w, double h) {
        moveTo(g, {x, y});
        lineTo(g, {x + w, y});
        lineTo(g, {x + w, y + h});
        lineTo(g, {x, y + h});
        close();
    }
    Point current() const { return last_; }

    // f/S/B...: the path becomes a shape. Returns its id ("" when the path was empty).
    std::string paint(const GState& g, bool fill, bool stroke, bool evenOdd) {
        flushSub();
        if (path_.empty()) return "";
        Shape s;
        s.kind = ShapeKind::Path;
        s.d = pathData(path_);
        path_.clear();
        s.fillRule = evenOdd ? "evenodd" : "nonzero";
        if (fill) {
            Color c = g.fill;
            c.a = static_cast<std::uint8_t>(std::lround(255 * std::clamp(g.fillAlpha, 0.0, 1.0)));
            s.fill = c;
        } else {
            s.fill.reset();
        }
        if (stroke) {
            Color c = g.stroke;
            c.a = static_cast<std::uint8_t>(std::lround(255 * std::clamp(g.strokeAlpha, 0.0, 1.0)));
            s.stroke = c;
            const double k = g.ctm.scaleFactor() * kPx;
            s.strokeWidth = std::max(g.width * k, 0.1);
            s.line = g.line;
            for (auto& d : s.line.dash) d *= k;
            s.line.dashOffset *= k;
        }
        s.parent = g.parent;
        const std::string id = doc_.add(std::move(s));
        doc_.shapes.back().parent = g.parent;
        ++count_;
        return id;
    }
    void discard() {
        flushSub();
        path_.clear();
    }
    // W n: a clip group for what follows in this q block
    std::string clipGroup(const std::string& parent, bool evenOdd) {
        flushSub();
        if (path_.empty()) return parent;
        Shape g;
        g.kind = ShapeKind::Group;
        g.name = "clip group";
        g.parent = parent;
        const std::string gid = doc_.add(std::move(g));
        doc_.shapes.back().parent = parent;
        Shape c;
        c.kind = ShapeKind::Path;
        c.name = "clip";
        c.d = pathData(path_);
        c.fillRule = evenOdd ? "evenodd" : "nonzero";
        c.parent = gid;
        const std::string cid = doc_.add(std::move(c));
        doc_.shapes.back().parent = gid;
        doc_.shapes[static_cast<std::size_t>(doc_.find(gid))].clip = cid;
        path_.clear();
        return gid;
    }
    int count() const { return count_; }

private:
    VectorDoc& doc_;
    double pageH_;
    BezierPath path_;
    SubPath cur_;
    Point last_{0, 0}, start_{0, 0};
    int count_ = 0;

    Node node(Point pagePoint) const {
        const Point p = toCanvas(pagePoint);
        return {p, p, p};
    }
    void flushSub() {
        if (!cur_.nodes.empty()) path_.push_back(std::move(cur_));
        cur_ = {};
    }
};

class PdfReader {
public:
    PdfReader(const pdf::PdfFile& file, Builder& b) : file_(file), b_(b) {}

    void run(const std::string& content, const pdf::Object& resources, GState g, int depth) {
        if (depth > 12) return;
        Lexer lex(content);
        Token t;
        std::vector<double> nums;
        std::vector<std::string> names;
        std::vector<double> array;
        bool inArray = false;
        std::vector<GState> stack;
        bool clipPending = false, clipEvenOdd = false;
        auto op = [&](const std::string& o) {
            auto n = [&](std::size_t k) { return k < nums.size() ? nums[nums.size() - (k + 1)] : 0.0; };  // from the end
            auto arg = [&](std::size_t i, std::size_t count) { return nums.size() >= count ? nums[nums.size() - count + i] : 0.0; };
            if (o == "q") stack.push_back(g);
            else if (o == "Q") {
                if (!stack.empty()) g = stack.back(), stack.pop_back();
            } else if (o == "cm" && nums.size() >= 6) {
                g.ctm = g.ctm * Matrix{arg(0, 6), arg(1, 6), arg(2, 6), arg(3, 6), arg(4, 6), arg(5, 6)};
            } else if (o == "m" && nums.size() >= 2) b_.moveTo(g, {arg(0, 2), arg(1, 2)});
            else if (o == "l" && nums.size() >= 2) b_.lineTo(g, {arg(0, 2), arg(1, 2)});
            else if (o == "c" && nums.size() >= 6) b_.curveTo(g, {arg(0, 6), arg(1, 6)}, {arg(2, 6), arg(3, 6)}, {arg(4, 6), arg(5, 6)});
            else if (o == "v" && nums.size() >= 4) b_.curveTo(g, b_.current(), {arg(0, 4), arg(1, 4)}, {arg(2, 4), arg(3, 4)});
            else if (o == "y" && nums.size() >= 4) b_.curveTo(g, {arg(0, 4), arg(1, 4)}, {arg(2, 4), arg(3, 4)}, {arg(2, 4), arg(3, 4)});
            else if (o == "h") b_.close();
            else if (o == "re" && nums.size() >= 4) b_.rect(g, arg(0, 4), arg(1, 4), arg(2, 4), arg(3, 4));
            else if (o == "W" || o == "W*") clipPending = true, clipEvenOdd = o == "W*";
            else if (o == "n") {
                if (clipPending) g.parent = b_.clipGroup(g.parent, clipEvenOdd);
                else b_.discard();
                clipPending = false;
            } else if (o == "f" || o == "F" || o == "f*" || o == "S" || o == "s" || o == "B" || o == "B*" || o == "b" || o == "b*") {
                if (o == "s" || o == "b" || o == "b*") b_.close();
                const bool fill = o != "S" && o != "s", stroke = o == "S" || o == "s" || o[0] == 'B' || o[0] == 'b';
                if (clipPending) {  // painted and clipping at once: paint, then clip with the same path (rare)
                    clipPending = false;
                }
                b_.paint(g, fill, stroke, o.find('*') != std::string::npos);
            } else if (o == "w" && !nums.empty()) g.width = n(0);
            else if (o == "J" && !nums.empty()) g.line.cap = n(0) == 0 ? "butt" : n(0) == 1 ? "round" : "square";
            else if (o == "j" && !nums.empty()) g.line.join = n(0) == 0 ? "miter" : n(0) == 1 ? "round" : "bevel";
            else if (o == "M" && !nums.empty()) g.line.miterLimit = std::max(1.0, n(0));
            else if (o == "d") {
                g.line.dash = array;
                g.line.dashOffset = nums.empty() ? 0 : n(0);
            } else if (o == "g" && !nums.empty()) g.fill = fromComps({n(0)}), g.fillComps = 1;
            else if (o == "G" && !nums.empty()) g.stroke = fromComps({n(0)}), g.strokeComps = 1;
            else if (o == "rg" && nums.size() >= 3) g.fill = fromComps({arg(0, 3), arg(1, 3), arg(2, 3)}), g.fillComps = 3;
            else if (o == "RG" && nums.size() >= 3) g.stroke = fromComps({arg(0, 3), arg(1, 3), arg(2, 3)}), g.strokeComps = 3;
            else if (o == "k" && nums.size() >= 4) g.fill = fromComps({arg(0, 4), arg(1, 4), arg(2, 4), arg(3, 4)}), g.fillComps = 4;
            else if (o == "K" && nums.size() >= 4) g.stroke = fromComps({arg(0, 4), arg(1, 4), arg(2, 4), arg(3, 4)}), g.strokeComps = 4;
            else if (o == "cs" || o == "CS") {
                int comps = 3;
                if (!names.empty()) comps = spaceComps(names.back(), resources);
                (o == "cs" ? g.fillComps : g.strokeComps) = comps;
                (o == "cs" ? g.fill : g.stroke) = comps == 4 ? Color{0, 0, 0, 255} : Color{0, 0, 0, 255};
            } else if (o == "sc" || o == "scn" || o == "SC" || o == "SCN") {
                const bool f = o[0] == 's';
                const int comps = f ? g.fillComps : g.strokeComps;
                std::vector<double> v;
                if (comps > 0 && static_cast<int>(nums.size()) >= comps) {
                    for (int i = 0; i < comps; ++i) v.push_back(arg(static_cast<std::size_t>(i), static_cast<std::size_t>(comps)));
                }
                (f ? g.fill : g.stroke) = v.empty() ? Color{128, 128, 128, 255} : fromComps(v);  // patterns: grey
            } else if (o == "gs" && !names.empty()) {
                extGState(names.back(), resources, g);
            } else if (o == "Do" && !names.empty()) {
                xobject(names.back(), resources, g, depth);
            }
        };
        while (lex.next(t)) {
            switch (t.kind) {
                case Token::Num:
                    if (inArray) array.push_back(t.num);
                    else nums.push_back(t.num);
                    break;
                case Token::Name:
                    if (!inArray) names.push_back(t.text);
                    break;
                case Token::ArrayStart:
                    inArray = true;
                    array.clear();
                    break;
                case Token::ArrayEnd: inArray = false; break;
                case Token::Op:
                    if (inArray) break;
                    op(t.text);
                    nums.clear();
                    names.clear();
                    break;
                default: break;
            }
        }
    }

private:
    const pdf::PdfFile& file_;
    Builder& b_;

    const pdf::Object* lookup(const pdf::Object& resources, const char* kind, const std::string& name) const {
        const pdf::Object& res = file_.resolve(resources);
        const pdf::Object* dict = res.get(kind);
        if (!dict) return nullptr;
        const pdf::Object& d = file_.resolve(*dict);
        const pdf::Object* v = d.get(name);
        return v ? &file_.resolve(*v) : nullptr;
    }

    int spaceComps(const std::string& name, const pdf::Object& resources) const {
        if (name == "DeviceGray" || name == "G" || name == "CalGray") return 1;
        if (name == "DeviceRGB" || name == "RGB" || name == "CalRGB") return 3;
        if (name == "DeviceCMYK" || name == "CMYK") return 4;
        if (name == "Pattern") return 0;
        if (const pdf::Object* cs = lookup(resources, "ColorSpace", name)) {
            if (cs->kind == pdf::Object::Array && !cs->items.empty()) {
                const pdf::Object& fam = file_.resolve(cs->items[0]);
                if (fam.isName("ICCBased") && cs->items.size() > 1) {
                    const pdf::Object& stream = file_.resolve(cs->items[1]);
                    if (const pdf::Object* n = stream.get("N")) return static_cast<int>(file_.resolve(*n).number);
                }
                if (fam.isName("DeviceCMYK")) return 4;
                if (fam.isName("DeviceGray") || fam.isName("CalGray")) return 1;
                if (fam.isName("Separation")) return 1;  // a spot colour's tint: read as gray
                if (fam.isName("Pattern")) return 0;
            } else if (cs->kind == pdf::Object::Name) {
                return spaceComps(cs->str, resources);
            }
        }
        return 3;
    }

    void extGState(const std::string& name, const pdf::Object& resources, GState& g) const {
        const pdf::Object* gs = lookup(resources, "ExtGState", name);
        if (!gs) return;
        if (const pdf::Object* ca = gs->get("ca")) g.fillAlpha = file_.resolve(*ca).number;
        if (const pdf::Object* CA = gs->get("CA")) g.strokeAlpha = file_.resolve(*CA).number;
        if (const pdf::Object* lw = gs->get("LW")) g.width = file_.resolve(*lw).number;
    }

    void xobject(const std::string& name, const pdf::Object& resources, const GState& g, int depth) {
        const pdf::Object* x = lookup(resources, "XObject", name);
        if (!x || x->kind != pdf::Object::Stream) return;
        const pdf::Object* sub = x->get("Subtype");
        if (!sub || !file_.resolve(*sub).isName("Form")) return;  // images are skipped
        GState inner = g;
        if (const pdf::Object* m = x->get("Matrix")) {
            const pdf::Object& a = file_.resolve(*m);
            if (a.items.size() == 6) {
                double v[6];
                for (int i = 0; i < 6; ++i) v[i] = file_.resolve(a.items[static_cast<std::size_t>(i)]).number;
                inner.ctm = g.ctm * Matrix{v[0], v[1], v[2], v[3], v[4], v[5]};
            }
        }
        const pdf::Object* res = x->get("Resources");
        run(file_.streamData(*x), res ? *res : resources, inner, depth + 1);
    }
};

// Just enough PostScript for EPS files: numbers, names, procedures and def, paths, colours, gsave.
class PsReader {
public:
    explicit PsReader(Builder& b) : b_(b) {}

    void run(const std::string& program) {
        Lexer lex(program);
        Token t;
        std::vector<Token> all;
        while (lex.next(t)) all.push_back(t);
        exec(all, 0, all.size(), 0);
    }

private:
    Builder& b_;
    GState g_;
    std::vector<GState> saved_;
    struct Item {
        bool isProc = false;
        double num = 0;
        std::string name;
        std::vector<Token> proc;
    };
    std::vector<Item> stack_;
    std::map<std::string, Item> dict_;

    double pop() {
        if (stack_.empty()) return 0;
        const double v = stack_.back().num;
        stack_.pop_back();
        return v;
    }

    void exec(const std::vector<Token>& code, std::size_t from, std::size_t to, int depth) {
        if (depth > 32) return;
        for (std::size_t i = from; i < to; ++i) {
            const Token& t = code[i];
            if (t.kind == Token::Num) stack_.push_back({false, t.num, "", {}});
            else if (t.kind == Token::Name) stack_.push_back({false, 0, t.text, {}});
            else if (t.kind == Token::ProcStart) {
                int level = 1;
                std::size_t j = i + 1;
                for (; j < to && level > 0; ++j) {
                    if (code[j].kind == Token::ProcStart) ++level;
                    if (code[j].kind == Token::ProcEnd) --level;
                }
                Item p;
                p.isProc = true;
                p.proc.assign(code.begin() + static_cast<std::ptrdiff_t>(i + 1), code.begin() + static_cast<std::ptrdiff_t>(j - 1));
                stack_.push_back(std::move(p));
                i = j - 1;
            } else if (t.kind == Token::Op) {
                op(t.text, depth);
            }
        }
    }

    void op(const std::string& o, int depth) {
        if (auto it = dict_.find(o); it != dict_.end()) {
            if (it->second.isProc) {
                const std::vector<Token> body = it->second.proc;
                exec(body, 0, body.size(), depth + 1);
            } else {
                stack_.push_back(it->second);
            }
            return;
        }
        if (o == "def") {
            if (stack_.size() >= 2) {
                Item v = stack_.back();
                stack_.pop_back();
                const std::string key = stack_.back().name;
                stack_.pop_back();
                if (!key.empty()) dict_[key] = std::move(v);
            }
        } else if (o == "bind" || o == "load") {
        } else if (o == "exch") {
            if (stack_.size() >= 2) std::swap(stack_[stack_.size() - 1], stack_[stack_.size() - 2]);
        } else if (o == "dup") {
            if (!stack_.empty()) stack_.push_back(stack_.back());
        } else if (o == "pop") {
            if (!stack_.empty()) stack_.pop_back();
        } else if (o == "add" || o == "sub" || o == "mul" || o == "div") {
            const double b = pop(), a = pop();
            stack_.push_back({false, o == "add" ? a + b : o == "sub" ? a - b : o == "mul" ? a * b : (b != 0 ? a / b : 0), "", {}});
        } else if (o == "neg") {
            stack_.push_back({false, -pop(), "", {}});
        } else if (o == "moveto") {
            const double y = pop(), x = pop();
            b_.moveTo(g_, {x, y});
        } else if (o == "rmoveto") {
            const double y = pop(), x = pop();
            const Point c = b_.current();
            b_.moveTo(g_, {c.x + x, c.y + y});
        } else if (o == "lineto") {
            const double y = pop(), x = pop();
            b_.lineTo(g_, {x, y});
        } else if (o == "rlineto") {
            const double y = pop(), x = pop();
            const Point c = b_.current();
            b_.lineTo(g_, {c.x + x, c.y + y});
        } else if (o == "curveto" || o == "rcurveto") {
            double v[6];
            for (int k = 5; k >= 0; --k) v[k] = pop();
            Point c{0, 0};
            if (o == "rcurveto") c = b_.current();
            b_.curveTo(g_, {c.x + v[0], c.y + v[1]}, {c.x + v[2], c.y + v[3]}, {c.x + v[4], c.y + v[5]});
        } else if (o == "closepath") {
            b_.close();
        } else if (o == "newpath") {
            b_.discard();
        } else if (o == "fill" || o == "eofill") {
            b_.paint(g_, true, false, o == "eofill");
        } else if (o == "stroke") {
            b_.paint(g_, false, true, false);
        } else if (o == "clip" || o == "eoclip") {
            g_.parent = b_.clipGroup(g_.parent, o == "eoclip");
        } else if (o == "setrgbcolor") {
            const double bl = pop(), gr = pop(), r = pop();
            g_.fill = g_.stroke = fromComps({r, gr, bl});
        } else if (o == "setgray") {
            g_.fill = g_.stroke = fromComps({pop()});
        } else if (o == "setcmykcolor") {
            const double k = pop(), y = pop(), m = pop(), c = pop();
            g_.fill = g_.stroke = fromComps({c, m, y, k});
        } else if (o == "setlinewidth") {
            g_.width = pop();
        } else if (o == "setlinecap") {
            const double v = pop();
            g_.line.cap = v == 0 ? "butt" : v == 1 ? "round" : "square";
        } else if (o == "setlinejoin") {
            const double v = pop();
            g_.line.join = v == 0 ? "miter" : v == 1 ? "round" : "bevel";
        } else if (o == "setmiterlimit") {
            g_.line.miterLimit = std::max(1.0, pop());
        } else if (o == "gsave") {
            saved_.push_back(g_);
        } else if (o == "grestore") {
            if (!saved_.empty()) g_ = saved_.back(), saved_.pop_back();
        } else if (o == "translate") {
            const double y = pop(), x = pop();
            g_.ctm = g_.ctm * Matrix::translate(x, y);
        } else if (o == "scale") {
            const double y = pop(), x = pop();
            g_.ctm = g_.ctm * Matrix::scale(x, y);
        } else if (o == "rotate") {
            g_.ctm = g_.ctm * Matrix::rotate(pop());
        } else if (o == "concat") {
            // the matrix came as an array: its numbers are the last six on the stack
            double v[6];
            for (int k = 5; k >= 0; --k) v[k] = pop();
            g_.ctm = g_.ctm * Matrix{v[0], v[1], v[2], v[3], v[4], v[5]};
        }
        // anything else (showpage, setdash, shfill, text, ...) is skipped
    }
};

}  // namespace

std::shared_ptr<VectorDoc> openPdf(const std::filesystem::path& path, int page) {
    pdf::PdfFile file = pdf::PdfFile::load(path);
    if (file.encrypted()) throw std::runtime_error(path.string() + " is encrypted");
    if (page < 1 || page > file.pageCount()) {
        throw std::runtime_error("page " + std::to_string(page) + " doesn't exist (the file has " + std::to_string(file.pageCount()) + ")");
    }
    const auto& info = file.pages()[static_cast<std::size_t>(page - 1)];
    auto doc = std::make_shared<VectorDoc>();
    doc->width = std::max(1, static_cast<int>(std::lround(info.width * kPx)));
    doc->height = std::max(1, static_cast<int>(std::lround(info.height * kPx)));
    doc->background = {255, 255, 255, 255};
    // the MediaBox may not start at 0,0
    double x0 = 0, y0 = 0;
    if (const pdf::Object* mb = info.dict.get("MediaBox")) {
        const pdf::Object& box = file.resolve(*mb);
        if (box.items.size() == 4) x0 = file.resolve(box.items[0]).number, y0 = file.resolve(box.items[1]).number;
    }
    Builder b(*doc, info.height);
    GState g;
    g.ctm = Matrix::translate(-x0, -y0);
    const pdf::Object* res = info.dict.get("Resources");
    PdfReader reader(file, b);
    reader.run(pdf::pageContent(file, page - 1), res ? *res : pdf::Object{}, g, 0);
    doc->path = path.string();
    doc->refresh();
    return doc;
}

std::shared_ptr<VectorDoc> openEps(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    // DOS EPS: a binary header pointing at the PostScript part
    if (text.size() > 30 && static_cast<unsigned char>(text[0]) == 0xC5 && static_cast<unsigned char>(text[1]) == 0xD0) {
        auto u32 = [&](std::size_t at) {
            return static_cast<std::size_t>(static_cast<unsigned char>(text[at])) | static_cast<std::size_t>(static_cast<unsigned char>(text[at + 1])) << 8 |
                   static_cast<std::size_t>(static_cast<unsigned char>(text[at + 2])) << 16 | static_cast<std::size_t>(static_cast<unsigned char>(text[at + 3])) << 24;
        };
        const std::size_t off = u32(4), len = u32(8);
        if (off < text.size()) text = text.substr(off, std::min(len, text.size() - off));
    }
    if (text.rfind("%!", 0) != 0) throw std::runtime_error(path.string() + " isn't PostScript/EPS");
    double bx0 = 0, by0 = 0, bx1 = 612, by1 = 792;
    for (const char* key : {"%%HiResBoundingBox:", "%%BoundingBox:"}) {
        const auto at = text.find(key);
        if (at == std::string::npos) continue;
        std::istringstream b(text.substr(at + std::strlen(key), 200));
        if (b >> bx0 >> by0 >> bx1 >> by1) break;
    }
    auto doc = std::make_shared<VectorDoc>();
    doc->width = std::max(1, static_cast<int>(std::lround((bx1 - bx0) * kPx)));
    doc->height = std::max(1, static_cast<int>(std::lround((by1 - by0) * kPx)));
    doc->background = {255, 255, 255, 255};
    Builder b(*doc, by1 - by0);
    PsReader reader(b);
    reader.run(text);
    // the bounding box may not start at 0,0: move everything by its corner
    if (bx0 != 0 || by0 != 0) {
        for (std::size_t i = 0; i < doc->shapes.size(); ++i) {
            if (doc->shapes[i].kind == ShapeKind::Path) {
                BezierPath bp = parsePath(doc->shapes[i].d);
                transformPath(bp, Matrix::translate(-bx0 * kPx, by0 * kPx));
                doc->shapes[i].d = pathData(bp);
            }
        }
    }
    doc->path = path.string();
    return doc;
}

std::shared_ptr<VectorDoc> openAny(const std::filesystem::path& path, int page) {
    std::string e = path.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (e == ".pdf" || e == ".ai") return openPdf(path, page);
    if (e == ".eps" || e == ".ps") return openEps(path);
    return openSvg(path);
}

}  // namespace wetwaresuite::vector
