#include "apps.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

namespace wetwaresuite::apps {

const std::vector<App>& all() {
    static const std::vector<App> table = {
        {"image", "edit photos, paint pictures and make textures", {"photo", "paint"}},
        {"draw", "vector art and logos, fonts, whiteboards and diagrams", {"vector", "type", "board"}},
        {"model", "3D modeling and sculpting, rendered pictures, animation", {"sculpt", "render", "animate"}},
        {"game", "game assets (meshes, sprites, textures) and game levels", {"forge", "levels"}},
        {"cad", "precise parts, technical drawings and floor plans, 3D printing", {"cad", "blueprint", "slicer"}},
        {"electronics", "circuit diagrams, circuit boards and flashing microcontrollers", {"circuit", "flash"}},
        {"sound", "record and clean up audio, make music", {"audio", "beats"}},
        {"video", "edit videos, take screenshots and record the screen", {"cut", "capture"}},
        {"office", "documents, spreadsheets, slides, PDFs, print layouts and notes", {"office", "layout", "notes"}},
        {"maps", "offline maps, GPS tracks and map data", {"maps"}},
        {"files", "open, convert, find and batch-process any file (always installed)",
         {"toolkit", "convert", "library", "automate"}, true},
        {"share", "share and live-sync folders with other devices", {"share"}},
    };
    return table;
}

const App* find(const std::string& id) {
    for (const auto& a : all())
        if (a.id == id) return &a;
    return nullptr;
}

const App* appOfTool(const std::string& tool) {
    for (const auto& a : all())
        if (std::find(a.tools.begin(), a.tools.end(), tool) != a.tools.end()) return &a;
    return nullptr;
}

std::string binaryName(const std::string& appId) { return "wetwaresuite-" + appId; }

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// extension -> app and the command that opens it there
const std::map<std::string, OpenRoute>& extensionTable() {
    static const std::map<std::string, OpenRoute> table = {
        // image
        {".png", {"image", "photo.open"}}, {".jpg", {"image", "photo.open"}}, {".jpeg", {"image", "photo.open"}},
        {".bmp", {"image", "photo.open"}}, {".tga", {"image", "photo.open"}}, {".ppm", {"image", "photo.open"}},
        {".pgm", {"image", "photo.open"}}, {".ora", {"image", "photo.open"}}, {".gif", {"image", "photo.open"}},
        {".webp", {"image", ""}}, {".psd", {"image", ""}},
        // draw
        {".svg", {"draw", "vector.open"}}, {".ttf", {"draw", "type.open"}}, {".otf", {"draw", "type.open"}},
        {".woff", {"draw", "type.open"}}, {".ufo", {"draw", "type.open"}}, {".excalidraw", {"draw", "board.open"}},
        {".drawio", {"draw", "board.open"}}, {".mmd", {"draw", ""}}, {".board.json", {"draw", "board.open"}},
        {".ai", {"draw", ""}}, {".eps", {"draw", ""}},
        // model
        {".obj", {"model", "sculpt.open"}}, {".ply", {"model", "sculpt.open"}}, {".off", {"model", "sculpt.open"}},
        {".glb", {"model", "sculpt.open"}}, {".gltf", {"model", "sculpt.open"}}, {".fbx", {"model", ""}},
        {".scene.json", {"model", "render.open"}}, {".anim.json", {"model", "animate.open"}},
        // game
        {".tmx", {"game", "levels.open"}}, {".tmj", {"game", "levels.open"}}, {".ldtk", {"game", "levels.open"}},
        // cad
        {".stl", {"cad", "slicer.load"}}, {".3mf", {"cad", "slicer.load"}}, {".dxf", {"cad", "blueprint.open"}},
        {".gcode", {"cad", ""}}, {".step", {"cad", ""}}, {".stp", {"cad", ""}}, {".achcad", {"cad", "cad.open"}},
        // electronics
        {".hex", {"electronics", ""}}, {".elf", {"electronics", ""}}, {".ino", {"electronics", ""}},
        {".gbr", {"electronics", ""}}, {".drl", {"electronics", ""}}, {".kicad_sch", {"electronics", ""}},
        {".circuit.json", {"electronics", "circuit.open"}},
        // sound
        {".wav", {"sound", "audio.open"}}, {".mp3", {"sound", "audio.open"}}, {".flac", {"sound", "audio.open"}},
        {".ogg", {"sound", "audio.open"}}, {".opus", {"sound", "audio.open"}}, {".m4a", {"sound", "audio.open"}},
        {".aac", {"sound", "audio.open"}}, {".aiff", {"sound", "audio.open"}}, {".aif", {"sound", "audio.open"}},
        {".mid", {"sound", ""}}, {".midi", {"sound", ""}}, {".beats.json", {"sound", "beats.open"}},
        // video
        {".mp4", {"video", ""}}, {".mov", {"video", ""}}, {".mkv", {"video", ""}}, {".webm", {"video", ""}},
        {".avi", {"video", ""}}, {".srt", {"video", ""}}, {".achcut", {"video", "cut.open"}},
        // office
        {".docx", {"office", "office.doc_open"}}, {".odt", {"office", "office.doc_open"}}, {".md", {"office", "office.doc_open"}},
        {".txt", {"office", "office.doc_open"}}, {".rtf", {"office", "office.doc_open"}}, {".html", {"office", "office.doc_open"}},
        {".xlsx", {"office", "office.sheet_open"}}, {".ods", {"office", "office.sheet_open"}}, {".csv", {"office", "office.sheet_open"}},
        {".pptx", {"office", "office.deck_open"}}, {".odp", {"office", "office.deck_open"}}, {".pdf", {"office", ""}},
        {".achlay", {"office", "layout.open"}},
        // maps
        {".geojson", {"maps", "maps.open"}}, {".kml", {"maps", "maps.open"}}, {".gpx", {"maps", "maps.open"}},
        {".shp", {"maps", "maps.open"}}, {".tif", {"maps", ""}}, {".tiff", {"maps", ""}}, {".pmtiles", {"maps", ""}},
        {".mbtiles", {"maps", ""}}, {".achmap", {"maps", "maps.open"}},
    };
    return table;
}

}  // namespace

