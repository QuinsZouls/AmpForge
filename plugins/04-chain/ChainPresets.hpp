#pragma once

/*
 * ChainPresets.hpp - the factory preset data table, shared between the
 * DSP side (ChainPlugin.cpp, which reports these to the host's program
 * list) and the UI side (ChainUI.cpp, which shows its own in-window
 * preset bar so presets are reachable without digging through the
 * host's own program dropdown).
 */

#include "ChainParameters.hpp"

START_NAMESPACE_DISTRHO

// --- Factory presets ---
//
// Each preset is just a full snapshot of every parameter's value, in
// the same order as the Parameters enum above. loadProgram() applies
// this snapshot by calling setParameterValue() for every entry, which
// keeps the chain's internal state and the EffectChain enabled/position
// flags perfectly in sync - the same code path a host uses when it
// moves a knob. (The Cabinet block's loaded IR file, if any, is DPF
// State rather than a parameter, so it isn't part of this snapshot -
// factory presets never carry a bundled IR.)
//
// Chain order (the *Position values) is kept at the default pedalboard
// layout for every preset here - only which blocks are on/off and their
// tone-shaping parameters differ between presets. That default layout is
// Gate(0) -> Comp(1) -> Wah(2) -> Screamer(3) -> Distortion(4) -> Amp(5)
// -> Cabinet(6) -> NAM(7) -> Chorus(8) -> Phaser(9) -> Tremolo(10) ->
// Delay(11) -> Reverb(12). NAM sits right after Cabinet - tonally
// alongside Amp/Cabinet, since it's a capture of a real amp/pedal/cab -
// rather than at the very end, even though it's off (and so functionally
// inert) in every preset below.
//
// Gain-staging rule these all follow: AmpBlock's Drive feeds a tanh()
// soft-clip, and Screamer/Distortion are hard clippers ahead of it (see
// their headers in core/). Stacking two hard clippers at high Drive back
// to back (Screamer into Distortion, or either one dimed straight into a
// maxed Amp Drive) mostly just flattens the waveform into a fizzy square
// wave rather than adding useful character - so each preset below leans
// on ONE stage for most of the grit and keeps the others as a modest
// boost/tone-shaper, and none of the output-level knobs (Screamer/
// Distortion Level, Amp Volume) stack positive dB on top of each other,
// since nothing downstream clips an overs signal for us.
//
// Concretely, that "one stage" still needs its own Drive kept in a range
// where it tracks a note's decay instead of just pinning at its clip
// ceiling for the note's entire audible length: verified by feeding a
// synthesized decaying note through each preset's active blocks and
// checking the output actually decays (not stuck at a squared-off max
// forever) and its peak doesn't run away past the input's own headroom.
// Screamer's clamp is +-0.7, so Drive much past ~2 already hard-clips a
// typical picked note almost everywhere except its very tail; Distortion's
// clamp is tighter still (+0.55/-0.85), so its usable Drive range is lower
// again, not higher, despite the larger nominal range in its header comment.
// Amp Drive's tanh is comparatively forgiving on its own, but every dB of
// Bass/Mid/Treble boost downstream eats back into that same headroom, so a
// hot mid-boost preset needs a correspondingly lower Drive than a flat-EQ
// one to land in the same place.
//
// Amp Type (see core/AmpBlock.hpp's kAmpVoicings) now varies per preset
// below, rather than every preset silently defaulting to Modern - each
// voicing besides Modern has its own driveTrimDB, which
// AmpBlock::updateDriveGain() subtracts from the user's own Drive dB
// before the tanh stage (a positive trim is *more* headroom, i.e. *less*
// effective gain at the same Drive setting - see that function's own
// comment). Wherever a preset switches off Modern, its own Amp Drive
// value has been adjusted by exactly that trim so the *effective*
// saturation into the tanh lands back at the same point this file's own
// decay-tracking verification above already checked - the voicing changes
// the EQ curve and clipping asymmetry a preset gets, not how hard it's
// actually being driven beyond what was already verified.
//
// Note on the five values each preset spells out just before its meter slots:
// NAM's bass/mid/treble/presence/quality. Quality in particular has to be
// written even though nothing was using it before: 0.0f would mean "the
// cheapest, smallest A2 submodel" - the worst-sounding setting - not "the old
// behaviour". So all three presets spell the five out: the four tone bands
// neutral (0dB) and Quality at 1.0 (full size), which together are exactly
// what NamBlock did before those controls existed.
//
// Note on the output-only parameters (Input Level, CPU Load, Tuner Frequency,
// Buffer Size, Sample Rate): these used to be the tail of the Parameters enum,
// which is what let every preset simply omit them - an aggregate initialiser
// zero-fills whatever is left at the end. They are no longer the tail (the
// external-plugin slots come after them), so all three presets now spell those
// five zeros out explicitly and then list the plugin slots. Getting this wrong
// is silent: the array is `float[kParamCount]`, so one missing zero shifts
// every value after it five slots off target and still compiles. That is
// exactly what happened when the plugin slots were first added - hence the
// explicit zeros and the enabled=true, position=N values the plugin slots now
// receive, which a static count of this file's own initialisers pins down.
static constexpr uint32_t kProgramCount = 3;

