#include "test_harness.hpp"

#include "ToneStack.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

/*
 * ToneStack is the Bass/Mid/Treble/Presence EQ that core/NamBlock.hpp
 * puts after a NAM capture, mirroring AmpBlock's "Modern" voicing
 * (core/AmpBlock.hpp's kAmpVoicings[0]). These tests pin down the things
 * that matter for that promise: it must be *transparent* at 0dB (NAM users
 * keep their capture's own tone, untouched, until they turn a knob), each
 * knob must actually move the band it names, Presence must stay
 * boost-only like AmpBlock's, and reset() must really clear the filters.
 */

namespace {

constexpr double kSampleRate = 48000.0;

// Gain of the stack's actual time-domain output at a steady sine, in dB.
//
// RMS, not peak: when the probe frequency divides the sample rate exactly
// (8000 Hz at 48 kHz, say) the sampled points never land on the sine's
// crest, so a peak-of-samples reading comes out up to 1.25 dB low
// depending on the filters' phase shift. RMS over a long window doesn't
// care about phase. (Same helper idea as tests/test_biquad.cpp.)
double measureSineGainDB(ampforge::ToneStack& stack, double frequencyHz,
                         int settleSamples = 4800, int measureSamples = 48000)
{
    const double amplitude = 0.1;
    const double omega = 2.0 * M_PI * frequencyHz / kSampleRate;

    // Let the filters settle before measuring, so the start-up transient
    // doesn't skew the result.
    for (int n = 0; n < settleSamples; ++n)
        stack.process(static_cast<float>(amplitude * std::sin(omega * n)));

    double sumIn = 0.0;
    double sumOut = 0.0;
    for (int n = settleSamples; n < settleSamples + measureSamples; ++n)
    {
        const double input = amplitude * std::sin(omega * n);
        const double output = stack.process(static_cast<float>(input));
        sumIn += input * input;
        sumOut += output * output;
    }

    if (sumIn <= 0.0 || sumOut <= 0.0)
        return -200.0;
    return 10.0 * std::log10(sumOut / sumIn);
}

} // namespace

AMPFORGE_TEST(tonestack_default_is_transparent_at_1khz)
{
    ampforge::ToneStack stack;
    stack.setSampleRate(kSampleRate);

    // All four bands at 0dB is an identity filter mathematically (for
    // every Biquad type here the b and a coefficients come out identical,
    // so the pole and the zero cancel exactly), but four RBJ biquads in
    // float are not bit-exact: the coefficients are rounded doubles, and
    // the 120 Hz low shelf in particular is a near-cancelling pair whose
    // recursion noise gets amplified by its own near-DC pole. Measured
    // worst-case deviation for this 0.05-amplitude probe is ~3.5e-6 (about
    // -100 dBFS - inaudible), and the 800 Hz peak/3-5 kHz shelves on their
    // own contribute only ~1e-8. The tolerance below is therefore 1e-5
    // rather than the 1e-6 floated in the spec: at 1e-6 this test would be
    // asserting float32 luck, not behaviour. A smaller probe amplitude
    // would pass 1e-6, but only because the error scales with the signal,
    // which would make the test weaker rather than stronger.
    const double omega = 2.0 * M_PI * 1000.0 / kSampleRate;
    const double amplitude = 0.05;

    double worstDeviation = 0.0;
    for (int n = 0; n < 48000; ++n)
    {
        const float input = static_cast<float>(amplitude * std::sin(omega * n));
        const float output = stack.process(input);
        CHECK_FINITE(output);

        // Skip the first ~1000 samples: the filters' step response from a
        // zeroed state hasn't converged yet.
        if (n >= 1000)
            worstDeviation = std::max(worstDeviation, static_cast<double>(std::fabs(output - input)));
    }

    CHECK_NEAR(worstDeviation, 0.0, 1e-5);
}

AMPFORGE_TEST(tonestack_bass_boost_raises_60hz_by_about_the_dialed_gain)
{
    // 60 Hz sits deep on the LowShelf's flat top (center 120 Hz, Q 0.707),
    // so the boost there should be very close to the full dialed amount -
    // 60 Hz is the low E on a guitar, the most meaningful check of "Bass".
    ampforge::ToneStack flat;
    flat.setSampleRate(kSampleRate);

    ampforge::ToneStack boosted;
    boosted.setSampleRate(kSampleRate);
    boosted.setBassDB(12.0f);

    const double flatDB = measureSineGainDB(flat, 60.0);
    const double boostedDB = measureSineGainDB(boosted, 60.0);
    const double deltaDB = boostedDB - flatDB;

    // +/-2 dB of slack covers the shelf's own slope at the measurement
    // frequency plus the settle window - the point is "roughly the dialed
    // amount", not an exact match of the analog prototype.
    CHECK_NEAR(deltaDB, 12.0, 2.0);
}

