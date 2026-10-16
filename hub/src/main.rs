//! hub: the wetwaresuite install manager. Lists the apps, installs only the ones
//! you want from a local folder or archive (never from the internet), makes
//! offline editions, and writes Claude's MCP config.

mod bundle;
mod catalog;
mod install;
mod packs;
mod paths;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use catalog::{APPS, TOOLS};

const VERSION: &str = env!("CARGO_PKG_VERSION");

fn usage() {
    println!(
        "usage: hub <command>\n\n  \
         list [--tools]                  show the apps (or every tool), their size and what they do\n  \
         install <app>... [--from SRC]   install apps from a folder or archive, offline; tool names work too\n                                  \
         (`files` always comes along; `all` = every app; `suite` = the all-in-one binary)\n                                  \
         [--to DIR] [--packs a,b|all|none]\n  \
         install --from SRC              install everything in an offline edition\n  \
         uninstall <app>... [--to DIR]   remove apps\n  \
         editions                        list the offline editions (1, 2, 4 ... GB) and what each holds\n  \
         bundle --size GB|auto --out DIR make an offline edition [--packs a,b] [--bin-dir DIR]... [--docs DIR]\n                                  \
         [--cache DIR] [--dry-run]  (may download; installing from it never does)\n  \
         mcp-config [--bin-dir DIR]      print the MCP config that connects Claude to installed apps\n  \
         share [ARGS...]                 run wetwaresuite-share: share or live-sync a folder (`hub share help`)\n  \
         --version\n\n\
         apps install into {}",
        paths::bin_dir().display()
    );
}

fn list(tools: bool) {
    if tools {
        println!("{:<11} {:<12} {:>5}  what it does", "tool", "app", "~MB");
        for t in TOOLS {
            let app = catalog::find_app(t.id).map_or("", |a| a.id);
            println!("{:<11} {:<12} {:>5}  {}", t.id, app, t.approx_mb, t.summary);
        }
        return;
    }
    let dest = paths::bin_dir();
    println!("{:<12} {:>5}  {:<10} what it does", "app", "~MB", "");
    for a in APPS {
        let state = if install::installed_at(&a.binary(), &dest).is_some() { "installed" } else { "" };
        println!("{:<12} {:>5}  {:<10} {}", a.id, a.approx_mb, state, a.summary);
        println!("{:<12} {:>5}  {:<10}   tools: {}", "", "", "", a.tools.join(", "));
    }
    println!("\ninstall only what you need, e.g. `hub install image model --from <release folder>`.");
    println!("`wetwaresuite` (every app in one binary) is for power users and Claude: `hub install suite`.");
}

#[derive(Default)]
struct Flags {
    names: Vec<String>,
    from: Option<PathBuf>,
    to: Option<PathBuf>,
    packs: Option<String>,
    size: Option<String>,
    out: Option<PathBuf>,
    bin_dirs: Vec<PathBuf>,
    docs: Option<PathBuf>,
    cache: Option<PathBuf>,
    dry_run: bool,
    tools: bool,
}

fn parse_flags(args: &[String]) -> Result<Flags, String> {
    let mut f = Flags::default();
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut value = || it.next().cloned().ok_or_else(|| format!("{a} needs a value"));
        match a.as_str() {
            "--from" => f.from = Some(value()?.into()),
            "--to" => f.to = Some(value()?.into()),
            "--packs" => f.packs = Some(value()?),
            "--size" => f.size = Some(value()?),
            "--out" => f.out = Some(value()?.into()),
            "--bin-dir" => f.bin_dirs.push(value()?.into()),
            "--docs" => f.docs = Some(value()?.into()),
            "--cache" => f.cache = Some(value()?.into()),
            "--dry-run" => f.dry_run = true,
            "--tools" => f.tools = true,
            s if s.starts_with("--") => return Err(format!("unknown option {s}")),
            s => f.names.push(s.to_string()),
        }
    }
    Ok(f)
}

