# Beats: make music from the command line

`beats` is the suite's music maker: a step sequencer, a piano roll, synths, a drum machine and a sampler, with no GUI and no plugins to install. A song is a plain document you build with commands, so you can write one in a script, generate one from a genre and a key, or ask Claude to make one with you.

- **Songs** have a tempo, a time signature (4/4, 3/4, 6/8, 7/8...) and swing.
- **Tracks** play one of three instruments:
  - **synth**: a built-in subtractive synth with two oscillators (saw, square, sine, triangle, noise), an ADSR envelope, and a resonant low-pass filter with its own envelope. Presets: `bass`, `lead`, `pad`, `pluck`, `keys`, `sub`, `brass`.
  - **drum**: a synthesized kit (`kick`, `snare`, `hat`, `openhat`, `clap`, `tom`, `hitom`, `lotom`, `rim`, `crash`, `cowbell`, `shaker`) plus sample slots, so any lane can play a WAV, MP3, FLAC or Ogg file.
  - **sampler**: plays one sample at the pitch of each note.
- **Patterns** hold step-sequencer lanes (`kick: x...x...x...x...`) and piano-roll notes (`C4 0 1, E4 1 1`). You can also type chord symbols (`Am F C G`).
- **The arrangement** places patterns on tracks by bar.
- **Mixing**: per-track volume, pan, mute and solo, plus four effects on every track: distortion, compression, delay and reverb. A limiter on the master stops clipping.
- **Output**: WAV built in; MP3, Ogg, FLAC, Opus and M4A when ffmpeg is installed. Single tracks render as stems. `beats.bounce` hands the mix to the [audio](audio.md) editor for mastering.
- **MIDI**: Standard MIDI Files, both export and import, so you can move parts to and from any DAW.
- **beats.generate** builds a starting point from a genre, a key and a seed: drums, bass, chords and a melody.
- **Pictures**: the arrangement, a pattern's step grid and piano roll, and the waveform, all as PNG. You and Claude can see what you're building.

`beats.play` plays the song (or a few bars, or one track) through your speakers, and can loop while you edit (see [playback](playback.md)). Each task below is shown three ways:

- **CLI**: single `wetwaresuite` commands. Songs only live inside one process, so the one-shot commands that read a song take `input=song.beats.json` (or `input=song.mid`) in place of `song=`, and `beats.generate` can `save=` straight to a file.
- **Batch**: a script for step-by-step building, with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools beats` serves just these tools, plus the audio tools so bounced mixes can be edited.

Every example was run against the real binary and the outputs are copied from those runs. Long outputs are shortened where marked with `...`; batch outputs show only the first line of each result.

## How beats works

**Songs and ids.** `beats.new`, `beats.open`, `beats.midi_import` and `beats.generate` each make a song document with an id such as `song1`. The editing commands take `song=song1`. Each edit is one undo step (`undo`, `redo`). `beats.save` writes a `.beats.json` file that you can reopen, diff and edit by hand. Samples are referenced by path, written relative to the song file when they sit next to it.

**Time.** Bars are numbered from 0. Inside a pattern, notes are placed in **beats**, counted in quarter notes from the pattern's start: `start=0 length=1` is a quarter note on the downbeat and `start=2.5 length=0.5` is an eighth note on the "and" of 3. `bpm` counts quarter notes per minute. In 6/8 a bar is 3 quarter notes long.

**Steps.** A step lane is a string with one character per step: `x` is a hit (velocity 100), `X` an accent (127), `o` a soft or ghost note (60), and `.` or `-` a rest. Spaces, `|` and `_` are ignored, so you can write `x... x... | x... x...`. `steps_per_beat=4` (the default) makes each step a 16th note, so a bar of 4/4 is 16 steps. A lane shorter than the pattern repeats: `hat: ..x.` puts a hat on every off-beat. On synth and sampler tracks a lane is named after a note (`C2: x---x---`) and plays that pitch, with `-` holding the note on.

**Tracks** can be named by number (`track=0`) or by name (`track=Drums`, case-insensitive). **Patterns** are named (`pattern=beat`) and can be placed on any number of tracks and bars. Editing a pattern changes every place it plays.

**Swing** (0..1) pushes every second step late by up to half a step: 0.3 is a light shuffle, about 0.66 is a full triplet swing. It applies to steps and to notes that sit exactly on those steps.

**Drum notes.** A drum track also plays piano-roll notes, using General MIDI drum numbers (36 kick, 38 snare, 42 hat...). That is how imported MIDI drums play.

## Task 1: a beat in one minute

Make a house track in A minor and get an MP3.

**CLI:**

```sh
$ wetwaresuite beats.generate genre=house key="A minor" seed=7 save=house.beats.json
made song1: house in A minor at 124 bpm, 8 bars: drums (4 lanes), bass, chords, melody; chords Am7 (i) - Fmaj7 (VI) - Cmaj7 (III) - G7 (VII); saved house.beats.json
song: song1
tracks: 4
patterns: 6
bars: 8
seconds: 15.4839
$ wetwaresuite beats.render input=house.beats.json path=house.mp3
rendered house.mp3: 0:17.400, peak -1 dBFS, -10.4 LUFS
path: house.mp3
seconds: 17.4
peak_db: -1
lufs: -10.3614
$ wetwaresuite beats.draw input=house.beats.json path=house.png
wrote house.png (arrangement: house: 124 bpm 4/4, 4 tracks, 6 patterns, 8 bars (0:15.484), house.beats.json)
```

The render runs about 2 seconds past the last bar. That's the reverb and the delay ringing out; silence after them is trimmed. Change `seed=` for a different groove, progression and melody in the same style. The same seed always gives the same song. `genre=` takes `house`, `techno`, `hiphop`, `trap`, `lofi`, `pop`, `rock`, `dnb`, `reggaeton` or `ambient`, and each genre sets its own tempo, swing, drum grooves, bass style and sounds. `bars=16` makes it longer. Multiples of 4 work best, because the chord progression is 4 bars. `bpm=` overrides the genre's tempo. `drums=false`, `bass=false`, `chords=false` and `melody=false` leave parts out.

`beats.info` shows what it made:

```sh
$ wetwaresuite beats.info input=house.beats.json
house: 124 bpm 4/4, 4 tracks, 6 patterns, 8 bars (0:15.484), house.beats.json, key A minor
track 0 "Drums" (drum): volume 0 dB, pan 0
  bar 0-2: house beat x3
  bar 3-3: house fill
  bar 4-6: house beat x3
  bar 7-7: house fill
  bar 4-4: crash
