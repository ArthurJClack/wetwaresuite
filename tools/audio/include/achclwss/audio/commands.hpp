// The audio editor as commands: open, generate, edit, clean up and measure
// recordings (aud1, aud2...), build multitrack projects (proj1...) and mix
// them down, and process whole folders. Everything runs headless; previews
// are waveform and spectrogram PNGs.
#pragma once

#include "achclwss/core/command.hpp"

namespace achclwss::audio {

// Registers every audio.* command.
void registerCommands(Session& session);

}  // namespace achclwss::audio
