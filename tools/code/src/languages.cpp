// The language table: file types, comments, quotes and word lists for the highlighter.
#include <algorithm>
#include <sstream>

#include "wetwaresuite/code/code.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::code {

namespace {

std::set<std::string> words(const char* text) {
    std::set<std::string> out;
    std::istringstream in(text);
    std::string w;
    while (in >> w) out.insert(w);
    return out;
}

std::vector<std::string> list(const char* text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

const char* kCKeywords =
    "auto break case const continue default do else enum extern for goto if inline register restrict return sizeof static "
    "struct switch typedef union volatile while _Alignas _Alignof _Atomic _Bool _Generic _Noreturn _Static_assert _Thread_local";
const char* kCTypes = "char short int long float double signed unsigned void bool size_t ssize_t ptrdiff_t int8_t int16_t int32_t "
                      "int64_t uint8_t uint16_t uint32_t uint64_t uintptr_t intptr_t FILE wchar_t";
const char* kCppKeywords =
    "alignas alignof and asm auto break case catch class co_await co_return co_yield concept const consteval constexpr constinit "
    "const_cast continue decltype default delete do dynamic_cast else enum explicit export extern final for friend goto if "
    "import inline module mutable namespace new noexcept not operator or override private protected public register "
    "reinterpret_cast requires return sizeof static static_assert static_cast struct switch template this thread_local throw "
    "try typedef typeid typename union using virtual volatile while";
const char* kCppTypes = "bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t size_t "
                        "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t std string vector map set "
                        "unordered_map unique_ptr shared_ptr optional variant array span string_view";

void addCFamily(std::vector<Language>& t) {
    Language c;
    c.id = "c";
    c.name = "C";
    c.extensions = list(".c .h");
    c.lineComments = {"//"};
    c.blockComments = {{"/*", "*/"}};
    c.preprocessor = true;
    c.keywords = words(kCKeywords);
    c.types = words(kCTypes);
    c.constants = words("NULL true false EOF stdin stdout stderr");
    c.builtins = words("printf scanf malloc free calloc realloc memcpy memset strlen strcmp fopen fclose");
    t.push_back(c);

    Language cpp = c;
    cpp.id = "cpp";
    cpp.name = "C++";
    cpp.extensions = list(".cpp .cc .cxx .c++ .hpp .hh .hxx .h++ .ipp .inl .tpp .cppm .ixx");
    cpp.keywords = words(kCppKeywords);
    cpp.types = words(kCppTypes);
    cpp.constants = words("nullptr true false NULL");
    cpp.builtins = words("cout cin cerr endl printf move forward make_unique make_shared");
    t.push_back(cpp);

    Language cs;
    cs.id = "csharp";
    cs.name = "C#";
    cs.extensions = list(".cs .csx");
    cs.lineComments = {"//"};
    cs.blockComments = {{"/*", "*/"}};
    cs.preprocessor = true;
    cs.capitalTypes = true;
    cs.keywords = words(
        "abstract as base break case catch checked class const continue default delegate do else enum event explicit extern "
        "finally fixed for foreach goto if implicit in interface internal is lock namespace new operator out override params "
        "private protected public readonly record ref return sealed sizeof stackalloc static struct switch this throw try "
        "typeof unchecked unsafe using virtual volatile while async await var get set init yield where when partial");
    cs.types = words("bool byte char decimal double float int long object sbyte short string uint ulong ushort void dynamic nint nuint");
    cs.constants = words("true false null");
    t.push_back(cs);

    Language java;
    java.id = "java";
    java.name = "Java";
    java.extensions = list(".java");
    java.lineComments = {"//"};
    java.blockComments = {{"/*", "*/"}};
    java.capitalTypes = true;
    java.keywords = words(
        "abstract assert break case catch class const continue default do else enum extends final finally for goto if "
        "implements import instanceof interface native new package private protected public return static strictfp super "
        "switch synchronized this throw throws transient try volatile while var record sealed permits yield non-sealed");
    java.types = words("boolean byte char double float int long short void String Object Integer List Map");
    java.constants = words("true false null");
    t.push_back(java);

    Language kt = java;
    kt.id = "kotlin";
    kt.name = "Kotlin";
    kt.extensions = list(".kt .kts");
    kt.multilineStrings = false;
    kt.tripleQuotes = true;
    kt.keywords = words(
        "as break class continue do else for fun if in interface is object package return super this throw try typealias "
        "typeof val var when while by catch constructor delegate dynamic field file finally get import init param property "
        "receiver set setparam where actual abstract annotation companion const crossinline data enum expect external final "
        "infix inline inner internal lateinit noinline open operator out override private protected public reified sealed "
        "suspend tailrec vararg");
    kt.types = words("Int Long Short Byte Double Float Boolean Char String Unit Any Nothing List Map Set Array");
    t.push_back(kt);

    Language swift = java;
    swift.id = "swift";
    swift.name = "Swift";
    swift.extensions = list(".swift");
    swift.tripleQuotes = true;
    swift.keywords = words(
        "associatedtype class deinit enum extension fileprivate func import init inout internal let open operator private "
        "protocol public rethrows static struct subscript typealias var break case continue default defer do else fallthrough "
        "for guard if in repeat return switch where while as catch is rethrows super self Self throw throws try async await "
        "some any mutating override final lazy weak");
    swift.types = words("Int Double Float Bool String Character Array Dictionary Set Optional Void");
    swift.constants = words("true false nil");
    t.push_back(swift);

    Language dart = java;
    dart.id = "dart";
    dart.name = "Dart";
    dart.extensions = list(".dart");
    dart.tripleQuotes = true;
    dart.keywords = words(
        "abstract as assert async await break case catch class const continue covariant default deferred do dynamic else enum "
        "export extends extension external factory final finally for get if implements import in interface is late library "
        "mixin new on operator part required rethrow return set show static super switch sync this throw try typedef var "
        "while with yield");
    dart.types = words("int double num bool String List Map Set void Future Stream Object");
    t.push_back(dart);

    Language scala = java;
    scala.id = "scala";
    scala.name = "Scala";
    scala.extensions = list(".scala .sc");
    scala.tripleQuotes = true;
    scala.keywords = words(
        "abstract case catch class def do else extends final finally for forSome if implicit import lazy match new object "
        "override package private protected return sealed super this throw trait try type val var while with yield given "
        "using enum then export");
    t.push_back(scala);
}

void addSystems(std::vector<Language>& t) {
    Language rust;
    rust.id = "rust";
    rust.name = "Rust";
    rust.extensions = list(".rs");
    rust.lineComments = {"//"};
    rust.blockComments = {{"/*", "*/"}};
    rust.quotes = "\"";
    rust.multilineStrings = true;
    rust.capitalTypes = true;
    rust.keywords = words(
        "as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move mut pub ref "
        "return self Self static struct super trait type unsafe use where while yield macro_rules union");
    rust.types = words("i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option Result Box");
    rust.constants = words("true false None Some Ok Err");
    rust.builtins = words("println! print! eprintln! format! vec! panic! assert! assert_eq! write! writeln!");
    t.push_back(rust);

    Language go;
    go.id = "go";
    go.name = "Go";
    go.extensions = list(".go");
    go.lineComments = {"//"};
    go.blockComments = {{"/*", "*/"}};
    go.quotes = "\"'`";
    go.keywords = words(
        "break case chan const continue default defer else fallthrough for func go goto if import interface map package range "
        "return select struct switch type var");
    go.types = words("bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 "
                     "uint32 uint64 uintptr any");
    go.constants = words("true false nil iota");
    go.builtins = words("append cap close complex copy delete imag len make new panic print println real recover min max clear");
    t.push_back(go);

    Language zig;
    zig.id = "zig";
    zig.name = "Zig";
    zig.extensions = list(".zig");
    zig.lineComments = {"//"};
    zig.quotes = "\"'";
    zig.keywords = words(
        "addrspace align allowzero and anyframe anytype asm async await break callconv catch comptime const continue defer else "
        "enum errdefer error export extern fn for if inline noalias nosuspend noinline opaque or orelse packed pub resume "
        "return linksection struct suspend switch test threadlocal try union unreachable usingnamespace var volatile while");
    zig.types = words("i8 i16 i32 i64 u8 u16 u32 u64 usize isize f32 f64 bool void type anyerror noreturn");
    zig.constants = words("true false null undefined");
    t.push_back(zig);
}

void addScripting(std::vector<Language>& t) {
    Language py;
    py.id = "python";
    py.name = "Python";
    py.extensions = list(".py .pyw .pyi");
    py.filenames = list("SConstruct SConscript");
    py.lineComments = {"#"};
    py.tripleQuotes = true;
    py.capitalTypes = true;
    py.keywords = words(
        "and as assert async await break class continue def del elif else except finally for from global if import in is "
        "lambda nonlocal not or pass raise return try while with yield match case type");
    py.types = words("int float str bool list dict set tuple bytes object complex frozenset bytearray");
    py.constants = words("True False None self cls");
    py.builtins = words("print len range open input enumerate zip map filter sorted sum min max abs isinstance super");
    t.push_back(py);

    Language js;
    js.id = "javascript";
    js.name = "JavaScript";
    js.extensions = list(".js .mjs .cjs .jsx");
    js.lineComments = {"//"};
    js.blockComments = {{"/*", "*/"}};
    js.quotes = "\"'`";
    js.capitalTypes = true;
    js.variablePrefix = "";
    js.keywords = words(
        "async await break case catch class const continue debugger default delete do else export extends finally for from "
        "function get if import in instanceof let new of return set static super switch this throw try typeof var void while "
        "with yield");
    js.constants = words("true false null undefined NaN Infinity");
    js.builtins = words("console window document require module exports Math JSON Promise Array Object String Number");
    t.push_back(js);

    Language ts = js;
    ts.id = "typescript";
    ts.name = "TypeScript";
    ts.extensions = list(".ts .tsx .mts .cts");
    for (const auto& w : words("abstract as declare enum implements interface keyof namespace private protected public readonly "
                               "type satisfies infer is asserts override unique"))
        ts.keywords.insert(w);
    ts.types = words("string number boolean any unknown never void object bigint symbol");
    t.push_back(ts);

    Language lua;
    lua.id = "lua";
    lua.name = "Lua";
    lua.extensions = list(".lua");
    lua.lineComments = {"--"};
    lua.blockComments = {{"--[[", "]]"}};
    lua.keywords = words("and break do else elseif end for function goto if in local not or repeat return then until while");
    lua.constants = words("true false nil self");
    lua.builtins = words("print pairs ipairs require tostring tonumber type table string math io os select error pcall setmetatable");
    t.push_back(lua);

    Language rb;
    rb.id = "ruby";
    rb.name = "Ruby";
    rb.extensions = list(".rb .rake .gemspec");
    rb.filenames = list("Rakefile Gemfile");
    rb.lineComments = {"#"};
    rb.blockComments = {{"=begin", "=end"}};
    rb.capitalTypes = true;
    rb.keywords = words(
        "alias and begin break case class def defined? do else elsif end ensure for if in module next not or redo rescue retry "
        "return self super then undef unless until when while yield require attr_accessor attr_reader private public");
    rb.constants = words("true false nil");
    t.push_back(rb);

    Language php;
    php.id = "php";
    php.name = "PHP";
    php.extensions = list(".php .phtml");
    php.lineComments = {"//", "#"};
    php.blockComments = {{"/*", "*/"}};
    php.variablePrefix = "$";
    php.keywords = words(
        "abstract and array as break callable case catch class clone const continue declare default do echo else elseif empty "
        "enddeclare endfor endforeach endif endswitch endwhile extends final finally fn for foreach function global goto if "
        "implements include include_once instanceof insteadof interface isset list match namespace new or print private "
        "protected public readonly require require_once return static switch throw trait try unset use var while yield");
    php.constants = words("true false null TRUE FALSE NULL");
    t.push_back(php);

    Language perl;
    perl.id = "perl";
    perl.name = "Perl";
    perl.extensions = list(".pl .pm .t");
    perl.lineComments = {"#"};
    perl.variablePrefix = "$";
    perl.keywords = words("my our local sub if elsif else unless while until for foreach do last next redo return use require package "
                          "and or not eq ne lt gt le ge print");
    t.push_back(perl);

    Language r;
    r.id = "r";
    r.name = "R";
    r.extensions = list(".r .R .rmd");
    r.lineComments = {"#"};
    r.keywords = words("if else repeat while function for in next break return library require");
    r.constants = words("TRUE FALSE NULL NA Inf NaN T F");
    t.push_back(r);

    Language jl;
    jl.id = "julia";
    jl.name = "Julia";
    jl.extensions = list(".jl");
    jl.lineComments = {"#"};
    jl.blockComments = {{"#=", "=#"}};
    jl.tripleQuotes = true;
    jl.capitalTypes = true;
    jl.keywords = words("baremodule begin break catch const continue do else elseif end export finally for function global if import "
                        "let local macro module quote return struct try using while mutable abstract primitive type where in");
    jl.constants = words("true false nothing missing");
    t.push_back(jl);

    Language ex;
    ex.id = "elixir";
    ex.name = "Elixir";
    ex.extensions = list(".ex .exs");
    ex.lineComments = {"#"};
    ex.tripleQuotes = true;
    ex.capitalTypes = true;
    ex.keywords = words("def defp defmodule defmacro defstruct do end if else unless case cond fn when with for receive try catch "
                        "rescue after import alias require use quote unquote and or not in");
    ex.constants = words("true false nil");
    t.push_back(ex);

    Language hs;
    hs.id = "haskell";
    hs.name = "Haskell";
    hs.extensions = list(".hs .lhs");
    hs.lineComments = {"--"};
    hs.blockComments = {{"{-", "-}"}};
    hs.quotes = "\"";
    hs.capitalTypes = true;
    hs.keywords = words("case class data default deriving do else foreign if import in infix infixl infixr instance let module "
                        "newtype of then type where qualified as hiding");
    hs.constants = words("True False Nothing Just");
    t.push_back(hs);
}

void addShells(std::vector<Language>& t) {
    Language sh;
    sh.id = "shell";
    sh.name = "Shell";
    sh.extensions = list(".sh .bash .zsh .ksh .bashrc .zshrc .profile");
    sh.filenames = list(".bashrc .zshrc .profile .bash_profile PKGBUILD");
    sh.lineComments = {"#"};
    sh.variablePrefix = "$";
    sh.multilineStrings = true;
    sh.keywords = words("if then else elif fi case esac for select while until do done in function time return exit break continue "
                        "local export readonly declare unset shift source alias");
    sh.builtins = words("echo printf read cd pwd test set eval exec trap wait kill true false");
    t.push_back(sh);

    Language fish = sh;
    fish.id = "fish";
    fish.name = "Fish";
    fish.extensions = list(".fish");
    fish.filenames = {};
    fish.keywords = words("if else switch case for in while function return break continue and or not begin end set");
    t.push_back(fish);

    Language ps;
    ps.id = "powershell";
    ps.name = "PowerShell";
    ps.extensions = list(".ps1 .psm1 .psd1");
    ps.lineComments = {"#"};
    ps.blockComments = {{"<#", "#>"}};
    ps.variablePrefix = "$";
    ps.caseInsensitive = true;
    ps.keywords = words("begin break catch class continue data define do dynamicparam else elseif end exit filter finally for "
                        "foreach from function if in param process return switch throw trap try until using var while");
    ps.constants = words("$true $false $null");
    t.push_back(ps);

    Language bat;
    bat.id = "batch";
    bat.name = "Batch";
    bat.extensions = list(".bat .cmd");
    bat.lineComments = {"REM", "rem", "::"};
    bat.quotes = "\"";
    bat.caseInsensitive = true;
    bat.variablePrefix = "%";
    bat.keywords = words("echo set if else goto call exit for in do not exist defined errorlevel setlocal endlocal pause shift");
    t.push_back(bat);

    Language mk;
    mk.id = "makefile";
    mk.name = "Makefile";
    mk.extensions = list(".mk .mak");
    mk.filenames = list("Makefile makefile GNUmakefile");
    mk.lineComments = {"#"};
    mk.variablePrefix = "$";
    mk.keywords = words("ifeq ifneq ifdef ifndef else endif include define endef export override .PHONY");
    t.push_back(mk);

    Language cm;
    cm.id = "cmake";
    cm.name = "CMake";
    cm.extensions = list(".cmake");
    cm.filenames = list("CMakeLists.txt");
    cm.lineComments = {"#"};
    cm.quotes = "\"";
    cm.caseInsensitive = true;
    cm.variablePrefix = "$";
    cm.keywords = words("if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro return set "
                        "option project add_executable add_library target_link_libraries target_include_directories "
                        "add_subdirectory find_package include message install cmake_minimum_required list string");
    t.push_back(cm);

    Language dock;
    dock.id = "dockerfile";
    dock.name = "Dockerfile";
    dock.extensions = list(".dockerfile");
    dock.filenames = list("Dockerfile Containerfile");
    dock.lineComments = {"#"};
    dock.caseInsensitive = true;
    dock.variablePrefix = "$";
    dock.keywords = words("FROM RUN CMD LABEL EXPOSE ENV ADD COPY ENTRYPOINT VOLUME USER WORKDIR ARG ONBUILD STOPSIGNAL HEALTHCHECK SHELL AS");
    t.push_back(dock);
}

void addData(std::vector<Language>& t) {
    Language html;
    html.id = "html";
    html.name = "HTML";
    html.extensions = list(".html .htm .xhtml .vue .svelte");
    html.mode = Mode::Markup;
    html.blockComments = {{"<!--", "-->"}};
    t.push_back(html);

    Language xml = html;
    xml.id = "xml";
    xml.name = "XML";
    xml.extensions = list(".xml .svg .xsd .xsl .plist .csproj .vcxproj .props .targets .kml .gpx .xaml");
    t.push_back(xml);

    Language css;
    css.id = "css";
    css.name = "CSS";
    css.extensions = list(".css .scss .sass .less");
    css.lineComments = {};
    css.blockComments = {{"/*", "*/"}};
    css.colonKeys = true;
    css.keywords = words("!important @media @import @font-face @keyframes @supports @layer @charset @use @include @mixin");
    css.constants = words("inherit initial unset auto none block inline flex grid absolute relative fixed sticky");
    t.push_back(css);

    Language json;
    json.id = "json";
    json.name = "JSON";
    json.extensions = list(".json .jsonc .json5 .geojson .ipynb .webmanifest");
    json.filenames = list(".babelrc .eslintrc .prettierrc");
    json.lineComments = {"//"};
    json.blockComments = {{"/*", "*/"}};
    json.quotes = "\"";
    json.colonKeys = true;
    json.constants = words("true false null");
    t.push_back(json);

    Language yaml;
    yaml.id = "yaml";
    yaml.name = "YAML";
    yaml.extensions = list(".yaml .yml");
    yaml.mode = Mode::Config;
    yaml.lineComments = {"#"};
    yaml.constants = words("true false null yes no on off ~ True False Null");
    t.push_back(yaml);

    Language toml = yaml;
    toml.id = "toml";
    toml.name = "TOML";
    toml.extensions = list(".toml");
    toml.filenames = list("Cargo.lock Pipfile");
    toml.tripleQuotes = true;
    toml.constants = words("true false inf nan");
    t.push_back(toml);

    Language ini = yaml;
    ini.id = "ini";
    ini.name = "INI";
    ini.extensions = list(".ini .cfg .conf .properties .env .editorconfig .gitconfig .desktop .service");
    ini.filenames = list(".env .gitconfig .editorconfig .gitattributes .gitignore .npmrc");
    ini.lineComments = {"#", ";"};
    ini.constants = words("true false yes no on off");
    t.push_back(ini);

    Language md;
    md.id = "markdown";
    md.name = "Markdown";
    md.extensions = list(".md .markdown .mdx .mkd");
    md.filenames = list("README CHANGELOG");
    md.mode = Mode::Markdown;
    t.push_back(md);

    Language sql;
    sql.id = "sql";
    sql.name = "SQL";
    sql.extensions = list(".sql .psql .ddl");
    sql.lineComments = {"--", "#"};
    sql.blockComments = {{"/*", "*/"}};
    sql.quotes = "'\"`";
    sql.caseInsensitive = true;
    sql.keywords = words(
        "select from where and or not insert into values update set delete create table view index drop alter add column primary "
        "key foreign references join inner left right outer full cross on group by order having limit offset union all distinct "
        "as case when then else end in is null like between exists begin commit rollback transaction with recursive returning "
        "default unique check constraint if replace trigger procedure function returns declare cascade asc desc");
    sql.types = words("int integer bigint smallint tinyint real float double decimal numeric varchar char text blob date time "
                      "timestamp datetime boolean bool serial uuid json jsonb");
    sql.constants = words("true false null");
    sql.builtins = words("count sum avg min max coalesce cast now length upper lower substr round");
    t.push_back(sql);
}

}  // namespace

const std::vector<Language>& languages() {
    static const std::vector<Language> table = [] {
        std::vector<Language> t;
        addCFamily(t);
        addSystems(t);
        addScripting(t);
        addShells(t);
        addData(t);
        return t;
    }();
    return table;
}

const Language& plainText() {
    static const Language plain = [] {
        Language l;
        l.id = "text";
        l.name = "Plain text";
        l.mode = Mode::Plain;
        l.extensions = {".txt", ".log", ".text"};
        return l;
    }();
    return plain;
}

const Language* languageById(const std::string& id) {
    const std::string want = sysutil::lowerAscii(id);
    for (const auto& l : languages()) {
        if (l.id == want || sysutil::lowerAscii(l.name) == want) return &l;
    }
    static const std::vector<std::pair<std::string, std::string>> aliases = {
        {"c++", "cpp"}, {"cxx", "cpp"}, {"cs", "csharp"}, {"c#", "csharp"}, {"js", "javascript"}, {"ts", "typescript"},
        {"py", "python"}, {"rs", "rust"}, {"sh", "shell"}, {"bash", "shell"}, {"zsh", "shell"}, {"ps1", "powershell"},
        {"yml", "yaml"}, {"md", "markdown"}, {"kt", "kotlin"}, {"rb", "ruby"}, {"golang", "go"}, {"htm", "html"}};
    for (const auto& [a, b] : aliases) {
        if (a == want) return languageById(b);
    }
    if (want == "text" || want == "plain") return &plainText();
    return nullptr;
}

const Language& detectLanguage(const std::string& fileName, const std::string& head) {
    std::string name = fileName;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    for (const auto& l : languages()) {
        if (std::find(l.filenames.begin(), l.filenames.end(), name) != l.filenames.end()) return l;
    }
    std::string lower = sysutil::lowerAscii(name);
    // longest extension first: "x.d.ts" is TypeScript either way, "a.tar.gz" is not code
    for (std::size_t dot = lower.find('.'); dot != std::string::npos; dot = lower.find('.', dot + 1)) {
        const std::string ext = lower.substr(dot);
        for (const auto& l : languages()) {
            for (const auto& e : l.extensions) {
                if (sysutil::lowerAscii(e) == ext) return l;
            }
        }
        if (ext == ".txt" || ext == ".log") return plainText();
    }
    if (lower.rfind("dockerfile", 0) == 0) return *languageById("dockerfile");
    if (head.rfind("#!", 0) == 0) {
        std::string line = head.substr(0, head.find('\n'));
        struct Shebang {
            const char* word;
            const char* id;
        };
        static const Shebang shebangs[] = {{"python", "python"}, {"node", "javascript"}, {"deno", "typescript"}, {"bun", "javascript"},
                                           {"bash", "shell"}, {"zsh", "shell"}, {"/sh", "shell"}, {"env sh", "shell"},
                                           {"ruby", "ruby"}, {"perl", "perl"}, {"php", "php"}, {"lua", "lua"}, {"fish", "fish"},
                                           {"pwsh", "powershell"}, {"Rscript", "r"}, {"julia", "julia"}, {"elixir", "elixir"}};
        for (const auto& s : shebangs) {
            if (line.find(s.word) != std::string::npos) return *languageById(s.id);
        }
    }
    if (head.rfind("<?xml", 0) == 0) return *languageById("xml");
    if (head.rfind("<!DOCTYPE html", 0) == 0 || head.rfind("<html", 0) == 0) return *languageById("html");
    return plainText();
}

}  // namespace wetwaresuite::code