fn split_list(s: &str) -> Vec<String> {
    s.split(',').map(str::trim).filter(|x| !x.is_empty()).map(String::from).collect()
}

fn install_cmd(f: &Flags) -> Result<(), String> {
    let src = match &f.from {
        Some(from) => install::open_source(from)?,
        None => install::default_source()?,
    };
    let dest = f.to.clone().unwrap_or_else(paths::bin_dir);
    let everything = f.names.is_empty();
    let (apps, suite) = if everything { (APPS.iter().collect(), true) } else { install::resolve(&f.names)? };
    let mut installed = Vec::new();
    let mut missing = Vec::new();
    for app in &apps {
        match install::install_binary(&src, &app.binary(), &dest) {
            Ok(p) => {
                println!("installed {:<12} -> {}", app.id, p.display());
                installed.push(app.id);
            }
            Err(e) if everything || app.always => println!("skipped {:<12}   {e}", app.id),
            Err(e) => missing.push(format!("{}: {e}", app.id)),
        }
    }
    if suite {
        match install::install_binary(&src, catalog::SUITE, &dest) {
            Ok(p) => println!("installed {:<12} -> {}", catalog::SUITE, p.display()),
            Err(e) if everything => println!("skipped {:<12}   {e}", catalog::SUITE),
            Err(e) => missing.push(e),
        }
    }

    let available = src.packs();
    let wanted: Vec<String> = match f.packs.as_deref() {
        Some("all") => available.clone(),
        Some("none") => Vec::new(),
        Some(list) => split_list(list),
        None if everything => available.clone(),
        None => {
            // packs the chosen apps use, plus ones for any app (samples)
            let all = packs::all();
            available
                .iter()
                .filter(|id| packs::find(&all, id).is_some_and(|p| p.app.is_empty() || installed.contains(&p.app)))
                .cloned()
                .collect()
        }
    };
    for id in &wanted {
        let bytes = install::install_pack(&src, id, &paths::packs_dir())?;
        println!("installed pack {id} ({:.1} MB) -> {}", bytes as f64 / 1e6, paths::packs_dir().join(id).display());
    }
    let docs = src.root.join("docs");
    if docs.is_dir() && src.is_bundle() {
        let target = paths::data_root().join("docs");
        paths::copy_tree(&docs, &target).map_err(|e| format!("can't copy the tutorials: {e}"))?;
        println!("installed tutorials -> {}", target.display());
    }
    if !missing.is_empty() {
        return Err(format!("not found in {}:\n  {}", src.root.display(), missing.join("\n  ")));
    }
    if !installed.is_empty() {
        println!("\nadd {} to your PATH to run them by name, or run them from there.", dest.display());
    }
    Ok(())
}

fn uninstall_cmd(f: &Flags) -> Result<(), String> {
    let dest = f.to.clone().unwrap_or_else(paths::bin_dir);
    let (apps, suite) = install::resolve(&f.names)?;
    let mut binaries: Vec<String> = apps
        .iter()
        .filter(|a| !a.always || f.names.iter().any(|n| n == a.id))
        .map(|a| a.binary())
        .collect();
    if suite {
        binaries.push(catalog::SUITE.into());
    }
    for b in binaries {
        let p = dest.join(paths::exe_name(&b));
        if p.is_file() {
            std::fs::remove_file(&p).map_err(|e| format!("can't remove {}: {e}", p.display()))?;
            println!("removed {}", p.display());
        } else {
            println!("{b} isn't installed in {}", dest.display());
        }
    }
    Ok(())
}

