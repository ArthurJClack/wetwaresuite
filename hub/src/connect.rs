//! `hub connect`: adds the wetwaresuite MCP servers to Claude Desktop and Claude
//! Code by editing their config files, so nobody has to hand-edit JSON. Backs
//! each file up first, changes only its `mcpServers` entries, and running it
//! twice changes nothing. `--remove` takes the entries out again.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use crate::json::{self, Json};

/// One app's config file.
#[derive(Debug, Clone)]
pub struct Target {
    pub app: &'static str,
    pub path: PathBuf,
    /// Make the file if it's missing but the app's folder exists.
    pub create: bool,
    /// Claude Code wants `"type": "stdio"` on each entry.
    pub typed: bool,
}

#[derive(Debug, Default)]
struct Options {
    dry_run: bool,
    remove: bool,
    bin_dir: Option<PathBuf>,
    config: Option<PathBuf>,
    desktop: bool,
    code: bool,
}

fn env_path(name: &str) -> Option<PathBuf> {
    std::env::var_os(name).filter(|v| !v.is_empty()).map(PathBuf::from)
}

fn home() -> Option<PathBuf> {
    env_path(if cfg!(windows) { "USERPROFILE" } else { "HOME" }).or_else(|| env_path("HOME"))
}

/// Where Claude Desktop and Claude Code keep their MCP servers on this OS.
pub fn targets() -> Vec<Target> {
    let mut out = Vec::new();
    let desktop_dir = if cfg!(windows) {
        env_path("APPDATA").map(|d| d.join("Claude"))
    } else if cfg!(target_os = "macos") {
        home().map(|h| h.join("Library").join("Application Support").join("Claude"))
    } else {
        env_path("XDG_CONFIG_HOME").or_else(|| home().map(|h| h.join(".config"))).map(|d| d.join("Claude"))
    };
    if let Some(dir) = desktop_dir {
        out.push(Target { app: "Claude Desktop", path: dir.join("claude_desktop_config.json"), create: true, typed: false });
    }
    // Claude Code keeps user-wide servers in ~/.claude.json (or $CLAUDE_CONFIG_DIR/.claude.json)
    if let Some(dir) = env_path("CLAUDE_CONFIG_DIR").or_else(home) {
        out.push(Target { app: "Claude Code", path: dir.join(".claude.json"), create: false, typed: true });
    }
    out
}

fn entry(command: &Path, typed: bool) -> Json {
    let mut fields = Vec::new();
    if typed {
        fields.push(("type".to_string(), Json::String("stdio".into())));
    }
    fields.push(("command".to_string(), Json::String(command.to_string_lossy().into_owned())));
    fields.push(("args".to_string(), Json::Array(vec![Json::String("mcp".into())])));
    if typed {
        fields.push(("env".to_string(), Json::Object(Vec::new())));
    }
    Json::Object(fields)
}

fn same_server(a: &Json, b: &Json) -> bool {
    a.get("command") == b.get("command") && a.get("args") == b.get("args")
}

/// What applying the change to one file's text did.
#[derive(Debug, Default, PartialEq)]
pub struct Change {
    /// The new file text, if anything changed.
    pub text: Option<String>,
    pub added: Vec<String>,
    pub updated: Vec<String>,
    pub unchanged: Vec<String>,
    pub removed: Vec<String>,
}

