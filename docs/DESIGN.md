# Axiom design

Axiom shares the flat, calm language of Prism and Snapline (a sidebar, hairline-bordered cards, one accent, dark and light) with its own palette and mark. The palette is **Ember**: warm graphite surfaces with a copper-orange accent, like a tube glowing behind the grille.

## Mark

An **A** whose crossbar is one period of a **soft-clipped sine**, in ember on a deep warm tile. The A is the axiom: the one thing everything else is built on. The crossbar is what the amp does to your signal. Masters come from [`design/logo/make.py`](../design/logo/make.py); the plugin draws the same geometry from `logoLegs()` and `logoWave()` in `Source/DesignTokens.h`. The wordmark is lowercase, with the dot of the *i* in ember.

## Colour

| Token | Dark | Light | Use |
| --- | --- | --- | --- |
| Background | `#171513` | `#F7F3EC` | Window |
| Sidebar | `#1E1B18` | `#EEE8DE` | Left column |
| Surface | `#26221E` | `#FFFFFF` | Cards |
| Elevated | `#302B26` | `#F2ECE2` | Controls, chips |
| Pressed | `#3A342E` | `#E9E1D5` | Knob tracks, hover |
| Text · Muted · Faint | `#F2ECE3` · `#A0978B` · `#6F675D` | `#231F1B` · `#6F675D` · `#A59C90` | Type |
| Accent (ember) | `#F08A3C` | `#D4611A` | Value arcs, switches, the one primary action |

Each amp voice has its own colour, used on the voice dot, the header chip and the gain arc:
**Clean** teal `#5BB5A6` / `#2F8F80`, **Classic** amber `#F2B84B` / `#B9801C`, **Modern** coral `#EE6A4D` / `#C8472E`.

[`Source/DesignTokens.h`](../Source/DesignTokens.h) is the implementation source of truth and [`design/theme/tokens.css`](../design/theme/tokens.css) mirrors it. No gradients, glass or glows.

## Layout

- **Sidebar**: the tone (preset) menu, amp voice, input channel, tuner, cabinet switch, theme.
- **Header**: the tone's name, chips for what it is made of (voice, gate, cabinet, *Edited*), input and output meters.
- **Cards**: Amp (gain, bass, middle, treble, presence, master), Input (input trim, gate, boost), Effects (chorus, delay, delay time, room) and Tone match.
- Arc knobs show their value in an arc: from the start for level controls, from the centre for cut and boost controls (EQ, input trim).

## Rules

- Colours only come from the palette; never hard-code a colour in a component.
- Every control has a tooltip that says what it does in plain words.
- The theme is saved with the session, not as a parameter, so it never touches automation.
- Refresh `docs/screenshots` with `AxiomSnapshot` whenever the interface changes.
