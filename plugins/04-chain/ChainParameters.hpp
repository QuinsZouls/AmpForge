#pragma once

/*
 * ChainParameters.hpp - the parameter index list, shared between the DSP
 * side (ChainPlugin.cpp) and the UI side (ChainUI.cpp).
 *
 * This has to live in its own header instead of inside ChainPlugin.cpp,
 * because the UI needs to know the exact same parameter indices to send
 * setParameterValue() calls that land on the right knob/switch - DPF
 * builds the DSP and UI as separate binaries, so they only agree on
 * "what parameter index N means" if they both include this file.
 */

START_NAMESPACE_DISTRHO

enum Parameters
{
    // Noise Gate
    kParamGateOn = 0,
    kParamGatePosition,
    kParamGateThreshold,
    kParamGateAttack,
    kParamGateRelease,

    // Compressor
    kParamCompOn,
    kParamCompPosition,
    kParamCompThreshold,
    kParamCompRatio,
    kParamCompAttack,
    kParamCompRelease,
    kParamCompMakeup,

    // Wah
    kParamWahOn,
    kParamWahPosition,
    kParamWahPedal,
    kParamWahQ,

    // Screamer
    kParamScreamerOn,
    kParamScreamerPosition,
    kParamScreamerDrive,
    kParamScreamerTone,
    kParamScreamerLevel,

    // Amp (always enabled)
    kParamAmpPosition,
    kParamAmpDrive,
    kParamAmpBass,
    kParamAmpMid,
    kParamAmpTreble,
    kParamAmpVolume,

    // Chorus
    kParamChorusOn,
    kParamChorusPosition,
    kParamChorusRate,
    kParamChorusDepth,
    kParamChorusMix,

    // Phaser
    kParamPhaserOn,
    kParamPhaserPosition,
    kParamPhaserRate,
    kParamPhaserDepth,
    kParamPhaserMix,

    // Tremolo
    kParamTremoloOn,
    kParamTremoloPosition,
    kParamTremoloRate,
    kParamTremoloDepth,

    // Delay
    kParamDelayOn,
    kParamDelayPosition,
    kParamDelayTime,
    kParamDelayFeedback,
    kParamDelayMix,

    // Reverb
    kParamReverbOn,
    kParamReverbPosition,
    kParamReverbRoomSize,
    kParamReverbDamping,
    kParamReverbMix,

    // Bypass switches - separate from *On* (which controls whether a
    // pedal is on the board at all). Bypass lets a pedal stay visible
    // in the rack while its processing is temporarily switched off,
    // like a real pedal's footswitch - added at the end of the enum so
    // every existing parameter index above stays unchanged.
    kParamGateBypass,
    kParamCompBypass,
    kParamWahBypass,
    kParamScreamerBypass,
    kParamChorusBypass,
    kParamPhaserBypass,
    kParamTremoloBypass,
    kParamDelayBypass,
    kParamReverbBypass,

    // Distortion - added after the original parameter set (including the
    // bypass block above) so every existing parameter index, and every
    // already-saved preset/session, keeps working unchanged. A harder,
    // more aggressive second gain stage alongside Screamer's overdrive -
    // see core/DistortionBlock.hpp.
    kParamDistortionOn,
    kParamDistortionPosition,
    kParamDistortionDrive,
    kParamDistortionTone,
    kParamDistortionLevel,
    kParamDistortionBypass,

    // Cabinet - convolves with a loaded speaker-cabinet impulse response
    // (see core/CabinetBlock.hpp). The IR file path itself isn't a plain
    // automatable value, so it isn't a parameter at all - it travels as
    // DPF plugin State instead, under kCabinetIRStateKey below, which is
    // why this block only has Mix/Level/On/Position/Bypass here.
    kParamCabinetOn,
    kParamCabinetPosition,
    kParamCabinetMix,
    kParamCabinetLevel,
    kParamCabinetBypass,

