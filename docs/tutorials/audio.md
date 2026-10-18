# Audio: edit, clean up and mix sound

`audio` is the suite's audio editor. It opens WAV, MP3, FLAC and Ogg Vorbis files without any other software (and Opus, M4A/AAC, AIFF, WMA and the sound from video files when ffmpeg is installed), and does what you would otherwise need a separate audio app for:

- **Clean-up**: noise reduction from a noise sample, EQ (high-pass, low-pass, shelves, bells, notches), compressor, limiter, noise gate, DC offset removal.
- **Levels**: gain, peak normalizing, loudness normalizing to LUFS (ITU-R BS.1770, the standard podcasts and streaming services use), measuring peak, true peak, RMS, loudness, loudness range and clipping.
- **Editing**: trim, cut, copy, paste (insert, overwrite or mix), insert silence, reverse, fades with five curve shapes, pan, channel conversion, resampling.
- **Effects**: reverb, echo/delay, tempo change without pitch change, pitch shift without tempo change, tape-style speed change.
- **Multitrack**: projects with tracks of clips at time offsets, per-track gain, pan, mute and solo, volume envelopes, automatic ducking (music drops under a voice), mixdown and export.
- **Folders**: batch convert, normalize, trim, filter, de-noise or re-tempo a whole folder; find silences and split a long recording into numbered files.
- **Pictures**: waveforms and spectrograms as PNG, so you (and Claude) can see what you're working on.

There is no playback and no GUI yet; the GUI that comes later will call these same commands. Each task below is shown three ways:

- **CLI**: single `achclwss` commands that read files and write files.
- **Batch**: a script for multi-step editing with `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)). Documents only live inside one `achclwss` process, so editing sessions are batch scripts or MCP.
- **Claude**: a prompt for Claude over MCP and the tools it calls (see [mcp.md](mcp.md)). `achclwss mcp --tools audio` serves just the audio tools.

Every example was run against the real binary and the outputs are copied from those runs. Long outputs are shortened where marked with `...`. The example recordings were made for this tutorial (a synthesized voice with hiss, mains hum and rumble added, a synth jingle, a drum loop, four drum hits), so your numbers will differ.

## How audio works

**Documents.** `audio.open` (or `audio.new`) loads a sound as a document with an id such as `aud1`. The editing commands take `doc=aud1` and change it in place; each change is one undo step (`undo`, `redo`). Inside, audio is 32-bit float, so nothing clips between steps; only saving to a 16-bit or 24-bit file does. `audio.save` writes it out and the extension picks the format.

**Time ranges and channels.** Most commands take `start=` and `end=` in seconds (default: the whole file) and `channels=` as channel numbers (0 = left, 1 = right; default: all). Effects applied to part of a file blend in and out over 5 ms at the edges so they don't click. `channels=1 start=9` means "the right channel from 9 seconds to the end".

**Projects.** `audio.project_new` makes a multitrack project (`proj1`). Tracks are numbered from 0; `audio.clip_add` copies a document or file onto a track at a time offset. `audio.mixdown` mixes it into a new document, `audio.export` straight to a file, `audio.project_save` keeps it as editable JSON.

**Formats.**

| | read | write |
| --- | --- | --- |
| built in | wav (8/16/24/32-bit PCM, 32/64-bit float), mp3, flac, ogg (Vorbis) | wav (8, 16, 24, 32, 32f) |
| with ffmpeg on PATH | opus, m4a, aac, aiff, wma, ac3, amr, caf, mka, wv, ape, and video files (mp4, mkv, mov, webm, avi...) | flac, mp3, ogg, opus, m4a, aac, aiff |

`bits=16` (the default), `24`, `32` or `32f` sets the WAV/FLAC sample format; 16-bit gets TPDF dither. `bitrate=` sets kbit/s for lossy formats; without it MP3 uses VBR quality 2 (about 190 kbit/s), Ogg quality 5, Opus 96k and M4A 160k.

**Levels in dB.** dBFS is decibels below full scale: 0 dBFS is the loudest a file can hold, -6 dBFS is half that. LUFS measures how loud something *sounds* (it weights frequencies like the ear and ignores silence). Targets: podcasts -16 LUFS (mono -19), Spotify/YouTube -14, broadcast (EBU R128) -23, with peaks at -1 dBFS or lower.

## Task 1: clean up a voice recording

`interview.wav` is a 14-second voice recording with a hissy room, a 60 Hz hum from the mains and some low rumble. It's also quiet. The goal is a clean voice at podcast loudness, -16 LUFS.

First, measure and look. Both are read-only, so they work as one-shot commands on the file:

```sh
$ achclwss audio.measure input=interview.wav
interview: duration 0:14.200, peak -12.4 dB (true peak -12.4 dB), RMS -26.4 dB, loudness -26 LUFS, range 2.54 LU
seconds: 14.2
peak_db: -12.3609
true_peak_db: -12.3576
rms_db: -26.3524
lufs: -26.0357
momentary_max_lufs: -21.8705
short_term_max_lufs: -25.4532
loudness_range: 2.53526
dc_offset: 0.000163707
clipped: 0
channel_peak_db: -12.3609
channel_rms_db: -26.3524

