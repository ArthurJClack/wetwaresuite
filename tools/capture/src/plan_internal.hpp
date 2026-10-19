// Pieces of the capture planner the scene recorder reuses.
#pragma once

#include <string>
#include <vector>

#include "wetwaresuite/capture/capture.hpp"

namespace wetwaresuite::capture {

// The capture method for the request (or the asked one, checked); empty with `error` set when there's none.
std::string pickBackend(const Environment& env, const std::string& asked, bool still, std::string& error);
// The ffmpeg input arguments that read the screen with `backend`, plus filters the picture needs
// (ddagrab's download, avfoundation's crop). duration 0 = no limit.
bool screenInput(const Environment& env, const std::string& backend, int monitor, const Region& asked, bool cursor, int fps,
                 double duration, bool audio, const std::string& audioDevice, std::vector<std::string>& args, std::vector<std::string>& filters,
                 std::string& error);

}  // namespace wetwaresuite::capture