fn editions() {
    let all = packs::all();
    println!("offline editions: everything works without internet; bigger ones add more free content\n");
    for gb in packs::edition_sizes(&all) {
        let set = packs::plan(&all, packs::budget_mb(gb)).unwrap_or_default();
        let optional: Vec<&str> = set.iter().filter(|p| !p.required).map(|p| p.id).collect();
        println!(
            "{:>4} GB  ~{:>6.1} GB used  apps + tutorials{}{}",
            gb,
            packs::needed_mb(&set) as f64 / 1000.0,
            if optional.is_empty() { "" } else { " + " },
            optional.join(", ")
        );
    }
    println!("\npacks:");
    for p in all.iter().filter(|p| !p.required) {
        println!("  {:<16} {:>7} MB  {}  [{}]", p.id, p.approx_mb, p.summary, p.license);
    }
    println!("\nmake one with `hub bundle --size 32 --out wetwaresuite-32gb` (or --size auto).");
}

fn bundle_cmd(f: &Flags) -> Result<(), String> {
    let size = match f.size.as_deref() {
        None | Some("auto") => bundle::Size::Auto,
        Some(n) => bundle::Size::Gb(
            n.trim_end_matches(['G', 'g', 'B', 'b'])
                .parse()
                .map_err(|_| format!("--size takes a number of GB or auto, not {n}"))?,
        ),
    };
    let out = f.out.clone().ok_or("bundle needs --out <folder>")?;
    bundle::bundle(&bundle::Options {
        size,
        out,
        packs: f.packs.as_deref().map(split_list),
        bin_dirs: f.bin_dirs.clone(),
        docs: f.docs.clone(),
        cache: f.cache.clone(),
        dry_run: f.dry_run,
    })
}

/// Escapes a string for a JSON string literal.
fn json_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// The MCP binaries to list for `bin_dir`: the all-in-one `wetwaresuite` if
/// it's there (it serves every tool), else each app found there; with nothing
/// installed yet, the suite. `share` is always its own entry.
fn mcp_binaries(bin_dir: &Path) -> Vec<String> {
    let has = |b: &str| bin_dir.join(paths::exe_name(b)).is_file();
    let mut out: Vec<String> = Vec::new();
    if !has(catalog::SUITE) {
        out = APPS.iter().map(|a| a.binary()).filter(|b| b != catalog::SHARE && has(b)).collect();
    }
    if out.is_empty() {
        out.push(catalog::SUITE.into());
    }
    out.push(catalog::SHARE.into());
    out
}

/// Builds the `mcpServers` block for Claude Desktop / Claude Code.
fn mcp_config(bin_dir: &Path) -> String {
    let entries: Vec<String> = mcp_binaries(bin_dir)
        .iter()
        .map(|b| {
            let command = bin_dir.join(paths::exe_name(b));
            format!(
                "    {}: {{\n      \"command\": {},\n      \"args\": [\"mcp\"]\n    }}",
                json_string(b),
                json_string(&command.to_string_lossy())
            )
        })
        .collect();
    format!("{{\n  \"mcpServers\": {{\n{}\n  }}\n}}", entries.join(",\n"))
}

fn default_mcp_dir() -> PathBuf {
    let installed = paths::bin_dir();
    if APPS.iter().any(|a| installed.join(paths::exe_name(&a.binary())).is_file()) {
        installed
    } else {
        paths::self_dir()
    }
}

/// Runs `wetwaresuite-share` from the install folder, next to the hub, or PATH.
fn share(args: &[String]) -> ExitCode {
    let program = install::installed_at(catalog::SHARE, &paths::bin_dir())
        .unwrap_or_else(|| PathBuf::from(paths::exe_name(catalog::SHARE)));
    match std::process::Command::new(&program).args(args).status() {
        Ok(status) => match status.code() {
            Some(0) => ExitCode::SUCCESS,
            Some(c) => ExitCode::from(u8::try_from(c).unwrap_or(1)),
            None => ExitCode::FAILURE,
        },
        Err(e) => {
            eprintln!(
                "can't run {}: {e}. Install it with `hub install share --from <folder>`, or build it with \
                 `cd share && cargo build --release`",
                program.display()
            );
            ExitCode::FAILURE
        }
    }
}