track 1 "Bass" (synth, bass): volume 0 dB, pan 0
  bar 0-7: bassline x2
track 2 "Chords" (synth, keys): volume -4 dB, pan 0, effects: reverb 0.25
  bar 0-7: chords x2
track 3 "Melody" (synth, pluck): volume -5 dB, pan 0, effects: delay 0.25 (0.75 beats) reverb 0.15
  bar 4-7: melody
pattern "house beat": 1 bar, 4 steps per beat
  kick: x...xo..x...x...
  clap: ....x.......x...
  hat: o.xo.ox.o.x...x.
  shaker: oxoooxoooxoooxoo
pattern "house fill": 1 bar, 4 steps per beat
...
pattern "bassline": 4 bars
  16 notes: A2 0.5 0.4 105, A2 1.5 0.4 105, A2 2.5 0.4 105, A3 3.5 0.4 105, F2 4.5 0.4 105, ...
pattern "melody": 4 bars
  12 notes: A4 0 1.5 105, C5 2.5 0.45 90, E5 3 0.95 90, F5 4 1.5 105, G5 6.5 0.45 90, ...
```

The melody comes in at bar 4 with a crash. The drums play a fill every fourth bar.

**Batch** (generate, look, change something, render):

```text
beats.generate genre=house key="A minor" seed=7 -> s
beats.draw song=$s path=house.png
beats.draw song=$s path=melody.png pattern=melody
beats.draw song=$s path=beat.png pattern="house beat"
beats.render song=$s path=house.wav
beats.draw song=$s path=wave.png view=waveform
```

```sh
$ wetwaresuite batch t1.txt
made song1: house in A minor at 124 bpm, 8 bars: drums (4 lanes), bass, chords, melody; chords Am7 (i) - Fmaj7 (VI) - Cmaj7 (III) - G7 (VII)
wrote house.png (arrangement: house: 124 bpm 4/4, 4 tracks, 6 patterns, 8 bars (0:15.484))
wrote melody.png (pattern melody: 0 step lanes, 12 notes)
wrote beat.png (pattern house beat: 4 step lanes, 0 notes)
rendered house.wav: 0:17.400, peak -1 dBFS, -10.4 LUFS
wrote wave.png (waveform of 0:17.400)
```

`house.png` is the arrangement view: one lane per track, a grid line every bar (brighter every 4 bars), drum hits as ticks (one row per voice), and notes as bars whose height is their pitch. `melody.png` is the piano roll, with black-key rows shaded, a line under each C and a white edge at each note's start. `beat.png` is the step grid, with one row per lane and brighter cells for louder hits. Muted tracks are drawn grey.

**Claude:**

> Make me an 8-bar lo-fi beat in F major and render it to lofi.mp3. Show me what it looks like.

Claude calls `beats_generate` (`genre: "lofi", key: "F major", preview: true`) and sees the arrangement picture, then calls `beats_render` (`path: "lofi.mp3"`). If you say "too busy", it might drop the melody with `beats_arrange_remove`, or try `seed: 2`.

## Task 2: program a drum beat

A boom-bap beat at 90 bpm with a lazy swing, ghost notes and a fill every 4 bars.

**Batch** (`drums.txt`):

```text
# a hip-hop beat programmed by hand: 90 bpm with a lazy swing
beats.new name=boombap bpm=90 swing=0.3 -> song
beats.track_add song=$song kind=drum
beats.pattern_add song=$song name=beat steps="kick: x......x..x.....; snare: ....x.......x...; hat: x.x.x.x.x.x.x.x."
# accents and ghost notes: X is loud, o is soft
beats.pattern_steps song=$song pattern=beat lane=hat steps="X.x.X.x.X.x.X.xo"
beats.pattern_steps song=$song pattern=beat lane=snare steps="....x..o....x..o"
# a fill for the end of each phrase, copied from the beat and changed
beats.pattern_add song=$song name=fill steps="kick: x......x..x.....; snare: ....x.......xxXX; hat: x.x.x.x.x.x.....; tom: ........x.x....."
beats.arrange_place song=$song track=Drums pattern=beat bar=0 repeat=3
beats.arrange_place song=$song track=Drums pattern=fill
beats.arrange_place song=$song track=Drums pattern=beat repeat=3
beats.arrange_place song=$song track=Drums pattern=fill
beats.draw song=$song pattern=beat path=beat.png
beats.render song=$song path=boombap.wav
beats.save song=$song path=boombap.beats.json
```

```sh
$ wetwaresuite batch drums.txt
made song1: boombap: 90 bpm 4/4, 0 tracks, 0 patterns, 0 bars (0.000 s)
added track 0 "Drums" (drum)
added pattern "beat" (1 bar, 3 lanes, 0 notes)
pattern "beat":
  kick: x......x..x.....
  snare: ....x.......x...
  hat: X.x.X.x.X.x.X.xo