$ achclwss audio.detect_silence input=interview.wav threshold=-35 min_silence=0.3
2 silent stretches in interview, 0:03.980 in all
  silence 1: 0.000 s to 0:02.230 (0:02.230)
  silence 2: 0:12.450 to 0:14.200 (0:01.750)
count: 2
silence_seconds: 3.98
regions: 0,2.23,12.45,14.2

$ achclwss audio.render input=interview.wav spectrogram=true output=interview_spec.png
wrote interview_spec.png: spectrogram of interview from 0.000 s to 0:14.200, 20 Hz to 22050 Hz (log scale)
output: interview_spec.png
width: 1200
height: 400
```

At -26 LUFS it is 10 LU too quiet. The first 2.2 seconds are room tone only: that's the noise sample for noise reduction. The spectrogram shows the hiss as a purple haze over everything and the hum and rumble as a bright band under 100 Hz.

**CLI.** `audio.batch` does a quick clean-up of every recording in a folder in one command: a high-pass filter for the rumble, noise reduction with the noise estimated from each file's quietest parts, loudness normalizing and silence trimming:

```sh
$ achclwss audio.batch input_dir=recordings output_dir=cleaned highpass=80 denoise=15 normalize=loudness target=-16 trim_silence=true threshold=-45
processed 1 of 1 files into cleaned
  cleaned/interview.wav (0:10.230, +10.4 dB)
processed: 1
skipped: 0
failed: 0

