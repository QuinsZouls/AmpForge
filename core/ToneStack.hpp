#pragma once
#include "Biquad.hpp"
#include <algorithm>

/*
 * ToneStack - the four-band Bass/Mid/Treble/Presence EQ that AmpBlock
 * applies after its saturation stage, extracted into a reusable class.
 *
 * Why it exists: a NAM capture is a recording of one specific amp *with
 * its tone stack already baked in* - the .nam file has no Bass/Mid/Treble
 * knobs of its own. Left as-is, running a preset through NamBlock means
 * the player loses those four controls entirely. Giving NamBlock its own
 * ToneStack (see core/NamBlock.hpp) puts them back, and because it is the
 * *same* class applying the *same* filter types, centers and Q as
 * AmpBlock's "Modern" voicing (kAmpVoicings[0] in core/AmpBlock.hpp),
 * dialing the knobs to 0dB on either block gets a comparable, familiar
 * response - NAM doesn't get a second, differently-shaped tone stack
 * bolted on next to the one the rest of the plugin already has.
 *
 * Signal flow: Bass -> Mid -> Treble -> Presence, four Biquads in series,
 * exactly AmpBlock::processSample()'s order (see its comment - Presence in
 * particular sits *above* Treble's frequency and is boost-only, same
 * reasoning as there).
 *
 * Not an AudioBlock on purpose: only NamBlock owns one, and it is reached
 * through that block, so nothing ever needs a ToneStack through an
 * AudioBlock*.
 *
 * AmpBlock still runs its own four Biquads (core/AmpBlock.hpp's
 * updateToneStack()) rather than a ToneStack member - it predates this class
 * and its centres come from the selected voicing at runtime. The constants
 * below are a copy of kAmpVoicings[0]'s ("Modern"), which is what the choice
 * was: NAM gets the same familiar response as AmpBlock's default voicing
 * without AmpBlock having to be refactored first. The duplication is the one
 * thing to watch here, so tests/test_tonestack.cpp asserts the constants
 * individually - if a voicing is ever retuned, that test is where the copy
 * gets noticed instead of drifting silently.
 */

namespace ampforge {

class ToneStack
{
public:
    // Centers and Q deliberately mirror kAmpVoicings[0] ("Modern") in
    // core/AmpBlock.hpp. Copied as constants rather than referenced so
    // this header only needs Biquad.hpp - callers that just want the EQ
    // (and the dependency-free unit tests) don't have to pull AmpBlock in.
    static constexpr float kBassFreq     = 120.0f,  kBassQ     = 0.707f;
    static constexpr float kMidFreq      = 800.0f,  kMidQ      = 1.0f;
    static constexpr float kTrebleFreq   = 3000.0f, kTrebleQ   = 0.707f;
    static constexpr float kPresenceFreq = 5000.0f, kPresenceQ = 0.7f;

    void setSampleRate(double sr)
    {
        bass.setSampleRate(sr);
        mid.setSampleRate(sr);
        treble.setSampleRate(sr);
        presence.setSampleRate(sr);
        updateAll();
    }

    // --- Parameter setters: same units, same names as AmpBlock's ---
    //
    // One band each, not updateAll(): on LV2 a parameter change is delivered
    // from inside run(), so these run on the audio thread, and recomputing all
    // four bands would mean four sets of sin/cos/sqrt/pow (~16 transcendentals)
    // to change one knob. Recomputing only the band that changed is the same
    // arithmetic a hardware EQ section does and costs one band's worth.
    //
    // The coefficients still step rather than ramp (there is no smoothing
    // stage here - AmpBlock's tone stack behaves identically, so this keeps
    // parity). On a fast automation sweep that is a small zipper; a
    // coefficient ramp is the fix if it ever becomes audible, and it belongs
    // in Biquad, not here, so every filter gets it.
    void setBassDB(float db)     { bassDB = db;     updateBass(); }
    void setMidDB(float db)      { midDB = db;      updateMid(); }
    void setTrebleDB(float db)   { trebleDB = db;   updateTreble(); }
    void setPresenceDB(float db) { presenceDB = db; updatePresence(); }

    // Clears all four filters' sample history so a block that was switched
    // off and back on doesn't replay its stale ring-down - same reasoning as
    // AudioBlock::reset() and Biquad::reset(). The coefficients (i.e. the
    // knob settings) are deliberately left untouched.
    void reset()
    {
        bass.reset();
        mid.reset();
        treble.reset();
        presence.reset();
    }

    float process(float x)
    {
        x = bass.process(x);
        x = mid.process(x);
        x = treble.process(x);
        x = presence.process(x);
        return x;
    }

private:
    void updateAll()
    {
        updateBass();
        updateMid();
        updateTreble();
        updatePresence();
    }

    void updateBass()   { bass.setParams(Biquad::Type::LowShelf,    kBassFreq,   bassDB,   kBassQ); }
    void updateMid()    { mid.setParams(Biquad::Type::Peak,         kMidFreq,    midDB,    kMidQ); }
    void updateTreble() { treble.setParams(Biquad::Type::HighShelf, kTrebleFreq, trebleDB, kTrebleQ); }

    void updatePresence()
    {
        // std::max: Presence is boost-only, exactly like
        // AmpBlock::updateToneStack()'s identical clamp - a negative value
        // would carve a notch just above Treble's own range instead of doing
        // nothing, which isn't what "0" on a boost-only knob should mean.
        presence.setParams(Biquad::Type::HighShelf, kPresenceFreq,
                           std::max(0.0f, presenceDB), kPresenceQ);
    }

    Biquad bass, mid, treble, presence;
    float bassDB = 0.0f, midDB = 0.0f, trebleDB = 0.0f, presenceDB = 0.0f;
};

} // namespace ampforge