pattern "beat":
  kick: x......x..x.....
  snare: ....x..o....x..o
  hat: X.x.X.x.X.x.X.xo
added pattern "fill" (1 bar, 4 lanes, 0 notes)
placed "beat" on track 0 "Drums", bars 0-2
placed "fill" on track 0 "Drums", bars 3-3
placed "beat" on track 0 "Drums", bars 4-6
placed "fill" on track 0 "Drums", bars 7-7
wrote beat.png (pattern beat: 3 step lanes, 0 notes)
rendered boombap.wav: 0:21.562, peak -1 dBFS, -13.2 LUFS
saved boombap.beats.json
```

Without `bar=`, `beats.arrange_place` puts the pattern straight after the track's last placement, so a song can be laid out in order. A lane can be edited on its own (`lane=hat steps=...`), and `steps=""` removes it. `beats.pattern_clear pattern=beat lane=hat` does the same. Mistakes are caught:

```sh
$ echo 'beats.new -> s
beats.pattern_add song=$s name=p steps="kick: x..y"' | wetwaresuite batch
...
line 2: steps use x (hit), X (accent), o (soft) and . or - (rest); 'y' isn't one of them
```

**CLI**: a hand-written song is easiest as a batch script, or as a `.beats.json` file you write yourself (the format is shown under [Song files](#song-files)), rendered with `wetwaresuite beats.render input=boombap.beats.json path=boombap.wav`.

**Claude:**

> Make a 90 bpm boom-bap drum beat with swing, kick on 1 and the "and" of 2, snare on 2 and 4, ghost snares, and a fill every 4th bar. Show me the pattern.

Claude calls `beats_new` (`bpm: 90, swing: 0.3`), `beats_track_add` (`kind: "drum"`), `beats_pattern_add` with the `steps` lanes and `preview: true` (the step grid comes back as an image), then `beats_arrange_place` once per section.

## Task 3: bass, chords and a melody

Put a band on top of the drum beat: keys playing chord symbols, a bass line in the piano roll, and a lead in the second half.

**Batch** (`band.txt`):

```text
# bass, chords and a melody on top of the drum beat
beats.open path=boombap.beats.json -> song
# chords: one per bar, Dm7 - G7 - Cmaj7 - Am7, played by the keys preset
beats.track_add song=$song kind=synth preset=keys name=Keys volume=-6
beats.pattern_add song=$song name=changes chords="Dm7 G7 Cmaj7 Am7" octave=3 track=Keys bar=0 repeat=2
# a bass line in the piano roll: note, start beat, length in beats, velocity
beats.track_add song=$song kind=synth preset=bass name=Bass
beats.pattern_add song=$song name=walk track=Bass bar=0 repeat=2 notes="D2 0 1.5 110, D2 1.75 0.25 80, F2 2 1, A2 3 1, G1 4 1.5 110, G1 5.75 0.25 80, B1 6 1, D2 7 1, C2 8 1.5 110, C2 9.75 0.25 80, E2 10 1, G2 11 1, A1 12 1.5 110, A1 13.75 0.25 80, C2 14 1, E2 15 1"
# a lead for the second half
beats.track_add song=$song kind=synth preset=lead name=Lead volume=-8 pan=0.3
beats.pattern_add song=$song name=hook notes="A4 0 0.5, C5 0.5 0.5, D5 1 1, F5 2.5 0.5, E5 3 1, D5 4 0.5, B4 4.5 0.5, G4 5 2, E5 8 1, D5 9 0.5, C5 9.5 1.5, E5 12 0.5, D5 12.5 0.5, C5 13 0.5, A4 13.5 2.5"
beats.arrange_place song=$song track=Lead pattern=hook bar=4
beats.info song=$song
beats.draw song=$song path=band.png
beats.draw song=$song pattern=walk path=walk.png
beats.render song=$song path=band.mp3
beats.save song=$song path=band.beats.json
```

```sh
$ wetwaresuite batch band.txt
opened song1: boombap: 90 bpm 4/4, 1 tracks, 2 patterns, 8 bars (0:21.333), boombap.beats.json
added track 1 "Keys" (synth, keys)
added pattern "changes" (4 bars, 0 lanes, 16 notes), placed on track 1 at bar 0 x2
added track 2 "Bass" (synth, bass)
added pattern "walk" (4 bars, 0 lanes, 16 notes), placed on track 2 at bar 0 x2
added track 3 "Lead" (synth, lead)
added pattern "hook" (4 bars, 0 lanes, 15 notes)
placed "hook" on track 3 "Lead", bars 4-7
boombap: 90 bpm 4/4, 4 tracks, 5 patterns, 8 bars (0:21.333), boombap.beats.json, swing 0.3
...
pattern "changes": 4 bars
  16 notes: D3 0 4 90, F3 0 4 90, A3 0 4 90, C4 0 4 90, G3 4 4 90, B3 4 4 90, D4 4 4 90, F4 4 4 90, ...