struct PresetDefinition
{
    const char* name;
    float values[kParamCount];
};

// clang-format off
static const PresetDefinition kPresets[kProgramCount] =
{
    // "Pristine Clean" - flat, uncolored headroom: Modern voicing, Amp
    // Drive at 0dB (barely warms the tanh stage), no boost pedal or
    // distortion stage, no modulation, nothing but a light comp for even
    // level and a short room reverb for space. Deliberately the "zero
    // coloration" reference point the other two presets are built away
    // from.
    {
        "Pristine Clean",
        {
            /* Gate       on,pos,thr,atk,rel      */ 0.0f, 0.0f, -50.0f, 5.0f, 150.0f,
            /* Comp       on,pos,thr,ratio,atk,rel,makeup */ 1.0f, 1.0f, -22.0f, 2.5f, 8.0f, 100.0f, 0.5f,
            /* Wah        on,pos,pedal,q  */ 0.0f, 2.0f, 0.5f, 3.0f,
            /* Screamer   on,pos,drive,tone,level */ 0.0f, 3.0f, 1.0f, 0.5f, 0.0f,
            /* Amp        pos,drive,bass,mid,treble,vol */ 5.0f, 0.0f, 1.0f, 0.0f, 2.0f, 0.0f,
            /* Chorus     on,pos,rate,depth,mix */ 0.0f, 13.0f, 1.0f, 5.0f, 0.5f,
            /* Phaser     on,pos,rate,depth,mix */ 0.0f, 14.0f, 0.5f, 0.7f, 0.5f,
            /* Tremolo    on,pos,rate,depth */ 0.0f, 15.0f, 5.0f, 0.5f,
            /* Delay      on,pos,time,fb,mix */ 0.0f, 16.0f, 300.0f, 0.3f, 0.3f,
            /* Reverb     on,pos,room,damp,mix */ 1.0f, 17.0f, 0.25f, 0.6f, 0.15f,
            /* Bypass: gate,comp,wah,screamer,chorus,phaser,tremolo,delay,reverb */ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Distortion on,pos,drive,tone,level,bypass */ 0.0f, 4.0f, 4.0f, 0.5f, 0.0f, 0.0f,
            /* Cabinet    on,pos,mix,level,bypass */ 0.0f, 6.0f, 1.0f, 0.0f, 0.0f,
            /* Gate Range */ 40.0f,
            /* Input Gain */ 0.0f,
            /* Delay/Tremolo/Chorus sync, Tuner on, Amp Type (0 = Modern) */
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* NAM on,pos,inputTrim,outputLevel,mix,bypass */ 0.0f, 7.0f, 0.0f, 0.0f, 1.0f, 0.0f,
            /* Amp on,bypass */                                1.0f, 0.0f,
            /* Autotune on,pos,key,scale,speed,bypass */       0.0f, 8.0f, 0.0f, 0.0f, 0.5f, 0.0f,
            /* De-esser on,pos,freq,threshold,reduction,bypass */ 0.0f, 9.0f, 6000.0f, -30.0f, 12.0f, 0.0f,
            /* EQ on,pos,lowFreq,lowGain,midFreq,midGain,highFreq,highGain,bypass */ 0.0f, 18.0f, 100.0f, 0.0f, 800.0f, 0.0f, 3000.0f, 0.0f, 0.0f,
            /* Amp Presence */ 2.0f,
            /* Doubler on,pos,delay,depth,mix,bypass */ 0.0f, 10.0f, 20.0f, 4.0f, 0.4f, 0.0f,
            /* Harmonizer on,pos,interval,mix,bypass */ 0.0f, 11.0f, 7.0f, 0.4f, 0.0f,
            /* Exciter on,pos,freq,amount,bypass */ 0.0f, 12.0f, 6000.0f, 0.3f, 0.0f,
            /* NAM bass,mid,treble,presence,quality */ 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
            /* Output-only meters - Input Level, CPU Load, Tuner Frequency,
               Buffer Size, Sample Rate. Spelled out as zeros rather than
               omitted: they sit *between* the NAM block and the plugin slots
               in the Parameters enum, and an aggregate initialiser can only
               omit values at the END, so leaving them out would shift every
               plugin value below five slots off target. */
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 1 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 19.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 2 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 20.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 3 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 21.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 4 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 22.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 5 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 23.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 6 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 24.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 7 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 25.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 8 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 26.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        }
    },

    // "Rock Crunch" - a light Screamer boost (its own hard clipper barely
    // engaging) pushes the Amp's Crunch voicing (-4dB trim - see
    // AmpBlock.hpp's updateDriveGain()) into an honest mid-gain rhythm/lead
    // breakup - Drive is 5.0 here specifically to land the *effective*
    // saturation at this file's originally-verified Modern/Drive=9.0 point
    // (5.0 - (-4) = 9dB). A tight noise gate and punchy comp keep chords
    // defined, a short slapback delay plus a bit of room reverb add space
    // without smearing the attack.
    {
        "Rock Crunch",
        {
            /* Gate       */ 1.0f, 0.0f, -42.0f, 3.0f, 100.0f,
            /* Comp       */ 1.0f, 1.0f, -20.0f, 3.5f, 8.0f, 120.0f, 1.5f,
            /* Wah        */ 0.0f, 2.0f, 0.5f, 3.0f,
            /* Screamer   */ 1.0f, 3.0f, 1.4f, 0.55f, 1.0f,
            /* Amp        */ 5.0f, 5.0f, 2.0f, 2.5f, 1.5f, 0.0f,
            /* Chorus     */ 0.0f, 13.0f, 1.0f, 5.0f, 0.5f,
            /* Phaser     */ 0.0f, 14.0f, 0.5f, 0.7f, 0.5f,
            /* Tremolo    */ 0.0f, 15.0f, 5.0f, 0.5f,
            /* Delay      */ 1.0f, 16.0f, 300.0f, 0.25f, 0.15f,
            /* Reverb     */ 1.0f, 17.0f, 0.4f, 0.5f, 0.18f,
            /* Bypass: gate,comp,wah,screamer,chorus,phaser,tremolo,delay,reverb */ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Distortion */ 0.0f, 4.0f, 4.0f, 0.5f, 0.0f, 0.0f,
            /* Cabinet    */ 0.0f, 6.0f, 1.0f, 0.0f, 0.0f,
            /* Gate Range */ 40.0f,
            /* Input Gain */ 0.0f,
            /* Delay/Tremolo/Chorus sync, Tuner on, Amp Type (2 = Crunch) */
            0.0f, 0.0f, 0.0f, 0.0f, 2.0f,
            /* NAM on,pos,inputTrim,outputLevel,mix,bypass */ 0.0f, 7.0f, 0.0f, 0.0f, 1.0f, 0.0f,
            /* Amp on,bypass */                                1.0f, 0.0f,
            /* Autotune on,pos,key,scale,speed,bypass */       0.0f, 8.0f, 0.0f, 0.0f, 0.5f, 0.0f,
            /* De-esser on,pos,freq,threshold,reduction,bypass */ 0.0f, 9.0f, 6000.0f, -30.0f, 12.0f, 0.0f,
            /* EQ on,pos,lowFreq,lowGain,midFreq,midGain,highFreq,highGain,bypass */ 0.0f, 18.0f, 100.0f, 0.0f, 800.0f, 0.0f, 3000.0f, 0.0f, 0.0f,
            /* Amp Presence */ 2.5f,
            /* Doubler on,pos,delay,depth,mix,bypass */ 0.0f, 10.0f, 20.0f, 4.0f, 0.4f, 0.0f,
            /* Harmonizer on,pos,interval,mix,bypass */ 0.0f, 11.0f, 7.0f, 0.4f, 0.0f,
            /* Exciter on,pos,freq,amount,bypass */ 0.0f, 12.0f, 6000.0f, 0.3f, 0.0f,
            /* NAM bass,mid,treble,presence,quality */ 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
            /* Output-only meters - Input Level, CPU Load, Tuner Frequency,
               Buffer Size, Sample Rate. Spelled out as zeros rather than
               omitted: they sit *between* the NAM block and the plugin slots
               in the Parameters enum, and an aggregate initialiser can only
               omit values at the END, so leaving them out would shift every
               plugin value below five slots off target. */
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 1 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 19.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 2 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 20.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 3 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 21.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 4 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 22.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 5 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 23.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 6 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 24.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 7 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 25.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 8 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 26.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        }
    },

    // "Heavy Distortion" - Distortion is the *only* gain stage (Screamer
    // stays off entirely - stacking two hard clippers just flattens the
    // wave into fizz rather than adding character, see this file's
    // gain-staging note above), on the Hi-Gain voicing (-8dB trim - see
    // AmpBlock.hpp's updateDriveGain()) with Amp Drive left low (0.0, so
    // effective saturation into the tanh is 8dB - Distortion is already
    // doing the heavy lifting, the Amp stage just adds a touch of its own
    // asymmetric compression on top). A tight, fast noise gate and a deep
    // mid scoop with a rolled-off treble keep palm-muted chugs tight
    // instead of fizzy; dry (no delay/reverb) so nothing softens the
    // attack.
    {
        "Heavy Distortion",
        {
            /* Gate       */ 1.0f, 0.0f, -38.0f, 1.0f, 60.0f,
            /* Comp       */ 0.0f, 1.0f, -18.0f, 4.0f, 10.0f, 100.0f, 0.0f,
            /* Wah        */ 0.0f, 2.0f, 0.5f, 3.0f,
            /* Screamer   */ 0.0f, 3.0f, 1.2f, 0.4f, 0.0f,
            /* Amp        */ 5.0f, 0.0f, 2.0f, -4.0f, 0.0f, 0.0f,
            /* Chorus     */ 0.0f, 13.0f, 1.0f, 5.0f, 0.5f,
            /* Phaser     */ 0.0f, 14.0f, 0.5f, 0.7f, 0.5f,
            /* Tremolo    */ 0.0f, 15.0f, 5.0f, 0.5f,
            /* Delay      */ 0.0f, 16.0f, 300.0f, 0.3f, 0.3f,
            /* Reverb     */ 0.0f, 17.0f, 0.5f, 0.5f, 0.3f,
            /* Bypass: gate,comp,wah,screamer,chorus,phaser,tremolo,delay,reverb */ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Distortion on,pos,drive,tone,level,bypass */ 1.0f, 4.0f, 2.5f, 0.3f, -1.0f, 0.0f,
            /* Cabinet    */ 0.0f, 6.0f, 1.0f, 0.0f, 0.0f,
            /* Gate Range */ 60.0f,
            /* Input Gain */ 0.0f,
            /* Delay/Tremolo/Chorus sync, Tuner on, Amp Type (3 = Hi-Gain) */
            0.0f, 0.0f, 0.0f, 0.0f, 3.0f,
            /* NAM on,pos,inputTrim,outputLevel,mix,bypass */ 0.0f, 7.0f, 0.0f, 0.0f, 1.0f, 0.0f,
            /* Amp on,bypass */                                1.0f, 0.0f,
            /* Autotune on,pos,key,scale,speed,bypass */       0.0f, 8.0f, 0.0f, 0.0f, 0.5f, 0.0f,
            /* De-esser on,pos,freq,threshold,reduction,bypass */ 0.0f, 9.0f, 6000.0f, -30.0f, 12.0f, 0.0f,
            /* EQ on,pos,lowFreq,lowGain,midFreq,midGain,highFreq,highGain,bypass */ 0.0f, 18.0f, 100.0f, 0.0f, 800.0f, 0.0f, 3000.0f, 0.0f, 0.0f,
            /* Amp Presence */ 1.5f,
            /* Doubler on,pos,delay,depth,mix,bypass */ 0.0f, 10.0f, 20.0f, 4.0f, 0.4f, 0.0f,
            /* Harmonizer on,pos,interval,mix,bypass */ 0.0f, 11.0f, 7.0f, 0.4f, 0.0f,
            /* Exciter on,pos,freq,amount,bypass */ 0.0f, 12.0f, 6000.0f, 0.3f, 0.0f,
            /* NAM bass,mid,treble,presence,quality */ 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
            /* Output-only meters - Input Level, CPU Load, Tuner Frequency,
               Buffer Size, Sample Rate. Spelled out as zeros rather than
               omitted: they sit *between* the NAM block and the plugin slots
               in the Parameters enum, and an aggregate initialiser can only
               omit values at the END, so leaving them out would shift every
               plugin value below five slots off target. */
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 1 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 19.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 2 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 20.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 3 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 21.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 4 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 22.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 5 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 23.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 6 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 24.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 7 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 25.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            /* Plugin 8 on,pos,bypass,mix,inTrim,outTrim,m1..m8 */ 0.0f, 26.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        }
    },
};
// clang-format on

END_NAMESPACE_DISTRHO
