// Object mode: add, set, select, duplicate, delete, parent, mode, cursor.
#include <algorithm>
#include <set>

#include "cmd.hpp"

namespace wetwaresuite::model::cmd {

namespace {

std::vector<std::string> addTypes() {
    std::vector<std::string> t = primitiveNames();
    t.insert(t.end(), {"empty", "light", "camera"});
    return t;
}

PrimitiveOptions primitiveOptions(const Params& p) {
    PrimitiveOptions o;
    o.size = numberParam(p, "size", o.size);
    o.radius = numberParam(p, "radius", o.radius);
    o.radius2 = numberParam(p, "radius2", o.radius2);
    o.depth = numberParam(p, "depth", o.depth);
    o.vertices = static_cast<int>(intParam(p, "vertices", o.vertices));
    o.rings = static_cast<int>(intParam(p, "rings", o.rings));
    o.subdivisions = static_cast<int>(intParam(p, "subdivisions", o.subdivisions));
    o.xSubdivisions = static_cast<int>(intParam(p, "x_subdivisions", o.xSubdivisions));
    o.ySubdivisions = static_cast<int>(intParam(p, "y_subdivisions", o.ySubdivisions));
    o.majorRadius = numberParam(p, "major_radius", o.majorRadius);
    o.minorRadius = numberParam(p, "minor_radius", o.minorRadius);
    o.majorSegments = static_cast<int>(intParam(p, "major_segments", o.majorSegments));
    o.minorSegments = static_cast<int>(intParam(p, "minor_segments", o.minorSegments));
    o.fill = stringParam(p, "fill", o.fill);
    return o;
}

std::string titleCase(std::string s) {
    if (s == "uv_sphere") return "Sphere";  // Blender's names
    if (s == "ico_sphere") return "Icosphere";
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') s[i] = ' ';
        if (i == 0 || s[i - 1] == ' ') s[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
    }
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    return s;
}

void applyLightParams(Object& o, const Params& p) {
    if (has(p, "light_type")) o.lightType = stringParam(p, "light_type");
    if (has(p, "power")) o.power = numberParam(p, "power");
    if (has(p, "size")) o.size = numberParam(p, "size");
    o.lightColor = color(p, "color", o.lightColor);
}

}  // namespace

void registerObjectCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"model.add",
             "Add object",
             "Add a primitive mesh (cube, plane, grid, circle, uv_sphere, ico_sphere, cylinder, cone, torus), "
             "an empty, a light or a camera at the 3D cursor (or location). In edit mode a primitive goes into the "
             "edited mesh instead (into=false makes a new object anyway), selected and ready to edit. Sizes follow "
             "Blender: 2 m cube, radius 1 spheres, 32-sided cylinders.",
             withDoc({{"type", ParamType::String, "what to add", true, addTypes()},
                      {"name", ParamType::String, "object name (default from the type)", false},
                      {"location", ParamType::NumberList, "x,y,z (default: the 3D cursor)", false},
                      {"rotation", ParamType::NumberList, "degrees about x,y,z", false},
                      {"scale", ParamType::NumberList, "x,y,z or one number", false},
                      {"size", ParamType::Number, "cube edge / plane side; light size", false},
                      {"radius", ParamType::Number, "circle, spheres, cylinder, cone (bottom)", false},
                      {"radius2", ParamType::Number, "cone top radius", false},
                      {"depth", ParamType::Number, "cylinder/cone height", false},
                      {"vertices", ParamType::Int, "circle/cylinder/cone sides, uv sphere segments", false},
                      {"rings", ParamType::Int, "uv sphere rings", false},
                      {"subdivisions", ParamType::Int, "ico sphere subdivisions", false},
                      {"x_subdivisions", ParamType::Int, "grid columns", false},
                      {"y_subdivisions", ParamType::Int, "grid rows", false},
                      {"major_radius", ParamType::Number, "torus ring radius", false},
                      {"minor_radius", ParamType::Number, "torus tube radius", false},
                      {"major_segments", ParamType::Int, "torus segments around", false},
                      {"minor_segments", ParamType::Int, "torus tube segments", false},
                      {"fill", ParamType::String, "caps of circle/cylinder/cone", false, {"ngon", "fan", "none"}},
                      {"light_type", ParamType::String, "light kind", false, {"point", "sun", "area"}},
                      {"power", ParamType::Number, "light watts (point/area) or strength (sun)", false},
                      {"color", ParamType::String, "light colour", false},
                      {"into", ParamType::Bool, "edit mode: add into the edited mesh (default true)", false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const std::string type = stringParam(p, "type");
                const Vec3 loc = vec3(p, "location", d.scene.cursor), rot = vec3(p, "rotation", {0, 0, 0}), scl = vec3(p, "scale", {1, 1, 1});
                Object* active = d.activeObject();
                const bool isPrim = std::find(primitiveNames().begin(), primitiveNames().end(), type) != primitiveNames().end();
                if (isPrim && d.scene.editMode && active && d.meshOf(*active) && boolParam(p, "into", true)) {
                    PolyMesh part = primitive(type, primitiveOptions(p));
                    selectAll(part, true);
                    PolyMesh& m = d.editMesh(*active);
                    selectAll(m, false);
                    const Mat4 t = d.world(*active).inverse() * Mat4::translate(loc) * Mat4::rotate(rot) * Mat4::scale(scl);
                    append(m, part, t);
                    flushSelection(m, d.scene.selectMode == SelectMode::Face ? SelectMode::Face : SelectMode::Vertex);
                    return done("added a " + type + " to " + active->name, d, p);
                }
                Object o;
                o.type = isPrim ? "mesh" : type;
                o.name = d.uniqueName(has(p, "name") ? stringParam(p, "name") : titleCase(type));
                o.location = loc;
                o.rotation = rot;
                o.scale = scl;
                if (isPrim) {
                    PolyMesh m = primitive(type, primitiveOptions(p));
                    selectAll(m, true);
                    o.data = d.uniqueMeshName(o.name);
                    d.scene.meshes[o.data] = std::make_shared<PolyMesh>(std::move(m));
                } else if (type == "light") {
                    o.lightType = stringParam(p, "light_type", "point");
                    o.power = o.lightType == "sun" ? 4 : 1000;
                    o.size = o.lightType == "area" ? 1 : o.lightType == "sun" ? 1 : 0.1;
                    applyLightParams(o, p);
                } else if (type == "camera") {
                    if (!has(p, "rotation")) o.rotation = {60, 0, 45};
                }
                if (o.type != "light" && has(p, "size") && !isPrim) o.scale = o.scale * numberParam(p, "size");
                for (auto& other : d.scene.objects) other.selected = false;
                o.selected = true;
                d.scene.active = o.name;
                d.scene.editMode = false;
                const std::string name = o.name;
                d.scene.objects.push_back(std::move(o));
                return done("added " + name, d, p);
            }));

