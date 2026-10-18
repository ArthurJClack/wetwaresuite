//! hub: the ACHCLWSS install manager. Lists tools, writes Claude's MCP config,
//! and will install tools once release packages are published.

mod catalog;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use catalog::{Status, TOOLS};

const VERSION: &str = env!("CARGO_PKG_VERSION");

fn usage() {
    println!(
        "usage: hub <command>\n\n  \
         list                       show every tool and its status\n  \
         install <tool>             install a tool\n  \
         mcp-config [--bin-dir DIR] print the MCP config that connects Claude to installed tools\n  \
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
            "{} builds from source for now (see README); downloadable packages come with the release pipeline",
            tool.name
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

/// Builds the `mcpServers` block for Claude Desktop / Claude Code.
fn mcp_config(bin_dir: &Path) -> String {
    let entries: Vec<String> = TOOLS
        .iter()
        .filter(|t| t.status == Status::Available)
        .filter_map(|t| t.mcp_binary.map(|b| (t, b)))
        .map(|(t, b)| {
            let command = bin_dir.join(exe_name(b));
            format!(
                "    {}: {{\n      \"command\": {},\n      \"args\": [\"mcp\"]\n    }}",
                json_string(&format!("achclwss-{}", t.id)),
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

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
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
        assert!(config.contains("\"achclwss-toolkit\""));
        assert!(config.contains("\"args\": [\"mcp\"]"));
        assert!(!config.contains("achclwss-photo"));
    }
}