AMPFORGE_TEST(tonestack_treble_cut_lowers_8khz)
{
    // 8 kHz is well above the HighShelf's 3000 Hz center, so it sits on
    // the shelf's flat top and the cut should show up nearly in full.
    ampforge::ToneStack flat;
    flat.setSampleRate(kSampleRate);

    ampforge::ToneStack cut;
    cut.setSampleRate(kSampleRate);
    cut.setTrebleDB(-12.0f);

    const double flatDB = measureSineGainDB(flat, 8000.0);
    const double cutDB = measureSineGainDB(cut, 8000.0);

    CHECK(cutDB < flatDB - 6.0);            // clearly quieter...
    CHECK_NEAR(cutDB - flatDB, -12.0, 2.0); // ...and roughly by the dialed amount
}

AMPFORGE_TEST(tonestack_negative_presence_behaves_like_zero_presence)
{
    // Presence is boost-only (see AmpBlock::updateToneStack()'s comment):
    // a negative value must clamp to 0dB, not carve a notch just above
    // Treble's range. Comparing sample-by-sample (not just RMS level)
    // proves the *coefficients* are identical, not merely a similar
    // average level.
    ampforge::ToneStack negative;
    negative.setSampleRate(kSampleRate);
    negative.setPresenceDB(-12.0f);

    ampforge::ToneStack zero;
    zero.setSampleRate(kSampleRate);
    zero.setPresenceDB(0.0f);

    // 8 kHz plus an odd low-frequency term, so the comparison exercises
    // both ends of the spectrum rather than one steady sine.
    const double omega = 2.0 * M_PI * 8000.0 / kSampleRate;
    bool identical = true;
    for (int n = 0; n < 4800; ++n)
    {
        const float input = static_cast<float>(0.3 * std::sin(omega * n) + 0.1 * std::sin(0.017 * n));
        if (negative.process(input) != zero.process(input))
            identical = false;
    }
    CHECK(identical);

    // A positive value must still do something - otherwise the clamp above
    // would be indistinguishable from "Presence is broken".
    ampforge::ToneStack boosted;
    boosted.setSampleRate(kSampleRate);
    boosted.setPresenceDB(12.0f);
    CHECK(measureSineGainDB(boosted, 12000.0) > 3.0);
}

AMPFORGE_TEST(tonestack_reset_clears_filter_state)
{
    // Same failure mode AudioBlock::reset() exists for: a block switched
    // off mid-ring-down must not replay that tail when switched back on.
    // Feeding a hot signal with the knobs boosted, then resetting, must
    // leave each biquad's history at exactly zero - so silence in gives
    // exactly 0.0f out (not a tiny leftover, not a denormal).
    ampforge::ToneStack stack;
    stack.setSampleRate(kSampleRate);
    stack.setBassDB(12.0f);
    stack.setMidDB(12.0f);
    stack.setTrebleDB(12.0f);
    stack.setPresenceDB(12.0f);

    const double omega = 2.0 * M_PI * 200.0 / kSampleRate;
    for (int n = 0; n < 2000; ++n)
        stack.process(static_cast<float>(0.5 * std::sin(omega * n)));

    stack.reset();

    bool allZero = true;
    for (int n = 0; n < 1000; ++n)
    {
        if (stack.process(0.0f) != 0.0f)
            allZero = false;
    }
    // Exact 0.0f, not "near zero": reset() zeroes x1/x2/y1/y2, so silence
    // in must yield bit-exact silence out.
    CHECK(allZero);
}

