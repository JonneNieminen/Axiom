# Contributing to Axiom

Thanks for helping! Axiom is a small project, so the process is light.

## Setup

```bash
git clone --depth 1 --branch 9.0.5 https://github.com/juce-framework/JUCE.git third_party/JUCE
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target Axiom_Standalone Axiom_VST3 Axiom_AU -j 4
```

Open `build/Axiom_artefacts/Release/Standalone/Axiom.app` to try your changes without a DAW.

## Before you open a pull request

- Build `Axiom_Standalone`, `Axiom_VST3` and `Axiom_AU` with no new warnings.
- Run `git diff --check`.
- Try the change in the standalone app, and in a VST3 and an AU host when it touches plugin behavior.

CI builds all three targets for Apple Silicon and Intel.

## Guidelines

- **Real-time safe audio.** No file I/O, heap allocation, locks, logging or UI calls in `processBlock`.
- **Stable parameters.** Preserve parameter IDs and their meanings; DAW sessions store them. Read automatable controls through `AudioProcessorValueTreeState` and smooth audible changes.
- **Explicit channel and sample-rate handling.** Mono and stereo, any sample rate.
- **Tone Match off the audio thread.** Decoding and analysis run on the worker thread; apply suggestions through host-notifying parameter calls.
- **Design tokens only.** Colors live in `Source/DesignTokens.h`. Keep `docs/DESIGN.md`, the SVG assets and the README screenshot aligned with the interface.
- **Original artwork.** Lumio is a visual reference, not an asset source.
- **Accurate docs.** Keep labels, failure states and supported file formats truthful.

## Reporting bugs

Use the bug report template and say whether you use the standalone app or a plugin. Please don't attach copyrighted audio; describe the file (format, length, sample rate) instead.
