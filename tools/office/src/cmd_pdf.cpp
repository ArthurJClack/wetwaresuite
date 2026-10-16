// office.pdf_* commands: page-level PDF tools that work on files directly
// (merge, split, extract, delete, rotate, text, images to PDF, render).
#include <algorithm>
#include <cmath>

#include "achclwss/core/process.hpp"
#include "achclwss/pdf/reader.hpp"
#include "achclwss/raster/codec.hpp"
#include "commands.hpp"
#include "text.hpp"

namespace achclwss::office {

namespace {

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    const char sep = text.find(';') != std::string::npos || text.find('\n') != std::string::npos ? ';' : ',';
    std::string cur;
    for (char c : text + std::string(1, sep)) {
        if (c == sep || c == '\n') {
            std::string t = trimCopy(cur);
            if (t.size() >= 2 && t.front() == '"' && t.back() == '"') t = t.substr(1, t.size() - 2);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

// "file.pdf[1-3]" -> ("file.pdf", "1-3")
std::pair<std::string, std::string> pageSelection(const std::string& item) {
    if (!item.empty() && item.back() == ']') {
        auto open = item.rfind('[');
        if (open != std::string::npos && open > 0) return {trimCopy(item.substr(0, open)), item.substr(open + 1, item.size() - open - 2)};
    }
    return {item, "all"};
}

std::string pageList(const std::vector<int>& pages) {
    std::string out;
    for (std::size_t i = 0; i < pages.size();) {
        std::size_t j = i;
        while (j + 1 < pages.size() && pages[j + 1] == pages[j] + 1) ++j;
        out += (out.empty() ? "" : ",") + std::to_string(pages[i] + 1) + (j > i ? "-" + std::to_string(pages[j] + 1) : "");
        i = j + 1;
    }
    return out;
}

std::string paperName(double w, double h) {
    struct P {
        const char* n;
        double w, h;
    };
    static const P ps[] = {{"A4", 595.28, 841.89}, {"Letter", 612, 792}, {"Legal", 612, 1008}, {"A3", 841.89, 1190.55}, {"A5", 419.53, 595.28}};
    for (const auto& p : ps) {
        if (std::fabs(p.w - w) < 3 && std::fabs(p.h - h) < 3) return p.n;
        if (std::fabs(p.h - w) < 3 && std::fabs(p.w - h) < 3) return std::string(p.n) + " landscape";
    }
    if (std::fabs(w / h - 16.0 / 9) < 0.01) return "16:9";
    return {};
}

Result writeAssembled(const std::vector<pdf::PageSource>& pages, const std::string& output, const std::string& what) {
    if (pages.empty()) return Result::failure("no pages selected");
    std::string bytes = pdf::assemblePdf(pages);
    writeBytes(output, bytes);
    return Result::success(what + " -> " + output + " (" + std::to_string(pages.size()) + " page" + (pages.size() == 1 ? "" : "s") + ")", output)
        .with("output", output)
        .with("pages", static_cast<std::int64_t>(pages.size()));
}

ParamSpec pathSpec() { return str("path", "PDF file", true); }

}  // namespace

void registerPdfCommands(Session& session) {
    auto& reg = session.registry();
    (void)session;

    reg.add({"office.pdf_info", "PDF Info", "Page count, page sizes, PDF version, title/author/producer and whether the file is encrypted or has text.",
             {pathSpec()}, false, true},
            plainCommand([](const Params& p) -> Result {
                const std::string path = stringParam(p, "path");
                pdf::PdfFile f = pdf::PdfFile::load(pathFromUtf8(path));
                std::string t = path + ": " + std::to_string(f.pageCount()) + " page(s), PDF " + f.version() + (f.encrypted() ? ", encrypted" : "");
                for (const char* key : {"Title", "Author", "Subject", "Creator", "Producer"}) {
                    std::string v = f.infoField(key);
                    if (!v.empty()) t += "\n" + lower(key) + ": " + v;
                }
                // page sizes, grouped
                std::string sizes;
                for (int i = 0; i < f.pageCount();) {
                    const auto& pi = f.pages()[static_cast<std::size_t>(i)];
                    int j = i;
                    auto same = [&](int k) {
                        const auto& q = f.pages()[static_cast<std::size_t>(k)];
                        return std::fabs(q.width - pi.width) < 0.5 && std::fabs(q.height - pi.height) < 0.5 && q.rotate == pi.rotate;
                    };
                    while (j + 1 < f.pageCount() && same(j + 1)) ++j;
                    std::string name = paperName(pi.width, pi.height);
                    sizes += "\n  page" + std::string(j > i ? "s " : " ") + std::to_string(i + 1) + (j > i ? "-" + std::to_string(j + 1) : "") + ": " +
                             textNumber(std::round(pi.width * 10) / 10) + " x " + textNumber(std::round(pi.height * 10) / 10) + " pt" +
                             (name.empty() ? "" : " (" + name + ")") + (pi.rotate ? ", rotated " + std::to_string(pi.rotate) : "");
                    i = j + 1;
                    if (sizes.size() > 3000) {
                        sizes += "\n  ...";
                        break;
                    }
                }
                t += sizes;
                bool text = false;
                if (!f.encrypted()) {
                    for (int i = 0; i < std::min(f.pageCount(), 3) && !text; ++i) text = !trimCopy(f.pageText(i)).empty();
                    t += text ? "\nhas extractable text" : "\nno extractable text on the first pages (scanned or outlined?)";
                }
                Result r = Result::success(t, static_cast<std::int64_t>(f.pageCount()));
                r.with("pages", static_cast<std::int64_t>(f.pageCount())).with("version", f.version()).with("encrypted", f.encrypted());
                if (f.pageCount() > 0) r.with("width", numValue(f.pages()[0].width)).with("height", numValue(f.pages()[0].height));
                return r;
            }));

    reg.add({"office.pdf_text", "PDF Text",
             "Extract the text of a PDF (all pages or pages=\"1-3,7\"), page by page. Works for text PDFs, not scans.",
             {pathSpec(), str("pages", "pages to read, e.g. 1-3,5,8- or last", false, std::string("all"))}, false, true},
            plainCommand([](const Params& p) -> Result {
                pdf::PdfFile f = pdf::PdfFile::load(pathFromUtf8(stringParam(p, "path")));
                if (f.encrypted()) return Result::failure("the PDF is encrypted");
                auto pages = pdf::parsePageList(stringParam(p, "pages", "all"), f.pageCount());
                std::string out;
                for (int i : pages) out += (out.empty() ? "" : "\n\n") + std::string("--- page ") + std::to_string(i + 1) + " ---\n" + trimCopy(f.pageText(i));
                return Result::success(out.empty() ? "(no pages)" : out, out).with("pages", static_cast<std::int64_t>(pages.size()));
            }));

    reg.add({"office.pdf_merge", "Merge PDFs",
             "Join PDFs into one, in order. inputs lists the files separated by ; (or commas), each optionally with a page "
             "selection in brackets: \"cover.pdf; report.pdf[2-5]; appendix.pdf[last]\".",
             {str("inputs", "files to join, separated by ; or ,", true), str("output", "the merged PDF", true)}, false, false},
            plainCommand([](const Params& p) -> Result {
                auto items = splitList(stringParam(p, "inputs"));
                if (items.empty()) return Result::failure("no input files given");
                std::vector<std::unique_ptr<pdf::PdfFile>> files;
                std::vector<pdf::PageSource> pages;
                std::string what;
                for (const auto& item : items) {
                    auto [file, sel] = pageSelection(item);
                    files.push_back(std::make_unique<pdf::PdfFile>(pdf::PdfFile::load(pathFromUtf8(file))));
                    auto list = pdf::parsePageList(sel, files.back()->pageCount());
                    for (int i : list) pages.push_back({files.back().get(), i, 0});
                    what += (what.empty() ? "" : " + ") + pathFromUtf8(file).filename().string() + (sel == "all" ? "" : "[" + sel + "]");
                }
                return writeAssembled(pages, stringParam(p, "output"), "merged " + what);
            }));

    reg.add({"office.pdf_extract", "Extract Pages",
             "Write the chosen pages (in the order given, repeats allowed) to a new PDF: pages=\"1-3,7,10-\", \"last\", \"3,1,2\" "
             "or \"5-1\" (reversed).",
             {pathSpec(), str("pages", "pages to keep", true), str("output", "new PDF", true)}, false, false},
            plainCommand([](const Params& p) -> Result {
                pdf::PdfFile f = pdf::PdfFile::load(pathFromUtf8(stringParam(p, "path")));
                auto list = pdf::parsePageList(stringParam(p, "pages"), f.pageCount());
                std::vector<pdf::PageSource> pages;
                for (int i : list) pages.push_back({&f, i, 0});
                return writeAssembled(pages, stringParam(p, "output"), "extracted pages " + pageList(list));
            }));

    reg.add({"office.pdf_delete_pages", "Delete Pages", "Remove pages from a PDF (written to output, or back to the same file).",
             {pathSpec(), str("pages", "pages to remove, e.g. 2,5-6", true), str("output", "new PDF (default: overwrite path)")}, false, false},
            plainCommand([](const Params& p) -> Result {
                const std::string path = stringParam(p, "path");
                pdf::PdfFile f = pdf::PdfFile::load(pathFromUtf8(path));
                auto drop = pdf::parsePageList(stringParam(p, "pages"), f.pageCount());
                std::vector<pdf::PageSource> pages;
                for (int i = 0; i < f.pageCount(); ++i) {
                    if (std::find(drop.begin(), drop.end(), i) == drop.end()) pages.push_back({&f, i, 0});
                }
                if (pages.empty()) return Result::failure("that would delete every page");
                std::sort(drop.begin(), drop.end());
                drop.erase(std::unique(drop.begin(), drop.end()), drop.end());
                return writeAssembled(pages, has(p, "output") ? stringParam(p, "output") : path, "deleted pages " + pageList(drop));
            }));

    reg.add({"office.pdf_split", "Split PDF",
             "Split a PDF into several files: every=N pages per file (default 1, one file per page), or ranges=\"1-3; 4-6; 7-\" for "
             "explicit parts. output is a name pattern where {n} becomes the part number (default: <name>-{n}.pdf next to the input).",
             {pathSpec(), integer("every", "pages per file", false, std::int64_t{1}), str("ranges", "explicit parts separated by ;"),
              str("output", "file name pattern with {n}, e.g. out/part-{n}.pdf")},
             false, false},
            plainCommand([](const Params& p) -> Result {
                const std::string path = stringParam(p, "path");
                pdf::PdfFile f = pdf::PdfFile::load(pathFromUtf8(path));
                std::vector<std::vector<int>> parts;
                if (has(p, "ranges")) {
                    std::string spec = stringParam(p, "ranges");
                    std::string cur;
                    for (char c : spec + ";") {
                        if (c == ';' || c == '\n') {
                            if (!trimCopy(cur).empty()) parts.push_back(pdf::parsePageList(trimCopy(cur), f.pageCount()));
                            cur.clear();
                        } else {
                            cur += c;
                        }
                    }
                } else {
                    const int every = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "every", 1), 1, 100000));
                    for (int i = 0; i < f.pageCount(); i += every) {
                        std::vector<int> part;
                        for (int k = i; k < std::min(f.pageCount(), i + every); ++k) part.push_back(k);
                        parts.push_back(part);
                    }
                }
                if (parts.empty()) return Result::failure("nothing to split");
                fs::path in = pathFromUtf8(path);
                std::string pattern = has(p, "output") ? stringParam(p, "output") : (in.parent_path() / (in.stem().string() + "-{n}.pdf")).string();
                if (pattern.find("{n}") == std::string::npos) {
                    fs::path pp = pathFromUtf8(pattern);
                    pattern = (pp.parent_path() / (pp.stem().string() + "-{n}" + (pp.has_extension() ? pp.extension().string() : std::string(".pdf")))).string();
                }
                const int digits = parts.size() >= 100 ? 3 : parts.size() >= 10 ? 2 : 1;
                std::string list, first;
                for (std::size_t k = 0; k < parts.size(); ++k) {
                    std::string num = std::to_string(k + 1);
                    while (static_cast<int>(num.size()) < digits) num = "0" + num;
                    std::string out = pattern;
                    out.replace(out.find("{n}"), 3, num);
                    std::vector<pdf::PageSource> pages;
                    for (int i : parts[k]) pages.push_back({&f, i, 0});
                    writeBytes(out, pdf::assemblePdf(pages));
                    if (first.empty()) first = out;
                    if (k < 20) list += "\n  " + out + ": pages " + pageList(parts[k]);
                }
                if (parts.size() > 20) list += "\n  ...";
                return Result::success("split " + path + " into " + std::to_string(parts.size()) + " files:" + list, first)
                    .with("files", static_cast<std::int64_t>(parts.size()))
                    .with("first", first);
            }));

