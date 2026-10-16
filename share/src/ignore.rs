//! `.gitignore`-style rules for sync: globs (`*`, `?`, `[a-z]`), `**`,
//! trailing `/` for folders only, leading `/` or a middle `/` to anchor to
//! the file's folder, `!` to re-include, `#` comments and `\` escapes.
//! Rules come from `.gitignore` and `.shareignore` in any folder; deeper
//! files and later lines win, and nothing under an ignored folder can be
//! re-included (as in git).

use std::path::Path;

/// Folders skipped unless `--include-heavy`: version control data, build
/// output and dependency caches, which are big, regenerable or both.
pub const HEAVY: [&str; 6] = [".git", "node_modules", "target", "build", ".venv", "__pycache__"];

/// Prefix of temporary files the tool writes while receiving; never synced.
pub const TEMP_PREFIX: &str = ".achclwss-share-";

pub const IGNORE_FILES: [&str; 2] = [".gitignore", ".shareignore"];

#[derive(Debug, Clone, PartialEq)]
enum Tok {
    Lit(char),
    Star,
    One,
    Class(Vec<(char, char)>, bool),
    /// `**/`: zero or more whole folders
    Dirs,
    /// trailing `/**` content: anything, including slashes
    All,
}

#[derive(Debug, Clone)]
pub struct Rule {
    toks: Vec<Tok>,
    negate: bool,
    dir_only: bool,
    anchored: bool,
}

impl Rule {
    /// Parses one line of an ignore file; `None` for blanks and comments.
    pub fn parse(line: &str) -> Option<Rule> {
        let line = line.strip_suffix('\r').unwrap_or(line);
        // trailing spaces are dropped unless escaped
        let mut line = line.to_string();
        while line.ends_with(' ') && !line.ends_with("\\ ") {
            line.pop();
        }
        if line.is_empty() || line.starts_with('#') {
            return None;
        }
        let mut s = line.as_str();
        let mut negate = false;
        if let Some(rest) = s.strip_prefix('!') {
            negate = true;
            s = rest;
        }
        let mut dir_only = false;
        if s.ends_with('/') && !s.ends_with("\\/") {
            dir_only = true;
            s = &s[..s.len() - 1];
        }
        let anchored = s.contains('/');
        let s = s.strip_prefix('/').unwrap_or(s);
        if s.is_empty() {
            return None;
        }
        Some(Rule { toks: tokenize(s), negate, dir_only, anchored })
    }

    pub fn matches(&self, path: &str, is_dir: bool) -> bool {
        if self.dir_only && !is_dir {
            return false;
        }
        let target = if self.anchored { path } else { path.rsplit('/').next().unwrap_or(path) };
        let chars: Vec<char> = target.chars().collect();
        glob(&self.toks, &chars)
    }
}

fn tokenize(p: &str) -> Vec<Tok> {
    let c: Vec<char> = p.chars().collect();
    let mut toks = Vec::new();
    let mut i = 0;
    while i < c.len() {
        match c[i] {
            '*' if c.get(i + 1) == Some(&'*') && (i == 0 || c[i - 1] == '/') => {
                if c.get(i + 2) == Some(&'/') {
                    toks.push(Tok::Dirs);
                    i += 3;
                } else if i + 2 == c.len() {
                    toks.push(Tok::All);
                    i += 2;
                } else {
                    toks.push(Tok::Star);
                    i += 2;
                }
            }
            '*' => {
                // collapse runs of stars that aren't a `**` segment
                while c.get(i + 1) == Some(&'*') {
                    i += 1;
                }
                toks.push(Tok::Star);
                i += 1;
            }
            '?' => {
                toks.push(Tok::One);
                i += 1;
            }
            '\\' if i + 1 < c.len() => {
                toks.push(Tok::Lit(c[i + 1]));
                i += 2;
            }
            '[' => match parse_class(&c, i) {
                Some((tok, next)) => {
                    toks.push(tok);
                    i = next;
                }
                None => {
                    toks.push(Tok::Lit('['));
                    i += 1;
                }
            },
            ch => {
                toks.push(Tok::Lit(ch));
                i += 1;
            }
        }
    }
    toks
}

fn parse_class(c: &[char], start: usize) -> Option<(Tok, usize)> {
    let mut i = start + 1;
    let mut negate = false;
    if matches!(c.get(i), Some('!') | Some('^')) {
        negate = true;
        i += 1;
    }
    let mut ranges = Vec::new();
    let mut first = true;
    while i < c.len() {
        if c[i] == ']' && !first {
            return Some((Tok::Class(ranges, negate), i + 1));
        }
        first = false;
        let lo = if c[i] == '\\' && i + 1 < c.len() {
            i += 1;
            c[i]
        } else {
            c[i]
        };
        if c.get(i + 1) == Some(&'-') && c.get(i + 2).is_some_and(|x| *x != ']') {
            ranges.push((lo, c[i + 2]));
            i += 3;
        } else {
            ranges.push((lo, lo));
            i += 1;
        }
    }
    None
}

