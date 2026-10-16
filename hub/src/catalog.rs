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

/// The `achclwss` binary serves every built tool over MCP with `achclwss mcp`.
pub const SUITE: &str = "achclwss";

const fn available(id: &'static str, name: &'static str, summary: &'static str, phase: u8, approx_mb: u32) -> Tool {
    Tool { id, name, summary, phase, approx_mb, status: Status::Available, mcp_binary: Some(SUITE) }
}

pub const TOOLS: &[Tool] = &[
    Tool {
        id: "toolkit",
        name: "toolkit",
        summary: "open anything: inspect, hex, archives, code",
        phase: 1,
        approx_mb: 10,
        status: Status::Available,
        mcp_binary: Some(SUITE),
    },
    available("photo", "photo", "photo editing: adjust, crop, resize, effects, batch", 2, 4),
    available("paint", "paint", "digital painting with pen pressure and layers", 2, 4),
    available("convert", "convert", "change any file's type: images, audio, video, docs", 2, 2),
    planned("vector", "vector", "vector design, SVG/AI/EPS", 2, 10),
    available("cad", "cad", "precision parametric CAD: feature tree, expressions, holes, patterns, STL/3MF", 3, 3),
    available("blueprint", "blueprint", "technical drafting and floor plans: DXF, PDF to scale", 3, 2),
    available("slicer", "slicer", "3D printing slicer: STL/3MF to G-code", 3, 20),
    available("forge", "asset forge", "game assets: meshes, textures, sprites", 3, 25),
    planned("audio", "audio", "multitrack and spectral audio editing", 4, 12),
    planned("cut", "cut", "video editing", 4, 20),
    planned("capture", "capture", "screen recording and screenshots", 4, 5),
    planned("office", "office", "documents, spreadsheets, slides, PDF", 4, 250),
    planned("layout", "layout", "print layout and publishing", 4, 10),
    available("library", "library", "asset browser for every format", 4, 3),
    available("automate", "automate", "Python scripting across tools", 4, 20),
    planned("sculpt", "sculpt", "digital sculpting and retopology", 5, 15),
    planned("render", "render", "3D scenes and path-traced renders", 5, 20),
    planned("animate", "animate", "2D and 3D animation", 5, 15),
    planned("levels", "levels", "game level and tilemap editor", 5, 10),
    planned("beats", "beats", "music making: sequencer, synths, samples", 5, 15),
    planned("circuit", "circuit", "schematics and PCB layout", 6, 25),
    planned("flash", "flash", "flash firmware to Arduino, ESP32 and STM32", 6, 5),
    planned("maps", "maps", "offline maps and GIS", 6, 15),
    planned("board", "board", "whiteboard and diagrams", 6, 5),
    planned("notes", "notes", "markdown notes and knowledge base", 6, 5),
    planned("type", "type", "font design", 6, 8),
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