fn finish(r: Result<(), String>) -> ExitCode {
    match r {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}

/// `hub list | head` should end quietly, not panic: restore the default
/// SIGPIPE behaviour that Rust turns off (libc is already linked by std).
#[cfg(unix)]
fn quiet_broken_pipe() {
    extern "C" {
        fn signal(signum: i32, handler: usize) -> usize;
    }
    // SAFETY: SIGPIPE (13) back to SIG_DFL (0) before any other thread exists.
    unsafe {
        signal(13, 0);
    }
}

#[cfg(not(unix))]
fn quiet_broken_pipe() {}

fn main() -> ExitCode {
    quiet_broken_pipe();
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(cmd) = args.first().map(String::as_str) else {
        usage();
        return ExitCode::from(2);
    };
    if cmd == "share" {
        return share(&args[1..]);
    }
    let flags = match parse_flags(&args[1..]) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::from(2);
        }
    };
    match cmd {
        "list" => list(flags.tools),
        "install" if flags.names.is_empty() && flags.from.is_none() => {
            eprintln!("say which apps to install, e.g. `hub install image --from <folder>`; `hub list` shows them");
            return ExitCode::from(2);
        }
        "install" => return finish(install_cmd(&flags)),
        "uninstall" if !flags.names.is_empty() => return finish(uninstall_cmd(&flags)),
        "editions" => editions(),
        "bundle" => return finish(bundle_cmd(&flags)),
        "mcp-config" => println!("{}", mcp_config(&flags.bin_dirs.first().cloned().unwrap_or_else(default_mcp_dir))),
        "--version" => println!("hub {VERSION}"),
        _ => {
            usage();
            return ExitCode::from(2);
        }
    }
    ExitCode::SUCCESS
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn json_escaping() {
        assert_eq!(json_string(r#"C:\Program Files\"x""#), r#""C:\\Program Files\\\"x\"""#);
        assert_eq!(json_string("a\nb"), "\"a\\nb\"");
    }

    #[test]
    fn mcp_config_with_nothing_installed_lists_the_suite() {
        let config = mcp_config(Path::new("bin"));
        assert!(config.contains("\"wetwaresuite\": {"));
        assert!(config.contains("\"wetwaresuite-share\": {"));
        assert!(config.contains("\"args\": [\"mcp\"]"));
        assert_eq!(config.matches("\"command\"").count(), 2);
    }

    #[test]
    fn mcp_config_lists_each_installed_app() {
        let dir = std::env::temp_dir().join(format!("hub-mcp-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for b in ["wetwaresuite-image", "wetwaresuite-model"] {
            std::fs::write(dir.join(paths::exe_name(b)), b"x").unwrap();
        }
        let config = mcp_config(&dir);
        assert!(config.contains("\"wetwaresuite-image\": {"));
        assert!(config.contains("\"wetwaresuite-model\": {"));
        assert!(!config.contains("\"wetwaresuite\": {"));
        assert_eq!(config.matches("\"command\"").count(), 3);
        // the all-in-one binary covers everything on its own
        std::fs::write(dir.join(paths::exe_name("wetwaresuite")), b"x").unwrap();
        assert_eq!(mcp_config(&dir).matches("\"command\"").count(), 2);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn flags_parse() {
        let args: Vec<String> =
            ["image", "--from", "rel.zip", "model", "--dry-run"].iter().map(|s| s.to_string()).collect();
        let f = parse_flags(&args).unwrap();
        assert_eq!(f.names, ["image", "model"]);
        assert_eq!(f.from.unwrap(), PathBuf::from("rel.zip"));
        assert!(f.dry_run);
        assert!(parse_flags(&["--from".to_string()]).is_err());
        assert!(parse_flags(&["--bogus".to_string()]).is_err());
        assert_eq!(split_list("a, b,,c"), ["a", "b", "c"]);
    }
}
