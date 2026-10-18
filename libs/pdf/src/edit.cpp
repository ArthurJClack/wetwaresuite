#include "wetwaresuite/pdf/edit.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/pdf/fonts.hpp"
#include "lexer.hpp"

namespace wetwaresuite::pdf {

namespace {

std::string numText(double v) {
    if (std::abs(v - std::round(v)) < 1e-9 && std::abs(v) < 1e15) return std::to_string(static_cast<long long>(std::llround(v)));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string nameText(const std::string& n) {
    std::string out = "/";
    for (char c : n) {
        unsigned char b = static_cast<unsigned char>(c);
        if (b < 33 || b > 126 || c == '#' || detail::isDelim(c)) {
            char buf[4];
            std::snprintf(buf, sizeof buf, "#%02X", b);
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

// a direct copy of a dictionary (resolving a reference to it)
Object directDict(const PdfFile* f, const Object* o) {
    Object d;
    d.kind = Object::Dict;
    if (!o) return d;
    const Object& r = f ? f->resolve(*o) : *o;
    if (r.kind == Object::Dict) d = r;
    return d;
}

}  // namespace

Rebuilder::Rebuilder(const PdfFile* main) : main_(main) {
    if (main_) {
        if (main_->encrypted()) throw std::runtime_error("encrypted PDFs can't be edited");
        fileIndex(main_);
    }
}

std::size_t Rebuilder::fileIndex(const PdfFile* f) {
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (files_[i] == f) return i;
    }
    if (f->encrypted()) throw std::runtime_error("encrypted PDFs can't be edited");
    files_.push_back(f);
    std::set<int> nums;
    for (const auto& pi : f->pages()) nums.insert(pi.object);
    pageObjects_.push_back(std::move(nums));
    return files_.size() - 1;
}

int Rebuilder::reserve() {
    out_.emplace_back();
    return static_cast<int>(out_.size());
}

void Rebuilder::set(int id, std::string body) { out_.at(static_cast<std::size_t>(id - 1)) = std::move(body); }

int Rebuilder::add(std::string body) {
    const int id = reserve();
    set(id, std::move(body));
    return id;
}

int Rebuilder::addStream(const std::string& dict, const std::string& data, bool compress) {
    if (compress) {
        const std::string z = pdf::deflate(data);
        return add("<< " + dict + " /Filter /FlateDecode /Length " + std::to_string(z.size()) + " >>\nstream\n" + z + "\nendstream");
    }
    return add("<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream");
}

Object Rebuilder::outputRef(int id) {
    Object o = Object::makeRef(id);
    o.gen = -1;
    return o;
}

void Rebuilder::replace(const PdfFile& file, int num, Object o) { overrides_[{fileIndex(&file), num}] = std::move(o); }

void Rebuilder::setCatalogEntry(const std::string& key, Object value) {
    for (auto& [k, v] : catalogEntries_) {
        if (k == key) {
            v = std::move(value);
            return;
        }
    }
    catalogEntries_.emplace_back(key, std::move(value));
}

const Object& Rebuilder::source(std::size_t f, int num) const {
    auto it = overrides_.find({f, num});
    if (it != overrides_.end()) return it->second;
    return files_[f]->object(num);
}

int Rebuilder::refFor(std::size_t f, int num) {
    Key k{f, num};
    auto sp = selectedPage_.find(k);
    if (sp != selectedPage_.end()) return sp->second;
    if (pageObjects_[f].count(num)) return 0;  // a page that isn't coming along
    auto it = mapped_.find(k);
    if (it != mapped_.end()) return it->second;
    if (source(f, num).kind == Object::Null) return 0;
    const int id = reserve();
    mapped_[k] = id;
    queue_.push_back(k);
    return id;
}

std::string Rebuilder::write(std::size_t f, const Object& o) {
    switch (o.kind) {
        case Object::Null: return "null";
        case Object::Bool: return o.boolean ? "true" : "false";
        case Object::Number: return numText(o.number);
        case Object::String: {
            std::string s = "<";
            static const char* hx = "0123456789ABCDEF";
            for (char c : o.str) {
                s += hx[(static_cast<unsigned char>(c) >> 4) & 15];
                s += hx[static_cast<unsigned char>(c) & 15];
            }
            return s + ">";
        }
        case Object::Name: return nameText(o.str);
        case Object::Array: {
            std::string s = "[";
            for (std::size_t i = 0; i < o.items.size(); ++i) s += (i ? " " : "") + write(f, o.items[i]);
            return s + "]";
        }
        case Object::Ref: {
            if (o.gen == -1) return std::to_string(static_cast<int>(o.number)) + " 0 R";
            const int id = f < files_.size() ? refFor(f, static_cast<int>(o.number)) : 0;
            return id ? std::to_string(id) + " 0 R" : "null";
        }
        case Object::Dict:
        case Object::Stream: {
            std::string s = "<<";
            for (std::size_t i = 0; i < o.keys.size(); ++i) {
                if (o.kind == Object::Stream && o.keys[i] == "Length") continue;
                s += nameText(o.keys[i]) + " " + write(f, o.items[i]) + " ";
            }
            if (o.kind == Object::Stream) return s + "/Length " + std::to_string(o.str.size()) + " >>\nstream\n" + o.str + "\nendstream";
            return s + ">>";
        }
    }
    return "null";
}

void Rebuilder::setPages(std::vector<Page> pages) {
    pages_ = std::move(pages);
    pagesId_ = reserve();
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        const int id = reserve();
        pageIds_.push_back(id);
        if (pages_[i].file) {
            const auto& p = pages_[i];
            if (p.index < 0 || p.index >= p.file->pageCount()) throw std::runtime_error("page out of range");
            Key k{fileIndex(p.file), p.file->pages()[static_cast<std::size_t>(p.index)].object};
            if (!selectedPage_.count(k)) selectedPage_[k] = id;
        }
    }
}

int Rebuilder::pageObject(std::size_t i) const { return pageIds_.at(i); }

std::string Rebuilder::pageBody(std::size_t i) {
    const Page& p = pages_[i];
    const std::size_t f = p.file ? fileIndex(p.file) : files_.size();
    Object d;
    d.kind = Object::Dict;
    int baseRotate = 0;
    if (p.file) {
        const auto& info = p.file->pages()[static_cast<std::size_t>(p.index)];
        d = info.dict;
        d.erase("Parent");
        d.erase("StructParents");
        d.erase("B");
        baseRotate = info.rotate;
    } else {
        d.set("Type", Object::makeName("Page"));
        Object box;
        box.kind = Object::Array;
        box.items = {Object::makeNumber(0), Object::makeNumber(0), Object::makeNumber(p.width), Object::makeNumber(p.height)};
        d.set("MediaBox", box);
    }
    const int rot = (((baseRotate + p.rotate) % 360) + 360) % 360;
    if (rot) d.set("Rotate", Object::makeNumber(rot));
    else d.erase("Rotate");

    // contents: the page's own (or its replacement) wrapped in q/Q, then the overlay
    Object contents;
    contents.kind = Object::Array;
    const bool overlay = !p.overlay.empty();
    if (overlay) contents.items.push_back(outputRef(addStream("", "q\n", false)));
    if (p.replaceContent) {
        contents.items.push_back(outputRef(addStream("", p.content)));
    } else if (const Object* c = d.get("Contents")) {
        const Object& cr = p.file->resolve(*c);
        if (cr.kind == Object::Array) {
            for (const auto& it : cr.items) contents.items.push_back(it);
        } else {
            contents.items.push_back(*c);
        }
    }
    if (overlay) {
        contents.items.push_back(outputRef(addStream("", "\nQ\n", false)));
        contents.items.push_back(outputRef(addStream("", p.overlay)));
    }
    if (contents.items.empty()) d.erase("Contents");
    else d.set("Contents", contents);

    // resources: the page's own plus what the overlay uses
    if (!p.fonts.empty() || !p.xobjects.empty() || !p.extGStates.empty() || !p.file) {
        Object res = directDict(p.file, d.get("Resources"));
        auto merge = [&](const char* key, const std::vector<std::pair<std::string, int>>& entries) {
            if (entries.empty()) return;
            Object sub = directDict(p.file, res.get(key));
            for (const auto& [name, id] : entries) sub.set(name, outputRef(id));
            res.set(key, sub);
        };
        merge("Font", p.fonts);
        merge("XObject", p.xobjects);
        merge("ExtGState", p.extGStates);
        d.set("Resources", res);
    }

    // annotations: the page's own (less the dropped ones) plus the new ones
    Object annots;
    annots.kind = Object::Array;
    if (const Object* a = d.get("Annots")) {
        const Object& arr = p.file ? p.file->resolve(*a) : *a;
        for (const auto& it : arr.items) {
            if (it.kind == Object::Ref && p.dropAnnots.count(static_cast<int>(it.number))) continue;
            annots.items.push_back(it);
        }
    }
    for (int id : p.annots) annots.items.push_back(outputRef(id));
    if (annots.items.empty()) d.erase("Annots");
    else d.set("Annots", annots);

    std::string body = write(f, d);
    body.insert(2, "/Parent " + std::to_string(pagesId_) + " 0 R ");
    return body;
}

std::string Rebuilder::finish(const std::map<std::string, std::string>& info) {
    if (pageIds_.empty()) throw std::runtime_error("no pages");
    for (std::size_t i = 0; i < pages_.size(); ++i) set(pageIds_[i], pageBody(i));

    // the catalog: the main file's, pointing at the new page tree
    Object cat;
    cat.kind = Object::Dict;
    if (main_) cat = main_->catalog();
    cat.kind = Object::Dict;
    cat.str.clear();
    cat.erase("Pages");
    cat.set("Type", Object::makeName("Catalog"));
    for (const auto& [k, v] : catalogEntries_) {
        if (v.kind == Object::Null) cat.erase(k);
        else cat.set(k, v);
    }
    std::string catBody = write(main_ ? 0 : files_.size(), cat);
    catBody.insert(2, "/Pages " + std::to_string(pagesId_) + " 0 R ");
    const int catalog = add(catBody);

    // document info: the main file's, with the new entries
    Object inf;
    inf.kind = Object::Dict;
    if (main_) {
        if (const Object* i = main_->trailer().get("Info")) inf = directDict(main_, i);
    }
    for (const auto& [k, v] : info) {
        Object str;
        str.kind = Object::String;
        bool ascii = true;
        for (char c : v) ascii = ascii && static_cast<unsigned char>(c) < 128;
        str.str = ascii ? v : "\xFE\xFF";
        for (unsigned cp : ascii ? std::vector<unsigned>{} : utf8Decode(v)) {
            auto unit = [&](unsigned u) {
                str.str += static_cast<char>((u >> 8) & 0xFF);
                str.str += static_cast<char>(u & 0xFF);
            };
            if (cp >= 0x10000) {
                cp -= 0x10000;
                unit(0xD800 + (cp >> 10));
                unit(0xDC00 + (cp & 0x3FF));
            } else {
                unit(cp);
            }
        }
        inf.set(k, str);
    }
    const std::string infoBody = write(main_ ? 0 : files_.size(), inf);
    const int infoId = add(infoBody);

    // everything the pages and catalog reach
    while (!queue_.empty()) {
        const Key k = queue_.back();
        queue_.pop_back();
        set(mapped_[k], write(k.file, source(k.file, k.num)));
    }
    std::string kids;
    for (int id : pageIds_) kids += (kids.empty() ? "" : " ") + std::to_string(id) + " 0 R";
    set(pagesId_, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageIds_.size()) + " >>");

    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < out_.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + (out_[i].empty() ? "null" : out_[i]) + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(out_.size() + 1) + "\n0000000000 65535 f \n";
    for (auto off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(out_.size() + 1) + " /Root " + std::to_string(catalog) + " 0 R /Info " +
           std::to_string(infoId) + " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

}  // namespace wetwaresuite::pdf
