// The fit document: scene pictures, edits with undo and a replayable log,
// projects (.wwfit), and helpers the command files share.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "internal.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/mesh/topology.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::fit {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;
using mesh::cross;
using mesh::dot;
using mesh::length;
using mesh::normalize;

const char* refKindName(RefKind k) {
    switch (k) {
        case RefKind::Point: return "point";
        case RefKind::Plane: return "plane";
        case RefKind::Cylinder: return "cylinder";
        case RefKind::Sphere: return "sphere";
        case RefKind::Circle: return "circle";
        case RefKind::Curve: return "curve";
    }
    return "point";
}

std::string FitDoc::summary() const {
    std::string s = state.scan ? "scan " + mesh::describe(*state.scan) : "no scan";
    s += ", " + std::to_string(state.refs.size()) + " references, " + std::to_string(state.parts.size()) + " parts";
    if (!path.empty()) s += ", " + path;
    return s;
}

const Bvh& FitDoc::bvh() const {
    if (!bvh_ || bvhVersion_ != state.version) {
        bvh_ = std::make_shared<Bvh>(state.scan ? state.scan : std::make_shared<const Mesh>());
        bvhVersion_ = state.version;
    }
    return *bvh_;
}

// ---- scene ----

namespace {

const raster::Color kScan{188, 191, 198, 255};
const raster::Color kRef{238, 140, 42, 255};
const raster::Color kPartColors[] = {{86, 140, 222, 255}, {72, 178, 140, 255}, {196, 108, 176, 255}, {214, 170, 60, 255}};

void addColored(Mesh& scene, std::vector<raster::Color>& colors, const Mesh& m, const raster::Color& c) {
    mesh::append(scene, m);
    colors.resize(scene.triangleCount(), c);
}

Mesh rod(const Vec3& a, const Vec3& b, double r) {
    const double l = length(b - a);
    Mesh c = mesh::cylinder(r, std::max(l, 1e-3), 8, false);
    mesh::transform(c, Mat4::translate(a) * rotationBetween({0, 0, 1}, l > 0 ? (b - a) / l : Vec3{0, 0, 1}));
    return c;
}

Mesh refMesh(const Ref& r, double scale) {
    const double thin = std::max(scale * 0.004, 0.25);
    switch (r.kind) {
        case RefKind::Point: return [&] { Mesh s = mesh::sphere(thin * 4, 12, 6); mesh::transform(s, Mat4::translate(r.point)); return s; }();
        case RefKind::Plane: {
            Mesh d = mesh::cylinder(std::max(r.radius, thin * 20), thin, 32, true);
            mesh::transform(d, Mat4::translate(r.point + r.dir * thin) * rotationBetween({0, 0, 1}, r.dir));
            return d;
        }
        case RefKind::Cylinder: {
            Mesh m;
            const Vec3 a = r.point - r.dir * (r.length / 2), b = r.point + r.dir * (r.length / 2);
            mesh::append(m, rod(a, b, thin));
            for (const Vec3& e : {a, b}) {
                Mesh t = mesh::torus(r.radius, thin, 48, 6);
                mesh::transform(t, Mat4::translate(e) * rotationBetween({0, 0, 1}, r.dir));
                mesh::append(m, t);
            }
            return m;
        }
        case RefKind::Sphere: {
            Mesh m = mesh::sphere(thin * 3, 12, 6);
            mesh::transform(m, Mat4::translate(r.point));
            Mesh t = mesh::torus(r.radius, thin, 48, 6);
            mesh::transform(t, Mat4::translate(r.point));
            mesh::append(m, t);
            return m;
        }
        case RefKind::Circle: {
            Mesh t = mesh::torus(r.radius, thin, 64, 6);
            mesh::transform(t, Mat4::translate(r.point) * rotationBetween({0, 0, 1}, r.dir));
            return t;
        }
        case RefKind::Curve: {
            Mesh m;
            const std::size_t step = std::max<std::size_t>(1, r.curve.size() / 80);
            for (std::size_t i = 0; i + step < r.curve.size(); i += step) mesh::append(m, rod(r.curve[i], r.curve[i + step], thin));
            return m;
        }
    }
    return {};
}

}  // namespace