    reg.add({"model.object_set",
             "Set object properties",
             "Change objects (object=, objects=a,b or the selected ones): location/rotation/scale set them, move/turn/grow "
             "add to them (grow multiplies); rename (one object), show or hide, light and camera settings, auto smooth angle.",
             withDoc({objectParam(),
                      {"objects", ParamType::String, "several objects, comma separated", false},
                      {"name", ParamType::String, "new name (one object)", false},
                      {"location", ParamType::NumberList, "x,y,z", false},
                      {"rotation", ParamType::NumberList, "degrees about x,y,z", false},
                      {"scale", ParamType::NumberList, "x,y,z or one number", false},
                      {"move", ParamType::NumberList, "add x,y,z to the location", false},
                      {"turn", ParamType::NumberList, "add degrees to the rotation", false},
                      {"grow", ParamType::NumberList, "multiply the scale (x,y,z or one number)", false},
                      {"visible", ParamType::Bool, "show or hide", false},
                      {"auto_smooth", ParamType::Number, "split smooth shading at edges sharper than this (degrees)", false},
                      {"light_type", ParamType::String, "light kind", false, {"point", "sun", "area"}},
                      {"power", ParamType::Number, "light watts (point/area) or strength (sun)", false},
                      {"color", ParamType::String, "light colour", false},
                      {"size", ParamType::Number, "light size", false},
                      {"fov", ParamType::Number, "camera field of view (degrees)", false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                auto objs = targets(d, p);
                if (has(p, "name")) {
                    if (objs.size() != 1) return Result::failure("rename one object at a time");
                    const std::string newName = stringParam(p, "name");
                    if (newName.empty()) return Result::failure("name can't be empty");
                    if (d.object(newName) && d.object(newName) != objs[0]) return Result::failure("'" + newName + "' is taken");
                    const std::string old = objs[0]->name;
                    for (auto& o : d.scene.objects) {
                        if (o.parent == old) o.parent = newName;
                        for (auto& m : o.modifiers)
                            if (m.text.count("object") && m.text["object"] == old) m.text["object"] = newName;
                    }
                    if (d.scene.active == old) d.scene.active = newName;
                    objs[0]->name = newName;
                }
                for (Object* o : objs) {
                    o->location = vec3(p, "location", o->location) + vec3(p, "move", {0, 0, 0});
                    o->rotation = vec3(p, "rotation", o->rotation) + vec3(p, "turn", {0, 0, 0});
                    const Vec3 g = vec3(p, "grow", {1, 1, 1});
                    const Vec3 s = vec3(p, "scale", o->scale);
                    o->scale = {s.x * g.x, s.y * g.y, s.z * g.z};
                    if (has(p, "visible")) o->visible = boolParam(p, "visible");
                    if (has(p, "auto_smooth")) o->autoSmooth = numberParam(p, "auto_smooth");
                    if (o->type == "light") applyLightParams(*o, p);
                    if (o->type == "camera" && has(p, "fov")) o->fov = std::clamp(numberParam(p, "fov"), 1.0, 170.0);
                }
                return done("updated " + std::to_string(objs.size()) + " object" + (objs.size() == 1 ? "" : "s"), d, p);
            }));

    reg.add({"model.select_object",
             "Select objects",
             "Object-mode selection: set, add or remove the named objects (comma separated), or select all, none or "
             "invert. active= makes one the active object (the one edit commands work on). type= limits all/invert to "
             "one kind (mesh, light, camera, empty).",
             withDoc({{"objects", ParamType::String, "object names, comma separated", false},
                      {"action", ParamType::String, "what to do", false, {"set", "add", "remove", "all", "none", "invert"}, std::string("set")},
                      {"active", ParamType::String, "make this the active object", false},
                      {"type", ParamType::String, "only this kind (all, invert)", false, {"mesh", "light", "camera", "empty"}}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const std::string action = stringParam(p, "action", "set");
                std::set<std::string> names;
                for (const auto& n : splitList(stringParam(p, "objects"))) {
                    if (!d.object(n)) return Result::failure("no object called '" + n + "'");
                    names.insert(n);
                }
                const std::string type = stringParam(p, "type");
                for (auto& o : d.scene.objects) {
                    const bool named = names.count(o.name) > 0, kind = type.empty() || o.type == type;
                    if (action == "set") o.selected = named;
                    else if (action == "add") o.selected = o.selected || named;
                    else if (action == "remove") o.selected = o.selected && !named;
                    else if (action == "all") o.selected = kind;
                    else if (action == "none") o.selected = false;
                    else if (action == "invert") o.selected = kind && !o.selected;
                }
                if (has(p, "active")) {
                    Object* a = d.object(stringParam(p, "active"));
                    if (!a) return Result::failure("no object called '" + stringParam(p, "active") + "'");
                    a->selected = true;
                    d.scene.active = a->name;
                } else if ((action == "set" || action == "add") && !names.empty()) {
                    d.scene.active = *names.begin();
                    for (const auto& n : splitList(stringParam(p, "objects"))) d.scene.active = n;  // the last named
                }
                d.scene.editMode = false;
                std::size_t n = 0;
                for (const auto& o : d.scene.objects) n += o.selected ? 1 : 0;
                return done(std::to_string(n) + " objects selected", d, p);
            }));

    reg.add({"model.duplicate_object",
             "Duplicate objects",
             "Copy objects (default: the selected ones), moved by offset. linked=true shares the mesh (Alt+D in Blender: "
             "editing one edits all); otherwise each copy gets its own mesh. The copies end up selected.",
             withDoc({{"objects", ParamType::String, "objects to copy (default: selected)", false},
                      objectParam(),
                      {"linked", ParamType::Bool, "share the mesh (an instance)", false, {}, false},
                      {"offset", ParamType::NumberList, "move the copies by x,y,z", false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const Vec3 off = vec3(p, "offset", {0, 0, 0});
                const bool linked = boolParam(p, "linked", false);
                std::vector<std::string> names;
                for (Object* o : targets(d, p)) names.push_back(o->name);
                std::vector<std::string> made;
                for (auto& o : d.scene.objects) o.selected = false;
                for (const auto& n : names) {
                    Object c = *d.object(n);
                    c.name = d.uniqueName(n);
                    c.location = c.location + off;
                    c.selected = true;
                    c.keys.clear();
                    if (c.type == "mesh" && !linked && d.meshOf(c)) {
                        const std::string mesh = d.uniqueMeshName(c.data);
                        d.scene.meshes[mesh] = std::make_shared<PolyMesh>(*d.meshOf(c));
                        c.data = mesh;
                    }
                    made.push_back(c.name);
                    d.scene.objects.push_back(std::move(c));
                }
                d.scene.active = made.back();
                d.scene.editMode = false;
                std::string list;
                for (const auto& m : made) list += (list.empty() ? "" : ", ") + m;
                return done((linked ? "instanced " : "duplicated ") + list, d, p);
            }));

    reg.add({"model.delete_object",
             "Delete objects",
             "Delete objects (default: the selected ones). Their children stay where they are, unparented.",
             withDoc({{"objects", ParamType::String, "objects to delete (default: selected)", false}, objectParam()}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                std::set<std::string> kill;
                for (Object* o : targets(d, p)) kill.insert(o->name);
                for (auto& o : d.scene.objects) {
                    if (kill.count(o.name) || !kill.count(o.parent)) continue;
                    const Mat4 w = d.world(o);
                    o.parent.clear();
                    setLocal(o, w);
                }
                for (auto& o : d.scene.objects)
                    while (!o.parent.empty() && kill.count(o.parent)) o.parent.clear();
                std::erase_if(d.scene.objects, [&](const Object& o) { return kill.count(o.name) > 0; });
                if (kill.count(d.scene.active)) d.scene.active.clear();
                d.scene.editMode = false;
                purgeMeshes(d);
                return done("deleted " + std::to_string(kill.size()) + " object" + (kill.size() == 1 ? "" : "s"), d, p);
            }));

    reg.add({"model.parent",
             "Parent objects",
             "Make objects (default: the selected ones other than the parent) children of parent=; they follow it when "
             "it moves. keep=true (default) keeps them where they are. parent= empty clears the parent.",
             withDoc({{"objects", ParamType::String, "children (default: selected)", false},
                      objectParam(),
                      {"parent", ParamType::String, "the parent object (empty clears)", true},
                      {"keep", ParamType::Bool, "keep the world transform", false, {}, true}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const std::string parent = stringParam(p, "parent");
                if (!parent.empty() && !d.object(parent)) return Result::failure("no object called '" + parent + "'");
                std::vector<std::string> kids;
                for (Object* o : targets(d, p))
                    if (o->name != parent) kids.push_back(o->name);
                if (kids.empty()) return Result::failure("no children to parent");
                for (const auto& k : kids) {
                    if (!parent.empty()) {
                        for (const Object* a = d.object(parent); a; a = d.object(a->parent))
                            if (a->name == k) return Result::failure("'" + parent + "' is a child of '" + k + "' already");
                    }
                    Object& o = *d.object(k);
                    const Mat4 w = d.world(o);
                    o.parent = parent;
                    if (boolParam(p, "keep", true)) setWorld(d, o, w);
                }
                return done((parent.empty() ? "cleared the parent of " : "parented to " + parent + ": ") + std::to_string(kids.size()) + " objects", d, p);
            }));

    reg.add({"model.mode",
             "Switch mode",
             "Object mode or edit mode (Tab in Blender) on the active mesh, and the edit-mode select mode (1/2/3: vert, "
             "edge, face; switching converts the selection like Blender does).",
             withDoc({objectParam(),
                      {"mode", ParamType::String, "object or edit (toggle flips)", false, {"object", "edit", "toggle"}},
                      {"select_mode", ParamType::String, "what edit-mode selection works on", false, {"vert", "edge", "face"}}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                if (has(p, "object")) {
                    Object& o = target(d, p);
                    for (auto& x : d.scene.objects) x.selected = x.name == o.name;
                    d.scene.active = o.name;
                }
                const std::string mode = stringParam(p, "mode", has(p, "select_mode") ? "edit" : "toggle");
                const bool editMode = mode == "toggle" ? !d.scene.editMode : mode == "edit";
                if (editMode) {
                    Object* a = d.activeObject();
                    if (!a || a->type != "mesh") return Result::failure("edit mode needs an active mesh object");
                }
                d.scene.editMode = editMode;
                if (has(p, "select_mode")) {
                    d.scene.selectMode = *parseSelectMode(stringParam(p, "select_mode"));
                    if (Object* a = d.activeObject(); a && d.meshOf(*a)) flushSelection(d.editMesh(*a), d.scene.selectMode);
                }
                return done(editMode ? "edit mode (" + std::string(toString(d.scene.selectMode)) + ")" : "object mode", d, p);
            }));

    reg.add({"model.cursor",
             "Place 3D cursor",
             "Move the 3D cursor (where model.add puts things, a pivot for model.transform and model.spin): to a "
             "location, or to=selected (the edit-mode selection or the selected objects), active, origin.",
             withDoc({{"location", ParamType::NumberList, "x,y,z", false},
                      {"to", ParamType::String, "snap it", false, {"selected", "active", "origin"}}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const std::string to = stringParam(p, "to");
                if (to == "origin") d.scene.cursor = {0, 0, 0};
                if (to == "active") {
                    const Object* a = d.activeObject();
                    if (!a) return Result::failure("no active object");
                    d.scene.cursor = d.world(*a).point({0, 0, 0});
                }
                if (to == "selected") {
                    Vec3 sum{0, 0, 0};
                    std::size_t n = 0;
                    const Object* a = d.activeObject();
                    if (d.scene.editMode && a && d.meshOf(*a)) {
                        const Mat4 w = d.world(*a);
                        for (const auto& v : d.meshOf(*a)->verts)
                            if (v.sel) {
                                sum = sum + w.point(v.p);
                                ++n;
                            }
                    } else {
                        for (const auto& o : d.scene.objects)
                            if (o.selected) {
                                sum = sum + d.world(o).point({0, 0, 0});
                                ++n;
                            }
                    }
                    if (!n) return Result::failure("nothing selected");
                    d.scene.cursor = sum / static_cast<double>(n);
                }
                d.scene.cursor = vec3(p, "location", d.scene.cursor);
                const Vec3 c = d.scene.cursor;
                return done("cursor at " + num(c.x) + ", " + num(c.y) + ", " + num(c.z), d, p);
            }));
}

}  // namespace wetwaresuite::model::cmd
