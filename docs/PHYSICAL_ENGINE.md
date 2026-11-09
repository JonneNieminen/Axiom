# Physical amp engine

Setting **Amp Engine** to **Physical** replaces Axiom's tube-style waveshaper stages with circuit-level simulations of the preamp, phase inverter, power amp and power supply. The Standard engine is unchanged: its output is bit-identical to Axiom before the Physical engine existed, and sessions, presets and factory tones open on Standard. A user tone saved on Physical restores Physical. Switching engines crossfades over 30 ms, and both engines have the same latency.

## Code

| File | What's there |
|---|---|
| [`PhysicalMath.h`](../Source/PhysicalMath.h) | Fast and Exact math policies, the tube equations and their analytic derivatives |
| [`PhysicalCircuit.h/.cpp`](../Source/PhysicalCircuit.h) | Netlists, Modified Nodal Analysis, the discrete K-method reduction (real-time safe), the N-port Newton solver, SPICE export |
| [`PhysicalTubeStage.h/.cpp`](../Source/PhysicalTubeStage.h) | The three voice circuits, power stage, transformer core, supply, thermal drift, noise, and the per-sample loop |

## Signal path

Everything below runs at 8× the host rate, through the same half-band polyphase IIR oversampler the Standard engine uses.

```
guitar ─▶ PREAMP: every 12AX7 stage, the gain pot (with its bright cap), the cathode follower
          and the passive tone stack, solved as ONE coupled circuit
       ─▶ PHASE INVERTER: long-tail pair, coupling caps, grid leaks, stoppers and both power-tube
          grids as ONE coupled circuit; the presence pot is part of its feedback network
       ─▶ POWER AMP: push-pull EL34/6L6 (a slightly mismatched pair), screen-current split,
          output transformer with a saturating, hysteretic core, reactive 8 Ω speaker
all of it drawing on a POWER SUPPLY: 50 Hz mains → rectifier → reservoir → choke/R → RC nodes
```

On the Physical engine the amp knobs are the circuit's own pots. **Gain** is the volume pot (1 MΩ audio taper). **Bass**, **Middle** and **Treble** are the tone-stack pots, and **Presence** is the presence pot in the negative-feedback network. A knob at 0 dB is a pot at noon. Pots are resistors in the netlists: when one moves, its circuit's matrices are rebuilt on the audio thread, without allocating, at most every 128 oversampled samples while the knob glides.

## Method

- **Netlist, then MNA, then DK state space.** Each circuit is written as components. Capacitors and inductors are discretised with the trapezoidal rule, which turns each into a conductance plus a "history" current; those histories are the states. Eliminating the linear part leaves the discrete K-method form (Yeh, Abel & Smith; Holters & Zölzer): `x' = Ax + Bu + Ci`, `y = Dx + Eu + Fi`, `v = Gx + Hu + Ki`, `i = f(v)`.
- **Coupled solves.** Solving stage by stage would drop the interactions that make an amp feel like one. Grid current in one stage loads the plate before it. Each grid's Miller capacitance hangs on the previous plate. A cathode follower's grid current loads its driver. The tone stack's impedance loads whatever drives it. So each circuit's tube currents are solved together: 4 ports for Clean's preamp, 6 for Classic's, 8 for Modern's, and 6 for the phase inverter (two triodes plus two power grids).
- **Newton–Raphson** on `v = p + K·i(v)`, every sample, with a hand-derived analytic Jacobian. `J = K·∂i/∂v − I` is built column by column from each tube's 2×2 block, then LU-factored.
  - It stops when the residual is below 1 µV, or when a step is below 30 µV (the residual after that step is then about 50 nV at worst).
  - A tangent predictor (last sample's Jacobian applied to the change in `p`) gets most samples there in 1.1–1.8 evaluations.
  - Backtracking handles overshoot, and every iterate is bounded to physical tube voltages, so even a solve that ends at its cap cannot inject runaway energy.
- **Iteration cap: 10**, in both modes. This departs from the usual 4–6. Those suffice for one triode, but these circuits are coupled, and under hard high-frequency drive a grid can swing across cutoff into conduction within one sample. At 6, about 2% of such samples ended short, and the small charge errors they left on coupling capacitors added up to audible low-frequency junk. Against a 30-iteration reference that junk is gone at a cap of 10 and still present at 8. On a hard-played riff the cap is reached on 0.03–0.12% of solves.
- **B+ is an input, not a matrix coefficient.** Each circuit takes its supply voltages from its own filter nodes through `u`. When the supply sags or ripples, `p` moves, and every operating point, gain and clipping symmetry moves with it on the next sample. The matrices depend only on component values and the sample period.

## Devices

- **12AX7:** Dempwolf & Zölzer's model, fitted to measured tubes. The cathode current is a smoothed power law in the effective grid voltage, and the grid takes its share of it (`Ip = Ik − Ig`), so plate current drops when the grid conducts. Grid current is a Child–Langmuir 3/2-power diode with a smooth 0.3 V onset, matched to Dempwolf's measured grid curve within about 15%.
- **EL34 / 6L6:** Koren's pentode equations. Cathode current is shared between plate and screen: as a plate swings down into its knee, the current it fails to collect lands on the screen, which loads the screen supply and compresses a cranked power stage. The second tube's kg1 is 5% off the first's, an unmatched pair, which gives even harmonics, a little DC imbalance in the transformer and some mains hum.
- **Output transformer core:** flux is the integral of the winding voltage. The magnetising current rises steeply past saturation and lags the flux by a coercive current that flips with direction (a Prandtl play hysteron). At full power the flux swing reaches 0.73× saturation at 30 Hz, against 0.35× at 160 Hz. The core also carries a DC magnetisation at full drive (about 0.5× saturation at 160 Hz), from the two tubes' unequal average currents: the long-tail pair's 82k/100k plates drive them unequally, and the pair is mismatched. That is real physics, but its size is larger than a simple estimate gives, so it is listed below as unverified.
- **Tube heating:** plate dissipation heats each power tube with a 4-second time constant. Above half its rated dissipation the grid starts to emit, pushing current into the grid leak and the bias upward, so a power section that has been played hard idles a little hotter.
- **Noise:** the first stage's thermal noise (input resistance plus the tube's equivalent noise resistance, 4kTR over the band) plus 1/f flicker with a 2 kHz corner.