    reg.add({"office.pdf_rotate", "Rotate Pages", "Rotate pages by 90, 180 or 270 degrees clockwise (-90 turns them back).",
             {pathSpec(), integer("angle", "degrees clockwise", false, std::int64_t{90}), str("pages", "pages to turn", false, std::string("all")),
              str("output", "new PDF (default: overwrite path)")},
             false, false},
            plainCommand([](const Params& p) -> Result {
                const std::string path = stringParam(p, "path");
                const int angle = static_cast<int>(intParam(p, "angle", 90));
                if (angle % 90 != 0) return Result::failure("angle must be a multiple of 90");
                pdf::PdfFile f = pdf::PdfFile::load(pathFromUtf8(path));
                auto turn = pdf::parsePageList(stringParam(p, "pages", "all"), f.pageCount());
                std::vector<pdf::PageSource> pages;
                for (int i = 0; i < f.pageCount(); ++i) {
                    const bool t = std::find(turn.begin(), turn.end(), i) != turn.end();
                    pages.push_back({&f, i, t ? angle : 0});
                }
                return writeAssembled(pages, has(p, "output") ? stringParam(p, "output") : path,
                                      "rotated pages " + pageList(turn) + " by " + std::to_string(angle) + " degrees");
            }));

    reg.add({"office.pdf_from_images", "Images to PDF",
             "Make a PDF with one image per page (PNG, JPEG, BMP, ...; JPEGs are embedded unchanged). page=fit sizes each page to "
             "its image; a4/letter centre the image on paper turned to match it.",
             {str("images", "image files separated by ; or ,", true), str("output", "the new PDF", true),
              str("page", "page size", false, std::string("fit"), {"fit", "a4", "letter"}), number("margin", "margin in pt on paper pages", false, 36.0),
              number("dpi", "page=fit: image pixels per inch", false, 96.0)},
             false, false},
            plainCommand([](const Params& p) -> Result {
                auto items = splitList(stringParam(p, "images"));
                if (items.empty()) return Result::failure("no images given");
                const std::string paper = stringParam(p, "page", "fit");
                const double margin = std::max(0.0, numberParam(p, "margin", 36));
                const double dpi = std::clamp(numberParam(p, "dpi", 96), 10.0, 2400.0);
                std::vector<pdf::Page> pages;
                for (const auto& item : items) {
                    auto img = pdf::loadImageData(readBytes(item));
                    const double iw = img->pixels.width(), ih = img->pixels.height();
                    pdf::Page page;
                    if (paper == "fit") {
                        page.width = iw * 72 / dpi;
                        page.height = ih * 72 / dpi;
                        page.image(0, 0, page.width, page.height, img);
                    } else {
                        double w = paper == "letter" ? 612 : 595.28, h = paper == "letter" ? 792 : 841.89;
                        if (iw > ih) std::swap(w, h);
                        page.width = w;
                        page.height = h;
                        const double aw = std::max(1.0, w - 2 * margin), ah = std::max(1.0, h - 2 * margin);
                        const double k = std::min(aw / iw, ah / ih);
                        page.image((w - iw * k) / 2, (h - ih * k) / 2, iw * k, ih * k, img);
                    }
                    pages.push_back(std::move(page));
                }
                const std::string output = stringParam(p, "output");
                writeBytes(output, pdf::writePdf(pages));
                return Result::success("made " + output + " from " + std::to_string(pages.size()) + " image(s)", output)
                    .with("output", output)
                    .with("pages", static_cast<std::int64_t>(pages.size()));
            }));

