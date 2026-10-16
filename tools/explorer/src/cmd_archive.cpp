// explorer.* commands for archives (zip, tar, tar.gz, gz) and file checks:
// checksums, comparing files and folders, finding duplicates.
#include <algorithm>
#include <map>

#include "internal.hpp"

namespace wetwaresuite::explorer::detail {

namespace {

std::string kindFromName(const fs::path& out) {
    const std::string n = sysutil::lowerAscii(sysutil::nameOf(out));
    auto ends = [&](const char* e) { return n.size() >= std::char_traits<char>::length(e) && n.compare(n.size() - std::char_traits<char>::length(e), std::string::npos, e) == 0; };
    if (ends(".tar.gz") || ends(".tgz")) return "tar.gz";
    if (ends(".tar")) return "tar";
    return "zip";
}

fs::path defaultExtractFolder(const fs::path& archive) {
    std::string n = sysutil::nameOf(archive);
    for (const char* e : {".tar.gz", ".tgz", ".tar", ".zip", ".gz"}) {
        std::string ext = e;
        if (n.size() > ext.size() && sysutil::lowerAscii(n.substr(n.size() - ext.size())) == ext) {
            n.resize(n.size() - ext.size());
            break;
        }
    }
    return freeName(archive.parent_path() / sysutil::toPath(n));
}

// one folder's files by path relative to it
std::map<std::string, fs::path> filesUnder(const fs::path& root) {
    std::map<std::string, fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (it->is_regular_file(e2)) out[sysutil::fromPath(it->path().lexically_relative(root))] = it->path();
    }
    return out;
}

bool sameFile(const fs::path& a, const fs::path& b, std::uint64_t* firstDiff = nullptr) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec)) {
        if (firstDiff) *firstDiff = 0;
        return false;
    }
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    std::vector<char> ba(1 << 16), bb(1 << 16);
    std::uint64_t at = 0;
    while (fa && fb) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        std::streamsize na = fa.gcount(), nb = fb.gcount();
        if (na != nb) return false;
        for (std::streamsize i = 0; i < na; ++i) {
            if (ba[static_cast<std::size_t>(i)] != bb[static_cast<std::size_t>(i)]) {
                if (firstDiff) *firstDiff = at + static_cast<std::uint64_t>(i);
                return false;
            }
        }
        at += static_cast<std::uint64_t>(na);
        if (na == 0) break;
    }
    return true;
}

}  // namespace