$ achclwss audio.measure input=cleaned/interview.wav
interview: duration 0:10.230, peak -1.8 dB (true peak -1.8 dB), RMS -14.9 dB, loudness -15.9 LUFS, range 0.966 LU
...
```

**Batch.** For the full treatment, script it. This is `cleanup.txt`:

```text
# clean up a voice recording for a podcast
audio.open path=interview.wav -> voice
audio.render doc=$voice spectrogram=true output=before.png
# 1. rumble and hum: high-pass at 80 Hz (24 dB/octave), notch out 60 Hz mains hum
audio.eq doc=$voice type=highpass frequency=80 order=2
audio.eq doc=$voice type=notch frequency=60 q=8
# 2. hiss: learn it from the first two seconds (room tone only) and take it down 18 dB
audio.denoise doc=$voice noise_start=0 noise_end=2 reduction=18
# 3. a touch of presence, then even out the level
audio.eq doc=$voice type=peaking frequency=3000 gain=2 q=1
audio.compress doc=$voice threshold=-30 ratio=3 attack=10 release=150 knee=6
# 4. podcast loudness: -16 LUFS with peaks no higher than -1 dBFS
audio.normalize doc=$voice mode=loudness target=-16 ceiling=-1
audio.trim doc=$voice silence=true threshold=-45 pad=0.3
audio.measure doc=$voice
audio.render doc=$voice spectrogram=true output=after.png
audio.save doc=$voice path=interview_clean.wav
```

```sh
$ achclwss batch cleanup.txt
opened interview.wav as aud1: 1 ch, 44100 Hz, 0:14.200
...
wrote before.png: spectrogram of interview from 0.000 s to 0:14.200, 20 Hz to 22050 Hz (log scale)
...
highpass at 80 Hz
...
notch at 60 Hz
...
reduced noise by up to 18 dB using noise from 0.000 s to 0:02.000
...
peaking at 3000 Hz, 2.0 dB
...
compressed 3:1 above -30.0 dB, up to 9.7 dB of reduction
...
loudness -32 -> -16 LUFS (gain 16.0 dB), peak -2.9 dB
...
trimmed 0:14.200 to 0:10.840
...
interview: duration 0:10.840, peak -2.9 dB (true peak -2.9 dB), RMS -15.2 dB, loudness -16 LUFS, range 0.639 LU
...
wrote after.png: spectrogram of interview from 0.000 s to 0:10.840, 20 Hz to 22050 Hz (log scale)
saved interview_clean.wav (0:10.840)
bytes: 956132
```

(Every edit also prints `doc`, `seconds`, `rate` and `channels` lines; they're cut here.) Compare `before.png` and `after.png`: the band under 100 Hz is gone and the haze between words is much darker.

Why this order: filters first so the noise profile and the compressor don't react to rumble; noise reduction before compression, because compression raises the quiet parts and the noise with them; loudness last, because every earlier step changes the level. The trim comes after normalizing so its threshold is measured on the final level.

Tips:

- **The noise sample** should be only noise: room tone before anyone speaks is ideal. Without `noise_start`/`noise_end`, `audio.denoise` uses a profile learned earlier with `audio.noise_profile`, or estimates the noise from the quietest tenth of the file.
- **Too much noise reduction** sounds watery or lispy. 12-18 dB is natural; raise `sensitivity` (default 1.5) to remove more, lower it to keep more detail.
- **Hum** in Europe is 50 Hz; add notches at the harmonics (`frequency=100`, `150`...) if it buzzes.
- **A noise gate** turns down what's left between phrases: `audio.gate doc=$voice threshold=-40 range=-20` on the cleaned file reports `gated below -40.0 dB (closed 7.23% of the time)`. `range=-20` turns the gaps down 20 dB rather than to dead silence, which sounds more natural. Set the threshold between the noise and the quietest words (measure a gap with `audio.measure start= end=`).
- **Mono podcasts** aim at -19 LUFS; stereo at -16.

**Claude.** Ask:

> Clean up interview.wav for my podcast: there's hiss, hum and rumble, and it's too quiet. The first two seconds are just room noise. Get it to -16 LUFS, show me a spectrogram before and after, and save it as interview_clean.wav.

Claude calls `audio_open`, `audio_measure` (reads `lufs: -26`), `audio_preview` with `spectrogram=true` (and sees the picture), `audio_detect_silence` to confirm the noise-only stretch, then `audio_eq` (highpass and notch), `audio_denoise` with `noise_start=0 noise_end=2`, `audio_compress`, `audio_normalize mode=loudness target=-16`, `audio_measure` again to check, `audio_preview spectrogram=true` to compare, and `audio_save`. If something sounds off, `undo` steps back one edit at a time.

## Task 2: a podcast episode from intro music and a voice

Put `intro.wav` (20 s of stereo music) on one track and `interview_clean.wav` from task 1 on another, starting at 5 seconds. The music should fade in, drop out of the way while the voice talks, come back up after, and fade out. Then export a -16 LUFS MP3.

Multitrack projects live in a session, so this is a batch script (or Claude). `episode.txt`:

```text
# a podcast episode: intro music that ducks under the voice, then fades out
audio.open path=intro.wav -> music
audio.open path=interview_clean.wav -> voice
audio.project_new rate=44100 channels=2 -> show
audio.track_add project=$show name=music gain=-4
audio.track_add project=$show name=voice
audio.clip_add project=$show track=0 doc=$music at=0 fade_in=1 fade_out=4
audio.clip_add project=$show track=1 doc=$voice at=5
# the music drops 14 dB while the voice talks, and comes back up when it stops
audio.track_set project=$show track=0 duck_under=1 duck_db=14 duck_attack=0.3 duck_release=1
audio.project_info project=$show
audio.render project=$show output=episode_tracks.png
audio.export project=$show path=episode.mp3 normalize=loudness target=-16 ceiling=-1
audio.project_save project=$show path=episode.json
audio.measure input=episode.mp3
```

```sh
$ achclwss batch episode.txt
...
made proj1: 0 tracks, 0 clips, 0.000 s, 44100 Hz stereo
project: proj1
added track 0 "music"
...
added track 1 "voice"
...
added clip 0 "intro" to track 0 at 0.000 s (0:20.000)
...
added clip 0 "interview_clean" to track 1 at 0:05.000 (0:10.840)
...
clip_end: 15.84
track 0 updated
track 0 "music": gain -4.0 dB, pan 0, ducks -14.0 dB under track 1
  clip 0 "intro": 0.000 s to 0:20.000, fade in 0:01.000, fade out 0:04.000
...
proj1: 2 tracks, 2 clips, 0:20.000, 44100 Hz stereo
track 0 "music": gain -4.0 dB, pan 0, ducks -14.0 dB under track 1
  clip 0 "intro": 0.000 s to 0:20.000, fade in 0:01.000, fade out 0:04.000
track 1 "voice": gain 0.0 dB, pan 0
  clip 0 "interview_clean": 0:05.000 to 0:15.840
