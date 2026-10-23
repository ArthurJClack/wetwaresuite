// Writing a changed copy of PDF files: pages picked from one or more files in
// any order and turn, each with extra content drawn on top, new annotations,
// replaced objects, and the main file's catalog carried over (form fields,
// bookmarks, named destinations, ...), so whatever the edits don't touch
// stays as it was. Only the objects the result still uses are written.
#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "wetwaresuite/pdf/reader.hpp"

namespace wetwaresuite::pdf {

class Rebuilder {
public:
    // `main` gives the catalog and document info; null makes a file of new pages only.
    explicit Rebuilder(const PdfFile* main);

    // New objects. Their numbers are output numbers: write "12 0 R" in bodies, or put
    // outputRef(12) in Objects handed to replace() and Page.
    int reserve();
    void set(int id, std::string body);  // without "n 0 obj" / "endobj"
    int add(std::string body);
    int addStream(const std::string& dict, const std::string& data, bool compress = true);  // dict: entries without << >>
    static Object outputRef(int id);

    // Writes `o` in place of object `num` of `file` (refs in `o` point into `file`, unless outputRef).
    void replace(const PdfFile& file, int num, Object o);
    // The catalog entry `key` (e.g. "AcroForm") replaced; a Null object removes it.
    void setCatalogEntry(const std::string& key, Object value);

    struct Page {
        const PdfFile* file = nullptr;  // null: a blank page of width x height
        int index = 0;
        double width = 612, height = 792;
        int rotate = 0;  // extra clockwise turn, a multiple of 90
        bool replaceContent = false;
        std::string content;  // the page's new (decoded) content when replaceContent
        std::string overlay;  // drawn on top, in the page's default user space
        std::vector<std::pair<std::string, int>> fonts, xobjects, extGStates;  // resource name -> output object
        std::vector<int> annots;   // new annotations (output objects)
        std::set<int> dropAnnots;  // object numbers of the page's own annotations to leave out
    };
    // Call once, before pageObject() and finish().
    void setPages(std::vector<Page> pages);
    int pageObject(std::size_t i) const;  // the output object number of page i

    // The finished file. `info` sets document info entries (Title, Author, ...) as UTF-8.
    std::string finish(const std::map<std::string, std::string>& info = {});

private:
    struct Key {
        std::size_t file;
        int num;
        bool operator<(const Key& o) const { return file != o.file ? file < o.file : num < o.num; }
    };
    const PdfFile* main_;
    std::vector<const PdfFile*> files_;
    std::vector<std::set<int>> pageObjects_;
    std::map<Key, int> selectedPage_, mapped_;
    std::map<Key, Object> overrides_;
    std::vector<std::pair<std::string, Object>> catalogEntries_;
    std::vector<std::string> out_;
    std::vector<Key> queue_;
    std::vector<Page> pages_;
    std::vector<int> pageIds_;
    int pagesId_ = 0;

    std::size_t fileIndex(const PdfFile* f);
    int refFor(std::size_t f, int num);
    std::string write(std::size_t f, const Object& o);
    const Object& source(std::size_t f, int num) const;
    std::string pageBody(std::size_t i);
};

}  // namespace wetwaresuite::pdf
