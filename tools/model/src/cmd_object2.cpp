// Object mode, part two: join, separate, origin, apply transform,
// collections, shading and edge marks.
#include <algorithm>
#include <map>
#include <set>

#include "cmd.hpp"

namespace wetwaresuite::model::cmd {

namespace {

int slotFor(Object& o, const std::string& material) {
    auto it = std::find(o.materials.begin(), o.materials.end(), material);
    if (it != o.materials.end()) return static_cast<int>(it - o.materials.begin());
    o.materials.push_back(material);
    return static_cast<int>(o.materials.size()) - 1;
}

// Makes a new mesh object like `like` (transform, materials, modifiers) holding m.
std::string addLike(ModelDoc& d, const Object& like, PolyMesh m) {
    Object o = like;
    o.name = d.uniqueName(like.name);
    o.data = d.uniqueMeshName(o.name);
    o.keys.clear();
    selectAll(m, false);
    d.scene.meshes[o.data] = std::make_shared<PolyMesh>(std::move(m));
    o.selected = true;
    d.scene.objects.push_back(o);
    return o.name;
}

}  // namespace

void registerObjectCommands2(Session& session) {
    auto& reg = session.registry();

    reg.add({"model.join",
             "Join objects",
             "Merge the selected mesh objects into the active one (Ctrl+J): their meshes move into its mesh, staying where "
             "they are, and their material slots carry over.",
             withDoc({{"objects", ParamType::String, "objects to join (default: selected); the active one receives them", false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object* a = d.activeObject();
                if (!a || !d.meshOf(*a)) return Result::failure("the active object must be a mesh");
                std::vector<std::string> names;
                for (Object* o : targets(d, p))
                    if (o->name != a->name && d.meshOf(*o)) names.push_back(o->name);
                if (names.empty()) return Result::failure("select other mesh objects to join into " + a->name);
                const std::string an = a->name;
                const Mat4 inv = d.world(*a).inverse();
                for (const auto& n : names) {
                    Object& src = *d.object(n);
                    Object& dst = *d.object(an);
                    std::map<int, int> slots;
                    for (std::size_t i = 0; i < src.materials.size(); ++i) slots[static_cast<int>(i)] = slotFor(dst, src.materials[i]);
                    PolyMesh part = *d.meshOf(src);
                    for (auto& f : part.faces) f.material = slots.count(f.material) ? slots[f.material] : 0;
                    const Mat4 t = inv * d.world(src);
                    PolyMesh& m = singleUser(d, dst);
                    append(m, part, t);
                }
                std::erase_if(d.scene.objects, [&](const Object& o) { return std::find(names.begin(), names.end(), o.name) != names.end(); });
                for (auto& o : d.scene.objects) {
                    if (std::find(names.begin(), names.end(), o.parent) != names.end()) o.parent.clear();
                    o.selected = o.name == an;
                }
                purgeMeshes(d);
                return done("joined " + std::to_string(names.size()) + " objects into " + an, d, p);
            }));

    reg.add({"model.separate",
             "Separate mesh",
             "Split the edited mesh into new objects (P in Blender): the selected faces, each loose part, or each "
             "material. The new objects keep the transform, materials and modifiers.",
             withDoc({objectParam(), {"type", ParamType::String, "how to split", false, {"selected", "loose", "material"}, std::string("selected")}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                const std::string type = stringParam(p, "type", "selected");
                const Object like = o;
                std::vector<std::string> made;
                if (type == "selected") {
                    if (selectedFaces(m).empty()) return Result::failure("select the faces to separate");
                    PolyMesh part = extractSelected(m);
                    remove(m, DeleteMode::Faces);
                    made.push_back(addLike(d, like, std::move(part)));
                } else if (type == "loose") {
                    auto parts = looseParts(m);
                    if (parts.size() < 2) return Result::failure("the mesh is one piece");
                    m = parts[0];
                    for (std::size_t i = 1; i < parts.size(); ++i) made.push_back(addLike(d, like, std::move(parts[i])));
                } else {
                    std::set<int> mats;
                    for (const auto& f : m.faces) mats.insert(f.material);
                    if (mats.size() < 2) return Result::failure("the mesh uses one material");
                    const PolyMesh all = m;
                    bool first = true;
                    for (int slot : mats) {
                        PolyMesh copy = all;
                        for (auto& f : copy.faces) f.sel = f.material == slot;
                        flushSelection(copy, SelectMode::Face);
                        PolyMesh part = extractSelected(copy);
                        if (first) {
                            m = part;
                            first = false;
                        } else {
                            made.push_back(addLike(d, like, std::move(part)));
                        }
                    }
                }
                std::string list;
                for (const auto& n : made) list += (list.empty() ? "" : ", ") + n;
                return done("separated into " + list, d, p);
            })));

    reg.add({"model.origin",
             "Set origin",
             "Move an object's origin (its pivot) without moving the mesh: to=geometry (the middle of its bounds), "
             "bottom (the middle of its base), cursor (the 3D cursor); or to=mesh moves the mesh so its middle sits on "
             "the origin.",
             withDoc({{"objects", ParamType::String, "objects (default: selected)", false},
                      objectParam(),
                      {"to", ParamType::String, "where", false, {"geometry", "bottom", "cursor", "mesh"}, std::string("geometry")}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const std::string to = stringParam(p, "to", "geometry");
                std::size_t n = 0;
                for (Object* o : targets(d, p)) {
                    if (!d.meshOf(*o)) continue;
                    const mesh::Bounds b = bounds(*d.meshOf(*o));
                    if (b.empty()) continue;
                    Vec3 shift;  // in local coordinates
                    if (to == "cursor") shift = d.world(*o).inverse().point(d.scene.cursor);
                    else if (to == "bottom") shift = {(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, b.min.z};
                    else shift = (b.min + b.max) * 0.5;
                    PolyMesh& m = singleUser(d, *o);
                    for (auto& v : m.verts) v.p = v.p - shift;
                    if (to != "mesh") o->location = o->local().point(shift);
                    ++n;
                }
                if (!n) return Result::failure("no mesh objects to change");
                return done("set the origin of " + std::to_string(n) + " objects", d, p);
            }));

    reg.add({"model.apply_transform",
             "Apply transform",
             "Bake location, rotation and/or scale into the mesh (Ctrl+A), so the object's transform resets while the "
             "mesh stays put. Do this before bevels and modifiers that depend on real sizes.",
             withDoc({{"objects", ParamType::String, "objects (default: selected)", false},
                      objectParam(),
                      {"location", ParamType::Bool, "apply location", false, {}, false},
                      {"rotation", ParamType::Bool, "apply rotation", false, {}, true},
                      {"scale", ParamType::Bool, "apply scale", false, {}, true}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const bool loc = boolParam(p, "location", false), rot = boolParam(p, "rotation", true), scl = boolParam(p, "scale", true);
                std::size_t n = 0;
                for (Object* o : targets(d, p)) {
                    if (!d.meshOf(*o)) continue;
                    const Mat4 full = o->local();
                    Object kept = *o;
                    if (loc) kept.location = {0, 0, 0};
                    if (rot) kept.rotation = {0, 0, 0};
                    if (scl) kept.scale = {1, 1, 1};
                    const Mat4 bake = kept.local().inverse() * full;
                    transform(singleUser(d, *o), bake);
                    o->location = kept.location;
                    o->rotation = kept.rotation;
                    o->scale = kept.scale;
                    ++n;
                }
                if (!n) return Result::failure("no mesh objects to change");
                return done("applied the transform of " + std::to_string(n) + " objects", d, p);
            }));

    reg.add({"model.collection",
             "Collections",
             "Group objects in collections (like Blender's outliner): move puts objects (default: selected) into the "
             "collection name= (made if missing), new just makes it, hide/show toggles everything in it, remove deletes "
             "the collection (its objects go back to Collection).",
             withDoc({{"name", ParamType::String, "collection", true},
                      {"action", ParamType::String, "what to do", false, {"move", "new", "hide", "show", "remove"}, std::string("move")},
                      {"objects", ParamType::String, "objects (default: selected)", false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const std::string name = stringParam(p, "name"), action = stringParam(p, "action", "move");
                auto& cols = d.scene.collections;
                auto find = [&]() { return std::find_if(cols.begin(), cols.end(), [&](const Collection& c) { return c.name == name; }); };
                if (name.empty()) return Result::failure("name can't be empty");
                if (action == "remove") {
                    if (name == "Collection") return Result::failure("the main Collection stays");
                    if (find() == cols.end()) return Result::failure("no collection called '" + name + "'");
                    cols.erase(find());
                    for (auto& o : d.scene.objects)
                        if (o.collection == name) o.collection = "Collection";
                    if (std::none_of(cols.begin(), cols.end(), [](const Collection& c) { return c.name == "Collection"; }))
                        cols.push_back({"Collection", true});
                    return done("removed collection " + name, d, p);
                }
                if (find() == cols.end()) {
                    if (action == "hide" || action == "show") return Result::failure("no collection called '" + name + "'");
                    cols.push_back({name, true});
                }
                if (action == "hide" || action == "show") {
                    find()->visible = action == "show";
                    return done((action == "show" ? "showing " : "hid ") + name, d, p);
                }
                if (action == "move") {
                    auto objs = targets(d, p);
                    for (Object* o : objs) o->collection = name;
                    return done("moved " + std::to_string(objs.size()) + " objects to " + name, d, p);
                }
                return done("made collection " + name, d, p);
            }));

    reg.add({"model.shade",
             "Shade smooth or flat",
             "Smooth or flat shading on an object's faces (in edit mode: the selected faces). auto smooths but keeps "
             "edges sharper than angle crisp (and edges marked sharp).",
             withDoc({{"objects", ParamType::String, "objects (default: selected)", false},
                      objectParam(),
                      {"type", ParamType::String, "shading", false, {"smooth", "flat", "auto"}, std::string("smooth")},
                      {"angle", ParamType::Number, "auto: split at edges sharper than this (degrees)", false, {}, 30.0}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const std::string type = stringParam(p, "type", "smooth");
                std::size_t faces = 0;
                for (Object* o : targets(d, p)) {
                    if (!d.meshOf(*o)) continue;
                    const bool onlySel = d.scene.editMode && o->name == d.scene.active && !selectedFaces(*d.meshOf(*o)).empty();
                    PolyMesh& m = d.editMesh(*o);
                    for (auto& f : m.faces) {
                        if (onlySel && !f.sel) continue;
                        f.smooth = type != "flat";
                        ++faces;
                    }
                    if (type == "auto") o->autoSmooth = numberParam(p, "angle", 30);
                    else if (!onlySel) o->autoSmooth = 180;
                }
                return done("shaded " + std::to_string(faces) + " faces " + type, d, p);
            }));

    reg.add({"model.mark",
             "Mark edges",
             "Mark (or clear=true) the selected edges as UV seams (where model.uv_unwrap cuts) or sharp (flat shading "
             "across them, crisp in subsurf, bevelled by the bevel modifier). angle= marks every edge sharper than "
             "that instead of the selection.",
             withDoc({objectParam(),
                      {"type", ParamType::String, "which mark", true, {"seam", "sharp"}},
                      {"clear", ParamType::Bool, "remove the mark", false, {}, false},
                      {"angle", ParamType::Number, "mark edges sharper than this (degrees) instead", false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                auto& set = stringParam(p, "type") == "seam" ? m.seams : m.sharp;
                std::size_t n = 0;
                if (has(p, "angle")) {
                    PolyMesh probe = m;
                    selectAll(probe, false);
                    selectSharpEdges(probe, numberParam(p, "angle"));
                    for (auto k : probe.esel) n += set.insert(k).second ? 1 : 0;
                } else {
                    for (auto k : selectedEdges(m)) n += boolParam(p, "clear", false) ? set.erase(k) : (set.insert(k).second ? 1 : 0);
                }
                return done((boolParam(p, "clear", false) ? "cleared " : "marked ") + std::to_string(n) + " " + stringParam(p, "type") + " edges", d, p);
            })));
}

}  // namespace wetwaresuite::model::cmd