...
rendered band.mp3: 0:21.702, peak -1 dBFS, -12.4 LUFS
saved band.beats.json
```

Things to know about notes and chords:

- A note is `pitch start length [velocity]`. Pitch is a name (`C4` is middle C, `F#3`, `Bb2`, `C-1` is MIDI 0) or a MIDI number (`60`). Notes are separated by commas, semicolons or new lines, and `C4:0:1:100` works as well as spaces.
- `chords=` takes symbols like `C`, `Am`, `F#m7`, `Bbmaj7`, `D7`, `Gsus4`, `Edim`, `Caug`, `Cadd9`, `C6`, `Am9` and slash chords like `C/E`. Each chord lasts `chord_beats=` beats (default one bar), and `-` leaves a gap. `octave=3` puts the roots in the octave below middle C.
- A pattern grows to fit its notes. `beats.pattern_notes` adds more notes (`mode=replace` swaps them all), `transpose=-2` shifts everything, and `beats.pattern_clear what=notes from=8 to=12` erases a range.
- The pattern's own `bars` is its loop length. `beats.pattern_set pattern=walk bars=2` shortens it, and notes past the end don't play.

**CLI:** an existing song can be changed in one go with a short batch piped in:

```sh
$ echo 'beats.open path=band.beats.json -> s
beats.pattern_notes song=$s pattern=changes transpose=2
beats.save song=$s path=band-up.beats.json' | wetwaresuite batch
```

**Claude:**

> Add a jazzy keys part playing Dm7 G7 Cmaj7 Am7, one chord a bar, and a walking bass that follows it. Then a short lead hook for the last 4 bars. Show me the piano roll for the bass.

Claude calls `beats_track_add` (`preset: "keys"`), then `beats_pattern_add` with `chords` and `track`, writes the bass notes itself into `beats_pattern_add`'s `notes` with `preview: true` to check the piano roll, and adds the lead with `beats_arrange_place` at `bar: 4`.

## Task 4: shape the sounds, add effects and mix

**Batch** (`sound.txt`):

```text
# shape the sounds of the band song: a darker bass, an acid lead, space on the keys
beats.open path=band.beats.json -> song
beats.track_set song=$song track=Bass cutoff=250 resonance=0.5 filter_env=3 filter_decay=0.12
beats.track_set song=$song track=Lead osc1=square osc2=saw detune=20 attack=0.02 release=0.4 cutoff=1200 resonance=0.7 filter_env=2.5
beats.effect song=$song track=Lead type=distortion amount=0.3
beats.effect song=$song track=Lead type=delay amount=0.3 time=0.75 feedback=0.4
beats.effect song=$song track=Keys type=reverb amount=0.35 size=0.8
beats.effect song=$song track=Drums type=compress amount=0.5
# hear the lead on its own, then everything
beats.render song=$song track=Lead start_bar=4 bars=2 path=lead-solo.wav
beats.track_set song=$song track=Keys mute=true
beats.render song=$song path=no-keys.wav
undo
beats.render song=$song path=band2.wav
beats.save song=$song path=band2.beats.json
```

```sh
$ wetwaresuite batch sound.txt
opened song1: boombap: 90 bpm 4/4, 4 tracks, 5 patterns, 8 bars (0:21.333), band.beats.json
track 2 "Bass" updated
track 3 "Lead" updated
distortion 0.3 on track 3 "Lead"
delay 0.3 on track 3 "Lead"
reverb 0.35 on track 1 "Keys"
compress 0.5 on track 0 "Drums"
rendered lead-solo.wav: 0:07.933, peak -16 dBFS, -16.3 LUFS
track 1 "Keys" updated
rendered no-keys.wav: 0:23.933, peak -1 dBFS, -12.9 LUFS
undone
rendered band2.wav: 0:23.933, peak -1 dBFS, -12.6 LUFS
saved band2.beats.json
```

How the synth works: two oscillators (osc2 can be tuned with `osc2_semitones`, `detune`d in cents and mixed with `osc2_level`) feed a resonant low-pass filter. `filter_env` opens the filter that many octaves above `cutoff` at the start of each note and lets it close over `filter_decay` seconds. That one setting makes plucks, acid basses and brass. The ADSR envelope (`attack`, `decay`, `sustain`, `release`) shapes the volume. `preset=` resets all of these to the preset first, and the other settings in the same call apply on top. `beats.presets` lists the presets with their values.

Each track's effects run in the order distortion, compression, pan, delay (ping-pong across the stereo field, `time=` in beats so it follows the tempo), reverb. `amount=0` turns an effect off. `track=` on `beats.render` renders one track alone and ignores mute and solo. `solo=true` on any track silences the tracks that aren't soloed.