...
wrote episode_tracks.png: tracks of proj1
...
exported episode.mp3: 2 ch, 44100 Hz, 0:20.000, loudness -16 LUFS, peak -4.8 dB
path: episode.mp3
seconds: 20
lufs: -16
peak_db: -4.78585
bytes: 252894
saved episode.json (clips in episode_clips)
path: episode.json
episode: duration 0:20.000, peak -4.9 dB (true peak -4.8 dB), RMS -18.5 dB, loudness -16 LUFS, range 5.84 LU
...
```

`episode_tracks.png` draws each track as it will sound: you can see the music fade in, drop to a thin line from 5 s to 16 s while the voice talks, swell back and fade out.

How ducking works: whenever the key track (`duck_under=1`) is louder than `duck_threshold` (default -40 dBFS), this track glides down by `duck_db` over `duck_attack` seconds, starting a little early so the first word is clear, holds through short pauses between words, and comes back over `duck_release` seconds. For hand-drawn volume changes use an envelope instead (or as well): `audio.track_set project=$show track=0 envelope=0,0,4.5,0,5,-14,16,-14,17,0` is a list of time,dB points with straight lines between them.

The mono voice is placed in the centre of the stereo mix automatically. More things you can do to a project:

- `audio.clip_set project=$show track=1 clip=0 at=6` moves the voice to 6 s; `to_track=` moves a clip to another track; `gain=`, `fade_in=`, `fade_out=` change it.
- `audio.track_set ... mute=true` or `solo=true`; when any track is solo, only solo tracks play.
- `audio.clip_add ... input=outro.wav` places a file without opening it; `start=`/`end=` take part of the source; leaving out `at=` puts the clip right after the track's last one.
- `audio.mixdown project=$show` gives you the mix as a new document to keep editing (normalize, limit, trim), and `audio.project_open path=episode.json` brings the project back later. The `episode_clips` folder next to the JSON holds every clip as a 32-bit float WAV, so keep them together.

**Claude.** Ask:

> Make a podcast episode: intro.wav as music on one track and interview_clean.wav as the voice starting 5 seconds in. Fade the music in over a second, duck it about 14 dB under the voice and fade it out at the end. Export episode.mp3 at -16 LUFS and show me the tracks.

Claude calls `audio_open` twice, `audio_project_new`, `audio_track_add` twice, `audio_clip_add` twice, `audio_track_set` with `duck_under=1 duck_db=14`, `audio_project_info` with `preview=true` (it sees the lanes picture, with the ducking drawn in), and `audio_export` with `normalize=loudness target=-16`. Then it can `audio_measure` the MP3 to confirm the loudness.

## Task 3: batch-normalize a folder of samples

`samples/` holds four drum hits recorded at very different levels (one of them an MP3), each with silence before and after:

```sh
$ achclwss audio.measure input=samples/hit_2.wav true_peak=false
hit_2: duration 0:02.200, peak -17.1 dB (true peak -17.1 dB), RMS -33.4 dB, loudness -27.6 LUFS
...
$ achclwss audio.measure input=samples/hit_3.mp3 true_peak=false
hit_3: duration 0:02.200, peak -29.5 dB (true peak -29.5 dB), RMS -45.9 dB, loudness -39.2 LUFS
...
```

**CLI.** Bring every peak to -1 dBFS, cut the silence around each hit, add a 10 ms fade at the end so the cut doesn't click, and write 24-bit WAVs into a new folder (the originals are never touched):

```sh
$ achclwss audio.batch input_dir=samples output_dir=samples_norm normalize=peak target=-1 trim_silence=true threshold=-36 fade_out=0.01 to=wav bits=24
processed 4 of 4 files into samples_norm
  samples_norm/hit_1.wav (0.740 s, +3.11 dB)
  samples_norm/hit_2.wav (0.750 s, +16.1 dB)
  samples_norm/hit_3.wav (0.750 s, +28.5 dB)
  samples_norm/hit_4.wav (0.740 s, +8.08 dB)
processed: 4
skipped: 0
failed: 0
```

Running it again skips files that already exist unless you pass `overwrite=true`. Without `to=`, each file keeps its own format (the MP3 stays an MP3):

```sh
$ achclwss audio.batch input_dir=samples output_dir=samples_norm normalize=peak target=-1
processed 1 of 4 files into samples_norm
  samples_norm/hit_3.mp3 (0:02.200, +28.5 dB)
3 skipped because the output exists (pass overwrite=true)
processed: 1
skipped: 3
failed: 0
```

To match how loud they *sound* rather than their peaks, use `normalize=loudness target=-14` (peaks over `ceiling`, -1 dBFS by default, are limited). Here as FLAC:

```sh
$ achclwss audio.batch input_dir=samples output_dir=samples_loud normalize=loudness target=-14 to=flac bits=24
processed 4 of 4 files into samples_loud
  samples_loud/hit_1.flac (0:02.200, +2.06 dB)
  samples_loud/hit_2.flac (0:02.200, +13.6 dB)