    // Gate Range - how far the gate pulls the signal down when closed,
    // instead of always slamming it to full silence. Added after every
    // other parameter (including Cabinet above) so existing parameter
    // indices, and every already-saved preset/session, keep working
    // unchanged - same reasoning as the Bypass block and Distortion/
    // Cabinet additions above.
    kParamGateRange,

    // Input Gain - a fixed pre-chain trim, always applied first and not
    // part of the reorderable pedalboard (see ChainUI.cpp's dedicated
    // Input panel). Guitars/interfaces feed wildly different levels into
    // the plugin; without a way to normalize that first, one Noise Gate
    // threshold default can't work for everyone - trimming here, ahead
    // of the gate, is what lets it. Appended at the end like every block
    // above, so existing parameter indices/presets stay unchanged.
    kParamInputGain,

    // Tempo sync - lets Delay/Tremolo/Chorus lock their rate to the host's
    // BPM (via DPF's TimePosition API) instead of only the free-running
    // ms/Hz knob. Each is a single index into kSyncDivisions below: 0 means
    // "Free" (sync off, the block's own Time/Rate parameter is used
    // unchanged), any other value picks a musical note division that
    // ChainPlugin::run() recomputes from the host tempo every block.
    // Appended at the end like every block above, so existing parameter
    // indices/presets stay unchanged.
    kParamDelaySync,
    kParamTremoloSync,
    kParamChorusSync,

    // Tuner - a standalone pitch-detection overlay (core/PitchDetector.hpp)
    // that taps a copy of the post-Input-Gain, pre-chain signal, entirely
    // outside the reorderable pedalboard, so it always reads the guitar's
    // actual pitch regardless of what Distortion/Wah/etc. are doing to the
    // signal that reaches the amp. kParamTunerOn is the UI's toggle button
    // (ChainPlugin::run() only bothers feeding the detector while it's on);
    // its output-only frequency reading lives at the very end of this enum,
    // with the other output-only parameters - see the comment down there
    // for why.
    kParamTunerOn,

    // Amp Type - selects between the handful of EQ-and-saturation
    // voicings defined in core/AmpBlock.hpp's kAmpVoicings (index 0,
    // "Modern", reproduces AmpBlock's original fixed behavior exactly, so
    // every preset/session saved before this parameter existed is
    // unaffected). Lives in AmpBlock.hpp rather than here since it's a
    // property of the Amp DSP block itself, reused as-is by the standalone
    // 02-amp plugin too - not something specific to this chain plugin the
    // way kSyncDivisions above is.
    kParamAmpType,

    // NAM - a neural network capture of one specific real amp/pedal/cab,
    // loaded from a .nam file (core/NamBlock.hpp), reusing NeuralAmpModelerCore
    // (github.com/sdatkinson/NeuralAmpModelerCore, includes A1 and the newer
    // A2 architecture). A separate, optional block from the parametric
    // AmpBlock above, not a replacement for it - the two are independent
    // tone sources the user can mix and match (see kParamAmpOn/Bypass
    // below for why Amp not being bypassable used to be a problem for
    // this pairing specifically). The .nam file
    // path itself isn't a plain automatable value, so like Cabinet's IR
    // above it isn't a parameter at all - it travels as DPF plugin State
    // under kNamModelStateKey below, which is why this block only has
    // On/Position/InputTrim/OutputLevel/Mix/Bypass here. Appended at the
    // end like every block above, so existing parameter indices/presets
    // stay unchanged.
    kParamNamOn,
    kParamNamPosition,
    kParamNamInputTrim,
    kParamNamOutputLevel,
    kParamNamMix,
    kParamNamBypass,

