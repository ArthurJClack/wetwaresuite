// model.new, open, import, save, export, close, info, elements.
#include <algorithm>
#include <filesystem>

#include "cmd.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::model::cmd {

namespace fs = std::filesystem;

namespace {

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::vector<ParamSpec> noDoc(std::vector<ParamSpec> v) {
    v.erase(v.begin());
    return v;
}

std::string formats() {
    std::string list = "wwmodel";
    for (const auto& e : mesh::readableExtensions()) list += ", " + e;
    return list;
}

Result opened(Session& s, const std::shared_ptr<ModelDoc>& doc, const std::string& message, const Params& p) {
    const std::string id = s.workspace().add(doc, "model");
    Params q = p;
    q["doc"] = id;
    Result r = done(message + " as " + id + ": " + doc->summary(), *doc, q);
    return r;
}

std::string objectLine(const ModelDoc& doc, const Object& o) {
    std::string s = o.name + " (" + o.type;
    if (const PolyMesh* m = doc.meshOf(o)) {
        s += " " + o.data + ": " + std::to_string(m->verts.size()) + " verts, " + std::to_string(m->faces.size()) + " faces";
        if (!o.modifiers.empty()) {
            const std::size_t faces = doc.shown(o) ? doc.evaluated(o.name).faces : evaluate(doc, o).faces.size();
            s += ", " + std::to_string(faces) + " after modifiers";
        }
    }
    if (o.type == "light") s += " " + o.lightType + " " + num(o.power);
    s += ") at " + num(o.location.x) + "," + num(o.location.y) + "," + num(o.location.z);
    if (o.rotation.x != 0 || o.rotation.y != 0 || o.rotation.z != 0)
        s += " rot " + num(o.rotation.x) + "," + num(o.rotation.y) + "," + num(o.rotation.z);
    if (o.scale.x != 1 || o.scale.y != 1 || o.scale.z != 1) s += " scale " + num(o.scale.x) + "," + num(o.scale.y) + "," + num(o.scale.z);
    if (!o.parent.empty()) s += ", parent " + o.parent;
    if (o.collection != "Collection") s += ", in " + o.collection;
    if (!o.materials.empty()) {
        s += ", materials";
        for (const auto& m : o.materials) s += " " + m;
    }
    if (!o.modifiers.empty()) {
        s += ", modifiers";
        for (const auto& m : o.modifiers) s += " " + m.name + (m.enabled ? "" : "(off)");
    }
    if (!o.keys.empty()) s += ", animated";
    if (!o.visible) s += ", hidden";
    if (o.selected) s += ", selected";
    if (o.name == doc.scene.active) s += ", active";
    return s;
}

}  // namespace

void registerDocCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"model.new",
             "New model",
             "Start a model the way Blender starts: a 2 m cube called Cube (with the material Material), a light and a "
             "camera. empty=true leaves the cube out. Everything after is model.* commands: model.add for more objects, "
             "model.select and the edit operators for polygon modeling, model.modifier_add for the modifier stack, "
             "model.render to path-trace it.",
             noDoc(withDoc({{"empty", ParamType::Bool, "no cube (light and camera only)", false, {}, false}})),
             false,
             true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    auto doc = newModel(!boolParam(p, "empty", false));
                    return opened(*s, doc, "new model", p);
                });
            });

    reg.add({"model.open",
             "Open model",
             "Open a .wwmodel file, or start a new scene holding a model file's objects: " + formats() +
                 ". OBJ keeps polygons, UVs, objects, smoothing and materials; other formats come in as triangles "
                 "(quads=true joins them back into quads).",
             noDoc(withDoc({{"path", ParamType::String, "file to open", true},
                            {"quads", ParamType::Bool, "join triangles into quads (triangle formats)", false, {}, true}})),
             false,
             true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    const std::string path = stringParam(p, "path");
                    try {
                        std::shared_ptr<ModelDoc> doc;
                        if (lowerExt(path) == "wwmodel") {
                            doc = loadModel(path);
                        } else {
                            doc = newModel(false);
                            importFile(*doc, path, boolParam(p, "quads", true));
                            doc->path.clear();
                        }
                        return opened(*s, doc, "opened " + path, p);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"model.import",
             "Import into model",
             "Add the objects of a file to the scene: " + formats() + ". They're selected and the first is active.",
             withDoc({{"path", ParamType::String, "file to import", true},
                      {"quads", ParamType::Bool, "join triangles into quads (triangle formats)", false, {}, true}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const auto names = importFile(d, stringParam(p, "path"), boolParam(p, "quads", true));
                std::string list;
                for (const auto& n : names) list += (list.empty() ? "" : ", ") + n;
                d.scene.editMode = false;
                return done("imported " + list, d, p);
            }));

    reg.add({"model.save",
             "Save model",
             "Save the whole scene as .wwmodel (JSON: objects, meshes with UVs and seams, modifiers, materials, "
             "keyframes). Without path it saves where it was opened or last saved. Use model.export for OBJ, glTF, FBX, STL.",
             withDoc({{"path", ParamType::String, "file.wwmodel (default: its current file)", false}}, false),
             false},
            query(session, [](ModelDoc& d, const Params& p) {
                std::string path = has(p, "path") ? stringParam(p, "path") : d.path;
                if (path.empty()) return Result::failure("give path=<file>.wwmodel");
                if (lowerExt(path) != "wwmodel") return Result::failure("model.save writes .wwmodel; use model.export for ." + lowerExt(path));
                saveModel(d, path);
                return Result::success("saved " + path).with("doc", stringParam(p, "doc")).with("path", path);
            }));

    reg.add({"model.export",
             "Export model",
             "Write the scene to a file. obj keeps polygons, UVs, smoothing, objects and materials (with a .mtl); glb, "
             "gltf, fbx, stl, ply, 3mf, off and step get the triangulated scene; wwmodel the whole scene; png or jpg a "
             "picture of the viewport. Modifiers are applied unless modifiers=false.",
             withDoc({{"path", ParamType::String, "file to write, e.g. chair.obj, chair.glb, chair.fbx, chair.stl", true},
                      {"modifiers", ParamType::Bool, "apply the modifier stacks", false, {}, true},
                      {"selected", ParamType::Bool, "only selected objects", false, {}, false},
                      {"objects", ParamType::String, "only these objects (comma separated)", false},
                      {"ascii", ParamType::Bool, "STL/PLY as text", false, {}, false},
                      {"size", ParamType::Int, "picture size in px (png/jpg)", false, {}, std::int64_t{1024}},
                      {"view", ParamType::String, "picture view", false, mesh::viewNames(), std::string("iso")},
                      {"yaw", ParamType::Number, "picture orbit yaw", false},
                      {"pitch", ParamType::Number, "picture orbit pitch", false}},
                     false),
             false},
            query(session, [](ModelDoc& d, const Params& p) {
                const std::string path = stringParam(p, "path");
                const std::string ext = lowerExt(path);
                if (ext == "png" || ext == "jpg" || ext == "jpeg") {
                    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 1024), 16, 8192));
                    std::vector<std::uint8_t> rgba;
                    d.renderView(viewFrom(p), size, size, rgba);
                    raster::Image img(size, size);
                    std::copy(rgba.begin(), rgba.end(), img.data());
                    raster::save(img, path);
                    return Result::success("wrote " + path).with("path", path);
                }
                ExportOptions o;
                o.modifiers = boolParam(p, "modifiers", true);
                o.selectedOnly = boolParam(p, "selected", false);
                if (has(p, "objects")) o.objects = splitList(stringParam(p, "objects"));
                o.ascii = boolParam(p, "ascii", false);
                return Result::success(exportFile(d, path, o)).with("path", path);
            }));

    reg.add({"model.close", "Close model", "Close an open model (unsaved changes are lost).", {docParam()}, false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    std::string id, error;
                    if (!findDoc(*s, p, id, error)) return Result::failure(error);
                    s->workspace().take(id);
                    return Result::success("closed " + id);
                });
            });

    reg.add({"model.info",
             "Model info",
             "List the scene: every object (type, mesh, size, transform, parent, collection, materials, modifiers), "
             "materials, collections, the mode, the 3D cursor and the frame range; problems modifiers ran into too.",
             withDoc({}),
             false,
             true},
            query(session, [](ModelDoc& d, const Params& p) {
                d.refresh();
                std::string msg = d.summary();
                std::string names;
                for (const auto& o : d.scene.objects) {
                    msg += "\n  " + objectLine(d, o);
                    names += (names.empty() ? "" : ",") + o.name;
                    for (const auto& e : d.evaluated(o.name).errors) msg += "\n    modifier problem: " + e;
                }
                msg += "\nmaterials:";
                for (const auto& m : d.scene.materials) msg += " " + m.name + "(" + hex(m.color) + ")";
                msg += "\ncollections:";
                for (const auto& c : d.scene.collections) msg += " " + c.name + (c.visible ? "" : "(hidden)");
                msg += "\ncursor " + num(d.scene.cursor.x) + "," + num(d.scene.cursor.y) + "," + num(d.scene.cursor.z) + "; frame " +
                       std::to_string(d.scene.frame) + " of " + std::to_string(d.scene.frameStart) + ".." +
                       std::to_string(d.scene.frameEnd) + " at " + num(d.scene.fps) + " fps";
                Result r = done(msg, d, p);
                r.with("objects", names);
                r.with("triangles", static_cast<std::int64_t>(d.mesh.triangleCount()));
                return r;
            }));

    reg.add({"model.elements",
             "List mesh elements",
             "Print an object's vertices (index and local position), edges (vertex pairs) or faces (vertex loops, "
             "material slot), selected ones marked with *, so you can pick indices for model.select. "
             "selected=true lists only the selection.",
             withDoc({objectParam(),
                      {"what", ParamType::String, "which elements", false, {"verts", "edges", "faces"}, std::string("verts")},
                      {"selected", ParamType::Bool, "only selected ones", false, {}, false},
                      {"max", ParamType::Int, "at most this many lines", false, {}, std::int64_t{200}}},
                     false),
             false,
             true},
            query(session, [](ModelDoc& d, const Params& p) {
                const Object& o = target(d, p);
                const PolyMesh* m = d.meshOf(o);
                if (!m) return Result::failure("'" + o.name + "' has no mesh");
                const std::string what = stringParam(p, "what", "verts");
                const bool onlySel = boolParam(p, "selected", false);
                const auto max = static_cast<std::size_t>(std::max<std::int64_t>(1, intParam(p, "max", 200)));
                std::string msg;
                std::size_t lines = 0, total = 0;
                NumberList idx;
                auto line = [&](const std::string& s) {
                    ++total;
                    if (lines < max) {
                        msg += "\n" + s;
                        ++lines;
                    }
                };
                if (what == "verts") {
                    for (std::size_t i = 0; i < m->verts.size(); ++i) {
                        const auto& v = m->verts[i];
                        if (onlySel && !v.sel) continue;
                        if (v.sel) idx.push_back(static_cast<double>(i));
                        line((v.sel ? "*" : " ") + std::to_string(i) + ": " + num(v.p.x) + ", " + num(v.p.y) + ", " + num(v.p.z));
                    }
                } else if (what == "edges") {
                    for (auto k : allEdges(*m)) {
                        const bool sel = m->esel.count(k) > 0;
                        if (onlySel && !sel) continue;
                        if (sel) {
                            idx.push_back(keyA(k));
                            idx.push_back(keyB(k));
                        }
                        std::string s = (sel ? "*" : " ") + std::to_string(keyA(k)) + "-" + std::to_string(keyB(k));
                        if (m->seams.count(k)) s += " seam";
                        if (m->sharp.count(k)) s += " sharp";
                        line(s);
                    }
                } else {
                    for (std::size_t i = 0; i < m->faces.size(); ++i) {
                        const auto& f = m->faces[i];
                        if (onlySel && !f.sel) continue;
                        if (f.sel) idx.push_back(static_cast<double>(i));
                        std::string s = (f.sel ? "*" : " ") + std::to_string(i) + ":";
                        for (auto v : f.v) s += " " + std::to_string(v);
                        const Vec3 n = faceNormal(*m, f);
                        s += "  normal " + num(n.x) + "," + num(n.y) + "," + num(n.z);
                        if (f.material) s += " material " + std::to_string(f.material);
                        line(s);
                    }
                }
                std::string head = o.name + " " + what + ": " + std::to_string(total);
                if (total > lines) head += " (first " + std::to_string(lines) + " shown)";
                return Result::success(head + msg).with("object", o.name).with("count", static_cast<std::int64_t>(total)).with("selected", idx);
            }));
}

}  // namespace wetwaresuite::model::cmd