## Voices

Original circuits in the style of each amp family, not copies of specific products.

| Voice | Preamp (one coupled circuit) | Power amp | Supply |
|---|---|---|---|
| Clean | V1A → blackface-style tone stack → volume (120 pF bright cap) → V1B → mixer | 2 × 6L6, 4.2 kΩ | solid-state rectifier, choke |
| Classic | V1A → volume (470 pF bright cap) → 470k mixing resistor → V2A → cathode follower → Marshall-style tone stack | 2 × EL34, 3.4 kΩ | GZ34-style tube rectifier (Child law): it sags about 90 V when played hard |
| Modern | V1A → gain (470 pF bright cap) → V1B (cold clipper, 10k cathode) → 470k‖470 pF → V2A → cathode follower → tone stack → master at a fixed −30 dB | 2 × EL34, 3.4 kΩ | solid-state rectifier, choke |

Modern's master volume is internal and fixed, so the preamp does the distorting and the power amp is driven, not slammed. Axiom's own **Master** knob stays an output level, so presets keep their meaning. The fixed bias of the power tubes is calibrated at prepare time to a target idle current, as a technician would set it. Output levels are trimmed so the factory tones sit within about ±3 dB of the Standard engine.

## Fast and Exact

**Tube Math** chooses how exp, log and atan are computed. **Fast**, the default, uses 64-entry tables with short polynomials (about 1e-13 relative error). **Exact** uses the system math library. The circuits, solver and cap are the same in both modes.

On a smooth chord the two agree to −150 dB on every preset. On the factory plucks they agree band by band, but not sample by sample. A pluck starts with a full-scale noise burst, and a 100 dB-gain clipping amp turns any difference into moved zero crossings of clipped noise. The test measures this: nudging one input sample by 1e-7 V moves the band levels more than switching the math does.

## Validation (`AxiomRender`)

- **SPICE:** the same netlists, exported with the same tube equations as behavioural sources, are simulated by ngspice (trapezoidal, same step). The engine's solves agree with ngspice to −85 dB (Modern preamp, whose edges are square) through −96 dB (Classic preamp), −99 dB (Clean preamp) and −107 dB (phase inverter), RMS error relative to the signal, while heavily overdriven. The check is skipped where ngspice is not installed, including CI.
- **Derivatives:** every analytic derivative, and the full Jacobian of the Modern preamp and the phase inverter, matches central differences to within 3.3e-6.
- **Bench measurements:** idle operating points, sag, cap rate, core saturation, tube heating, idle noise and hum (at Gain 3.5: Clean −100 dBFS, Classic −82, Modern −77), aliasing, THD and levels against the Standard engine.
- **Robustness:** sweeping every pot while playing and switching engines mid-note are both click-free, and every preset stays bounded at 44.1–192 kHz.

## CPU

Measured on an Apple M1 Pro: one core, 44.1 kHz, 128-sample blocks, whole plugin. Standard uses 2–4%. Physical uses about 22% (Clean), 27% (Classic) and 32% (Modern) in Fast mode, and roughly 15% more in Exact.

This is far above the 5% originally aimed at. That target held for the earlier stage-by-stage model; coupling the circuits roughly doubled the cost. Each oversampled sample is an inherently serial chain: predictor → tube evaluations → Jacobian → LU factorisation → solve → state update, through two 6–8-dimensional circuits plus the power stage, 352,800 times a second.

Optimisations already in place:
- table-based math;
- force-inlined, fully unrolled fixed-size linear algebra, column-major and with split accumulators so it vectorises;
- a pivot-free LU fast path;
- the supply stepped at the base rate;
- the three blocks software-pipelined across samples.

Precomputed solution tables, the usual way to make DK models cheap, do not scale to coupled 6–8-dimensional nonlinearities.

## Simplifications that remain

- The phase inverter is driven by the preamp output as an ideal source: its own grid current does not load the tone stack.
- The two halves of the output transformer primary share one leakage inductance.
- Core hysteresis is a single Prandtl hysteron, not Jiles–Atherton.
- The rectifier sees an ideal 50 Hz sine: no mains impedance and no power-transformer saturation.
- The pipelined loop adds three oversampled samples of delay (about 8.5 µs at 44.1 kHz), which is not reported to the host.
- No re-amp null test against a real amplifier was possible here (it needs a real amp and load box). The circuits are validated against SPICE and voiced against the Standard engine's levels and distortion curves.
- The transformer's DC magnetisation at full drive (above) has not been checked against an independent simulation: the transformer and power stage are not part of the ngspice comparison.