    // Amp On/Bypass - added well after the original Amp parameter block
    // above, which had no way to disable it at all (every comment calling
    // it "the always-on parametric AmpBlock" meant it literally: no other
    // block in this whole enum lacks an On/Bypass pair). That turned out
    // to matter once NAM existed: AmpBlock's Drive/saturation stage is a
    // tanh() soft-clip that runs unconditionally, even with Drive/Bass/
    // Mid/Treble/Volume all left at their neutral 0dB defaults - measured
    // at ~2% third-harmonic THD for a realistic guitar peak level (~0.5),
    // growing well past that for a hotter signal. NAM's own model expects
    // a clean, unprocessed input (it's already a capture of one specific
    // amp/pedal/cab), so with NAM sitting after Amp at their respective
    // default positions, that unavoidable pre-saturation quietly feeds
    // NAM something it was never trained on. On/Bypass here let Amp
    // actually be taken out of the signal path - same as every other
    // block already could - instead of just approximating "off" via
    // Drive=0dB. Both default to on/not-bypassed (see initParameter() in
    // ChainPlugin.cpp and kBlankPresetValues/kPresets) so every preset or
    // session saved before these existed still sounds identical.
    kParamAmpOn,
    kParamAmpBypass,

    // Autotune - classic "hard-tune" vocal pitch correction
    // (core/AutotuneBlock.hpp), the first block in the chain that isn't
    // guitar-oriented (Chorus/Phaser/Tremolo/Delay/Reverb are generic
    // enough to double as vocal effects too, but this one only makes
    // sense for a voice). Sits in the same reorderable pedalboard as
    // everything else - a "vocal chain" is just Amp/Cabinet/NAM turned
    // off and this (plus Delay/Reverb) turned on, same mechanism as
    // switching between guitar tones. Key is 0-11 (C..B), Scale indexes
    // ampforge::kScales (Chromatic/Major/Minor), Speed is the classic
    // autotune "Retune Speed" knob (0 = slow glide into the correction,
    // 1 = the near-instant robotic snap) - see AutotuneBlock.hpp's
    // comment for what it actually controls. Appended at the end like
    // every block above, so existing parameter indices/presets stay
    // unchanged.
    kParamAutotuneOn,
    kParamAutotunePosition,
    kParamAutotuneKey,
    kParamAutotuneScale,
    kParamAutotuneSpeed,
    kParamAutotuneBypass,

    // De-esser - the second vocal-only block (core/DeEsserBlock.hpp),
    // tames harsh "s"/"sh" sibilance. Frequency is where the sibilant
    // band starts (typically 4-9kHz depending on the voice), Threshold
    // is the level (in that band) above which it kicks in, Reduction is
    // the maximum amount of gain reduction it's allowed to apply -
    // see DeEsserBlock.hpp's comment for the split-band technique this
    // uses. Appended at the end like every block above, so existing
    // parameter indices/presets stay unchanged.
    kParamDeEsserOn,
    kParamDeEsserPosition,
    kParamDeEsserFrequency,
    kParamDeEsserThreshold,
    kParamDeEsserReduction,
    kParamDeEsserBypass,

    // EQ - a 3-band parametric equalizer pedal (core/EqBlock.hpp), distinct
    // from Amp's built-in Bass/Mid/Treble tone stack (core/AmpBlock.hpp) in
    // that every band's *center frequency* is its own knob here too, not
    // fixed per amp voicing - a real sweepable EQ pedal rather than a tone
    // stack baked into the amp. Low/High are shelving filters, Mid is a
    // peaking filter (same three filter types Amp's own tone stack already
    // uses, see core/Biquad.hpp) - only each band's Q is fixed rather than
    // a knob, same reasoning as De-esser's fixed Q above, keeping this
    // pedal to 6 knobs instead of 9. Appended at the end like every block
    // above, so existing parameter indices/presets stay unchanged.
    kParamEqOn,
    kParamEqPosition,
    kParamEqLowFreq,
    kParamEqLowGain,
    kParamEqMidFreq,
    kParamEqMidGain,
    kParamEqHighFreq,
    kParamEqHighGain,
    kParamEqBypass,

