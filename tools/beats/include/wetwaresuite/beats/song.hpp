// The beats song model: tempo, time signature, swing, tracks (synth, drum
// kit, pitched sampler), patterns (step grids and piano-roll notes) and an
// arrangement that places patterns on tracks by bar. Rendering, MIDI files,
// the generator and the preview pictures all work on this plain data, so they
// can be tested without a Session.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/audio/buffer.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::beats {

// ---- notes ----
// "C4" = 60, "A4" = 69 (440 Hz), "C#3", "Eb5", "c-1" = 0; a bare number is a MIDI note. -1 when unknown.
int parseNote(const std::string& text);
std::string noteName(int midi);  // 61 -> "C#4"
double noteFrequency(double midi);

struct Note {
    int pitch = 60;        // MIDI note number
    double start = 0;      // beats (quarter notes) from the pattern start
    double length = 1;     // beats
    int velocity = 100;    // 1..127
};

// "C4 0 1 100, E4 1 0.5; 64:2:1" -> notes. Each note is pitch, start, length and an
// optional velocity separated by spaces or ':'; notes are separated by ',', ';' or new lines.
bool parseNotes(const std::string& text, std::vector<Note>& out, std::string& error);
std::string formatNotes(const std::vector<Note>& notes);

// Chord symbols: "C", "Am", "F#m7", "Bbmaj7", "Gsus4", "Edim", "Caug", "D7", "Cadd9", "Am/E".
// Returns MIDI notes with the root in `octave` (4 = middle C's octave); empty when unknown.
std::vector<int> chordNotes(const std::string& symbol, int octave = 3);

// ---- tracks ----
enum class TrackKind { Synth, Drum, Sampler };
std::string toString(TrackKind k);
bool parseTrackKind(const std::string& text, TrackKind& out);

enum class Osc { Saw, Square, Sine, Triangle, Noise, Off };
std::string toString(Osc o);
bool parseOsc(const std::string& text, Osc& out);
const std::vector<std::string>& oscNames();

struct SynthSettings {
    Osc osc1 = Osc::Saw;
    Osc osc2 = Osc::Off;
    double osc2Semitones = 0;  // osc2 pitch offset
    double detune = 0;         // cents between osc1 and osc2 (and unison spread)
    double osc2Level = 0.5;    // 0..1
    int octave = 0;            // whole-instrument shift
    double attack = 0.005;     // seconds
    double decay = 0.2;
    double sustain = 0.7;      // 0..1
    double release = 0.15;
    double cutoff = 4000;      // Hz, low-pass
    double resonance = 0.2;    // 0..1
    double filterEnv = 0;      // octaves the filter opens at a note's start
    double filterDecay = 0.25; // seconds the filter envelope takes to close
};
// bass, lead, pad, pluck, keys, sub, brass.
const std::vector<std::string>& presetNames();
bool preset(const std::string& name, SynthSettings& out);

// A loaded sample shared by copies of the song (undo snapshots).
struct Sample {
    std::string path;                            // as given (or as read from the song file)
    std::shared_ptr<const audio::Buffer> audio;  // mono, at the song's rate; null when it couldn't load
};

struct Effects {
    double distortion = 0;       // drive 0..1
    double compress = 0;         // amount 0..1 (threshold and ratio together)
    double delayMix = 0;         // 0..1
    double delayBeats = 0.75;    // echo time in beats
    double delayFeedback = 0.35; // 0..0.95
    double reverb = 0;           // wet 0..1
    double reverbSize = 0.6;     // 0..1
    bool any() const { return distortion > 0 || compress > 0 || delayMix > 0 || reverb > 0; }
};

struct Track {
    std::string name;
    TrackKind kind = TrackKind::Synth;
    std::string presetName;  // last preset applied, informational
    SynthSettings synth;     // synth voice; attack/release also shape the sampler
    std::map<std::string, Sample> slots;  // drum lanes played from samples (overrides the built-in voice)
    Sample sample;                        // sampler track
    int root = 60;                        // sampler: the note the sample plays at its own pitch
    double volumeDb = 0;
    double pan = 0;  // -1..1
    bool mute = false;
    bool solo = false;
    Effects fx;
};

// Built-in drum voices: kick, snare, hat, openhat, clap, tom, hitom, lotom, rim, crash, cowbell, shaker.
const std::vector<std::string>& drumVoices();
bool isDrumVoice(const std::string& name);
// General MIDI percussion numbers (kick 36, snare 38, hat 42...) and back (nearest voice).
int drumMidi(const std::string& voice);
std::string drumVoiceForMidi(int midi);
// One hit of a built-in voice, mono.
audio::Buffer drumHit(const std::string& voice, int rate);