...
$ achclwss audio.measure input=samples_loud/hit_3.flac
hit_3: duration 0:02.200, peak -4.4 dB (true peak -4.4 dB), RMS -20.7 dB, loudness -14 LUFS
...
```

The trim threshold is measured after normalizing, so a quiet recording's amplified noise floor counts as sound if the threshold is too low: -36 dBFS works here, where -50 would keep the hiss of the quietest hit.

**Batch.** Often all the hits are in one long take. Split it at the silences into numbered files, then level them. `takes.txt` first builds such a take from the four samples:

```text
# one long "take" of four hits with gaps, then split it into numbered files and level them
audio.new kind=silence seconds=10 channels=2 -> take
audio.mix doc=$take input=samples/hit_1.wav at=0
audio.mix doc=$take input=samples/hit_2.wav at=2.5
audio.mix doc=$take input=samples/hit_3.mp3 at=5
audio.mix doc=$take input=samples/hit_4.wav at=7.5
audio.save doc=$take path=take.wav
audio.split_on_silence doc=$take output_dir=hits prefix=hit threshold=-60 min_silence=0.4
audio.batch input_dir=hits output_dir=hits_level normalize=peak target=-1 trim_silence=true threshold=-36 fade_out=0.01 bits=24
```

```sh
$ achclwss batch takes.txt
made aud1: silence, 2 ch, 44100 Hz, 0:10.000
...
mixed in 0:02.200 at 0.000 s
...
saved take.wav (0:10.000)
bytes: 1764044
wrote 4 pieces:
  hits/hit_001.wav (0.300 s to 0:01.500)
  hits/hit_002.wav (0:02.800 to 0:03.920)
  hits/hit_003.wav (0:05.300 to 0:06.130)
  hits/hit_004.wav (0:07.800 to 0:09.000)
count: 4
files: hits/hit_001.wav, hits/hit_002.wav, hits/hit_003.wav, hits/hit_004.wav
regions: 0.3,1.5,2.8,3.92,5.3,6.13,7.8,9
processed 4 of 4 files into hits_level
  hits_level/hit_001.wav (0.740 s, +3.11 dB)
  hits_level/hit_002.wav (0.750 s, +16.1 dB)
  hits_level/hit_003.wav (0.720 s, +28.5 dB)
  hits_level/hit_004.wav (0.740 s, +8.08 dB)
...
```

`audio.split_on_silence` also works straight on a file (`input=take.wav output_dir=hits`), so it's a one-shot command too. Each piece keeps `pad` seconds (0.1) of the silence around it and gets a 5 ms fade at both ends.

**Claude.** Ask:

> Normalize every sample in my samples folder to -1 dB peak, trim the silence around them and put 24-bit WAVs in samples_norm.

Claude calls `audio_measure` on a file or two to pick a sensible silence threshold, then `audio_batch` with `normalize=peak target=-1 trim_silence=true bits=24`. For the long take: "split take.wav into one file per hit" calls `audio_detect_silence` to see the gaps, then `audio_split_on_silence`.

## Task 4: see a spectrogram

A spectrogram shows time left to right, frequency bottom to top, and loudness as brightness. It shows what a waveform can't: hum (a bright horizontal line), hiss (a haze across the top), clicks (thin vertical lines), and what range an instrument covers.

**CLI.**

```sh
$ achclwss audio.render input=interview.wav spectrogram=true output=interview_spec.png
wrote interview_spec.png: spectrogram of interview from 0.000 s to 0:14.200, 20 Hz to 22050 Hz (log scale)
output: interview_spec.png
width: 1200
height: 400

