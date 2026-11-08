# Axiom contributor guide

This file is the shared source of project guidance for coding agents. Provider-specific instruction files should point here instead of duplicating rules.

## Project

- JUCE 9.0.5+ and C++17 guitar amplifier effect.
- macOS formats: VST3 and AU. CMake owns the build.
- `Source/AxiomDsp.*`: the signal path (no JUCE plugin types, so `tools/render` can use it). `Source/Presets.h`: factory tones. `Source/PluginProcessor.*`: parameters, presets as programs and state. `Source/PluginEditor.*`: UI, tuner display and reference analysis. `Source/PitchDetector.h`: YIN pitch detection for the tuner (message thread only). `Source/DesignTokens.h`: UI palette.
- Tone Match is a local spectral-profile suggestion tool. It is not a model that clones a full recording chain.

## Rules

- Keep `processBlock` real-time safe: no file access, heap allocation, mutex waits, UI calls, or logging.
- Keep reference-file reading and FFT analysis on the worker thread. Use bounded input and safe message-thread callbacks.
- Change parameters through host-aware APIs. Preserve parameter IDs and their meanings for existing sessions. A new parameter's default must leave old sessions sounding the same (the gate defaults to Off).
- The tuner only copies input samples in `processBlock`; pitch detection runs in the editor timer.
- Treat input files as local reference material; do not upload, persist, or embed them.
- Preserve mono/stereo support and sample-rate safety.
- Keep the UI palette centralized, the SVG assets original, and README/docs accurate.
- Use the Lumio theme only as a design reference; do not copy its logo or assets.

## Build

Follow `README.md` to fetch JUCE and build both macOS targets. After DSP changes also run `axiom-render` (configure with `-DAXIOM_BUILD_TOOLS=ON`) and check levels, aliasing and the checks-passed line. After UI changes regenerate `docs/screenshots/amp.png` with `AxiomSnapshot` (same option) and look at it. At minimum, after code changes, build `Axiom_VST3` and `Axiom_AU`, then run `git diff --check`. Do not claim host compatibility beyond formats actually built or checked.