Mesh sceneMesh(const FitDoc& doc, const SceneOptions& o, std::vector<raster::Color>* colorsOut) {
    Mesh scene;
    std::vector<raster::Color> colors;
    const FitState& st = doc.state;
    double scale = st.scan ? mesh::bounds(*st.scan).diagonal() : 100;
    if (o.scan && st.scan) addColored(scene, colors, *st.scan, kScan);
    if (o.parts) {
        int k = 0;
        for (const auto& [name, part] : st.parts) {
            if (!part.mesh) continue;
            if (!o.heatPart.empty() && name == o.heatPart && st.scan) {
                const FitReport rep = checkFit(doc.bvh(), *part.mesh, 10, part.gap);
                mesh::append(scene, *part.mesh);
                for (std::size_t t = 0; t < part.mesh->triangleCount(); ++t) {
                    double g = 0;
                    int c = 0;
                    for (int j = 0; j < 3; ++j) {
                        const double v = rep.vertexGap[part.mesh->indices[t * 3 + static_cast<std::size_t>(j)]];
                        if (!std::isnan(v)) {
                            g += v;
                            ++c;
                        }
                    }
                    const Rgb rgb = gapColor(c ? g / c : std::nan(""), part.gap);
                    colors.push_back({rgb.r, rgb.g, rgb.b, 255});
                }
            } else {
                addColored(scene, colors, *part.mesh, kPartColors[k % 4]);
            }
            ++k;
        }
    }
    if (o.refs) {
        for (const auto& [name, r] : st.refs) addColored(scene, colors, refMesh(r, scale), kRef);
    }
    if (colorsOut) *colorsOut = std::move(colors);
    return scene;
}

std::unique_ptr<Attachment> FitDoc::preview() const {
    std::vector<raster::Color> colors;
    Mesh m = sceneMesh(*this, {}, &colors);
    mesh::RenderOptions ro;
    ro.faceColors = std::move(colors);
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(mesh::render(m, 512, 512, ro)), "preview.png"});
}

namespace detail {

// ---- params ----

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

std::string vecText(const Vec3& v) { return "(" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + ")"; }

NumberList toList(const Vec3& v) { return {v.x, v.y, v.z}; }

std::optional<Vec3> vec3Param(const Params& p, const std::string& key) {
    const NumberList l = listParam(p, key);
    if (l.size() != 3) return std::nullopt;
    return Vec3{l[0], l[1], l[2]};
}

Vec3 requireVec3(const Params& p, const std::string& key) {
    auto v = vec3Param(p, key);
    if (!v) throw std::runtime_error("give " + key + "=x,y,z (three numbers, in mm)");
    return *v;
}

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string joinList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : ", ") + i;
    return out;
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open scan, e.g. fit1 (from fit.open)", true}; }

ParamSpec nameParam(const std::string& what) {
    return {"name", ParamType::String, "name for the new " + what + " (default: the next free one, like " + what + "1)", false};
}

std::vector<ParamSpec> withPreview(std::vector<ParamSpec> ps) {
    ps.push_back({"preview", ParamType::Bool, "attach a PNG picture of the scan, references and parts", false, {}, false});
    ps.push_back({"view", ParamType::String, "camera: iso, front, back, left, right, top, bottom", false, mesh::viewNames(), std::string("iso")});
    ps.push_back({"yaw", ParamType::Number, "turn the scene this many degrees about Z before viewing", false, {}, 0.0});
    ps.push_back({"pitch", ParamType::Number, "tilt the scene this many degrees toward the camera", false, {}, 0.0});
    return ps;
}

raster::Image renderScene(const FitDoc& doc, const Params& p, int size, const SceneOptions& so) {
    std::vector<raster::Color> colors;
    Mesh m = sceneMesh(doc, so, &colors);
    sculpt::ViewRay v;
    if (auto parsed = mesh::parseView(stringParam(p, "view", "iso"))) v.view = *parsed;
    v.yaw = numberParam(p, "yaw", 0);
    v.pitch = numberParam(p, "pitch", 0);
    mesh::RenderOptions ro;
    ro.view = v.view;
    ro.faceColors = std::move(colors);
    return mesh::render(sculpt::turnedForView(m, v), size, size, ro);
}

Result& addPreview(Result& r, const FitDoc& doc, const Params& p, const SceneOptions& so) {
    if (r.ok && boolParam(p, "preview", false)) r.attach({"image/png", raster::encodePng(renderScene(doc, p, 512, so)), "preview.png"});
    return r;
}

Result done(const std::string& message, const FitDoc& doc, const Params& p, const SceneOptions& so) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"));
    return addPreview(r, doc, p, so);
}

// ---- document helpers ----

namespace {
std::uint64_t nextVersion() {
    static std::uint64_t v = 0;
    return ++v;
}
}  // namespace