    reg.add({"office.pdf_render", "Render PDF Page",
             "Render a page of any PDF to PNG with pdftoppm (poppler-utils, optional), attached and optionally written to output. "
             "Lets Claude look at a PDF made elsewhere.",
             {pathSpec(), integer("page", "page number", false, std::int64_t{1}), number("dpi", "resolution", false, 100.0),
              str("output", "write the PNG here too")},
             false, false},
            plainCommand([](const Params& p) -> Result {
                auto exe = findProgram("pdftoppm");
                if (!exe) return Result::failure("rendering other PDFs needs pdftoppm (poppler-utils) on PATH; PDFs made here can be previewed with doc_preview/deck_preview");
                const std::string path = stringParam(p, "path");
                pdf::PdfFile f = pdf::PdfFile::load(pathFromUtf8(path));
                const int page = static_cast<int>(intParam(p, "page", 1));
                if (page < 1 || page > f.pageCount()) return Result::failure("page must be 1.." + std::to_string(f.pageCount()));
                const double dpi = std::clamp(numberParam(p, "dpi", 100), 10.0, 1200.0);
                std::error_code ec;
                fs::path dir = fs::temp_directory_path(ec) / ("achclwss-office-render-" + std::to_string(std::hash<std::string>{}(path + std::to_string(page))));
                fs::create_directories(dir, ec);
                fs::path prefix = dir / "page";
                auto r = runProgram({exe->string(), "-png", "-r", std::to_string(static_cast<int>(dpi)), "-f", std::to_string(page), "-l", std::to_string(page),
                                     "-singlefile", pathFromUtf8(path).string(), prefix.string()});
                fs::path png = prefix;
                png += ".png";
                if (r.exitCode != 0 || !fs::exists(png, ec)) {
                    fs::remove_all(dir, ec);
                    return Result::failure("pdftoppm failed: " + trimCopy(r.output));
                }
                std::string bytes = readBytes(png.string());
                fs::remove_all(dir, ec);
                raster::Image img = raster::decode(bytes);
                Result res = Result::success("page " + std::to_string(page) + " of " + std::to_string(f.pageCount()) + " at " + textNumber(dpi) + " dpi (" +
                                             std::to_string(img.width()) + "x" + std::to_string(img.height()) + ")");
                if (has(p, "output")) {
                    writeBytes(stringParam(p, "output"), bytes);
                    res.message += "; wrote " + stringParam(p, "output");
                    res.with("output", stringParam(p, "output"));
                }
                res.attach({"image/png", bytes, "page" + std::to_string(page) + ".png"});
                return res;
            }));
}

}  // namespace achclwss::office