$ achclwss audio.render input=interview.wav spectrogram=true log_frequency=false max_frequency=8000 start=2 end=5 height=300 output=zoom.png
wrote zoom.png: spectrogram of interview from 0:02.000 to 0:05.000, 0 to 8000 Hz
output: zoom.png
width: 1200
height: 300
```

The default log frequency scale gives every octave the same height (as you hear it), with labelled lines at 50, 100, 200, 500 Hz, 1k, 2k, 5k, 10k and 20k; `log_frequency=false` is linear in Hz, with a line every 1 or 2 kHz. Time is labelled along the bottom. `fft=4096` gives finer frequency detail (to separate a 50 Hz hum from its 100 Hz harmonic), `fft=512` sharper timing (to see drum hits); `floor=-80` hides quieter detail. `channel=0` draws just the left channel.

Waveforms work the same way without `spectrogram=true`: one lane per channel, the peak outline in blue with the RMS (average level) lighter inside, samples at full scale marked red at the lane edges, dashed lines at -6 dB. `decibels=true` uses a dB scale so quiet passages show.

**Batch.** In a script, `audio.render` draws documents at any stage, as in task 1's before/after pictures.

**Claude.** `audio_preview` attaches the PNG to the tool result, so Claude sees it:

> Show me a spectrogram of interview.wav. Is there any hum?

Claude calls `audio_open` then `audio_preview` with `spectrogram=true`, sees the bright band below 100 Hz, may zoom in with `log_frequency=false max_frequency=500 fft=8192`, and answers (here: yes, 60 Hz). `audio_preview` also takes `input=` to look at a file without opening it. Use `audio_render` when you want the picture saved as a file.

## Task 5: change the pitch and tempo of a loop

`loop.wav` is a 4-second two-bar drum and bass loop at 120 bpm.

**Batch.** `loop.txt`:

```text
# a 120 bpm loop: slow it to 100 bpm, then a version three semitones down
audio.open path=loop.wav -> loop
audio.stretch doc=$loop from_bpm=120 to_bpm=100
audio.save doc=$loop path=loop_100bpm.wav
audio.pitch doc=$loop semitones=-3
audio.save doc=$loop path=loop_100bpm_down3.wav
# tape-style: twice as fast and an octave up, from the original
audio.open path=loop.wav -> tape
audio.speed doc=$tape factor=2
audio.save doc=$tape path=loop_chipmunk.wav
```

```sh
$ achclwss batch loop.txt
opened loop.wav as aud1: 2 ch, 44100 Hz, 0:04.000
...
stretched 0:04.000 to 0:04.800 (1.2x), pitch unchanged
...
saved loop_100bpm.wav (0:04.800)
bytes: 846764
pitch -3 semitones
...
saved loop_100bpm_down3.wav (0:04.800)
bytes: 846764
opened loop.wav as aud2: 2 ch, 44100 Hz, 0:04.000
...
speed x2
...
saved loop_chipmunk.wav (0:02.000)
bytes: 352844
```

Three different commands:

- `audio.stretch` changes the length and keeps the pitch (WSOLA time stretching). Give `factor=` (2 = twice as long), `tempo=` (1.25 = 25% faster), `seconds=` (the new length) or `from_bpm=`/`to_bpm=`. With `start`/`end` it stretches only that part.
- `audio.pitch` changes the pitch and keeps the length: `semitones=` (+12 is an octave up) and `cents=` for fine tuning.
- `audio.speed` is a record played at the wrong speed: both change together.

**CLI.** `audio.batch` takes `tempo=` and `pitch=` too, for a whole folder of loops:

```sh
$ achclwss audio.batch input_dir=loops output_dir=loops_100bpm tempo=0.8333 to=wav
```

**Claude.** Ask:

> loop.wav is 120 bpm. Make a 100 bpm version, and another one 3 semitones lower, as WAVs.

Claude calls `audio_open`, `audio_stretch` with `from_bpm=120 to_bpm=100`, `audio_save`, `audio_pitch` with `semitones=-3`, `audio_save`.

Stretching works best within about half to double speed; beyond that, drums smear and voices get a metallic edge. Pitch shifts of a few semitones sound natural on instruments; voices shifted far sound like chipmunks or giants because their formants move too.

## More editing

Everyday edits in one script (`edit.txt`):

```text
# everyday edits on the cleaned interview: cut a stumble, add a pause, effects on parts
audio.open path=interview_clean.wav -> v
audio.cut doc=$v start=3.2 end=3.9
audio.insert_silence doc=$v at=6 seconds=0.5
audio.copy doc=$v start=0 end=1.5
audio.paste doc=$v mode=insert
audio.channels doc=$v mode=stereo
audio.pan doc=$v position=-0.3
audio.echo doc=$v delay=0.25 feedback=0.3 mix=0.3 start=9 channels=1
audio.reverb doc=$v room=0.4 wet=0.15 tail=1.5
# changed your mind about the reverb? undo goes back one step
undo
audio.info doc=$v
audio.fade doc=$v out=1.5 curve=s_curve
audio.save doc=$v path=interview_edit.flac bits=24
```

```sh
$ achclwss batch edit.txt
opened interview_clean.wav as aud1: 1 ch, 44100 Hz, 0:10.840
...
cut 0.700 s to the clipboard
...
inserted 0.500 s of silence at 0:06.000
...
copied 0:01.500 to the clipboard
...
pasted 0:01.500 at 0:10.640 (insert)
...
now 2 channels
...
panned to -0.3
...
echo every 0.250 s from 0:09.000 to 0:12.140 on channel 1
...
reverb (room 0.4, wet 0.15)
seconds: 13.64
undone
aud1: 2 ch, 44100 Hz, 0:12.140, interview_clean.wav
...
faded out over 0:01.500
...
saved interview_edit.flac (0:12.140)
bytes: 1909098
```

`audio.paste` without `at=` appends at the end; `mode=overwrite` replaces what's there and `mode=mix` layers on top; `source=aud2` pastes a whole other document instead of the clipboard. `tail=` on reverb and echo adds silence at the end first so the effect can ring out. `audio.new` makes test material: `kind=tone frequency=1000 level=-20`, `kind=sweep frequency=20 to_frequency=20000` (good for checking an EQ with a spectrogram), `kind=noise color=pink`, or `kind=silence` to build on.

## Asking Claude, in general

Connect with `claude mcp add achclwss -- /path/to/achclwss mcp` (or `achclwss mcp --tools audio` for just the audio tools). Things Claude handles well because it can measure and look:

- "Why does this recording sound muddy?": `audio_preview spectrogram=true`, `audio_measure`, then a low shelf or a peaking cut around 250 Hz with `audio_eq`.
- "Is this master too loud for Spotify?": `audio_measure` reads `lufs`, `true_peak_db` and `clipped`, and compares with -14 LUFS and -1 dBTP.
- "Remove the cough at about 0:42": `audio_preview start=41 end=44` to find it, `audio_cut` the exact range, `audio_preview` again to check the join.
- "Make every file in this folder an MP3 at -16 LUFS": `audio_batch`.

Every edit is undoable with `undo`, `history` lists the steps, and `documents` lists what's open.

## Reference

Common parameters: `doc` (an open document), `start`/`end` (seconds; default the whole file), `channels` (list of channel numbers, 0 = left), `preview` (true attaches a waveform PNG of the result). Run `achclwss help <command>` for the full list of any command.

### Files, pictures and measuring

| command | what it does | key parameters |
| --- | --- | --- |
| `audio.open` | open a file as a document | `path` |
| `audio.new` | silence, tone, sweep or noise | `kind`, `seconds`, `rate` (44100), `channels` (2), `frequency` (440), `to_frequency`, `waveform` (sine, square, saw, triangle), `color` (white, pink, brown), `level` (-6 dBFS) |
| `audio.save` | write a file; extension picks the format | `path`, `bits` (16), `bitrate`, `dither` (true), `start`/`end` to save a part |
| `audio.close` | close a document or project | `doc` |
| `audio.info` | length, rate, channels | `doc` |
| `audio.measure` | peak, true peak, RMS, LUFS, loudness range, DC, clipping | `doc` or `input`, `start`/`end`, `true_peak` (true) |
| `audio.preview` | attach a waveform or spectrogram PNG (read-only) | `doc` or `input`, `spectrogram`, `width` (1200), `height`, `log_frequency` (true), `max_frequency`, `fft` (2048), `floor` (-100), `channel`, `decibels` |
| `audio.render` | the same picture, or a project's lanes, written to a PNG | as preview, plus `output`, `project` |
| `audio.detect_silence` | list silent stretches | `doc` or `input`, `threshold` (-40 dBFS), `min_silence` (0.5 s) |
| `audio.split_on_silence` | write each sounding part as a numbered file | `doc` or `input`, `output_dir`, `prefix`, `format` (wav), `threshold` (-40), `min_silence` (0.5), `min_length` (0.25), `pad` (0.1), `fade` (0.005) |
| `audio.batch` | process a folder | `input_dir`, `output_dir`, `to`, `channels`, `rate`, `highpass`, `denoise` (dB), `tempo`, `pitch`, `normalize` (none, peak, loudness), `target`, `ceiling` (-1), `trim_silence`, `threshold` (-50), `fade_in`, `fade_out`, `bits`, `bitrate`, `recursive`, `overwrite` |

### Levels, editing, channels

| command | what it does | key parameters |
| --- | --- | --- |
| `audio.gain` | louder or quieter | `db` |
| `audio.normalize` | to a peak or a loudness | `mode` (peak, loudness), `target` (-1 dBFS or -16 LUFS), `ceiling` (-1), `limit` (true) |
| `audio.fade` | fade in and/or out | `in`, `out` (seconds), `curve` (linear, exponential, logarithmic, s_curve, equal_power) |
| `audio.remove_dc` | centre the waveform on zero | |
| `audio.trim` | keep start..end, or cut silence at the ends | `silence`, `threshold` (-50), `pad` (0.05) |
| `audio.cut` / `audio.copy` | to the clipboard | `start`, `end` |
| `audio.paste` | clipboard or `source` into the audio | `at` (default the end), `mode` (insert, overwrite, mix), `source`, `gain` |
| `audio.insert_silence` | add a gap | `seconds`, `at` |
| `audio.reverse` | backwards | |
| `audio.mix` | layer another document or file on top | `source` or `input`, `at`, `gain` |
| `audio.pan` | left/right placement (mono becomes stereo) | `position` (-1..1) |
| `audio.channels` | mono, stereo, swap, left, right, count | `mode`, `count` |
| `audio.resample` | new sample rate | `rate`, `quality` (best = windowed sinc, fast = cubic) |

### Filters, dynamics, noise, effects

| command | what it does | key parameters |
| --- | --- | --- |
| `audio.eq` | one EQ band | `type` (lowpass, highpass, bandpass, notch, peaking, lowshelf, highshelf), `frequency`, `gain` (dB), `q` (0.707), `order` (1..8, steeper) |
| `audio.compress` | even out the level | `threshold` (-18), `ratio` (3), `attack` (10 ms), `release` (120 ms), `knee` (6 dB), `makeup`, `auto_makeup` |
| `audio.limit` | brickwall look-ahead limiter | `ceiling` (-1), `release` (60 ms) |
| `audio.gate` | silence quiet parts | `threshold` (-45), `attack` (2), `hold` (60), `release` (150), `range` (-80 dB) |
| `audio.noise_profile` | learn the noise from a noise-only stretch | `start`, `end`, `fft` |
| `audio.denoise` | spectral noise reduction | `reduction` (12 dB), `sensitivity` (1.5), `noise_start`/`noise_end`, `fft` (2048) |
| `audio.reverb` | room ambience (Freeverb) | `room` (0.6), `damping` (0.5), `wet` (0.25), `dry` (1), `width` (1), `predelay` (ms), `tail` (s) |
| `audio.echo` | repeating delay | `delay` (0.3 s), `feedback` (0.4), `mix` (0.5), `ping_pong`, `tail` |
| `audio.stretch` | change length, keep pitch | `factor`, `tempo`, `seconds`, or `from_bpm` + `to_bpm` |
| `audio.pitch` | change pitch, keep length | `semitones`, `cents` |
| `audio.speed` | tape-style speed | `factor` |

### Projects

| command | what it does | key parameters |
| --- | --- | --- |
| `audio.project_new` | start a project | `rate` (44100), `channels` (2), `tracks` (0) |
| `audio.project_open` / `audio.project_save` | JSON project + `<name>_clips/` folder | `path` |
| `audio.project_info` | tracks and clips; `preview` draws the lanes | `project` |
| `audio.track_add` | new track, returns its number | `name`, `gain`, `pan` |
| `audio.track_set` | track settings | `name`, `gain`, `pan`, `mute`, `solo`, `duck_under` (-1 off), `duck_db` (12), `duck_threshold` (-40), `duck_attack` (0.15 s), `duck_release` (0.6 s), `envelope` (time,dB pairs) |
| `audio.track_remove` | delete a track | `track` |
| `audio.clip_add` | place a document or file on a track | `track`, `doc` or `input`, `at`, `start`/`end` of the source, `gain`, `fade_in`, `fade_out`, `name` |
| `audio.clip_set` | move or change a clip | `track`, `clip`, `at`, `to_track`, `gain`, `fade_in`, `fade_out`, `name` |
| `audio.clip_remove` | delete a clip | `track`, `clip` |
| `audio.mixdown` | mix into a new document | `normalize`, `target`, `ceiling` |
| `audio.export` | mix straight to a file | `path`, `bits`, `bitrate`, `normalize`, `target`, `ceiling` |

## Limitations

- **No playback** and no recording yet: you judge by measuring and looking (waveforms, spectrograms), or by opening the saved file in any player. The GUI will add both.
- **Undo keeps a full copy** of the document for every step, so very long files (an hour of stereo is about 1.3 GB as float) use a lot of memory over many edits. Close documents you've saved, or work on parts.
- **Effects on a range** only process that range: a reverb or echo tail stops at `end` (blended over 5 ms). Use `tail=` on whole-file edits, or leave room after the range.
- **Noise reduction** is spectral gating, good on steady noise (hiss, hum, fans, room tone). It doesn't remove clicks, crackle, wind, reverb or a TV in the background, and pushed hard it sounds watery.
- **Time stretching and pitch shifting** are WSOLA-based: clean on speech and steady material within about 0.5x-2x and a few semitones; extreme settings smear transients, and pitch shifting moves formants (no formant preservation).
- **Loudness** follows ITU-R BS.1770-4 (K-weighting, 400 ms gated blocks); true peak is 4x oversampled near the loudest samples. Surround channel weights assume the L R C LFE Ls Rs order.
- **Multitrack**: clips are copies made when added (later edits to the document don't reach the project); no per-clip effects, crossfades are done with overlapping clip fades, and ducking listens to one track.
- **Formats**: without ffmpeg only WAV, MP3, FLAC and Ogg Vorbis open and only WAV saves. WAV files are limited to 4 GB (no RF64); AIFF is written through ffmpeg as 16- or 24-bit. Metadata (tags, cover art, markers) isn't kept.
- **Batch and split** write files and can't be undone; they never change their input folder.