    // Amp Presence - a high-shelf sitting above Treble's own frequency,
    // modeling a real amp's negative-feedback high-frequency boost (see
    // core/AmpBlock.hpp's comment). Lives on the Amp block itself (like
    // Bass/Mid/Treble above), not a separate pedal, since it's part of
    // the amp's own tone stack. Boost-only (0 = off, matching every
    // preset/session saved before this existed), unlike Bass/Mid/Treble.
    // Appended at the end like every block above, so existing parameter
    // indices/presets stay unchanged.
    kParamAmpPresence,

    // Doubler - Automatic Double Tracking (core/DoublerBlock.hpp), a
    // second vocal-only block alongside Autotune/De-esser above (though,
    // like Chorus/Delay/Reverb, nothing stops a guitarist from using it
    // too). Delay is the base delay time, Depth is how far a filtered-
    // noise LFO wobbles that delay (see DoublerBlock.hpp's own comment
    // for why noise, not a clean sine like Chorus). Appended at the end
    // like every block above, so existing parameter indices/presets stay
    // unchanged.
    kParamDoublerOn,
    kParamDoublerPosition,
    kParamDoublerDelay,
    kParamDoublerDepth,
    kParamDoublerMix,
    kParamDoublerBypass,

    // Harmonizer - adds a second voice a fixed interval above/below the
    // input (core/HarmonizerBlock.hpp) - Interval is in semitones (e.g.
    // +7 = a fifth up). Unlike Autotune, this doesn't track the input's
    // pitch first; see HarmonizerBlock.hpp's own comment for why a fixed
    // interval is the deliberate choice here, not a missing feature.
    // Appended at the end like every block above, so existing parameter
    // indices/presets stay unchanged.
    kParamHarmonizerOn,
    kParamHarmonizerPosition,
    kParamHarmonizerInterval,
    kParamHarmonizerMix,
    kParamHarmonizerBypass,

    // Exciter - a "vocal exciter"/air-band brightness enhancer
    // (core/ExciterBlock.hpp), the third vocal-oriented block. Frequency
    // is where the enhanced "air" band starts, Amount is how much of the
    // newly-generated harmonic content gets layered back on top of the
    // dry signal (see ExciterBlock.hpp's own comment for why this is
    // additive, not a dry/wet Mix like every other block here). Appended
    // at the end like every block above, so existing parameter indices/
    // presets stay unchanged.
    kParamExciterOn,
    kParamExciterPosition,
    kParamExciterFrequency,
    kParamExciterAmount,
    kParamExciterBypass,

    // NAM's guitar-amp controls - the tone knobs a real amp has and a .nam
    // capture does not, plus A2's CPU/accuracy trade-off.
    //
    // The tone stack is NamBlock's own (core/NamBlock.hpp, core/ToneStack.hpp):
    // Bass/Mid/Treble/Presence, same filter types, centers and Q as
    // AmpBlock's "Modern" voicing (kAmpVoicings[0]) and the same dB units as
    // the Amp parameters above, so the two blocks' tone controls feel
    // identical. That is what makes the NAM node read like an amp rather than
    // like a bare capture: a .nam file has its amp's tone stack baked into the
    // recording, so without these the player loses all four knobs.
    //
    // Quality is the A2 "slimmable" model size: 1.0 = full size (the only
    // behaviour that existed before this parameter), 0.0 = the smallest and
    // cheapest submodel, and it is a no-op for every non-A2 capture.
    //
    // Why these five sit here rather than next to kParamNamOn with the rest
    // of the NAM block: appending is mandatory, exactly like every earlier
    // addition in this enum. Inserting them next to kParamNamOn would
    // renumber every parameter after it, which shifts existing DAW
    // automation lanes, saved sessions and older preset files onto the wrong
    // controls. They are *not* at the literal end of the enum only because
    // the output-only meters below have to be (see their own comment) - what
    // matters is that every automatable parameter comes before them.
    // (The append-only rule is also why the *display* names of
    // kParamNamInputTrim/kParamNamOutputLevel were changed to "NAM Gain"/
    // "NAM Master" rather than their symbols - nam_input_trim and
    // nam_output_level keep their values and both indices untouched, because
    // some hosts key saved automation/sessions by symbol.)
    //
    // All five default to neutral/1.0 - 0dB on the four tone knobs, full size
    // on Quality - which is precisely the state NamBlock had before they
    // existed, so every preset and session saved earlier sounds exactly the
    // same. (Note this is also why they are *not* left to zero-init in the
    // factory presets in ChainPresets.hpp: 0.0f on Quality would mean "the
    // cheapest, worst-sounding submodel", not "the old behaviour".)
    kParamNamBass,
    kParamNamMid,
    kParamNamTreble,
    kParamNamPresence,
    kParamNamQuality,