OpenRoute routeForFile(const std::string& path) {
    std::string name = lower(std::filesystem::path(path).filename().string());
    // try the longest dotted suffix first: "song.beats.json" -> ".beats.json", then ".json"
    for (std::size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', dot + 1)) {
        auto it = extensionTable().find(name.substr(dot));
        if (it != extensionTable().end()) return it->second;
    }
    return {"files", "toolkit.inspect"};
}

const std::vector<SendRoute>& sendRoutes() {
    static const std::vector<SendRoute> table = {
        // pictures out of image
        {"image", "model", "photo.save", "path", ".png", "texture", "",
         "use it as a texture: render.material doc=<scene> name=<material> texture=FILE, or emboss it: sculpt.displace doc=<sculpt> image=FILE"},
        {"image", "game", "photo.save", "path", ".png", "texture", "",
         "make it tile: forge.seamless input=FILE output=<tile.png>; bumps: forge.normal_map input=FILE output=<normal.png>"},
        {"image", "video", "photo.save", "path", ".png", "still", "", "add it to a timeline: cut.add_image doc=<cut> path=FILE"},
        {"image", "office", "photo.save", "path", ".png", "picture", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"image", "draw", "photo.save", "path", ".png", "picture", "", "use it as a reference (draw files are vector; the picture is at FILE)"},
        // draw
        {"vector", "office", "vector.save", "path", ".svg", "artwork", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"vector", "image", "vector.save", "path", ".png", "picture", "photo.open", "opened as a picture to paint on"},
        {"vector", "video", "vector.save", "path", ".png", "still", "", "add it to a timeline: cut.add_image doc=<cut> path=FILE"},
        {"board", "office", "board.export", "path", ".svg", "diagram", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"board", "image", "board.export", "path", ".png", "picture", "photo.open", "opened as a picture"},
        {"font", "office", "type.export", "path", ".ttf", "font", "", "use it in a layout: layout.font_add doc=<layout> path=FILE"},
        // model
        {"sculpt", "game", "sculpt.export", "path", ".glb", "mesh", "forge.mesh_open", "opened as a game mesh: add LODs, fix scale, save"},
        {"sculpt", "cad", "sculpt.export", "path", ".stl", "mesh", "slicer.load", "loaded on the printer bed: slicer.export_gcode doc=<print> path=<part.gcode>; or put it in a part: cad.import doc=<cad> path=FILE"},
        {"sculpt", "image", "sculpt.export", "path", ".png", "picture", "photo.open", "a picture of the model"},
        {"scene", "image", "render.render", "output", ".png", "picture", "photo.open", "the rendered picture"},
        {"scene", "video", "render.render", "output", ".png", "still", "", "add it to a timeline: cut.add_image doc=<cut> path=FILE"},
        {"scene", "office", "render.render", "output", ".png", "picture", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"animation", "game", "animate.export_sheet", "output", ".png", "sprite sheet", "",
         "a sprite sheet; cut it into frames with forge.sprite_split input=FILE output_dir=<folder>"},
        {"animation", "video", "animate.export_gif", "output", ".gif", "clip", "", "add it to a timeline: cut.add_clip doc=<cut> path=FILE"},
        // game
        {"mesh", "model", "forge.mesh_save", "path", ".glb", "mesh", "sculpt.open", "opened for sculpting; or render it with render.model path=FILE"},
        {"mesh", "cad", "forge.mesh_save", "path", ".stl", "mesh", "slicer.load", "loaded on the printer bed: slicer.export_gcode doc=<print> path=<part.gcode>; or put it in a part: cad.import doc=<cad> path=FILE"},
        {"mesh", "image", "forge.mesh_save", "path", ".png", "picture", "photo.open", "a picture of the mesh"},
        {"level", "image", "levels.save", "path", ".png", "picture", "photo.open", "a picture of the level"},
        // cad
        {"cad", "model", "cad.save", "path", ".stl", "mesh", "sculpt.open", "opened for sculpting; or render it with render.model path=FILE"},
        {"cad", "game", "cad.save", "path", ".glb", "mesh", "forge.mesh_open", "opened as a game mesh"},
        {"cad", "image", "cad.save", "path", ".png", "picture", "photo.open", "a picture of the part"},
        {"blueprint", "draw", "blueprint.export", "path", ".svg", "drawing", "vector.open", "opened as vector art"},
        {"blueprint", "office", "blueprint.export", "path", ".svg", "drawing", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        // electronics
        {"circuit", "office", "circuit.export", "path", ".svg", "schematic", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"circuit", "draw", "circuit.export", "path", ".svg", "schematic", "vector.open", "opened as vector art"},
        // sound
        {"audio", "video", "audio.save", "path", ".wav", "audio", "", "add it to a timeline: cut.add_audio doc=<cut> path=FILE"},
        {"audio project", "video", "audio.export", "path", ".wav", "audio", "", "add it to a timeline: cut.add_audio doc=<cut> path=FILE", "project"},
        {"song", "video", "beats.render", "path", ".wav", "music", "", "add it to a timeline: cut.add_audio doc=<cut> path=FILE", "song"},
        {"song", "game", "beats.render", "path", ".wav", "music", "", "game music: ship FILE with your level", "song"},
        // office and maps
        {"layout", "office", "layout.export", "path", ".pdf", "document", "", "the print-ready PDF (office.pdf_* tools work on it)"},
        {"map", "image", "maps.render", "path", ".png", "picture", "photo.open", "a picture of the map"},
        {"map", "office", "maps.render", "path", ".svg", "map", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
    };
    return table;
}

std::optional<SendRoute> sendRoute(const std::string& docKind, const std::string& to) {
    for (const auto& r : sendRoutes())
        if (r.docKind == docKind && r.to == to) return r;
    return std::nullopt;
}

std::vector<std::string> sendTargets(const std::string& docKind) {
    std::vector<std::string> out;
    for (const auto& r : sendRoutes())
        if (r.docKind == docKind) out.push_back(r.to);
    return out;
}

std::string toolGuide(const std::string& tool) {
    static const std::map<std::string, std::string> guides = {
        {"photo", "photo_* edits pictures: photo_open or photo_new returns a doc id like img1; pass it as `doc`; photo_preview (or preview=true) shows the result; photo_save writes it. Coordinates are pixels from the top-left; colours accept #rrggbb, rgb(r,g,b) or names."},
        {"paint", "paint_* paints on image documents (paint_new returns img1): strokes take points as a flat [x1,y1,x2,y2,...] list with pressure brushes, plus fills, shapes, gradients and layers; save with photo_save."},
        {"vector", "vector_* draws shapes, paths and text (vector_new returns vec1); vector_save writes SVG or PNG."},
        {"type", "type_* designs fonts and exports TTF/WOFF (type_new returns font1; type_proof shows a specimen)."},
        {"board", "board_* makes whiteboards and diagrams (board_from_mermaid turns Mermaid text into a laid-out diagram; board_export writes PNG/SVG/PDF)."},
        {"sculpt", "sculpt_* sculpts meshes in mm with z up using brush strokes along point paths (sculpt_new returns sculpt1; strokes accept space=screen points on a preview; sculpt_export writes STL/OBJ/glTF)."},
        {"render", "render_* builds 3D scenes and path-traces them (render_new returns scene1; render_model renders a model file in one go; render_render without output returns the image)."},
        {"animate", "animate_* keyframes 2D layers, cels, bone rigs and 3D meshes and exports GIF, sprite sheets or video."},
        {"forge", "forge_* prepares game assets: meshes (forge_mesh_open, LODs, scale, up axis), sprite sheets and atlases, tileable textures and normal maps."},
        {"levels", "levels_* edits tilemaps and generates caves and dungeons from a seed (Tiled TMX/TMJ, LDtk)."},
        {"cad", "cad_* builds parametric parts in mm with z up (cad_new returns cad1; numeric fields take expressions over cad_param_set parameters; cad_save exports STL/3MF)."},
        {"blueprint", "blueprint_* makes technical drawings and floor plans in real units (mm, m, ft) with y up, drawn to a scale such as 1:50 (blueprint_new returns bp1; blueprint_export writes PDF/SVG/PNG to scale)."},
        {"slicer", "slicer_* turns a model into G-code for a 3D printer (slicer_slice_file is the one-shot)."},
        {"circuit", "circuit_* draws schematics in mm and lays out PCBs (circuit_new returns ckt1; circuit_erc and circuit_drc check them; circuit_gerber writes fab files)."},
        {"flash", "flash_* finds boards on serial ports and flashes Arduino, ESP32 and STM32 firmware (flash_ports first; sim: boards let you try it without hardware)."},
        {"audio", "audio_* edits sound in seconds (audio_open returns aud1; audio_preview shows a waveform or spectrogram; audio_measure gives LUFS)."},
        {"beats", "beats_* makes music from step patterns, notes and chords (beats_generate makes a starter song; beats_render writes audio)."},
        {"cut", "cut_* edits video on a timeline (cut_new returns cut1; cut_preview shows a frame; cut_render exports; needs ffmpeg for video files)."},
        {"capture", "capture_screenshot without output returns an image of the screen; capture_* also records the screen."},
        {"office", "office_* handles spreadsheets with formulas, documents, slide decks and PDF tools."},
        {"layout", "layout_* builds print layouts (posters, flyers, booklets) exported to PDF."},
        {"notes", "notes_* works on an Obsidian-style folder of Markdown notes (notes_search, notes_backlinks, notes_tasks)."},
        {"maps", "maps_* handles GeoJSON/KML/GPX/Shapefile data in lon/lat and renders maps (maps_new returns map1)."},
        {"toolkit", "toolkit_inspect identifies any file and says which app opens it."},
        {"convert", "convert_file changes file formats; convert_formats shows what's available."},
        {"library", "library_* indexes and searches asset folders (by type, size, tags, duplicates)."},
        {"automate", "automate_* runs batch scripts, pipelines over folders and folder watches."},
    };
    auto it = guides.find(tool);
    return it == guides.end() ? std::string() : it->second;
}

std::string appInstructions(const std::string& appId) {
    const App* app = find(appId);
    if (!app) return {};
    std::string text = binaryName(appId) + " is the wetwaresuite " + appId + " app: " + app->summary +
                       ". Every edit is undoable with undo/redo, `documents` lists what's open, and opened or new "
                       "documents get short ids you pass as `doc`.";
    for (const auto& t : app->tools) {
        std::string g = toolGuide(t);
        if (!g.empty()) text += " " + g;
    }
    return text;
}

}  // namespace wetwaresuite::apps