/// Adds (or with `remove`, deletes) `servers` in a config file's text. `None`
/// text means the file doesn't exist yet. Only `mcpServers` is touched.
pub fn apply(text: Option<&str>, servers: &[(String, PathBuf)], typed: bool, remove: bool) -> Result<Change, String> {
    let trimmed = text.map(str::trim).unwrap_or("");
    let mut root = if trimmed.is_empty() { Json::Object(Vec::new()) } else { json::parse(trimmed)? };
    if !matches!(root, Json::Object(_)) {
        return Err("the file isn't a JSON object".into());
    }
    let mut change = Change::default();
    if root.get("mcpServers").is_none() {
        if remove {
            return Ok(change);
        }
        root.set("mcpServers", Json::Object(Vec::new()));
    }
    let block = root.get_mut("mcpServers").ok_or("no mcpServers")?;
    if !matches!(block, Json::Object(_)) {
        return Err("\"mcpServers\" isn't a JSON object".into());
    }
    for (name, command) in servers {
        if remove {
            if block.remove(name) {
                change.removed.push(name.clone());
            }
            continue;
        }
        let wanted = entry(command, typed);
        match block.get(name) {
            Some(existing) if same_server(existing, &wanted) => change.unchanged.push(name.clone()),
            Some(_) => {
                block.set(name, wanted);
                change.updated.push(name.clone());
            }
            None => {
                block.set(name, wanted);
                change.added.push(name.clone());
            }
        }
    }
    if !change.added.is_empty() || !change.updated.is_empty() || !change.removed.is_empty() {
        let newline = if text.is_some_and(|t| t.contains("\r\n")) { "\r\n" } else { "\n" };
        let mut out = root.pretty();
        if newline == "\r\n" {
            out = out.replace('\n', "\r\n");
        }
        out.push_str(newline);
        change.text = Some(out);
    }
    Ok(change)
}

fn parse_options(args: &[String]) -> Result<Options, String> {
    let mut o = Options::default();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--dry-run" | "-n" => o.dry_run = true,
            "--remove" => o.remove = true,
            "--desktop" => o.desktop = true,
            "--code" => o.code = true,
            "--bin-dir" => o.bin_dir = Some(PathBuf::from(it.next().ok_or("--bin-dir needs a folder")?)),
            "--config" => o.config = Some(PathBuf::from(it.next().ok_or("--config needs a file")?)),
            other => return Err(format!("unknown option '{other}'")),
        }
    }
    Ok(o)
}

pub const USAGE: &str = "usage: hub connect [--dry-run] [--remove] [--desktop] [--code] [--bin-dir DIR] [--config FILE]\n\n  \
    Adds the wetwaresuite tools to Claude Desktop and Claude Code (whichever are installed).\n  \
    --dry-run      show what would change, change nothing\n  \
    --remove       take the wetwaresuite entries out again\n  \
    --desktop      only Claude Desktop      --code   only Claude Code\n  \
    --bin-dir DIR  where the wetwaresuite programs are (default: next to hub)\n  \
    --config FILE  edit this MCP config file instead of looking for the apps";

fn list(names: &[String]) -> String {
    names.join(", ")
}

fn backup_path(path: &Path) -> PathBuf {
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let mut name = path.file_name().unwrap_or_default().to_os_string();
    name.push(format!(".backup-{secs}"));
    path.with_file_name(name)
}

fn write_atomically(path: &Path, text: &str) -> std::io::Result<()> {
    let mut tmp = path.as_os_str().to_os_string();
    tmp.push(".hub-tmp");
    let tmp = PathBuf::from(tmp);
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, path).inspect_err(|_| {
        let _ = std::fs::remove_file(&tmp);
    })
}