fn glob(toks: &[Tok], s: &[char]) -> bool {
    let Some((tok, rest)) = toks.split_first() else { return s.is_empty() };
    match tok {
        Tok::Lit(ch) => s.first() == Some(ch) && glob(rest, &s[1..]),
        Tok::One => s.first().is_some_and(|c| *c != '/') && glob(rest, &s[1..]),
        Tok::Class(ranges, negate) => {
            s.first().is_some_and(|c| *c != '/' && ranges.iter().any(|(lo, hi)| lo <= c && c <= hi) != *negate)
                && glob(rest, &s[1..])
        }
        Tok::Star => {
            for i in 0..=s.len() {
                if glob(rest, &s[i..]) {
                    return true;
                }
                if i < s.len() && s[i] == '/' {
                    break;
                }
            }
            false
        }
        Tok::All => !s.is_empty(),
        Tok::Dirs => {
            // zero folders, or skip through each following slash
            if glob(rest, s) {
                return true;
            }
            for i in 0..s.len() {
                if s[i] == '/' && glob(rest, &s[i + 1..]) {
                    return true;
                }
            }
            false
        }
    }
}

/// The rules in force for one tree.
#[derive(Debug, Clone, Default)]
pub struct Ignorer {
    pub include_heavy: bool,
    /// (folder relative to the root, its rules) in load order, shallow first
    sets: Vec<(String, Vec<Rule>)>,
}

impl Ignorer {
    pub fn new(include_heavy: bool) -> Ignorer {
        Ignorer { include_heavy, sets: Vec::new() }
    }

    /// Adds rules as if they came from an ignore file in folder `base`.
    pub fn add_rules(&mut self, base: &str, text: &str) {
        let rules: Vec<Rule> = text.lines().filter_map(Rule::parse).collect();
        if !rules.is_empty() {
            self.sets.push((base.to_string(), rules));
            // shallow folders first so deeper files override them
            self.sets.sort_by_key(|(b, _)| if b.is_empty() { 0 } else { b.matches('/').count() + 1 });
        }
    }

    /// Reads `.gitignore` and `.shareignore` from `dir` (relative path `rel`).
    pub fn load_dir(&mut self, dir: &Path, rel: &str) {
        for name in IGNORE_FILES {
            if let Ok(text) = std::fs::read_to_string(dir.join(name)) {
                self.add_rules(rel, &text);
            }
        }
    }

    fn rules_say(&self, path: &str, is_dir: bool) -> bool {
        let mut ignored = false;
        for (base, rules) in &self.sets {
            let sub = if base.is_empty() {
                path
            } else if let Some(rest) = path.strip_prefix(base.as_str()).and_then(|r| r.strip_prefix('/')) {
                rest
            } else {
                continue;
            };
            for r in rules {
                if r.matches(sub, is_dir) {
                    ignored = !r.negate;
                }
            }
        }
        ignored
    }

    fn name_skipped(&self, name: &str, is_dir: bool) -> bool {
        if name.starts_with(TEMP_PREFIX) {
            return true;
        }
        !self.include_heavy && (is_dir || name == ".git") && HEAVY.contains(&name)
    }

    /// Whether one entry is ignored, given that its parent folder is not.
    /// Used while walking, which never descends into ignored folders.
    pub fn ignored_entry(&self, path: &str, is_dir: bool) -> bool {
        let name = path.rsplit('/').next().unwrap_or(path);
        self.name_skipped(name, is_dir) || self.rules_say(path, is_dir)
    }