    // Output-only meters (kParameterIsOutput) - deliberately grouped here,
    // all together at the very end, rather than appended next to the
    // input/feature they each report on (which is where they originally
    // lived, and read more naturally). DPF's LV2 presets.ttl exporter
    // writes the non-output parameters as one comma-separated lv2:port
    // list and only closes it with a "." once it hits either the last
    // parameter or an output one - so *any* output parameter that isn't
    // at the very end splits that list into multiple statements with no
    // subject on the second one, which is invalid Turtle syntax (verified
    // with lv2_validate: Carla's own lilv-based loader shrugs it off and
    // loads the plugin fine, but it leaves the host's *own* separate LV2
    // preset browser broken/empty for that bundle). Hence: every
    // automatable parameter must come before this group, and nothing may
    // ever be appended after it.
    //
    // That rule is why the NAM tone/quality parameters above were added
    // *ahead* of this group rather than at the literal end of the enum, and
    // why this group moved down by five slots. Appending them after
    // kParamSampleRate was the first thing tried and it does break the
    // export exactly as described: presets.ttl came out with the NAM
    // parameters as a second, subject-less "[ ... ] ." statement. Moving
    // these five instead is safe in a way moving anything else would not
    // be: they are output-only - not automatable, never part of a preset
    // (see ChainPresets.hpp's comment), and re-read from the DSP every
    // block - so renumbering them cannot strand a saved automation lane or
    // session on the wrong control. Their own order and meaning are
    // unchanged.
    //
    // KNOWN VIOLATION of that rule, inherited from the plugin-hosting change
    // and deliberately not fixed here: the external-plugin slots
    // (kParamPlugin1On onwards, further down) were appended *after* this
    // group, so they do split the preset export exactly as described above -
    // "nothing may ever be appended after it" now reads as a rule that has
    // already been broken once. The fix is to move that whole block above
    // kParamInputLevel, which cannot strand a saved automation lane as long as
    // the plugin feature has not shipped: it is one contiguous renumbering of
    // parameters that were introduced together. It is left out of this change
    // on purpose - the NAM parameters above do not care where the plugin slots
    // live, and relocating 112 unrelated parameters (plus their preset columns)
    // belongs with the plugin feature, not with NAM's tone stack.
    //
    // Input Level - the post-trim peak level, reported back to the UI so
    // players can see how hot their signal is while dialing Input Gain
    // and the Noise Gate in together. ChainPlugin::run() writes it every
    // block, the host polls it and forwards the value to the UI's
    // parameterChanged() - the same mechanism DPF's own Meters example
    // uses.
    kParamInputLevel,
    // CPU Load - how much of the available per-block time run() actually
    // used (1.0 = took the whole block interval, i.e. right at the edge of
    // an audio dropout), reported back to the UI the same way Input Level
    // is above. ChainPlugin::run() times itself with std::chrono and
    // writes this every block.
    kParamCpuLoad,
    // Tuner Frequency - the detected pitch in Hz for kParamTunerOn above
    // (0 = no confident pitch right now), which the UI turns into a note
    // name and cents offset for display.
    kParamTunerFrequency,
    // Buffer Size - the host's last-seen block size in samples (the
    // `frames` argument to ChainPlugin::run()), reported back to the UI's
    // status sidebar the same way as the meters above. Purely informational
    // - hosts are free to vary this from call to call, so it reflects
    // "what just happened", not a fixed setting AmpForge controls.
    kParamBufferSize,
    // Sample Rate - the rate the host actually reported to the DSP, in Hz,
    // reported back to the UI the same way Buffer Size above is. The UI has
    // its own DPF-provided getSampleRate(), but hosts are not obliged to
    // tell the *UI* anything about the rate (Ardour at 48 kHz left it at
    // DPF's 44100 default, which the sidebar then displayed - see issue
    // #1). The DSP always gets a real sampleRateChanged() call, so this
    // mirrors what the audio path is genuinely running at, and the EQ
    // curve is drawn from it too instead of from the UI's own guess.
    kParamSampleRate,