void registerArchiveCommands(Session& s) {
    auto& reg = s.registry();
    reg.add({"explorer.archive_list", "What's in an archive", "List what is inside a zip, tar, tar.gz or gz file: names, sizes and dates.",
             {req("path", ParamType::String, "the archive")}, false, true},
            reader([](const Params& p) {
                const fs::path path = pathParam(p, "path");
                auto items = archiveList(path);
                std::string text = sysutil::fromPath(path) + " (" + archiveKind(path) + ")\n", rows;
                std::uint64_t total = 0;
                for (const auto& i : items) {
                    total += i.size;
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%10s  %16s  ", i.dir ? "" : sysutil::humanSize(i.size).c_str(), formatTime(i.modified).c_str());
                    text += buf + i.name + "\n";
                    rows += std::string(i.dir ? "dir" : "file") + "\t" + i.name + "\t" + std::to_string(i.size) + "\t" + std::to_string(i.packed) + "\t" + std::to_string(i.modified) + "\n";
                }
                return Result::success(text + sysutil::plural(items.size(), "item") + ", " + sysutil::humanSize(total) + " unpacked")
                    .with("count", static_cast<int64_t>(items.size()))
                    .with("bytes", static_cast<int64_t>(total))
                    .with("format", archiveKind(path))
                    .with("rows", rows);
            }));

    reg.add({"explorer.archive", "Make archive",
             "Pack files and folders into a zip, tar or tar.gz archive (the format comes from the output name unless format= "
             "says). Undo moves the archive to the trash.",
             {pathsParam("what to pack: files or folders, separated by new lines or |"), req("output", ParamType::String, "the archive to write, e.g. backup.zip"),
              {"format", ParamType::String, "archive format (default: from the output name)", false, {"zip", "tar", "tar.gz"}},
              opt("level", ParamType::Int, "compression 0 (none, fastest) to 9 (smallest)", int64_t{6}),
              opt("overwrite", ParamType::Bool, "replace an archive with that name (the old one goes to the trash)", false)},
             true},
            changer([](const Params& p, UndoList& undo) {
                const fs::path out = pathParam(p, "output");
                std::error_code ec;
                const fs::path absOut = fs::absolute(out, ec);
                if (fs::exists(out, ec)) {
                    if (!boolParam(p, "overwrite", false)) throw std::runtime_error(stringParam(p, "output") + " already exists (overwrite=true replaces it)");
                    std::string id = moveToTrash(absOut);
                    undo.push_back([id, absOut] { restoreFromTrash(id, absOut); });
                }
                const std::string kind = stringParam(p, "format").empty() ? kindFromName(out) : stringParam(p, "format");
                std::size_t n = archiveCreate(out, splitPaths(stringParam(p, "paths")), kind, static_cast<int>(intParam(p, "level", 6)));
                undo.push_back([absOut] {
                    std::error_code e2;
                    if (fs::exists(absOut, e2)) moveToTrash(absOut);
                });
                const auto size = fs::file_size(out, ec);
                return Result::success("packed " + sysutil::plural(n, "file") + " into " + sysutil::fromPath(out) + " (" + sysutil::humanSize(size) + ", " + kind + ")")
                    .with("path", sysutil::fromPath(out))
                    .with("files", static_cast<int64_t>(n))
                    .with("bytes", static_cast<int64_t>(size));
            }));

    reg.add({"explorer.extract", "Extract",
             "Unpack a zip, tar, tar.gz or gz archive, into to= or a new folder named after it next to it. Unsafe names that "
             "would land outside the folder are refused. Undo moves what was unpacked to the trash.",
             {req("path", ParamType::String, "the archive"), opt("to", ParamType::String, "folder to unpack into"),
              opt("only", ParamType::String, "only these names or folders inside it (separated by new lines or |)"),
              opt("overwrite", ParamType::Bool, "replace files that are already there", false)},
             true},
            changer([](const Params& p, UndoList& undo) {
                const fs::path archive = pathParam(p, "path");
                std::error_code ec;
                const fs::path dest = stringParam(p, "to").empty() ? defaultExtractFolder(fs::absolute(archive, ec)) : pathParam(p, "to");
                std::vector<std::string> only;
                if (!stringParam(p, "only").empty()) {
                    for (const auto& o : splitPaths(stringParam(p, "only"))) only.push_back(sysutil::fromPath(o));
                }
                auto written = archiveExtract(archive, dest, only, boolParam(p, "overwrite", false));
                // the first things made are the outermost; trash those (their insides go with them)
                std::vector<fs::path> tops;
                for (const auto& w : written) {
                    bool inside = false;
                    for (const auto& t : tops) {
                        auto rel = w.lexically_relative(t);
                        if (!rel.empty() && *rel.begin() != "..") inside = true;
                    }
                    if (!inside) tops.push_back(w);
                }
                for (const auto& t : tops) {
                    fs::path abs = fs::absolute(t, ec);
                    undo.push_back([abs] {
                        std::error_code e2;
                        if (fs::exists(abs, e2)) moveToTrash(abs);
                    });
                }
                std::size_t files = 0;
                for (const auto& w : written) files += fs::is_regular_file(w, ec) ? 1 : 0;
                return Result::success("unpacked " + sysutil::plural(files, "file") + " into " + sysutil::fromPath(dest))
                    .with("folder", sysutil::fromPath(dest))
                    .with("files", static_cast<int64_t>(files));
            }));
}

