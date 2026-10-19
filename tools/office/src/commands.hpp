// Plumbing shared by the office.* command sources: the workspace documents,
// snapshot undo, param specs, file helpers and the optional LibreOffice backend.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/office/deck.hpp"
#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/office/sheet.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::office {

namespace fs = std::filesystem;

// ---- documents in the workspace ------------------------------------------------------

class SheetDoc : public Document {
public:
    using Data = Workbook;
    static constexpr const char* kKind = "sheet";
    static constexpr const char* kNoun = "spreadsheet";
    Data data;
    std::string kind() const override { return kKind; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
    bool grid(CellGrid& out) const override;
    bool gridCell(int sheet, int row, int col, GridCell& out) const override;
    double gridColumnChars(int sheet, int col) const override;
    std::vector<std::string> gridSetCommand(int sheet, int row, int col, const std::string& input) const override;
};

class TextDoc : public Document {
public:
    using Data = TextDocument;
    static constexpr const char* kKind = "doc";
    static constexpr const char* kNoun = "document";
    Data data;
    std::string kind() const override { return kKind; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
};

class DeckDoc : public Document {
public:
    using Data = Deck;
    static constexpr const char* kKind = "deck";
    static constexpr const char* kNoun = "slide deck";
    Data data;
    std::string kind() const override { return kKind; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
};

// Runs an edit on a copy-on-apply snapshot: undo restores the whole document.
template <typename D>
class SnapshotEdit : public Action {
public:
    SnapshotEdit(std::shared_ptr<D> doc, std::function<Result(D&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override {
        before_ = std::make_unique<typename D::Data>(doc_->data);
        Result r;
        try {
            r = fn_(*doc_);
        } catch (const std::exception& e) {
            r = Result::failure(e.what());
        }
        if (!r.ok) revert();
        return r;
    }
    void revert() override {
        if (before_) doc_->data = *before_;
    }

private:
    std::shared_ptr<D> doc_;
    std::function<Result(D&)> fn_;
    std::unique_ptr<typename D::Data> before_;
};

// Closing is undoable: undo puts the document back under the same id.
class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override;
    void revert() override;

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

// The document a command names with doc=..., or the only open one of that kind.
bool has(const Params& p, const std::string& key);

// Multi-line text params: the value of `key`, or the contents of the file named by `key`_file.
// A value without real line breaks may spell them \n (and tabs \t), since a command line can't hold newlines.
bool hasText(const Params& p, const std::string& key);
std::string textParam(const Params& p, const std::string& key, bool unescape = true);

template <typename D>
std::shared_ptr<D> findDoc(Session& s, const Params& p, std::string& id, std::string& error) {
    const std::string kind = D::kKind;
    if (has(p, "doc")) {
        id = stringParam(p, "doc");
        auto doc = s.workspace().getAs<D>(id);
        if (!doc) error = "no open " + std::string(D::kNoun) + " called '" + id + "' (office." + kind + "_new or office." + kind + "_open makes one)";
        return doc;
    }
    std::vector<std::string> found;
    for (const auto& i : s.workspace().ids()) {
        if (s.workspace().getAs<D>(i)) found.push_back(i);
    }
    if (found.size() == 1) {
        id = found.front();
        return s.workspace().getAs<D>(id);
    }
    if (found.empty()) error = "no " + std::string(D::kNoun) + " is open (office." + kind + "_new or office." + kind + "_open makes one)";
    else error = "several " + std::string(D::kNoun) + "s are open; say which with doc=<id>";
    return nullptr;
}

// Reads a file into a workspace document of that kind (used by file= on reading commands). Throws.
void loadInto(SheetDoc& d, const std::string& path);
void loadInto(TextDoc& d, const std::string& path);
void loadInto(DeckDoc& d, const std::string& path);

template <typename D>
ActionFactory docCommand(Session& session, std::function<Result(D&, const Params&, const std::string& id)> fn, bool undoable) {
    return [&session, fn, undoable](const Params& p) -> std::unique_ptr<Action> {
        std::string id, error;
        if (!undoable && has(p, "file") && !has(p, "doc")) {
            // one-shot use: work on a file without opening it first
            return std::make_unique<FunctionAction>([fn, p]() -> Result {
                try {
                    D temp;
                    const std::string path = stringParam(p, "file");
                    loadInto(temp, path);
                    return fn(temp, p, path);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        auto doc = findDoc<D>(session, p, id, error);
        if (!doc) return std::make_unique<FunctionAction>([error] { return Result::failure(error); });
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, fn, p, id]() -> Result {
                try {
                    return fn(*doc, p, id);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<SnapshotEdit<D>>(doc, [fn, p, id](D& d) { return fn(d, p, id); });
    };
}

// A non-undoable action that turns exceptions into failures.
ActionFactory plainCommand(std::function<Result(const Params&)> fn);

// Adds a new document to the workspace and reports its id.
Result created(Session& s, std::shared_ptr<Document> doc, const std::string& verb, const std::string& extra = {});

// ---- param specs -----------------------------------------------------------------------

ParamSpec docSpec(const char* kind, const char* example);
ParamSpec fileSpec(const char* kind);  // file=: read a file instead of an open document
ParamSpec previewSpec();
ParamSpec str(const std::string& name, const std::string& description, bool required = false, Value fallback = {},
              std::vector<std::string> choices = {});
ParamSpec integer(const std::string& name, const std::string& description, bool required = false, Value fallback = {});
ParamSpec number(const std::string& name, const std::string& description, bool required = false, Value fallback = {});
ParamSpec flag(const std::string& name, const std::string& description, Value fallback = {});

// ---- files -----------------------------------------------------------------------------

std::string extOf(const std::string& path);  // lower case, without the dot
std::string readBytes(const std::string& path);  // throws std::runtime_error
void writeBytes(const std::string& path, const std::string& bytes);  // creates parent folders; throws
std::string displayPath(const std::string& path);

// LibreOffice (soffice --headless) as an optional backend. Converts `input` to the format named
// by `toExt` and returns the produced bytes; false with a reason when LibreOffice is missing or fails.
bool libreOffice(const fs::path& input, const std::string& toExt, std::string& out, std::string& error);
// Writes bytes we produced in `fromExt` to `path` in its own format through LibreOffice.
bool libreOfficeWrite(const std::string& bytes, const std::string& fromExt, const std::string& path, std::string& error);
bool haveLibreOffice();

// Attaches a PNG when preview=true.
void maybePreview(Result& r, const Document& doc, const Params& p);
Attachment png(const raster::Image& img, const std::string& name);

// Number output that prints as an integer when whole.
Value numValue(double v);

// ---- registration per area -------------------------------------------------------------

void registerSheetCommands(Session& session);
void registerDocCommands(Session& session);
void registerDeckCommands(Session& session);
void registerPdfCommands(Session& session);
void registerConvertCommands(Session& session);

// Shared by convert and the per-kind open/save commands.
Workbook loadWorkbook(const std::string& path, char delimiter = 0);  // throws
Workbook loadWorkbook(const std::string& path, const CsvOptions& csv);  // CSV import options; throws
void saveWorkbook(const Workbook& wb, const std::string& path, int sheet);  // throws
TextDocument loadTextDocument(const std::string& path);  // throws
void saveTextDocument(const TextDocument& doc, const std::string& path);  // throws
Deck loadDeck(const std::string& path);  // throws
void saveDeck(const Deck& deck, const std::string& path);  // throws

// Slide decks as Markdown outlines ("# Title", bullets, "> notes", "<!-- layout: two_column -->").
Deck deckFromOutline(const std::string& markdown, const std::string& baseDir);
std::string deckToOutline(const Deck& deck, const std::string& mediaPrefix, std::vector<MediaFile>& media);

}  // namespace wetwaresuite::office