    /// Whether a path is ignored, checking every parent folder too.
    pub fn is_ignored(&self, path: &str, is_dir: bool) -> bool {
        let segs: Vec<&str> = path.split('/').collect();
        for i in 1..segs.len() {
            if self.ignored_entry(&segs[..i].join("/"), true) {
                return true;
            }
        }
        self.ignored_entry(path, is_dir)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ig(text: &str) -> Ignorer {
        let mut i = Ignorer::new(false);
        i.add_rules("", text);
        i
    }

    #[test]
    fn basic_globs() {
        let i = ig("*.log\n# comment\n\n?.tmp\n[abc]x.o\n[!0-9]y\n");
        assert!(i.is_ignored("debug.log", false));
        assert!(i.is_ignored("deep/dir/debug.log", false));
        assert!(!i.is_ignored("debug.log.txt", false));
        assert!(i.is_ignored("a.tmp", false));
        assert!(!i.is_ignored("ab.tmp", false));
        assert!(i.is_ignored("bx.o", false));
        assert!(!i.is_ignored("dx.o", false));
        assert!(i.is_ignored("zy", false));
        assert!(!i.is_ignored("5y", false));
        assert!(!i.is_ignored("# comment", false));
    }

    #[test]
    fn anchoring_and_dirs() {
        let i = ig("/root.txt\ndocs/*.md\nout/\nlogs\n");
        assert!(i.is_ignored("root.txt", false));
        assert!(!i.is_ignored("sub/root.txt", false));
        assert!(i.is_ignored("docs/a.md", false));
        assert!(!i.is_ignored("docs/sub/a.md", false));
        assert!(!i.is_ignored("x/docs/a.md", false));
        assert!(i.is_ignored("out", true));
        assert!(!i.is_ignored("out", false)); // a file named out is kept
        assert!(i.is_ignored("out/file.bin", false)); // inside the folder
        assert!(i.is_ignored("a/b/out/file.bin", false));
        assert!(i.is_ignored("logs", false));
        assert!(i.is_ignored("x/logs/y", false));
    }

    #[test]
    fn double_star() {
        let i = ig("**/cache\nassets/**\na/**/z.txt\n**/*.pyc\n");
        assert!(i.is_ignored("cache", true));
        assert!(i.is_ignored("x/y/cache", true));
        assert!(i.is_ignored("assets/a/b.png", false));
        assert!(!i.is_ignored("assets", true));
        assert!(i.is_ignored("a/z.txt", false));
        assert!(i.is_ignored("a/b/c/z.txt", false));
        assert!(!i.is_ignored("b/z.txt", false));
        assert!(i.is_ignored("m/n/o.pyc", false));
    }

    #[test]
    fn negation_and_order() {
        let i = ig("*.env\n!example.env\n");
        assert!(i.is_ignored("prod.env", false));
        assert!(!i.is_ignored("example.env", false));
        // can't re-include inside an ignored folder
        let i = ig("vendor/\n!vendor/keep.txt\n");
        assert!(i.is_ignored("vendor/keep.txt", false));
        // escapes
        let i = ig("\\#hash\n\\!bang\nspace\\ \n");
        assert!(i.is_ignored("#hash", false));
        assert!(i.is_ignored("!bang", false));
        assert!(i.is_ignored("space ", false));
    }

    #[test]
    fn nested_files_override() {
        let mut i = Ignorer::new(false);
        i.add_rules("", "*.txt\n");
        i.add_rules("sub", "!keep.txt\n/local.bin\n");
        assert!(i.is_ignored("a.txt", false));
        assert!(i.is_ignored("sub/a.txt", false));
        assert!(!i.is_ignored("sub/keep.txt", false));
        assert!(i.is_ignored("sub/local.bin", false));
        assert!(!i.is_ignored("local.bin", false));
        assert!(!i.is_ignored("sub/deeper/local.bin", false));
    }

    /// The example from docs/tutorials/share.md.
    #[test]
    fn tutorial_example() {
        let i = ig("# c\n# c2\n\n*.log\n!keep.log\n/local.env\ndist/\ndocs/*.pdf\nassets/**/raw\n**/cache\n\\#notes.txt\n");
        assert!(i.is_ignored("a/b/x.log", false));
        assert!(!i.is_ignored("keep.log", false));
        assert!(!i.is_ignored("sub/keep.log", false));
        assert!(i.is_ignored("local.env", false));
        assert!(!i.is_ignored("app/local.env", false));
        assert!(i.is_ignored("web/dist/bundle.js", false));
        assert!(i.is_ignored("docs/spec.pdf", false));
        assert!(!i.is_ignored("docs/old/spec.pdf", false));
        assert!(i.is_ignored("assets/raw/a.wav", false));
        assert!(i.is_ignored("assets/x/y/raw/a.wav", false));
        assert!(i.is_ignored("deep/cache/f", false));
        assert!(i.is_ignored("#notes.txt", false));
        assert!(!i.is_ignored("src/main.rs", false));
    }

    #[test]
    fn heavy_folders() {
        let i = Ignorer::new(false);
        for p in [".git/config", "node_modules/x/index.js", "crate/target/debug/app", "build/out.o", ".venv/bin/python", "pkg/__pycache__/m.pyc"] {
            assert!(i.is_ignored(p, false), "{p}");
        }
        assert!(i.is_ignored(".git", false)); // a worktree's .git file too
        assert!(!i.is_ignored("src/build.rs", false));
        assert!(!i.is_ignored("target.txt", false));
        assert!(i.is_ignored(".achclwss-share-tmp-123", false));
        let heavy = Ignorer::new(true);
        assert!(!heavy.is_ignored("node_modules/x/index.js", false));
        assert!(heavy.is_ignored(".achclwss-share-tmp-123", false));
    }
}