**CLI:** stems, one file per track, at equal lengths (`tail=3` fixes the ring-out instead of trimming it):

```sh
$ wetwaresuite beats.render input=band2.beats.json track=Drums tail=3 path=stems/drums.wav
rendered stems/drums.wav: 0:24.333, peak -1 dBFS, -14.7 LUFS
$ wetwaresuite beats.render input=band2.beats.json track=Bass tail=3 path=stems/bass.wav
rendered stems/bass.wav: 0:24.333, peak -5.52 dBFS, -20.2 LUFS
```

**Claude:**

> The lead is too clean. Make it a squelchy acid sound with a dotted-eighth delay, put the keys in a big room, and glue the drums with some compression. Render just the lead so I can hear it.

Claude calls `beats_track_set` for the lead's oscillators and filter, `beats_effect` three or four times, and `beats_render` with `track: "Lead"`.

## Task 5: your own samples

Any WAV, MP3, FLAC or Ogg file can be a drum sound or a pitched instrument. This script first makes two samples with the audio tool so that it runs anywhere. Use your own files instead.

**Batch** (`samples.txt`):

```text
# make two samples to play with (any wav, mp3, flac or ogg file works the same)
audio.new kind=sweep seconds=0.15 frequency=2000 to_frequency=200 channels=1 -> zap
audio.fade doc=$zap out=0.15
audio.save doc=$zap path=samples/zap.wav
audio.new kind=tone waveform=saw seconds=2 frequency=261.63 channels=1 level=-3 -> c4
audio.fade doc=$c4 out=1.5
audio.save doc=$c4 path=samples/saw-c4.wav

beats.new bpm=128 -> song
# drum kit with our sample on a new lane, and a sample replacing the built-in clap
beats.track_add song=$song kind=drum
beats.track_set song=$song track=Drums slot=zap sample=samples/zap.wav
beats.track_set song=$song track=Drums slot=clap sample=samples/zap.wav
beats.pattern_add song=$song name=beat track=Drums repeat=4 steps="kick: x...x...x...x...; clap: ....x.......x...; zap: ......x.......xx; openhat: ..x...x...x...x."
# a sampler track: the sample sounds unchanged at its root note and is pitched for others
beats.track_add song=$song kind=sampler name=Stab sample=samples/saw-c4.wav root=C4 release=0.2 volume=-6
beats.pattern_add song=$song name=stabs track=Stab repeat=2 notes="C4 0.5 0.25, Eb4 0.5 0.25, G4 0.5 0.25, C4 2.5 0.25, Eb4 2.5 0.25, G4 2.5 0.25, Bb3 4.5 0.25, D4 4.5 0.25, F4 4.5 0.25, Bb3 6.5 0.5, D4 6.5 0.5, F4 6.5 0.5"
beats.info song=$song
beats.render song=$song path=samples-demo.wav
beats.save song=$song path=samples-demo.beats.json
```

```sh
$ wetwaresuite batch samples.txt
...
added track 0 "Drums" (drum)
track 0 "Drums" updated
track 0 "Drums" updated
added pattern "beat" (1 bar, 4 lanes, 0 notes), placed on track 0 at bar 0 x4
added track 1 "Stab" (sampler)
added pattern "stabs" (2 bars, 0 lanes, 12 notes), placed on track 1 at bar 0 x2
song: 128 bpm 4/4, 2 tracks, 2 patterns, 4 bars (0:07.500)
track 0 "Drums" (drum): volume 0 dB, pan 0, slot clap = samples/zap.wav, slot zap = samples/zap.wav
  bar 0-3: beat x4
track 1 "Stab" (sampler): volume -6 dB, pan 0, sample samples/saw-c4.wav at C4
  bar 0-3: stabs x2
...
rendered samples-demo.wav: 0:08.100, peak -1 dBFS, -10.7 LUFS
saved samples-demo.beats.json
```

A slot named after a kit voice (`slot=clap`) replaces that voice, and any other name (`slot=zap`) adds a new lane. `sample=""` with `slot=` removes the slot. Samples are mixed down to mono at the song's rate when they're loaded. The sampler plays the sample faster or slower for each note, so pitch and length change together, like a tape. Its `attack` and `release` fade each note in and out, and a note stops when it ends or when the sample runs out. In the saved song, `samples/zap.wav` is stored relative to the song file, so the folder can be moved as a whole.

**Claude:**

> Use ~/samples/kick.wav as the kick and ~/samples/vox.wav as a sampler track rooted at A3, playing A3 C4 E4 on the off-beats.

Claude calls `beats_track_set` (`slot: "kick", sample: ...`), then `beats_track_add` (`kind: "sampler", sample: ..., root: "A3"`) and `beats_pattern_add` with `notes`.

## Task 6: MIDI in and out

**CLI:**

```sh
$ wetwaresuite beats.midi_export input=band2.beats.json path=band.mid
wrote band.mid (4 tracks, 1889 bytes)
$ wetwaresuite beats.render input=band.mid path=band.flac
rendered band.flac: 0:21.702, peak -1 dBFS, -12.4 LUFS
```

The second command renders any `.mid` file straight to audio with the built-in instruments. Channel 10 plays on the drum kit, and the other channels get a synth preset guessed from their General MIDI program.

**Batch** (`midi.txt`):

