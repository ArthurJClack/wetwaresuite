// Helpers shared by the files that register maps.* commands (commands.cpp,
// edit_commands.cpp, track_commands.cpp, nav_commands.cpp): parameters,
// layer lookups, results and the undoable MapEdit wrapper.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "internal.hpp"

namespace wetwaresuite::maps::cmd {

using Edit = std::function<Result(MapDoc&, const Params&)>;

bool has(const Params& p, const std::string& key);
std::string plural(std::size_t n, const std::string& word);
std::string metres(double m);
std::string squareMetres(double a);
std::string duration(double s);
std::vector<std::string> splitList(const std::string& text, char sep = ',');
// "name=Cafe; rating=4.5" or "name=Cafe, rating=4.5"
Props parseProps(const std::string& text);

ParamSpec docParam();
ParamSpec previewParam();
ParamSpec layerParam(const std::string& what = "layer name or index (0 = bottom); default: the top vector layer");
ParamSpec outputParam(const std::string& what);
ParamSpec whereParam(const std::string& what = "only features matching this expression, e.g. \"pop_est > 1e7 and continent == 'Europe'\"");
ParamSpec crsParam(const std::string& what);
std::string crsHelp();
std::optional<Crs> crsFrom(const Params& p, const std::string& key, std::string& error);
std::optional<BBox> bboxFrom(const Params& p, const std::string& key, std::string& error);
std::string bboxText(const BBox& b);

// The vector layer a command works on: "layer", else the top vector layer.
Layer* vectorLayer(Map& m, const Params& p, std::string& error, const std::string& key = "layer");
// Without layer=, the top vector layer holding any feature of this type (falls back to the top vector layer).
Layer* layerWith(Map& m, const Params& p, std::string& error, GeomType type);
Layer* anyLayer(Map& m, const Params& p, std::string& error);
std::optional<Expr> whereFrom(const Params& p, std::string& error, const std::string& key = "where");
std::string unknownFieldsNote(const Layer& l, const Expr& e);

Result& maybePreview(Result& r, const MapDoc& doc, const Params& p);
Result edited(const std::string& message, const Params& p);
// Runs `edit` on the map named by doc=: inside a MapEdit (one undo step) when undoable.
ActionFactory mapCommand(Session& session, Edit edit, bool undoable = true);
ActionFactory plainCommand(std::function<Result(const Params&)> fn);

std::string layerLine(const Layer& l, std::size_t i);
std::string geometryLine(const Geometry& g);
std::string propsLine(const Props& props, const std::vector<std::string>& only, std::size_t maxLen = 400);
Result docCreated(Session& session, std::shared_ptr<MapDoc> doc, const std::string& verb, const Params& p, const std::vector<std::string>& warnings = {});
// Adds a new layer named after `name` (made unique) and returns it.
Layer& addLayer(Map& m, Layer l, const std::string& name);

std::vector<ParamSpec> viewParams();
bool viewOptions(const MapDoc& doc, const Params& p, RenderOptions& o, std::string& error);

// A feature by index= in a layer; error says the valid range.
Feature* featureAt(Layer& l, const Params& p, std::string& error, const std::string& key = "index");
// "lat,lon" from a NumberList param; false (with error) when missing or not two numbers.
bool latLon(const Params& p, const std::string& key, Coord& out, std::string& error);
// The name-like property of a feature ("name", "NAME", "title", ...), empty when none.
std::string featureName(const Feature& f);

// Command groups registered by the other files.
void registerEditCommands(Session& session);
void registerOverlayCommands(Session& session);
void registerTrackToolCommands(Session& session);
void registerNavCommands(Session& session);
void registerPlaceCommands(Session& session);

}  // namespace wetwaresuite::maps::cmd
