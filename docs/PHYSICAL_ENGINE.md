# Physical amp engine

**Amp Engine → Physical** replaces Axiom's tube-style waveshaper stages with a circuit-level model of a tube amplifier. The code is in [`Source/PhysicalTubeStage.h`](../Source/PhysicalTubeStage.h), which holds the per-sample solver and the physics, with comments throughout, and [`PhysicalTubeStage.cpp`](../Source/PhysicalTubeStage.cpp), which holds the netlists, the MNA reduction, the DC operating points and the three voice circuits.

The Standard engine is unchanged; its output is bit-identical to before the Physical engine existed. Sessions, presets and factory tones open on Standard. A user tone saved on Physical restores Physical. Switching engines crossfades over 30 ms, and both engines have the same latency.

## Signal path

All of this runs at 8× the host rate, through JUCE's half-band polyphase IIR oversampler (the same one the Standard engine uses):

```
guitar → [12AX7 stage] → gain pot → [12AX7] → ([12AX7]) → coupling cap → tone stack
       → phase inverter (− negative feedback from the speaker, via a compensation low-pass)
       → 2 × [coupling cap · grid leak · grid stopper · power-tube grid]
       → push-pull pentodes → output transformer → reactive speaker load
every tube draws from: rectifier → reservoir C → choke/R → screen C → R → PI C → R → preamp C → R → V1 C
```

## Method

- **Netlist → MNA → DK state space.** Each linear sub-circuit is a netlist of resistors, capacitors, inductors and sources. In `prepare()` it is stamped into a Modified Nodal Analysis matrix, the reactive elements are discretised with the trapezoidal rule, and the system is reduced to the discrete K-method form (Yeh, Abel & Smith; Holters & Zölzer): `x' = Ax + Bu + Ci`, `y = Dx + Eu + Fi`, `v = Gx + Hu + Ki`, `i = f(v)`. The states are the capacitor and inductor memories.
- **Tube equations.** 12AX7: Koren's triode equation for plate current, which includes the plate-resistance modulation, plus a Child–Langmuir 3/2-power grid diode with a smooth onset. EL34 and 6L6: Koren's pentode equations, with screen current and the same grid diode. Each triode stage includes its inter-electrode capacitances Cgk and Cgp (Miller). The next stage's input capacitance is lumped onto the plate.
- **Newton–Raphson.** Each sample, each triode solves the 2×2 system `v = p + K i(v)` in (Vgk, Vpk), using a hand-derived analytic Jacobian. It stops when the residual is below 1 µV, when the Newton step is below 30 µV (the remaining residual is then quadratically small, around 50 nV at worst), or at a hard cap: 4 iterations in Fast mode, 6 in Exact. The power-tube grids and the push-pull plates are 1-D solves capped at 6. A tangent predictor, which applies last sample's Jacobian to the change in `p`, makes about 1.0–1.2 evaluations per solve typical. Backtracking (2-D) and bracketed Newton (1-D) handle transients that throw a stage across cutoff or into grid conduction within one sample. When the cap is hit, the last *evaluated* point is kept, so the currents fed to the state update always belong to the voltages.
- **Sag and bias shift.** Each stage's B+ is an input to its state space, taken from its own node of the supply network. When the reservoir is pulled down, every operating point moves on the next sample. Gain, clipping symmetry and headroom change with it, and so do the cross-stage intermodulation and the bloom of a tube rectifier. Grid current charging the coupling capacitors shifts bias the same way (blocking distortion). The Modern voice's middle stage idles cold (10 kΩ cathode) and clips one side first.

## Voices

| Voice | Preamp | Power amp | Supply |
|---|---|---|---|
| Clean | 2 stages, fully bypassed cathodes, tone-stack treble-cap load | 2 × 6L6, 4.2 kΩ | solid state, choke |
| Classic | 2 stages, partially bypassed (plexi-style bright first stage) | 2 × EL34, 3.4 kΩ | GZ34-style tube rectifier: about 20 V of sag when played hard |
| Modern | 3 stages, cold-clipper middle stage, small coupling caps, treble-bleed caps | 2 × EL34, 3.4 kΩ | solid state, choke |

The power-tube bias is calibrated at prepare time to a target idle current, the way a tech would set it. Levels are trimmed so the factory tones sit within about ±2 dB of the Standard engine. These are original circuits in the style of each amp family, not copies of specific products.

## Fast and Exact

**Tube Math** chooses how exp, log and atan are computed. **Fast**, the default, uses 64-entry tables with short polynomials (about 1e-13 relative error) and caps the triode solve at 4 iterations. **Exact** uses the system math library and a cap of 6. Over all factory tones they differ by about −110 dB relative to the signal. Tube Math is a per-computer preference and is not saved in user tones.

## Measured (Apple M1 Pro, `AxiomRender`)

- **CPU** (one core, 44.1 kHz, 128-sample blocks, whole plugin): Standard 2.2–3.6 %; Physical Fast about 10 % (Clean, Classic) and 13 % (Modern); Physical Exact 12.5–17 %. The original goal was under 5 %, and the Physical engine does not meet it. Each oversampled sample does 2–3 triode solves, two grid solves, a push-pull solve, a 6-state transformer/speaker update and a 5-node supply, at 352.8 kHz. Software pipelining across stages, force-inlining, column-major state updates and table-based math have already roughly halved the first working version's cost.
- **Aliasing** (inharmonic energy with a 3.1 kHz tone at gain 9, a deliberately harsh test): Classic −56 dB and Modern −54 dB, in line with the Standard engine's −54 to −57 dB. Clean is −47 dB: two fully bypassed stages driven that hard clip with very sharp edges.
- **Solver**: about 0.01 % of solves reach the iteration cap on a hard-played riff. Idle output with every control at maximum is −180 dBFS, so there is no feedback oscillation.

## Simplifications

Each stage is solved separately, driven by the previous plate as an ideal source. The rectifier is a DC source with a resistive or Child-law drop: it models sag dynamics, not mains ripple or hum. The phase inverter is linear. The output transformer does not saturate. The pipelined loop adds one oversampled sample of delay per block (about 11 µs at 44.1 kHz), which is not reported to the host.