```text
# MIDI out to a DAW, and back in
beats.open path=band2.beats.json -> song
beats.midi_export song=$song path=band.mid
beats.midi_import path=band.mid -> back
beats.info song=$back
# swap the imported keys for a pad, slow it down, render
beats.track_set song=$back track=Keys preset=pad
beats.set song=$back bpm=80
beats.render song=$back path=band-from-midi.wav
```

```sh
$ wetwaresuite batch midi.txt
opened song1: boombap: 90 bpm 4/4, 4 tracks, 5 patterns, 8 bars (0:21.333), band2.beats.json
wrote band.mid (4 tracks, 1889 bytes)
imported song2: band: 90 bpm 4/4, 4 tracks, 4 patterns, 8 bars (0:21.333)
track 0 "Drums" (drum): volume 0 dB, pan 0
  bar 0-7: Drums
track 1 "Keys" (synth, keys): volume -6 dB, pan 0
  bar 0-7: Keys
track 2 "Bass" (synth, bass): volume 0 dB, pan 0
  bar 0-7: Bass
track 3 "Lead" (synth, lead): volume -8 dB, pan 0.302
  bar 0-7: Lead
pattern "Drums": 8 bars
  128 notes: F#2 0 0.25 127, C2 0 0.25 100, F#2 0.5 0.25 100, D2 1 0.25 100, ...
...
rendered band-from-midi.wav: 0:25.218, peak -1 dBFS, -12.3 LUFS
```

What goes into the file (format 1, 480 ticks per beat): the tempo, the time signature and the song name in a first track, then one MIDI track per song track. Each carries its name, its pan, its volume (muted tracks are written at volume 0) and, for synths, a General MIDI program matching the preset. Drum tracks go on channel 10 with GM drum numbers; sample slots with names that aren't kit voices go out as a rim shot. Swing is written into the note timings.

What comes back from a file: format 0 and 1 files are read, with running status and SysEx skipped. Tempo and time signature come from the first ones found. Each channel of each MIDI track becomes a song track holding one pattern with all its notes, placed at bar 0. So an imported song has its swing baked in (`swing` is 0), and its parts are long patterns rather than loops.

**Claude:**

> Here's song.mid. Load it, make the piano a pad, slow it to 80 bpm and render it.

Claude calls `beats_midi_import`, looks at the track list it returns, then calls `beats_track_set`, `beats_set` and `beats_render`.

## Task 7: master the mix in the audio editor

`beats.bounce` renders the song into an audio document (`aud1`), so every `audio.*` command can finish it.

**Batch** (`master.txt`):

```text
# bounce the song into the audio editor, master it to streaming loudness, save an mp3
beats.open path=band2.beats.json -> song
beats.bounce song=$song -> mix
audio.normalize doc=$mix mode=loudness target=-14 ceiling=-1
audio.fade doc=$mix out=2
audio.measure doc=$mix
audio.save doc=$mix path=band-master.mp3
# and stems: one file per track
beats.render song=$song track=Drums tail=3 path=stems/drums.wav
beats.render song=$song track=Bass tail=3 path=stems/bass.wav
beats.render song=$song track=Keys tail=3 path=stems/keys.wav
beats.render song=$song track=Lead tail=3 path=stems/lead.wav
```

```sh
$ wetwaresuite batch master.txt
opened song1: boombap: 90 bpm 4/4, 4 tracks, 5 patterns, 8 bars (0:21.333), band2.beats.json
bounced to aud1: 2 ch, 44100 Hz, 0:23.933
loudness -12.6 -> -14 LUFS (gain -1.4 dB), peak -2.4 dB
faded out over 0:02.000
aud1: duration 0:23.933, peak -2.4 dB (true peak -1.1 dB), RMS -15.9 dB, loudness -14 LUFS, range 2.04 LU
saved band-master.mp3 (0:23.933)
rendered stems/drums.wav: 0:24.333, peak -1 dBFS, -14.7 LUFS
rendered stems/bass.wav: 0:24.333, peak -5.52 dBFS, -20.2 LUFS
rendered stems/keys.wav: 0:24.333, peak -7.34 dBFS, -21.8 LUFS
rendered stems/lead.wav: 0:24.333, peak -16 dBFS, -16.6 LUFS
```

**Claude:**

> Master the song to -14 LUFS with a 2 second fade out and give me an mp3 and stems.

Claude calls `beats_bounce`, `audio_normalize`, `audio_fade` and `audio_save`, then `beats_render` once per track.

## Other time signatures

```text
# a waltz in 3/4: three beats a bar, so a bar of 16ths is 12 steps
beats.new bpm=150 time_signature=3/4 -> song
beats.track_add song=$song kind=drum
beats.pattern_add song=$song name=oom track=Drums repeat=4 steps="kick: x...........; hat: ....x...x...; rim: ....x...x..."
beats.track_add song=$song preset=keys
beats.pattern_add song=$song name=chords track=Keys repeat=1 chords="C G7 G7 C" octave=4
beats.render song=$song path=waltz.wav
```

```sh
$ wetwaresuite batch waltz.txt
made song1: song: 150 bpm 3/4, 0 tracks, 0 patterns, 0 bars (0.000 s)
added track 0 "Drums" (drum)
added pattern "oom" (1 bar, 3 lanes, 0 notes), placed on track 0 at bar 0 x4
added track 1 "Keys" (synth, keys)
added pattern "chords" (4 bars, 0 lanes, 14 notes), placed on track 1 at bar 0
rendered waltz.wav: 0:05.169, peak -1 dBFS, -12.8 LUFS
```