    // External Plugin Slots (1 - 8)
    // Note: The original fixed user-facing slots ("Plugin 1" .. "Plugin 4") are DEPRECATED.
    // AmpForge now exposes a single generic "Plugin" node (Slot 0) in the palette, while
    // additional slots are dynamically assigned to built-in / configured plugins loaded
    // from ~/.config/ampforge/plugins.json.
    //
    // Slot 1 (Default / Generic "Plugin" node)
    kParamPlugin1On,
    kParamPlugin1Position,
    kParamPlugin1Bypass,
    kParamPlugin1Mix,
    kParamPlugin1InTrim,
    kParamPlugin1OutTrim,
    kParamPlugin1Macro1,
    kParamPlugin1Macro2,
    kParamPlugin1Macro3,
    kParamPlugin1Macro4,
    kParamPlugin1Macro5,
    kParamPlugin1Macro6,
    kParamPlugin1Macro7,
    kParamPlugin1Macro8,

    // Slot 2 (Built-in Plugin Slot 1)
    kParamPlugin2On,
    kParamPlugin2Position,
    kParamPlugin2Bypass,
    kParamPlugin2Mix,
    kParamPlugin2InTrim,
    kParamPlugin2OutTrim,
    kParamPlugin2Macro1,
    kParamPlugin2Macro2,
    kParamPlugin2Macro3,
    kParamPlugin2Macro4,
    kParamPlugin2Macro5,
    kParamPlugin2Macro6,
    kParamPlugin2Macro7,
    kParamPlugin2Macro8,

    // Slot 3 (Built-in Plugin Slot 2)
    kParamPlugin3On,
    kParamPlugin3Position,
    kParamPlugin3Bypass,
    kParamPlugin3Mix,
    kParamPlugin3InTrim,
    kParamPlugin3OutTrim,
    kParamPlugin3Macro1,
    kParamPlugin3Macro2,
    kParamPlugin3Macro3,
    kParamPlugin3Macro4,
    kParamPlugin3Macro5,
    kParamPlugin3Macro6,
    kParamPlugin3Macro7,
    kParamPlugin3Macro8,

    // Slot 4 (Built-in Plugin Slot 3)
    kParamPlugin4On,
    kParamPlugin4Position,
    kParamPlugin4Bypass,
    kParamPlugin4Mix,
    kParamPlugin4InTrim,
    kParamPlugin4OutTrim,
    kParamPlugin4Macro1,
    kParamPlugin4Macro2,
    kParamPlugin4Macro3,
    kParamPlugin4Macro4,
    kParamPlugin4Macro5,
    kParamPlugin4Macro6,
    kParamPlugin4Macro7,
    kParamPlugin4Macro8,