void setScan(FitDoc& doc, Mesh m) {
    m.normals.clear();
    doc.state.scan = std::make_shared<const Mesh>(std::move(m));
    doc.state.version = nextVersion();
}

void transformAll(FitDoc& doc, const Mat4& t) {
    FitState& st = doc.state;
    if (st.scan) {
        Mesh m = *st.scan;
        mesh::transform(m, t);
        setScan(doc, std::move(m));
    }
    for (auto& [name, r] : st.refs) {
        r.point = t.point(r.point);
        if (length(r.dir) > 0) r.dir = normalize(t.direction(r.dir));
        for (auto& c : r.curve) c = t.point(c);
        // uniform scales change sizes too
        const double s = std::cbrt(std::fabs(t.determinant3()));
        r.radius *= s;
        r.length *= s;
        r.rms *= s;
    }
    for (auto& [name, part] : st.parts) {
        if (!part.mesh) continue;
        Mesh m = *part.mesh;
        mesh::transform(m, t);
        part.mesh = std::make_shared<const Mesh>(std::move(m));
        if (length(part.normal) > 0) part.normal = normalize(t.direction(part.normal));
    }
}

std::string newName(FitDoc& doc, const Params& p, const std::string& prefix) {
    std::string name = stringParam(p, "name");
    if (!name.empty()) {
        for (char c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
                throw std::runtime_error("names use letters, digits, _ and - only");
            }
        }
        return name;
    }
    auto& st = doc.state;
    for (;;) {
        name = prefix + std::to_string(++st.counters[prefix]);
        if (!st.refs.count(name) && !st.parts.count(name)) return name;
    }
}

const Ref& refOf(const FitDoc& doc, const std::string& name, RefKind kind) {
    auto it = doc.state.refs.find(name);
    if (it == doc.state.refs.end()) {
        throw std::runtime_error("no reference called '" + name + "' (fit.refs lists them; fit." + refKindName(kind) + " makes one)");
    }
    if (it->second.kind != kind) {
        throw std::runtime_error("'" + name + "' is a " + refKindName(it->second.kind) + ", not a " + refKindName(kind));
    }
    return it->second;
}

Part& partOf(FitDoc& doc, const std::string& name) {
    auto it = doc.state.parts.find(name);
    if (it == doc.state.parts.end()) {
        std::vector<std::string> names;
        for (const auto& [n, p] : doc.state.parts) names.push_back(n);
        throw std::runtime_error("no part called '" + name + "'" + (names.empty() ? " (no parts yet)" : " (parts: " + joinList(names) + ")"));
    }
    return it->second;
}

const Ref& refParam(const FitDoc& doc, const Params& p, const char* key, RefKind kind) {
    const std::string name = stringParam(p, key);
    return refOf(doc, name, kind);
}

Part& partParam(FitDoc& doc, const Params& p, const char* key) {
    const std::string name = stringParam(p, key);
    return partOf(doc, name);
}

std::string describeRef(const std::string& name, const Ref& r) {
    std::string s = name + ": " + refKindName(r.kind);
    switch (r.kind) {
        case RefKind::Point: s += " at " + vecText(r.point); break;
        case RefKind::Plane: s += " through " + vecText(r.point) + " facing " + vecText(r.dir); break;
        case RefKind::Cylinder:
            s += " radius " + num(r.radius) + " (diameter " + num(2 * r.radius) + "), axis " + vecText(r.dir) + " through " + vecText(r.point) +
                 ", " + num(r.length) + " long";
            break;
        case RefKind::Sphere: s += " radius " + num(r.radius) + " at " + vecText(r.point); break;
        case RefKind::Circle: s += " diameter " + num(2 * r.radius) + " at " + vecText(r.point) + " facing " + vecText(r.dir); break;
        case RefKind::Curve: s += " of " + std::to_string(r.curve.size()) + " points, " + num(polylineLength(r.curve, false)) + " mm long"; break;
    }
    if (r.rms > 0) s += ", fit error " + num(r.rms) + " mm";
    return s;
}

std::string describePart(const std::string& name, const Part& p) {
    return name + ": " + p.kind + ", " + (p.mesh ? mesh::describe(*p.mesh) : std::string("empty"));
}

void putPart(FitDoc& doc, const std::string& name, Mesh m, const std::string& kind, const Vec3& normal, double gap) {
    if (m.empty()) throw std::runtime_error("the " + kind + " came out empty");
    m.normals.clear();
    m.uvs.clear();
    Part p;
    p.mesh = std::make_shared<const Mesh>(std::move(m));
    p.kind = kind;
    p.normal = normal;
    p.gap = gap;
    p.step = doc.state.log.size();  // the entry this edit is about to get
    doc.state.parts[name] = std::move(p);
}