In 6/8 a bar is 3 quarter notes, so 12 steps at `steps_per_beat=4`, or `steps_per_beat=2` for one step per eighth note (6 steps). `steps_per_beat=3` gives a triplet grid. If a lane's length doesn't fit the pattern, the result says so (`note: lane hat has 17 steps but the pattern has 16...`).

## Asking Claude, in general

`wetwaresuite mcp` (or `--tools beats`) gives Claude every command as a tool (`beats_generate`, `beats_pattern_steps`...). Some things that help it:

- Most edits take `preview: true`. Track and arrangement edits return the arrangement picture, and pattern edits return that pattern's grid and piano roll. `beats_preview` draws a view on demand, and `view: "waveform"` renders the audio to show it.
- `beats_info` lists every track, placement, step lane and note as text, so Claude can read a song before changing it.
- `beats_presets` lists the presets, kit voices, effects and genres.
- Undo works step by step (`undo`), so "go back to the last version" is safe. Undoing a `beats_generate` that made a new song closes that song.
- Claude can't hear the result. It judges from the pictures, the levels that `beats_render` reports, and what you tell it.

Good prompts are concrete about the musical idea: "kick on every beat, open hat on the off-beats", "Am F C G, one chord a bar", "make the bass an octave lower" (Claude uses `beats_track_set` with `octave: -1`, or `beats_pattern_notes` with `transpose: -12`).

## Reference

All commands take `song=` (an open song id) unless noted. "file" commands also take `input=song.beats.json` or `input=song.mid` in place of `song=`.

### Songs and files

| command | what it does | key parameters |
| --- | --- | --- |
| `beats.new` | empty song | `name`, `bpm` (120), `time_signature` (4/4), `swing` (0..1), `rate` (44100) |
| `beats.open` | open `.beats.json` | `path`, `preview` |
| `beats.save` | write `.beats.json` | `path` (default: where it came from) |
| `beats.close` | close a song | `song` |
| `beats.info` | text listing of everything (file) | `preview` |
| `beats.set` | song settings | `bpm`, `time_signature`, `swing`, `master` (dB), `name`, `key` |
| `beats.presets` | built-in sounds and genres | none |
| `beats.generate` | starting point from a style | `genre`, `key` (C minor), `seed` (1), `bars` (8), `bpm`, `drums`/`bass`/`chords`/`melody` (true), `song` (add to it), `save` (write file) |

### Tracks and effects

| command | what it does | key parameters |
| --- | --- | --- |
| `beats.track_add` | new track | `kind` (synth, drum, sampler) plus any `beats.track_set` setting |
| `beats.track_set` | change a track | `track`, see the table below |
| `beats.track_remove` | delete a track and its placements | `track` |
| `beats.effect` | set one effect | `track`, `type` (distortion, compress, delay, reverb), `amount` 0..1, `time` (delay, beats), `feedback` (delay, 0..0.95), `size` (reverb, 0..1) |

| track setting | meaning | range |
| --- | --- | --- |
| `name` | track name | |
| `preset` | bass, lead, pad, pluck, keys, sub, brass (resets the synth first) | |
| `osc1`, `osc2` | saw, square, sine, triangle, noise, off | |
| `osc2_semitones`, `osc2_level`, `detune` | second oscillator pitch, level, detune in cents | -48..48, 0..1, -100..100 |
| `octave` | shift the instrument | -4..4 |
| `attack`, `decay`, `sustain`, `release` | volume envelope (seconds; sustain is a level) | 0..10, sustain 0..1 |
| `cutoff`, `resonance` | low-pass filter | 20..20000 Hz, 0..1 |
| `filter_env`, `filter_decay` | filter opens by octaves at note start, closes over seconds | -8..8, 0.005..10 |
| `volume`, `pan` | dB, position | -60..12, -1..1 |
| `mute`, `solo` | | true/false |
| `sample` | sampler: the file it plays; drum: with `slot=`, the lane's file (`""` removes) | wav, mp3, flac, ogg |
| `slot` | drum lane for `sample=` | a kit voice or any name |
| `root` | sampler: note the sample plays unchanged | e.g. C4 |

### Patterns and the arrangement

| command | what it does | key parameters |
| --- | --- | --- |
| `beats.pattern_add` | new pattern, optionally placed | `name`, `bars`, `steps_per_beat` (4), `steps`, `notes`, `chords`, `chord_beats`, `octave` (3), `velocity` (90), `at`, `track`, `bar`, `repeat` |
| `beats.pattern_set` | rename, resize | `pattern`, `name`, `bars`, `steps_per_beat` |
| `beats.pattern_steps` | set step lanes | `pattern`, `lane` + `steps`, or `steps="kick: ...; hat: ..."` |
| `beats.pattern_notes` | add or replace notes | `pattern`, `notes`, `chords`, `mode` (add, replace), `transpose`, `at` |
| `beats.pattern_clear` | empty a pattern | `pattern`, `what` (all, steps, notes), `lane`, `from`/`to` (beats) |
| `beats.pattern_remove` | delete a pattern and its placements | `pattern` |
| `beats.arrange_place` | put a pattern on a track | `track`, `pattern`, `bar` (default: after the last), `repeat` |
| `beats.arrange_remove` | take placements out | `track` + `bar`, `pattern`, or `all=true` |