// ---- patterns and the arrangement ----
struct Pattern {
    std::string name;
    int bars = 1;
    int stepsPerBeat = 4;  // step grid resolution (4 = 16th notes)
    // Lane name -> steps. Drum tracks: a voice ("kick") or a sample slot. Synth and sampler
    // tracks: a note ("C2"). 'x' hit, 'X' accent, 'o' soft, '.' or '-' rest; spaces and '|' ignored.
    std::vector<std::pair<std::string, std::string>> lanes;
    std::vector<Note> notes;  // piano roll; on drum tracks GM drum numbers pick the voice
};
// Steps with spaces and bars removed; false when a character isn't one of x X o . -
bool cleanSteps(const std::string& text, std::string& out, std::string& error);
int stepVelocity(char c);  // 'x' 100, 'X' 127, 'o' 60, else 0

struct Placement {
    int track = 0;
    std::string pattern;
    int bar = 0;     // from 0
    int repeat = 1;  // times in a row
};

struct Song {
    std::string name = "song";
    double bpm = 120;
    int beatsPerBar = 4;  // time signature top
    int beatUnit = 4;     // time signature bottom
    double swing = 0;     // 0..1: delays every second step by up to half a step (0.33 ~ triplet feel)
    int rate = 44100;
    double masterDb = 0;
    std::string key;  // informational, set by generate
    std::vector<Track> tracks;
    std::vector<Pattern> patterns;
    std::vector<Placement> arrangement;

    double beatsInBar() const { return beatsPerBar * 4.0 / beatUnit; }  // in quarter notes
    double secondsPerBeat() const { return 60.0 / bpm; }
    double barSeconds() const { return beatsInBar() * secondsPerBeat(); }
    int bars() const;  // end of the last placement
    double seconds() const { return bars() * barSeconds(); }
    Pattern* findPattern(const std::string& name);
    const Pattern* findPattern(const std::string& name) const;
};

// Index of a track by number or by name (case-insensitive); -1 when there's none.
int findTrack(const Song& s, const std::string& ref);

// ---- playing ----
struct Event {
    double beat = 0;    // from the song start, swing applied
    double length = 0;  // beats
    int pitch = 60;     // synth/sampler: MIDI note; drum: GM number when `voice` is empty
    int velocity = 100;
    std::string voice;  // drum lane name (voice or slot)
};
// Every note and hit a track plays, in time order.
std::vector<Event> trackEvents(const Song& s, int track);
// Every note and hit a pattern plays on a track of the given kind, from its start, without swing.
std::vector<Event> patternEvents(const Song& s, const Pattern& p, TrackKind kind);

struct RenderOptions {
    int startBar = 0;
    int bars = -1;            // -1 = to the end of the arrangement
    double tailSeconds = -1;  // let notes and reverb ring past the end; -1 = automatic (up to 3 s)
    bool limit = true;        // brickwall limiter at -1 dBFS on the master
    int onlyTrack = -1;       // render one track alone (ignores mute/solo)
};
// Stereo mix of the arrangement.
audio::Buffer render(const Song& s, const RenderOptions& o = {});
// One synth note, mono (exposed for tests and the sampler preview).
audio::Buffer synthNote(const SynthSettings& st, int rate, double frequency, double seconds, double velocity);

// ---- files ----
std::string toJson(const Song& s, const std::filesystem::path& songFile = {});
Song fromJson(const std::string& text, const std::filesystem::path& songFile = {});  // throws std::runtime_error
void saveSong(const Song& s, const std::filesystem::path& path);
Song loadSong(const std::filesystem::path& path);
// Loads an audio file as a mono sample at `rate` (throws).
Sample loadSample(const std::string& path, int rate, const std::filesystem::path& relativeTo = {});

// Standard MIDI File (format 1, 480 ticks per beat): a tempo track, then one track per song track;
// drum tracks on channel 10 with General MIDI numbers.
std::string exportMidi(const Song& s);
// Format 0 or 1. Each MIDI channel becomes a track (channel 10 a drum track) holding one pattern.
Song importMidi(const std::string& bytes, const std::string& name = "midi");  // throws std::runtime_error

// ---- generator ----
// house, techno, hiphop, trap, lofi, pop, rock, dnb, reggaeton, ambient.
const std::vector<std::string>& genreNames();
struct GenerateOptions {
    std::string genre = "house";
    std::string key = "C minor";  // "A minor", "Am", "F# major", "Eb"
    unsigned seed = 1;
    int bars = 8;
    double bpm = 0;  // 0 = the genre's tempo
    bool drums = true, bass = true, chords = true, melody = true;
};
// Adds tracks, patterns and placements to `s` (whose tempo/swing/key it sets). Returns a summary.
std::string generate(Song& s, const GenerateOptions& o);
// "A minor" -> root 9 (A), minor. False when unknown.
bool parseKey(const std::string& text, int& root, bool& minor);

// ---- pictures ----
raster::Image drawArrangement(const Song& s, int width = 1200);
raster::Image drawPattern(const Song& s, const Pattern& p, TrackKind kind, int width = 1200);

}  // namespace wetwaresuite::beats