SurfacePoint snap(const FitDoc& doc, const Vec3& p) {
    const Bvh& b = doc.bvh();
    if (b.empty()) throw std::runtime_error("the scan is empty");
    const auto n = b.nearest(p);
    SurfacePoint s;
    s.point = n.point;
    s.distance = n.distance;
    // average the facing of triangles near the spot, so one noisy triangle doesn't decide it
    const Mesh& m = b.mesh();
    const double r = std::max(meanEdgeLength(m) * 3, 2.0);
    Vec3 acc;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const Vec3 c = (m.corner(t, 0) + m.corner(t, 1) + m.corner(t, 2)) / 3.0;
        if (length(c - s.point) <= r) acc += m.faceNormal(t);
    }
    if (length(acc) == 0) acc = m.faceNormal(n.tri);
    s.normal = normalize(acc);
    return s;
}

std::vector<Vec3> scanVertexNormals(const FitDoc& doc) { return doc.state.scan ? mesh::vertexNormals(*doc.state.scan) : std::vector<Vec3>{}; }

// ---- edits, undo, log ----

std::map<std::string, Handler>& handlers() {
    static std::map<std::string, Handler> h;
    return h;
}

namespace {

// Params worth keeping in the log: not the document, not how the result is shown.
Params loggable(const Params& p) {
    Params q = p;
    for (const char* k : {"doc", "preview", "view", "yaw", "pitch"}) q.erase(k);
    return q;
}

class FitEdit : public Action {
public:
    FitEdit(std::shared_ptr<FitDoc> doc, std::string id, Handler fn, Params p)
        : doc_(std::move(doc)), id_(std::move(id)), fn_(std::move(fn)), p_(std::move(p)) {}
    Result apply() override {
        before_ = doc_->state;
        Result r;
        try {
            r = fn_(*doc_, p_);
        } catch (const std::exception& e) {
            r = Result::failure(e.what());
        }
        if (!r.ok) {
            doc_->state = before_;
            return r;
        }
        if (id_ != "fit.rebuild") doc_->state.log.emplace_back(id_, loggable(p_));
        return r;
    }
    void revert() override { doc_->state = before_; }

private:
    std::shared_ptr<FitDoc> doc_;
    std::string id_;
    Handler fn_;
    Params p_;
    FitState before_;
};

ActionFactory factory(Session& session, const std::string& id, Handler fn, bool edit) {
    return [&session, id, fn, edit](const Params& p) -> std::unique_ptr<Action> {
        const std::string docId = stringParam(p, "doc");
        auto doc = session.workspace().getAs<FitDoc>(docId);
        if (!doc) {
            return std::make_unique<FunctionAction>([docId] { return Result::failure("no open scan called '" + docId + "' (fit.open makes one)"); });
        }
        if (edit) return std::make_unique<FitEdit>(doc, id, fn, p);
        return std::make_unique<FunctionAction>([doc, fn, p] {
            try {
                return fn(*doc, p);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        });
    };
}

}  // namespace

void addEdit(Session& session, CommandInfo info, Handler fn) {
    info.undoable = true;
    handlers()[info.id] = fn;
    const std::string id = info.id;
    session.registry().add(std::move(info), factory(session, id, std::move(fn), true));
}

void addRead(Session& session, CommandInfo info, Handler fn) {
    const std::string id = info.id;
    session.registry().add(std::move(info), factory(session, id, std::move(fn), false));
}

FitState replay(const FitState& from, const std::vector<std::pair<std::string, Params>>& log) {
    FitDoc tmp;
    tmp.state.original = from.original;
    tmp.state.info = from.info;
    tmp.state.scanPath = from.scanPath;
    tmp.state.openParams = from.openParams;
    setScan(tmp, from.original ? *from.original : Mesh{});
    for (std::size_t i = 0; i < log.size(); ++i) {
        const auto& [id, params] = log[i];
        auto it = handlers().find(id);
        if (it == handlers().end()) throw std::runtime_error("step " + std::to_string(i + 1) + ": unknown command " + id);
        Result r;
        try {
            r = it->second(tmp, params);
        } catch (const std::exception& e) {
            r = Result::failure(e.what());
        }
        if (!r.ok) throw std::runtime_error("step " + std::to_string(i + 1) + " (" + id + ") failed: " + r.message);
        tmp.state.log.emplace_back(id, params);
    }
    return tmp.state;
}

// ---- opening and projects ----

std::shared_ptr<FitDoc> openScan(const std::string& path, const Params& p) {
    ScanOptions o;
    o.units = stringParam(p, "units", "auto");
    o.up = stringParam(p, "up", "auto");
    o.budget = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "budget", static_cast<std::int64_t>(kDefaultBudget))));
    o.cloudCell = numberParam(p, "cell", 0);
    auto doc = std::make_shared<FitDoc>();
    Mesh m = loadScan(path, o, doc->state.info);
    doc->state.original = std::make_shared<const Mesh>(m);
    setScan(*doc, std::move(m));
    doc->state.scanPath = fs::absolute(path).lexically_normal().string();
    Params op;
    for (const char* k : {"units", "up", "budget", "cell"}) {
        if (p.count(k)) op[k] = p.at(k);
    }
    doc->state.openParams = op;
    return doc;
}