    // Slot 5 (Built-in Plugin Slot 4)
    kParamPlugin5On,
    kParamPlugin5Position,
    kParamPlugin5Bypass,
    kParamPlugin5Mix,
    kParamPlugin5InTrim,
    kParamPlugin5OutTrim,
    kParamPlugin5Macro1,
    kParamPlugin5Macro2,
    kParamPlugin5Macro3,
    kParamPlugin5Macro4,
    kParamPlugin5Macro5,
    kParamPlugin5Macro6,
    kParamPlugin5Macro7,
    kParamPlugin5Macro8,

    // Slot 6 (Built-in Plugin Slot 5)
    kParamPlugin6On,
    kParamPlugin6Position,
    kParamPlugin6Bypass,
    kParamPlugin6Mix,
    kParamPlugin6InTrim,
    kParamPlugin6OutTrim,
    kParamPlugin6Macro1,
    kParamPlugin6Macro2,
    kParamPlugin6Macro3,
    kParamPlugin6Macro4,
    kParamPlugin6Macro5,
    kParamPlugin6Macro6,
    kParamPlugin6Macro7,
    kParamPlugin6Macro8,

    // Slot 7 (Built-in Plugin Slot 6)
    kParamPlugin7On,
    kParamPlugin7Position,
    kParamPlugin7Bypass,
    kParamPlugin7Mix,
    kParamPlugin7InTrim,
    kParamPlugin7OutTrim,
    kParamPlugin7Macro1,
    kParamPlugin7Macro2,
    kParamPlugin7Macro3,
    kParamPlugin7Macro4,
    kParamPlugin7Macro5,
    kParamPlugin7Macro6,
    kParamPlugin7Macro7,
    kParamPlugin7Macro8,

    // Slot 8 (Built-in Plugin Slot 7)
    kParamPlugin8On,
    kParamPlugin8Position,
    kParamPlugin8Bypass,
    kParamPlugin8Mix,
    kParamPlugin8InTrim,
    kParamPlugin8OutTrim,
    kParamPlugin8Macro1,
    kParamPlugin8Macro2,
    kParamPlugin8Macro3,
    kParamPlugin8Macro4,
    kParamPlugin8Macro5,
    kParamPlugin8Macro6,
    kParamPlugin8Macro7,
    kParamPlugin8Macro8,

    kParamCount
};

// Aliases for the single generic "Plugin" node (mapped to Slot 0 / Plugin 1 parameters):
static constexpr int kParamPluginOn       = kParamPlugin1On;
static constexpr int kParamPluginPosition = kParamPlugin1Position;
static constexpr int kParamPluginBypass   = kParamPlugin1Bypass;
static constexpr int kParamPluginMix      = kParamPlugin1Mix;
static constexpr int kParamPluginInTrim   = kParamPlugin1InTrim;
static constexpr int kParamPluginOutTrim  = kParamPlugin1OutTrim;
static constexpr int kParamPluginMacro1   = kParamPlugin1Macro1;
static constexpr int kParamPluginMacro2   = kParamPlugin1Macro2;
static constexpr int kParamPluginMacro3   = kParamPlugin1Macro3;
static constexpr int kParamPluginMacro4   = kParamPlugin1Macro4;
static constexpr int kParamPluginMacro5   = kParamPlugin1Macro5;
static constexpr int kParamPluginMacro6   = kParamPlugin1Macro6;
static constexpr int kParamPluginMacro7   = kParamPlugin1Macro7;
static constexpr int kParamPluginMacro8   = kParamPlugin1Macro8;

static constexpr int kPluginSlotCount = 8;
static constexpr int kPluginMacroCount = 8;

static const char* const kPluginSelectFileStateKey = "plugin_select_file";

