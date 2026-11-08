#pragma once
#include "AxiomDsp.h"
#include <array>

namespace axiom
{
struct AxiomPreset
{
    const char* name;
    AxiomParams params;
};

/** Factory tones. They are inspired by the amps, guitars and pedals behind each sound; they
    are starting points, not models of any specific rig. `source` is a hardware choice and is
    never changed by a preset. Field order follows AxiomParams. */
inline const std::array<AxiomPreset, 7> factoryPresets {{
    // voice, source, input, gain, bass, mid, treble, presence, master, room, boost, chorus, delayMix, delayMs, cabinet, gate (dB, -80 = off)
    { "Axiom Default",                 { 1, 0, 0.0f, 3.5f,  0.0f,  0.0f,  0.0f, 0.0f, -3.0f, 16.0f,  0.0f,  0.0f,  0.0f, 380.0f, true, -80.0f } },
    { "George Lynch (Dokken)",         { 2, 0, 0.0f, 6.8f, -1.0f,  1.5f,  1.5f, 2.0f, -3.0f, 12.0f, 20.0f,  0.0f, 14.0f, 340.0f, true, -62.0f } },
    { "Van Halen",                     { 1, 0, 0.0f, 7.6f,  1.0f,  2.0f,  0.5f, 1.5f, -5.5f, 10.0f,  0.0f,  8.0f, 10.0f, 290.0f, true, -68.0f } },
    { "David Gilmour",                 { 1, 0, 0.0f, 5.0f,  1.0f,  1.0f, -0.5f, 0.0f, -5.0f, 28.0f, 65.0f,  0.0f, 32.0f, 440.0f, true, -80.0f } },
    { "Mark Knopfler (Clean)",         { 0, 0, 0.0f, 3.6f,  0.0f, -1.0f,  2.5f, 1.5f, -3.0f, 14.0f,  0.0f,  0.0f,  0.0f, 380.0f, true, -80.0f } },
    { "Mark Knopfler (Les Paul Dist)", { 1, 0, 0.0f, 6.0f, -1.0f,  4.0f,  0.0f, 0.5f, -5.5f, 12.0f, 35.0f,  0.0f,  0.0f, 380.0f, true, -70.0f } },
    { "80s Clean (Huge Chorus)",       { 0, 0, 0.0f, 2.8f,  1.0f, -1.5f,  3.0f, 2.5f, -1.0f, 32.0f,  0.0f, 70.0f, 12.0f, 360.0f, true, -80.0f } },
}};
}
