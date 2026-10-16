// Helpers shared by the fit.* command files (internal to the fit tool).
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/fit/fit.hpp"

namespace wetwaresuite::fit::detail {

// An edit: changes a fit document, gets an undo step and a line in the
// document's log (so projects and fit.rebuild can replay it).
using Handler = std::function<Result(FitDoc&, const Params&)>;
std::map<std::string, Handler>& handlers();
void addEdit(Session& session, CommandInfo info, Handler fn);
// A command that reads a document (or writes files) without changing it.
void addRead(Session& session, CommandInfo info, Handler fn);

// Runs `log` on a fresh state made from the opened scan. Throws std::runtime_error naming the failing step.
FitState replay(const FitState& from, const std::vector<std::pair<std::string, Params>>& log);

// Opening and projects.
std::shared_ptr<FitDoc> openScan(const std::string& path, const Params& p);
void saveProject(const FitDoc& doc, const std::filesystem::path& path);
std::shared_ptr<FitDoc> loadProject(const std::filesystem::path& path);

// ---- params ----
ParamSpec docParam();
ParamSpec nameParam(const std::string& what);
std::vector<ParamSpec> withPreview(std::vector<ParamSpec> ps);
std::string num(double v);
std::string vecText(const Vec3& v);
std::optional<Vec3> vec3Param(const Params& p, const std::string& key);
Vec3 requireVec3(const Params& p, const std::string& key);  // throws std::runtime_error
std::string lowerExt(const std::filesystem::path& p);
std::string joinList(const std::vector<std::string>& items);
NumberList toList(const Vec3& v);

// ---- results ----
Result& addPreview(Result& r, const FitDoc& doc, const Params& p, const SceneOptions& so = {});
raster::Image renderScene(const FitDoc& doc, const Params& p, int size, const SceneOptions& so);
Result done(const std::string& message, const FitDoc& doc, const Params& p, const SceneOptions& so = {});

// ---- the document ----
// Replaces the scan (new version, so the index rebuilds).
void setScan(FitDoc& doc, Mesh m);
// Applies a transform to the scan, every reference and every part.
void transformAll(FitDoc& doc, const Mat4& m);
// Next free name like "plane3", or the given name (checked).
std::string newName(FitDoc& doc, const Params& p, const std::string& prefix);
const Ref& refOf(const FitDoc& doc, const std::string& name, RefKind kind);  // throws std::runtime_error
Part& partOf(FitDoc& doc, const std::string& name);                         // throws std::runtime_error
// The same, named by a string param.
const Ref& refParam(const FitDoc& doc, const Params& p, const char* key, RefKind kind);
Part& partParam(FitDoc& doc, const Params& p, const char* key);
std::string describeRef(const std::string& name, const Ref& r);
std::string describePart(const std::string& name, const Part& p);
void putPart(FitDoc& doc, const std::string& name, Mesh m, const std::string& kind, const Vec3& normal, double gap);

// The nearest point on the scan and its facing direction (from the triangles around it).
struct SurfacePoint {
    Vec3 point, normal;
    double distance = 0;
};
SurfacePoint snap(const FitDoc& doc, const Vec3& p);
std::vector<Vec3> scanVertexNormals(const FitDoc& doc);

// Registration, split so no function grows too big for every compiler.
void registerScan(Session& s);
void registerMeasure(Session& s);
void registerRefs(Session& s);
void registerParts(Session& s);
void registerPresets(Session& s);
void registerOutput(Session& s);

}  // namespace wetwaresuite::fit::detail
