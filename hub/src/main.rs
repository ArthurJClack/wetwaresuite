//! hub: the wetwaresuite install manager. Lists tools, installs them from a
//! local folder or archive, and connects them to Claude (optional). Never
//! touches the network.

mod catalog;
mod connect;
mod json;
mod local_install;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use catalog::{Status, TOOLS};

const VERSION: &str = env!("CARGO_PKG_VERSION");

fn usage() {
    println!(
        "usage: hub <command>\n\n  \
         list                       show every tool and its status\n  \
         install <tool> --from P    install a tool from a folder, archive or program file on this computer\n  \
         connect [--dry-run] [--remove]  add the tools to Claude Desktop / Claude Code (`hub connect --help`)\n  \
         mcp-config [--bin-dir DIR] print the MCP config that connects Claude to installed tools\n  \
         share [ARGS...]            run wetwaresuite-share: share or live-sync a folder (`hub share help`)\n  \
         --version"
    );
}

fn list() {
    println!("{:<12} {:<10} {:>6}  what it does", "tool", "status", "~MB");
    for t in TOOLS {
        let status = match t.status {
            Status::Available => "available".to_string(),
            Status::Planned => format!("phase {}", t.phase),
        };
        println!("{:<12} {:<10} {:>6}  {}", t.id, status, t.approx_mb, t.summary);
    }
}

fn install(id: &str) -> ExitCode {
    let Some(tool) = catalog::find(id) else {
        eprintln!("unknown tool '{id}', run `hub list` to see them");
        return ExitCode::FAILURE;
    };
    match tool.status {
        Status::Planned => eprintln!("{} is planned for phase {} and isn't built yet", tool.name, tool.phase),
        Status::Available => eprintln!(
            "{} builds from source (see README). If you have a build or a release archive on this computer, \
             run `hub install {} --from <folder or archive>`",
            tool.name, tool.id
        ),
    }
    ExitCode::FAILURE
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

fn exe_name(binary: &str) -> String {
    if cfg!(windows) {
        format!("{binary}.exe")
    } else {
        binary.to_string()
    }
}

/// Builds the `mcpServers` block for Claude Desktop / Claude Code: one entry
/// per MCP binary, each serving every available tool it contains.
fn mcp_binaries() -> Vec<&'static str> {
    let mut binaries: Vec<&str> = Vec::new();
    for t in TOOLS.iter().filter(|t| t.status == Status::Available) {
        if let Some(b) = t.mcp_binary {
            if !binaries.contains(&b) {
                binaries.push(b);
            }
        }
    }
    binaries
}

fn mcp_config(bin_dir: &Path) -> String {
    let binaries = mcp_binaries();
    let entries: Vec<String> = binaries
        .iter()
        .map(|b| {
            let command = bin_dir.join(exe_name(b));
            format!(
                "    {}: {{\n      \"command\": {},\n      \"args\": [\"mcp\"]\n    }}",
                json_string(b),
                json_string(&command.to_string_lossy())
            )
        })
        .collect();
    format!("{{\n  \"mcpServers\": {{\n{}\n  }}\n}}", entries.join(",\n"))
}

fn default_bin_dir() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// Runs `wetwaresuite-share` from next to the hub (or PATH) with the given args.
fn share(args: &[String]) -> ExitCode {
    let local = default_bin_dir().join(exe_name(catalog::SHARE));
    let program = if local.is_file() { local } else { PathBuf::from(exe_name(catalog::SHARE)) };
    match std::process::Command::new(&program).args(args).status() {
        Ok(status) => match status.code() {
            Some(0) => ExitCode::SUCCESS,
            Some(c) => ExitCode::from(u8::try_from(c).unwrap_or(1)),
            None => ExitCode::FAILURE,
        },
        Err(e) => {
            eprintln!(
                "can't run {}: {e}. Build it with `cd share && cargo build --release` and put it next to hub or on PATH",
                program.display()
            );
            ExitCode::FAILURE
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("share") => return share(&args[1..]),
        Some("connect") if args.iter().any(|a| a == "--help" || a == "-h") => {
            println!("{}", connect::USAGE);
            return ExitCode::SUCCESS;
        }
        Some("connect") => return connect::run(&args[1..], &mcp_binaries(), &default_bin_dir(), exe_name),
        Some("install") if args.len() > 2 => {
            let Some(tool) = catalog::find(&args[1]).filter(|t| t.status == Status::Available) else {
                eprintln!("unknown or unbuilt tool '{}', run `hub list` to see them", args[1]);
                return ExitCode::FAILURE;
            };
            let binary = tool.mcp_binary.unwrap_or(catalog::SUITE);
            return local_install::run(tool.id, binary, &args[2..], &default_bin_dir(), exe_name);
        }
        _ => {}
    }
    match args.iter().map(String::as_str).collect::<Vec<_>>().as_slice() {
        ["list"] => list(),
        ["install", tool] => return install(tool),
        ["mcp-config"] => println!("{}", mcp_config(&default_bin_dir())),
        ["mcp-config", "--bin-dir", dir] => println!("{}", mcp_config(Path::new(dir))),
        ["--version"] => println!("hub {VERSION}"),
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
    fn mcp_config_lists_available_tools() {
        let config = mcp_config(Path::new("bin"));
        assert!(config.contains("\"wetwaresuite\": {"));
        assert!(config.contains("\"wetwaresuite-share\": {"));
        assert!(config.contains("\"args\": [\"mcp\"]"));
        // one entry per MCP binary: the suite, and the standalone share tool
        assert_eq!(config.matches("\"command\"").count(), 2);
    }
}
