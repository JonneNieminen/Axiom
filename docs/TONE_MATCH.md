# Tone Match

Tone Match analyzes a local audio file as a reference and suggests an Axiom amp voice, preamp gain, and four EQ values. It does not process or export the reference clip.

## Analysis

- Reads the first 15 seconds of WAV, AIFF, FLAC, or OGG audio on a worker thread. On macOS, Core Audio also allows MP3, M4A, and AAC.
- Computes five broad spectral bands with a 4096-sample Hann-window FFT and 50% overlap.
- Estimates signal crest factor from peak and RMS levels.
- Maps the spectral balance to Bass, Middle, Treble, and Presence suggestions.
- Maps the crest factor to a Clean, Classic, or Modern voice and a starting Gain value.

## Apply behavior

Nothing changes until the user selects **Apply tone**. Applying updates only `voice`, `gain`, `bass`, `mid`, `treble`, and `presence`, through the host-notifying parameter API. Input trim, master level, room amount, and cabinet state are preserved. The DAW saves the resulting parameter values with the project.

The reference file is read-only, remains on the user's machine, and is not stored in plugin state. Suggestions are broad tone-shaping guidance; they cannot infer a unique amplifier, instrument, pickup, microphone, or recording chain from a mixed audio file.
