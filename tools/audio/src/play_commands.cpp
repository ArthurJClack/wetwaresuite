// audio.play and the transport (pause, stop, seek...): hear a clip, a project
// mix or a file through the speakers, headless or from the window.
#include "common.hpp"
#include "wetwaresuite/playback/commands.hpp"

namespace wetwaresuite::audio::tool {

void registerPlayCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    std::vector<ParamSpec> params = {
        {"doc", ParamType::String, "an open audio document, e.g. aud1", false},
        {"project", ParamType::String, "an open project to play the mix of, e.g. proj1", false},
        {"path", ParamType::String, "or an audio file to play without opening it", false}};
    for (auto& p : playback::playParams()) params.push_back(p);

    reg.add({"audio.play",
             "Play",
             "Play audio through the speakers: an open clip (doc), a project's mix (project) or a file (path), from "
             "start to end, optionally looping. Returns at once (wait=true blocks until done); audio.playhead says where "
             "it is, audio.pause/resume/stop/seek control it. From the command line the program stays open until the "
             "sound finishes. With no sound card it plays silently so scripts still work.",
             params, false, true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    try {
                        std::shared_ptr<const Buffer> buffer;
                        std::string source;
                        if (p.count("doc")) {
                            source = stringParam(p, "doc");
                            auto doc = s->workspace().getAs<AudioDoc>(source);
                            if (!doc) return failed("no audio document " + source + " (open one with audio.open)");
                            buffer = std::make_shared<const Buffer>(doc->audio);
                        } else if (p.count("project")) {
                            source = stringParam(p, "project");
                            auto doc = s->workspace().getAs<ProjectDoc>(source);
                            if (!doc) return failed("no audio project " + source);
                            if (doc->project.seconds() <= 0) return failed("the project has no clips yet (add some with audio.clip_add)");
                            buffer = std::make_shared<const Buffer>(mixdown(doc->project));
                        } else if (p.count("path")) {
                            const fs::path path = stringParam(p, "path");
                            source = path.filename().string();
                            buffer = std::make_shared<const Buffer>(load(path));
                        } else {
                            return failed("say what to play: doc=aud1, project=proj1 or path=file.wav");
                        }
                        return playback::startPlayback(std::move(buffer), source, p);
                    } catch (const std::exception& e) {
                        return failed(e.what());
                    }
                });
            });

    playback::registerTransportCommands(session, "audio");
}

}  // namespace wetwaresuite::audio::tool