static const char* const kPluginPathStateKeys[kPluginSlotCount] = {
    "plugin_0_path", "plugin_1_path", "plugin_2_path", "plugin_3_path",
    "plugin_4_path", "plugin_5_path", "plugin_6_path", "plugin_7_path"
};
static const char* const kPluginIdStateKeys[kPluginSlotCount] = {
    "plugin_0_id", "plugin_1_id", "plugin_2_id", "plugin_3_id",
    "plugin_4_id", "plugin_5_id", "plugin_6_id", "plugin_7_id"
};
static const char* const kPluginFormatStateKeys[kPluginSlotCount] = {
    "plugin_0_format", "plugin_1_format", "plugin_2_format", "plugin_3_format",
    "plugin_4_format", "plugin_5_format", "plugin_6_format", "plugin_7_format"
};
static const char* const kPluginMapStateKeys[kPluginSlotCount] = {
    "plugin_0_map", "plugin_1_map", "plugin_2_map", "plugin_3_map",
    "plugin_4_map", "plugin_5_map", "plugin_6_map", "plugin_7_map"
};
static const char* const kPluginBlobStateKeys[kPluginSlotCount] = {
    "plugin_0_blob", "plugin_1_blob", "plugin_2_blob", "plugin_3_blob",
    "plugin_4_blob", "plugin_5_blob", "plugin_6_blob", "plugin_7_blob"
};

// Tempo-sync note divisions, shared between the DSP side (ChainPlugin.cpp,
// which turns a division into an actual ms/Hz value from the host's BPM)
// and the UI side (ChainUI.cpp, which shows the division's label on the
// Sync knob) - same reasoning as kCabinetIRStateKey below. Index 0 ("Free")
// means tempo sync is off; beatMultiplier is relative to a quarter note
// (e.g. 0.5 = an eighth note), so `beatMs * beatMultiplier` gives the
// synced time in milliseconds for any host BPM.
struct SyncDivision
{
    const char* label;
    float beatMultiplier;
};

static constexpr int kSyncDivisionCount = 8;
static const SyncDivision kSyncDivisions[kSyncDivisionCount] =
{
    { "Free", 0.0f },
    { "1/1",  4.0f },
    { "1/2",  2.0f },
    { "1/4",  1.0f },
    { "1/8",  0.5f },
    { "1/16", 0.25f },
    { "1/8.", 0.75f },
    { "1/8T", 1.0f / 3.0f },
};

// The DPF State key that carries the Cabinet block's loaded impulse-
// response file path. Shared between ChainPlugin.cpp (the state's actual
// owner) and ChainUI.cpp (which requests/display it), the same way the
// Parameters enum above is shared, so the two sides can't drift apart.
static const char* const kCabinetIRStateKey = "cabinet_ir_path";

// The DPF State key that carries the NAM block's loaded .nam model file
// path - same reasoning and pattern as kCabinetIRStateKey above.
static const char* const kNamModelStateKey = "nam_model_path";

// NAM model architecture display and load-failure notification used to be
// two more DPF State keys here (kNamModelInfoStateKey, kNamModelErrorStateKey),
// pushed from ChainPlugin::setState() via Plugin::updateStateValue(). That
// push is a no-op on this project's vendored DPF for the VST3 backend (its
// callback is wired to nullptr) and for CLAP (its updateState() is a stub
// that never notifies the UI) - only LV2/Carla-native/standalone jack ever
// delivered it, so on the two most commonly used formats the status
// sidebar never heard about a successful load, and a failed one never got
// a toast. Both are now derived entirely client-side in ChainUI.cpp
// (see readNamFileInfo()) by reading the .nam file directly off of
// the path in kNamModelStateKey below - a path ChainUI always has
// correctly, on every format, since DPF's file-browser glue echoes it to
// the UI directly rather than through this plugin-initiated push.

// The DPF State key used to open a native "pick a file" dialog for
// importing a preset (see ChainUI.cpp's importPresetsFromFile() and
// handleControlBarClick()'s Import button). kStateIsOnlyForUI (see
// ChainPlugin.cpp's initState()) keeps the picked path from ever being
// persisted as part of the session - unlike the Cabinet IR above, it's a
// one-shot action, not a lasting characteristic of the plugin instance.
static const char* const kPresetImportStateKey = "preset_import_path";

END_NAMESPACE_DISTRHO
