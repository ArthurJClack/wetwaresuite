// How an app binary decides between its window and its command line, plus the
// small bits of process handling that go with it (freeing the Windows console,
// starting another app). decideLaunch is pure so ctest covers it without a screen.
//
//   wetwaresuite-image                  window
//   wetwaresuite-image photo.jpg        window, opening photo.jpg (also what a drag onto the icon does)
//   wetwaresuite-image open a.png b.png window, opening both
//   wetwaresuite-image gui              window
//   wetwaresuite-image list / help / run / batch / mcp / <command> ...   command line, as before
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wetwaresuite::apps {

enum class LaunchKind { Cli, Gui };

struct Launch {
    LaunchKind kind = LaunchKind::Cli;
    std::vector<std::string> files;  // to open in the window
    std::string action;              // gui --action ID: start on this form
    std::string screenshot;          // gui --screenshot FILE: draw, save, quit
    std::string error;               // a bad window option, in plain words ("" = fine)
};

// The words that always mean the command line, whatever files lie around.
bool isCliKeyword(const std::string& arg);

// `isFile` says whether an argument names an existing file; `isCommand` whether
// it is one of this binary's command ids. Rules, in order:
//   no arguments                       -> window
//   gui / open [options] [files]       -> window (options: --action ID, --screenshot FILE)
//   a CLI keyword or a command id      -> command line
//   every argument is an existing file -> window opening them (drag and drop onto the exe)
//   anything else                      -> command line (which explains the mistake)
// macOS's old "-psn_..." Finder argument is ignored.
Launch decideLaunch(const std::vector<std::string>& args, const std::function<bool(const std::string&)>& isFile,
                    const std::function<bool(const std::string&)>& isCommand);

// Windows: when this process is the only one on its console (it was started by
// double-clicking in Explorer, so Windows made a console just for it), let go of
// it so the black box disappears and only the window stays. From a terminal the
// console is shared and kept, so printed messages still show. Elsewhere: nothing.
void releaseConsoleIfOwned();

// Starts a program without waiting for it and without tying it to this one
// (closing this app leaves it running). Returns "" or why it couldn't.
std::string spawnDetached(const std::filesystem::path& program, const std::vector<std::string>& args);

}  // namespace wetwaresuite::apps
