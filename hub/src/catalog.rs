//! The list of tools the hub knows about. Sizes are estimates from the plan
//! until real release packages exist.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    /// Built from this repo today.
    Available,
    /// On the roadmap, not built yet.
    Planned,
}

#[derive(Debug, Clone, Copy)]
pub struct Tool {
    pub id: &'static str,
    pub name: &'static str,
    pub summary: &'static str,
    pub phase: u8,
    pub approx_mb: u32,
    pub status: Status,
    /// Executable that serves MCP with `<binary> mcp`, if the tool has one yet.
    pub mcp_binary: Option<&'static str>,
}

const fn planned(id: &'static str, name: &'static str, summary: &'static str, phase: u8, approx_mb: u32) -> Tool {
    Tool { id, name, summary, phase, approx_mb, status: Status::Planned, mcp_binary: None }
}

pub const TOOLS: &[Tool] = &[
    Tool {
        id: "toolkit",
        name: "toolkit",
        summary: "open anything: inspect, hex, archives, code",
        phase: 1,
        approx_mb: 10,
        status: Status::Available,
        mcp_binary: Some("achclwss-toolkit"),
    },
    planned("photo", "photo", "photo editing and RAW development", 2, 15),
    planned("paint", "paint", "digital painting with pen pressure", 2, 12),
    planned("vector", "vector", "vector design, SVG/AI/EPS", 2, 10),
    planned("cad", "cad", "precision parametric 2D/3D CAD", 3, 45),
    planned("blueprint", "blueprint", "technical drafting and floor plans", 3, 8),
    planned("slicer", "slicer", "3D printing slicer", 3, 20),
    planned("forge", "asset forge", "game assets: meshes, textures, sprites", 3, 25),
    planned("audio", "audio", "multitrack and spectral audio editing", 4, 12),
    planned("cut", "cut", "video editing", 4, 20),
    planned("capture", "capture", "screen recording and screenshots", 4, 5),
    planned("office", "office", "documents, spreadsheets, slides, PDF", 4, 250),
    planned("layout", "layout", "print layout and publishing", 4, 10),
    planned("library", "library", "asset browser for every format", 4, 3),
    planned("automate", "automate", "Python scripting across tools", 4, 20),
];

pub fn find(id: &str) -> Option<&'static Tool> {
    let id = id.to_ascii_lowercase();
    TOOLS.iter().find(|t| t.id == id || t.name == id)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ids_are_unique() {
        for (i, a) in TOOLS.iter().enumerate() {
            for b in &TOOLS[i + 1..] {
                assert_ne!(a.id, b.id);
            }
        }
    }

    #[test]
    fn find_by_id_or_name() {
        assert_eq!(find("TOOLKIT").unwrap().id, "toolkit");
        assert_eq!(find("asset forge").unwrap().id, "forge");
        assert!(find("photoshop").is_none());
    }

    #[test]
    fn available_tools_have_mcp() {
        for t in TOOLS.iter().filter(|t| t.status == Status::Available) {
            assert!(t.mcp_binary.is_some(), "{} has no MCP server", t.id);
        }
    }
}
