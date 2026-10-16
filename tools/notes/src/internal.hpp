// Shared pieces of the notes.* commands.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "achclwss/notes/notes.hpp"

namespace achclwss::notes::detail {

// An open vault in the session workspace ("vault1").
class VaultDoc : public Document {
public:
    Vault vault;
    std::string kind() const override { return "vault"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
};

struct State {
    Session* session = nullptr;
    std::string current;  // the vault id the last command used
};
using StatePtr = std::shared_ptr<State>;

// File changes that can be put back exactly: what undo of a notes edit does.
class FileTxn {
public:
    void write(const fs::path& path, const std::string& bytes);
    void remove(const fs::path& path);
    void move(const fs::path& from, const fs::path& to);  // fails if `to` exists
    void restore();  // undoes everything, newest first
    bool empty() const { return ops_.empty(); }

private:
    struct Op {
        enum Kind { Write, Remove, Move } kind = Write;
        fs::path a, b;
        bool existed = false;
        std::string bytes;
        fs::file_time_type time{};
    };
    void ensureParent(const fs::path& p);
    std::vector<Op> ops_;
    std::vector<fs::path> dirs_;
};

using ReadFn = std::function<Result(Vault&, const Params&)>;
using EditFn = std::function<Result(Vault&, FileTxn&, const Params&)>;

// Factories: a read-only command, and an undoable edit of vault files.
ActionFactory reader(StatePtr st, ReadFn fn);
ActionFactory editor(StatePtr st, EditFn fn);

// The vault a command means (vault= id or folder, else the last used, else one found from the
// current folder), refreshed from disk. Null with `error` set when there is none.
std::shared_ptr<VaultDoc> getVault(State& st, const Params& p, std::string& error, std::string* id = nullptr);

// ---- params --------------------------------------------------------------------------
ParamSpec vaultParam();
ParamSpec noteParam(const std::string& what = "the note: a path like Projects/Plan.md, a name, a title or an alias", bool required = true);
ParamSpec boolParamSpec(const std::string& name, const std::string& what, bool fallback = false);
ParamSpec jsonParam(const std::string& what);
ParamSpec limitParam(std::int64_t fallback = 50);
ParamSpec dateParam(const std::string& what);
std::vector<ParamSpec> textParams(const std::string& name, const std::string& what, bool required);

bool has(const Params& p, const std::string& key);
// name= text, with \n and \t turned into line breaks and tabs when it has no real ones; or name_file=.
std::string textParam(const Params& p, const std::string& name);
bool hasText(const Params& p, const std::string& name);

// The note a param names; throws std::runtime_error ("no note called ...") when it isn't there.
std::string needNote(const Vault& v, const Params& p, const std::string& key = "note", bool notesOnly = true);
Date baseDate(const Params& p);  // today, or the today= param (tests and scripts pin dates)

// ---- files and text ------------------------------------------------------------------
std::string readText(const fs::path& path);
std::vector<std::string> splitLines(const std::string& text);  // without line breaks
std::string withoutMd(const std::string& rel);
std::string folderOf(const std::string& rel);
std::string fileNameOf(const std::string& rel);
// A new note's path from a name or path ("Ideas/New idea" -> "Ideas/New idea.md"), cleaned of
// characters links can't hold. Throws std::runtime_error for paths leaving the vault.
std::string newNoteRel(const std::string& spec, const std::string& folder = {});
std::string joinRel(const std::string& folder, const std::string& name);
std::string plural(std::size_t n, const std::string& word);
// Appends text to a note's content (under a heading's section when given), with tidy blank lines.
std::string appendText(const std::string& content, const std::string& text, const std::string& heading, bool prepend);

// ---- templates ----------------------------------------------------------------------
std::string findTemplate(const Vault& v, const std::string& name);  // rel or ""
// {{title}}, {{date}}, {{time}}, {{date:FORMAT}}, {{time:FORMAT}}, {{yesterday}}, {{tomorrow}}, {{folder}} and vars.
std::string expandTemplate(const std::string& text, const Vault& v, const std::string& title, const Date& date,
                           const std::map<std::string, std::string>& vars, const std::string& folder = {});
std::map<std::string, std::string> parseVars(const std::string& text);  // "a=1; b=two" or one per line
// Joins a template's frontmatter into a note's (keys the note lacks) and appends its body.
std::string mergeTemplate(const std::string& note, const std::string& expanded);

// ---- links ---------------------------------------------------------------------------
// Moves a note or attachment, rewriting every link to it (and relative links inside it).
// Returns the number of links rewritten; `changed` gets the notes whose text changed.
std::size_t moveWithLinks(Vault& v, FileTxn& txn, const std::string& from, const std::string& to, bool updateLinks,
                          std::vector<std::string>& changed);
// Rewrites inline #tags and frontmatter tags; returns the number of notes changed.
std::size_t renameTag(Vault& v, FileTxn& txn, const std::string& from, const std::string& to);

// ---- daily notes ----------------------------------------------------------------------
std::string dailyRel(const Vault& v, const Date& d, const std::string& folder, const std::string& format);
// Creates the daily note when missing (from the template). Returns its rel; `created` says if it was new.
std::string ensureDaily(Vault& v, FileTxn& txn, const Date& d, const Params& p, bool& created);

// ---- registration (one function per section keeps each under ~400 lines) ---------------
void registerVaultCommands(CommandRegistry& reg, const StatePtr& st);
void registerNoteCommands(CommandRegistry& reg, const StatePtr& st);
void registerMetaCommands(CommandRegistry& reg, const StatePtr& st);
void registerTaskCommands(CommandRegistry& reg, const StatePtr& st);
void registerLinkCommands(CommandRegistry& reg, const StatePtr& st);
void registerExportCommands(CommandRegistry& reg, const StatePtr& st);

}  // namespace achclwss::notes::detail