void registerToolCommands(Session& s) {
    auto& reg = s.registry();
    reg.add({"explorer.checksum", "Checksum",
             "A file's checksum (SHA-256 by default; also SHA-1, MD5, CRC-32), to check a download or compare copies. Give "
             "expected= to check it matches.",
             {pathsParam("files, separated by new lines or |"),
              {"algorithm", ParamType::String, "which checksum", false, {"sha256", "sha1", "md5", "crc32"}, std::string("sha256")},
              opt("expected", ParamType::String, "the checksum it should have")},
             false, true},
            reader([](const Params& p) {
                const std::string algo = stringParam(p, "algorithm", "sha256");
                std::string text, first, rows;
                for (const auto& f : splitPaths(stringParam(p, "paths"))) {
                    std::string sum = checksumFile(f, algo);
                    if (first.empty()) first = sum;
                    text += sum + "  " + sysutil::fromPath(f) + "\n";
                    rows += sum + "\t" + sysutil::fromPath(f) + "\n";
                }
                Result r = Result::success(text, first);
                r.with("checksum", first).with("algorithm", algo).with("rows", rows);
                std::string expected = sysutil::lowerAscii(sysutil::trim(stringParam(p, "expected")));
                if (!expected.empty()) {
                    const bool ok = expected == first;
                    r.with("matches", ok);
                    r.message += ok ? "matches the expected checksum" : "DOES NOT match the expected checksum " + expected;
                    if (!ok) r.ok = false;
                }
                return r;
            }));

    reg.add({"explorer.compare", "Compare",
             "Compare two files (same or not, and where they first differ) or two folders (files only in one, files that differ).",
             {req("a", ParamType::String, "first file or folder"), req("b", ParamType::String, "second file or folder")}, false, true},
            reader([](const Params& p) {
                const fs::path a = pathParam(p, "a"), b = pathParam(p, "b");
                std::error_code ec;
                if (fs::is_regular_file(a, ec) && fs::is_regular_file(b, ec)) {
                    std::uint64_t at = 0;
                    const bool same = sameFile(a, b, &at);
                    std::string msg = same ? "the files are the same" : fs::file_size(a, ec) != fs::file_size(b, ec) ? "the files differ (different sizes: " + sysutil::humanSize(fs::file_size(a, ec)) + " and " + sysutil::humanSize(fs::file_size(b, ec)) + ")" : "the files differ, first at byte " + std::to_string(at);
                    return Result::success(msg, same).with("same", same);
                }
                if (!fs::is_directory(a, ec) || !fs::is_directory(b, ec)) throw std::runtime_error("give two files or two folders");
                auto fa = filesUnder(a), fb = filesUnder(b);
                std::string onlyA, onlyB, differ, rows;
                std::size_t nA = 0, nB = 0, nD = 0, nSame = 0;
                for (const auto& [rel, pa] : fa) {
                    auto it = fb.find(rel);
                    if (it == fb.end()) {
                        ++nA;
                        onlyA += "  " + rel + "\n";
                        rows += "only_a\t" + rel + "\n";
                    } else if (!sameFile(pa, it->second)) {
                        ++nD;
                        differ += "  " + rel + "\n";
                        rows += "differ\t" + rel + "\n";
                    } else {
                        ++nSame;
                    }
                }
                for (const auto& [rel, pb] : fb) {
                    if (!fa.count(rel)) {
                        ++nB;
                        onlyB += "  " + rel + "\n";
                        rows += "only_b\t" + rel + "\n";
                    }
                }
                const bool same = !nA && !nB && !nD;
                std::string text = same ? "the folders have the same files (" + sysutil::plural(nSame, "file") + ")\n"
                                        : sysutil::plural(nSame, "file") + " the same, " + std::to_string(nD) + " different, " + std::to_string(nA) + " only in a, " + std::to_string(nB) + " only in b\n";
                if (nD) text += "different:\n" + differ;
                if (nA) text += "only in " + sysutil::fromPath(a) + ":\n" + onlyA;
                if (nB) text += "only in " + sysutil::fromPath(b) + ":\n" + onlyB;
                return Result::success(text, same)
                    .with("same", same)
                    .with("different", static_cast<int64_t>(nD))
                    .with("only_a", static_cast<int64_t>(nA))
                    .with("only_b", static_cast<int64_t>(nB))
                    .with("rows", rows);
            }));

    reg.add({"explorer.duplicates", "Find duplicates",
             "Find files with exactly the same contents under a folder (same size, then same SHA-256), and how much space the extra copies take.",
             {opt("folder", ParamType::String, "where to look (default: home)"), opt("min_size", ParamType::Number, "ignore files smaller than this many bytes", 1.0)},
             false, true},
            reader([](const Params& p) {
                const fs::path root = pathParam(p, "folder");
                const auto minSize = static_cast<std::uint64_t>(std::max(0.0, numberParam(p, "min_size", 1)));
                std::map<std::uint64_t, std::vector<fs::path>> bySize;
                std::error_code ec;
                for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                    if (ec) break;
                    std::error_code e2;
                    const std::string name = sysutil::nameOf(it->path());
                    if (it->is_directory(e2) && !name.empty() && name[0] == '.') {
                        it.disable_recursion_pending();
                        continue;
                    }
                    if (!it->is_regular_file(e2) || it->is_symlink(e2)) continue;
                    std::uint64_t sz = it->file_size(e2);
                    if (!e2 && sz >= minSize) bySize[sz].push_back(it->path());
                }
                std::string text, rows;
                std::size_t groups = 0;
                std::uint64_t wasted = 0;
                for (auto& [size, files] : bySize) {
                    if (files.size() < 2) continue;
                    std::map<std::string, std::vector<fs::path>> byHash;
                    for (const auto& f : files) {
                        try {
                            byHash[checksumFile(f, "sha256")].push_back(f);
                        } catch (const std::exception&) {
                        }
                    }
                    for (auto& [hash, same] : byHash) {
                        if (same.size() < 2) continue;
                        ++groups;
                        wasted += size * (same.size() - 1);
                        text += sysutil::humanSize(size) + " x " + std::to_string(same.size()) + ":\n";
                        for (const auto& f : same) {
                            text += "  " + sysutil::fromPath(f) + "\n";
                            rows += std::to_string(groups) + "\t" + std::to_string(size) + "\t" + sysutil::fromPath(f) + "\n";
                        }
                    }
                }
                return Result::success(groups ? sysutil::plural(groups, "set") + " of duplicates, " + sysutil::humanSize(wasted) + " in extra copies\n" + text : "no duplicates found")
                    .with("groups", static_cast<int64_t>(groups))
                    .with("wasted", static_cast<int64_t>(wasted))
                    .with("rows", rows);
            }));
}

}  // namespace wetwaresuite::explorer::detail