namespace {

json toJson(const Value& v) {
    if (auto b = std::get_if<bool>(&v)) return *b;
    if (auto i = std::get_if<std::int64_t>(&v)) return *i;
    if (auto d = std::get_if<double>(&v)) return *d;
    if (auto s = std::get_if<std::string>(&v)) return *s;
    if (auto l = std::get_if<NumberList>(&v)) return *l;
    return nullptr;
}

Value fromJson(const json& j) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number_integer()) return j.get<std::int64_t>();
    if (j.is_number()) return j.get<double>();
    if (j.is_string()) return j.get<std::string>();
    if (j.is_array()) {
        NumberList l;
        for (const auto& x : j) l.push_back(x.get<double>());
        return l;
    }
    return {};
}

json paramsJson(const Params& p) {
    json o = json::object();
    for (const auto& [k, v] : p) o[k] = toJson(v);
    return o;
}

Params paramsFrom(const json& j) {
    Params p;
    for (auto it = j.begin(); it != j.end(); ++it) p[it.key()] = fromJson(it.value());
    return p;
}

}  // namespace

void saveProject(const FitDoc& doc, const fs::path& path) {
    const FitState& st = doc.state;
    json j;
    j["format"] = "wetwaresuite-fit";
    j["version"] = 1;
    std::string scan = st.scanPath;
    // relative to the project when it sits beside it or below it, so the pair can move together
    const fs::path base = fs::absolute(path).parent_path();
    std::error_code ec;
    const fs::path rel = fs::relative(st.scanPath, base, ec);
    if (!ec && !rel.empty() && rel.string().rfind("..", 0) != 0) scan = rel.generic_string();
    j["scan"] = scan;
    j["scan_absolute"] = st.scanPath;
    j["open"] = paramsJson(st.openParams);
    json steps = json::array();
    for (const auto& [id, params] : st.log) steps.push_back({{"command", id}, {"params", paramsJson(params)}});
    j["steps"] = steps;
    if (auto parent = path.parent_path(); !parent.empty()) fs::create_directories(parent, ec);
    std::ofstream f(path, std::ios::binary);
    f << j.dump(2) << "\n";
    if (!f) throw std::runtime_error("couldn't write " + path.string());
}

std::shared_ptr<FitDoc> loadProject(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("can't read " + path.string());
    json j;
    try {
        j = json::parse(f);
    } catch (const std::exception& e) {
        throw std::runtime_error(path.string() + " isn't a fit project: " + e.what());
    }
    if (j.value("format", "") != "wetwaresuite-fit") throw std::runtime_error(path.string() + " isn't a fit project");
    fs::path scan = j.value("scan", "");
    if (scan.is_relative()) scan = fs::absolute(path).parent_path() / scan;
    if (!fs::exists(scan)) scan = j.value("scan_absolute", "");
    if (!fs::exists(scan)) {
        throw std::runtime_error("the project's scan " + j.value("scan", std::string()) + " is missing; put it next to the project file");
    }
    auto doc = openScan(scan.string(), paramsFrom(j.value("open", json::object())));
    std::vector<std::pair<std::string, Params>> log;
    for (const auto& s : j.value("steps", json::array())) log.emplace_back(s.value("command", ""), paramsFrom(s.value("params", json::object())));
    doc->state = replay(doc->state, log);
    doc->path = path.string();
    return doc;
}

}  // namespace detail

void registerCommands(Session& session) {
    detail::registerScan(session);
    detail::registerMeasure(session);
    detail::registerRefs(session);
    detail::registerParts(session);
    detail::registerPresets(session);
    detail::registerOutput(session);
}

}  // namespace wetwaresuite::fit