/// Runs `hub connect`. `binaries` are the MCP programs to connect; `default_bin_dir`
/// is where to look for them unless `--bin-dir` says otherwise.
pub fn run(args: &[String], binaries: &[&str], default_bin_dir: &Path, exe_name: fn(&str) -> String) -> ExitCode {
    let o = match parse_options(args) {
        Ok(o) => o,
        Err(e) => {
            eprintln!("{e}\n\n{USAGE}");
            return ExitCode::from(2);
        }
    };
    let bin_dir = o.bin_dir.clone().unwrap_or_else(|| default_bin_dir.to_path_buf());
    let bin_dir = std::path::absolute(&bin_dir).unwrap_or(bin_dir);

    let mut servers: Vec<(String, PathBuf)> = Vec::new();
    for b in binaries {
        let program = bin_dir.join(exe_name(b));
        if o.remove || program.is_file() {
            servers.push((b.to_string(), program));
        } else {
            println!("Skipping {b}: it isn't in {} (build or install it, then run this again).", bin_dir.display());
        }
    }
    if servers.is_empty() {
        eprintln!(
            "Couldn't find the wetwaresuite programs in {}. Run hub from the folder that has them, \
             or point at it with --bin-dir FOLDER.",
            bin_dir.display()
        );
        return ExitCode::FAILURE;
    }

    let targets: Vec<Target> = match &o.config {
        Some(file) => vec![Target { app: "MCP config", path: file.clone(), create: true, typed: false }],
        None => targets()
            .into_iter()
            .filter(|t| (!o.desktop && !o.code) || (o.desktop && t.app == "Claude Desktop") || (o.code && t.app == "Claude Code"))
            .collect(),
    };

    let mut found_any = false;
    let mut failed = false;
    for t in &targets {
        let exists = t.path.is_file();
        let folder_exists = t.path.parent().is_some_and(Path::is_dir);
        if !exists && !(t.create && folder_exists) {
            let hint = if t.app == "Claude Code" { " (if it's installed, start it once and try again)" } else { "" };
            println!("{}: not found (looked for {}){hint}.", t.app, t.path.display());
            continue;
        }
        found_any = true;
        let text = if exists {
            match std::fs::read_to_string(&t.path) {
                Ok(s) => Some(s),
                Err(e) => {
                    println!("{}: couldn't read {}: {e}. Left it alone.", t.app, t.path.display());
                    failed = true;
                    continue;
                }
            }
        } else {
            None
        };
        let change = match apply(text.as_deref(), &servers, t.typed, o.remove) {
            Ok(c) => c,
            Err(e) => {
                println!("{}: {} isn't valid JSON ({e}). Left it alone; fix or delete it and run this again.", t.app, t.path.display());
                failed = true;
                continue;
            }
        };
        let Some(new_text) = change.text else {
            if o.remove {
                println!("{}: nothing to remove, wetwaresuite isn't in {}.", t.app, t.path.display());
            } else {
                println!("{}: already connected ({}), nothing to change.", t.app, list(&change.unchanged));
            }
            continue;
        };
        let mut what = Vec::new();
        let mut done = Vec::new();
        if !change.added.is_empty() {
            what.push(format!("add {}", list(&change.added)));
            done.push(format!("added {}", list(&change.added)));
        }
        if !change.updated.is_empty() {
            what.push(format!("update {} to the programs in {}", list(&change.updated), bin_dir.display()));
            done.push(format!("updated {} to the programs in {}", list(&change.updated), bin_dir.display()));
        }
        if !change.removed.is_empty() {
            what.push(format!("remove {}", list(&change.removed)));
            done.push(format!("removed {}", list(&change.removed)));
        }
        if o.dry_run {
            println!("{}: would {} ({}). Dry run, so nothing was written.", t.app, what.join(" and "), t.path.display());
            continue;
        }
        if exists {
            let backup = backup_path(&t.path);
            if let Err(e) = std::fs::copy(&t.path, &backup) {
                println!("{}: couldn't back up {} ({e}), so didn't change it.", t.app, t.path.display());
                failed = true;
                continue;
            }
            println!("{}: saved a backup as {}.", t.app, backup.display());
        }
        if let Err(e) = write_atomically(&t.path, &new_text) {
            println!("{}: couldn't write {}: {e}.", t.app, t.path.display());
            failed = true;
            continue;
        }
        println!("{}: {} ({}).", t.app, done.join(" and "), t.path.display());
        println!("  Quit and reopen {} to {} the tools.", if t.app == "MCP config" { "your MCP client" } else { t.app }, if o.remove { "drop" } else { "load" });
    }
    if !found_any {
        println!(
            "Didn't find Claude Desktop or Claude Code here. Install one, or use `hub mcp-config` to print the settings \
             and paste them into any MCP client yourself. Everything in wetwaresuite also works without any AI."
        );
    }
    if failed {
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn servers() -> Vec<(String, PathBuf)> {
        vec![("wetwaresuite".into(), PathBuf::from("/opt/ww/wetwaresuite"))]
    }

    #[test]
    fn adds_to_a_missing_file() {
        let c = apply(None, &servers(), false, false).unwrap();
        assert_eq!(c.added, vec!["wetwaresuite"]);
        let text = c.text.unwrap();
        assert_eq!(
            text,
            "{\n  \"mcpServers\": {\n    \"wetwaresuite\": {\n      \"command\": \"/opt/ww/wetwaresuite\",\n      \"args\": [\n        \"mcp\"\n      ]\n    }\n  }\n}\n"
        );
    }

    #[test]
    fn merges_keeps_other_settings_and_is_idempotent() {
        let original = "{\r\n  \"theme\": \"dark\",\r\n  \"mcpServers\": {\r\n    \"other\": {\"command\": \"x\"}\r\n  },\r\n  \"n\": 1.0\r\n}\r\n";
        let first = apply(Some(original), &servers(), true, false).unwrap();
        let text = first.text.unwrap();
        assert!(text.contains("\"theme\": \"dark\""));
        assert!(text.contains("\"other\": {"));
        assert!(text.contains("\"type\": \"stdio\""));
        assert!(text.contains("\"n\": 1.0"));
        assert!(text.contains("\r\n"));
        assert!(text.find("\"other\"").unwrap() < text.find("\"wetwaresuite\"").unwrap());
        let second = apply(Some(&text), &servers(), true, false).unwrap();
        assert_eq!(second.text, None);
        assert_eq!(second.unchanged, vec!["wetwaresuite"]);
    }

    #[test]
    fn updates_a_moved_binary() {
        let text = apply(None, &servers(), false, false).unwrap().text.unwrap();
        let moved = vec![("wetwaresuite".to_string(), PathBuf::from("/new/wetwaresuite"))];
        let c = apply(Some(&text), &moved, false, false).unwrap();
        assert_eq!(c.updated, vec!["wetwaresuite"]);
        assert!(c.text.unwrap().contains("/new/wetwaresuite"));
    }

    #[test]
    fn removes_only_ours() {
        let original = r#"{"mcpServers": {"other": {"command": "x"}, "wetwaresuite": {"command": "y"}}}"#;
        let c = apply(Some(original), &servers(), false, true).unwrap();
        assert_eq!(c.removed, vec!["wetwaresuite"]);
        let text = c.text.unwrap();
        assert!(text.contains("\"other\"") && !text.contains("wetwaresuite"));
        assert_eq!(apply(Some(&text), &servers(), false, true).unwrap().text, None);
        assert_eq!(apply(Some("{}"), &servers(), false, true).unwrap().text, None);
    }

    #[test]
    fn refuses_broken_files() {
        assert!(apply(Some("{ not json"), &servers(), false, false).is_err());
        assert!(apply(Some("[1]"), &servers(), false, false).is_err());
        assert!(apply(Some(r#"{"mcpServers": 3}"#), &servers(), false, false).is_err());
    }

    #[test]
    fn writes_a_real_file_with_backup() {
        let dir = std::env::temp_dir().join(format!("hub-connect-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let bin = dir.join("bin");
        std::fs::create_dir_all(&bin).unwrap();
        let exe = |b: &str| if cfg!(windows) { format!("{b}.exe") } else { b.to_string() };
        std::fs::write(bin.join(exe("wetwaresuite")), "").unwrap();
        let config = dir.join("config.json");
        std::fs::write(&config, "{\"keep\": true}\n").unwrap();
        let args: Vec<String> =
            vec!["--config".into(), config.to_string_lossy().into(), "--bin-dir".into(), bin.to_string_lossy().into()];

        let dry: Vec<String> = args.iter().cloned().chain(["--dry-run".to_string()]).collect();
        assert_eq!(run(&dry, &["wetwaresuite", "wetwaresuite-share"], &bin, exe), ExitCode::SUCCESS);
        assert_eq!(std::fs::read_to_string(&config).unwrap(), "{\"keep\": true}\n");

        assert_eq!(run(&args, &["wetwaresuite", "wetwaresuite-share"], &bin, exe), ExitCode::SUCCESS);
        let text = std::fs::read_to_string(&config).unwrap();
        assert!(text.contains("\"keep\": true") && text.contains("\"wetwaresuite\"") && !text.contains("share"));
        let backups = std::fs::read_dir(&dir).unwrap().filter(|e| e.as_ref().unwrap().file_name().to_string_lossy().contains(".backup-")).count();
        assert_eq!(backups, 1);

        let remove: Vec<String> = args.iter().cloned().chain(["--remove".to_string()]).collect();
        assert_eq!(run(&remove, &["wetwaresuite", "wetwaresuite-share"], &bin, exe), ExitCode::SUCCESS);
        assert!(!std::fs::read_to_string(&config).unwrap().contains("wetwaresuite"));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