// --- the stack survives a non-finite sample ------------------------------
//
// A biquad's feedback path makes it a latch for anything non-finite: one NaN in
// (a NaN input gain, a corrupt preset parsed with strtof) and every subsequent
// output is NaN forever - the filter keeps multiplying the NaN by its own
// coefficients. The fix is to notice and clear the history, which is what this
// asserts: after a NaN the stack must come back to real audio, not stay
// poisoned.
//
// This is not a hypothetical: ChainPlugin forwards whatever a host sends, and a
// NaN parameter is one of the ways a plugin gets a "bricked" channel report.
AMPFORGE_TEST(tonestack_recovers_from_a_non_finite_sample)
{
    ampforge::ToneStack stack;
    stack.setSampleRate(kSampleRate);
    stack.setBassDB(6.0f);
    stack.setMidDB(-6.0f);
    stack.setTrebleDB(6.0f);
    stack.setPresenceDB(6.0f);

    // Establish a real signal first, so the recovery has to come back from a
    // running filter rather than from an untouched one.
    const double omega = 2.0 * M_PI * 440.0 / kSampleRate;
    for (int n = 0; n < 500; ++n)
        stack.process(static_cast<float>(0.3 * std::sin(omega * n)));

    // The poison sample is swallowed at the source: Biquad::process() checks its
    // own output and clears its history rather than letting the NaN into the
    // feedback path. What comes out is therefore finite - it is the *tail* of
    // the bands behind the one that reset (their ring-down is still real
    // audio), not silence, which is why this checks finiteness rather than
    // zero.
    CHECK_FINITE(stack.process(std::nanf("")));
    CHECK_FINITE(stack.process(0.0f));

    // Inf is caught by the same check (it is not finite either), and would
    // otherwise become NaN on the first Inf - Inf inside the filter.
    CHECK_FINITE(stack.process(std::numeric_limits<float>::infinity()));

    // And the filter is usable again - not merely finite, but actually passing
    // signal.
    double sumSquares = 0.0;
    const int measured = 2000;
    for (int n = 0; n < measured; ++n)
    {
        const float y = stack.process(static_cast<float>(0.3 * std::sin(omega * n)));
        CHECK_FINITE(y);
        sumSquares += static_cast<double>(y) * y;
    }
    const double rms = std::sqrt(sumSquares / measured);
    CHECK(rms > 0.05); // a 0.3-amplitude sine through a near-unity stack
}

// --- the stack stays stable at low sample rates --------------------------
//
// Biquad's coefficients come from the RBJ formulas, which are only valid for
// centre frequencies below Nyquist: above it the shelves' poles leave the unit
// circle and the filter turns into an oscillator. Presence's 5 kHz centre is
// exactly that case at an 8 kHz project rate (and only just stable at 11.025
// kHz), so Biquad clamps every band to 0.45*sr. Without the clamp, +12 dB of
// Presence at 8 kHz is full-scale noise - which is the kind of thing a user
// finds, not a test. This drives it at the two rates where the unclamped maths
// misbehaves.
AMPFORGE_TEST(tonestack_is_stable_at_low_sample_rates)
{
    const double rates[] = { 8000.0, 11025.0, 22050.0 };

    for (double sr : rates)
    {
        ampforge::ToneStack stack;
        stack.setSampleRate(sr);
        // Everything boosted: the worst case for a shelf whose poles are
        // drifting outward.
        stack.setBassDB(12.0f);
        stack.setMidDB(12.0f);
        stack.setTrebleDB(12.0f);
        stack.setPresenceDB(12.0f);

        const double omega = 2.0 * M_PI * 200.0 / sr;
        bool bounded = true;
        bool finite = true;
        double sumSquares = 0.0;
        const int measured = 20000;
        for (int n = 0; n < measured; ++n)
        {
            const float y = stack.process(static_cast<float>(0.1 * std::sin(omega * n)));
            finite = finite && std::isfinite(y);
            // A stable filter fed 0.1 can't come out louder than the largest
            // boost the stack can produce (four +12 dB shelves overlapping is
            // well under 100x); anything above that is a runaway.
            bounded = bounded && std::fabs(y) < 10.0f;
            sumSquares += static_cast<double>(y) * y;
        }
        CHECK(finite);
        CHECK(bounded);
        // ...and it is still passing audio. "Finite and bounded" is also true of
        // a filter that returns 0 for everything, which is exactly what a
        // too-aggressive clamp would produce - so a signal floor is what makes
        // the two checks above mean "stable" instead of "dead".
        CHECK(std::sqrt(sumSquares / measured) > 0.03); // 0.1 sine, +/-12 dB, still audible
    }
}

// --- a zero sample rate doesn't produce NaN coefficients -----------------
//
// setSampleRate() divides by the rate, so 0 (an uninitialised host field, or a
// block configured before the chain is) would make every coefficient NaN and
// every output NaN. Biquad falls back to a sane rate instead; this pins that
// down because the alternative is a silent, permanent NaN channel.
AMPFORGE_TEST(tonestack_survives_a_zero_sample_rate)
{
    ampforge::ToneStack stack;
    stack.setSampleRate(0.0);
    stack.setBassDB(6.0f);
    stack.setPresenceDB(6.0f);

    bool finite = true;
    double sumSquares = 0.0;
    const int measured = 2000;
    for (int n = 0; n < measured; ++n)
    {
        const float y = stack.process(static_cast<float>(0.1 * std::sin(2.0 * M_PI * 200.0 * n / 48000.0)));
        finite = finite && std::isfinite(y);
        sumSquares += static_cast<double>(y) * y;
    }
    CHECK(finite);
    // Not "not NaN" - the point is that the 44100 Hz fallback actually filters
    // audio, so a stack that went silent instead of NaN would fail here.
    CHECK(std::sqrt(sumSquares / measured) > 0.05);
}
