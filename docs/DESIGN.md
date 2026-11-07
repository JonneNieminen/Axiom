# Axiom visual system

Axiom borrows Lumio's calm, instrument-like restraint: deep indigo planes, warm cream type, fine borders, and amber reserved for active signal and the primary action. Its amplifier mark and layout are original to Axiom.

## Color tokens

| Token | Value | Use |
| --- | --- | --- |
| Background | `#12152B` | Main canvas |
| Panel | `#1A1E3C` | Secondary control surface |
| Surface | `#232850` | Main amp faceplate and controls |
| Raised surface | `#2C3260` | Hover and popup surfaces |
| Border | `#343A6B` | Hairlines and control outlines |
| Strong border | `#4A5190` | Focus and selected control edges |
| Cream | `#F4EFE2` | Primary labels and display type |
| Secondary | `#B9BCD9` | Supporting copy and values |
| Muted | `#8388B3` | Captions and metadata |
| Amber | `#FFC857` | Signal markers and the primary action |

`Source/DesignTokens.h` is the implementation source of truth. Keep `assets/axiom-mark.svg`, `assets/axiom-lockup.svg`, and the screenshots in `docs/screenshots/` aligned with it when tokens change.

## Interface principles

- Keep the amp controls as the visual focus; place secondary workflow below them.
- Use generous spacing, rounded 10–16 px surfaces, and fine borders.
- Use one amber-filled primary action. Use amber markers sparingly for active values and signal.
- Keep section labels small and quiet; keep parameter values legible and direct.
- Do not reuse Lumio's logo or icon geometry. The Axiom mark combines an amp-like A and a quiet ring.