| step character | meaning |
| --- | --- |
| `x` | hit, velocity 100 |
| `X` | accent, velocity 127 |
| `o` | soft / ghost, velocity 60 |
| `.` | rest |
| `-` | rest; on synth lanes, holds the previous note |
| space, `\|`, `_` | ignored (for readability) |

### Output and pictures

| command | what it does | key parameters |
| --- | --- | --- |
| `beats.render` | mix to an audio file (file) | `path` (.wav built in; .mp3 .ogg .flac .opus .m4a via ffmpeg), `start_bar`, `bars`, `track`, `tail` (seconds), `limit` (true), `bits` (16, 24, 32, 32f), `bitrate`, `preview` |
| `beats.bounce` | mix into an audio document `aud1` | `start_bar`, `bars`, `track`, `tail`, `limit` |
| `beats.midi_export` | write `.mid` (file) | `path` |
| `beats.midi_import` | `.mid` to a new song | `path` |
| `beats.preview` | attach a picture (file) | `view` (arrangement, pattern, waveform), `pattern`, `width`, waveform: `start_bar`, `bars`, `track` |
| `beats.draw` | write that picture to a PNG (file) | `path` plus the `beats.preview` parameters |

### Built-in sounds

| drum voice | GM note | | drum voice | GM note |
| --- | --- | --- | --- | --- |
| kick | 36 | | lotom | 43 |
| snare | 38 | | rim | 37 |
| hat | 42 | | crash | 49 |
| openhat | 46 | | cowbell | 56 |
| clap | 39 | | shaker | 70 |
| tom | 47 | | hitom | 50 |

| genre | tempo | swing | feel |
| --- | --- | --- | --- |
| house | 124 | 0 | four on the floor, off-beat bass, chord stabs |
| techno | 130 | 0 | four on the floor, rolling 16th bass |
| hiphop | 90 | 0.2 | boom-bap, syncopated sub bass, held seventh chords |
| trap | 140 | 0 | 16th hats, long sub notes, pad |
| lofi | 78 | 0.45 | lazy swing, seventh chords on keys |
| pop | 110 | 0 | eighth-note bass, chords on every beat |
| rock | 120 | 0 | eighth-note bass, brass chords, crash on 1 |
| dnb | 174 | 0 | two-step breakbeat, pad |
| reggaeton | 95 | 0 | dembow snare |
| ambient | 70 | 0 | sparse percussion, arpeggiated pad |

### Song files

A `.beats.json` file is readable and can be written by hand:

```json
{
  "format": "wetwaresuite-beats",
  "version": 1,
  "name": "boombap",
  "bpm": 90.0,
  "time_signature": "4/4",
  "swing": 0.3,
  "tracks": [
    {"name": "Drums", "kind": "drum", "volume_db": 0.0, "pan": 0.0, "mute": false, "solo": false},
    {"name": "Bass", "kind": "synth", "preset": "bass", "synth": {"osc1": "saw", "cutoff": 380.0}}
  ],
  "patterns": [
    {"name": "beat", "bars": 1, "steps_per_beat": 4, "steps": {"kick": "x......x..x.....", "snare": "....x.......x..."}, "notes": []},
    {"name": "walk", "bars": 1, "notes": [["D2", 0, 1.5, 110], ["F2", 2, 1, 100]]}
  ],
  "arrangement": [
    {"track": 0, "pattern": "beat", "bar": 0, "repeat": 4},
    {"track": 1, "pattern": "walk", "bar": 0, "repeat": 4}
  ]
}
```

Fields left out take their defaults (`rate` 44100, volume 0 dB, no effects). A track's `preset` is applied first and its `synth` settings on top, so `"preset": "bass"` alone gives the bass sound.

## Limitations

- No MIDI input or real-time recording. `beats.play` mixes the song before it starts, so edits are heard on the next play, not mid-loop.
- Sound quality is "good sketch" rather than studio. The synth uses PolyBLEP oscillators with one filter per note and no LFOs, glide, unison stacks or modulation matrix. The drum kit is synthesized and simple, and the sampler pitches by resampling, so pitch and length change together.
- One tempo and one time signature per song, with no tempo changes or automation. Volume, pan and effect settings are fixed for the whole song.
- Effects are a fixed chain per track (distortion, compression, delay, reverb) with no sends, EQ or master bus effects other than the limiter. For more processing, bounce the mix and use the audio tool's EQ, compressor and loudness tools.
- Generated songs are templates. Drum grooves come from a small table per genre with seeded variations, progressions are diatonic triads or sevenths, and melodies are simple. Treat them as a starting point to edit.
- MIDI import keeps notes, tempo, time signature, program, pan and volume. It drops controllers, pitch bend, tempo changes after the first, and SMPTE-timed files. Format 2 files aren't read. Each imported part is one long pattern, and swing comes in baked into the timing.
- Pictures have no text labels. Lanes run in track order, as `beats.info` lists them.
- MP3, Ogg, FLAC, Opus and M4A output need ffmpeg on PATH. WAV always works.
