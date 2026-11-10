/*
 * AmpForge - Main Chain Plugin (Phase 8)
 *
 * The main AmpForge plugin: a single plugin instance running a
 * reorderable chain of 12 modules, the way Guitar Rig / BIAS FX work.
 *
 * Default chain order (a typical pedalboard layout):
 *   0: Noise Gate  1: Compressor  2: Wah         3: Screamer  4: Distortion
 *   5: Amp         6: Cabinet     7: Chorus      8: Phaser    9: Tremolo
 *   10: Delay      11: Reverb
 *
 * Every block has a "Position" parameter (0-11) controlling where it
 * sits in the chain - this is what EffectChain (core/EffectChain.hpp)
 * uses to decide processing order. Until we build the GUI, position
 * is set via plain automatable parameters from the host.
 *
 * Ahead of all 12 of those sits Input Gain: a fixed pre-chain trim,
 * applied directly in run() rather than through an EffectChain slot,
 * since (unlike every block above) it isn't reorderable and can't be
 * turned off - see kParamInputGain's comment in ChainParameters.hpp.
 */

#include "DistrhoPlugin.hpp"
#include "ChainParameters.hpp"
#include "ChainPresets.hpp"
#include "ScreamerBlock.hpp"
#include "DistortionBlock.hpp"
#include "CabinetBlock.hpp"
#include "NamBlock.hpp"
#include "AmpBlock.hpp"
#include "DelayBlock.hpp"
#include "ReverbBlock.hpp"
#include "CompressorBlock.hpp"
#include "NoiseGateBlock.hpp"
#include "ChorusBlock.hpp"
#include "PhaserBlock.hpp"
#include "TremoloBlock.hpp"
#include "WahBlock.hpp"
#include "AutotuneBlock.hpp"
#include "DeEsserBlock.hpp"
#include "EqBlock.hpp"
#include "DoublerBlock.hpp"
#include "HarmonizerBlock.hpp"
#include "ExciterBlock.hpp"
#include "EffectChain.hpp"
#include "PitchDetector.hpp"
#include "PluginNodeBlock.hpp"
#include "host/PluginScanner.hpp"
#include "host/HostedPluginProvider.hpp"
#include "extra/Base64.hpp"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <sstream>

START_NAMESPACE_DISTRHO

// clang-format on

// How long the Input Level meter's peak takes to fall back down between
// hits, in ms - fast enough to track playing dynamics, slow enough not to
// flicker illegibly on every sample. Same idea as NoiseGateBlock's release.
static constexpr float kInputMeterDecayMs = 250.0f;

// Time constant for the CPU Load meter's decay - see cpuLoad's comment
// below. Slower than the Input Level meter's, since CPU spikes (a big
// convolution block, a burst of denormals) are worth lingering on rather
// than just tracking playing dynamics.
static constexpr float kCpuMeterDecayTauSec = 0.5f;


class ChainPlugin : public Plugin, public ampforge::HostedPluginProvider
{
public:
    ChainPlugin()
        : Plugin(kParamCount, kProgramCount, 3 + kPluginSlotCount * 5 + 1)
    {
        // Allocated once, here, at the largest block anything will ask for -
        // and never resized (see preChainBuffer's own comment). Doing it in the
        // constructor is what lets updateMaxBlockSizes() be callable from a host
        // callback that runs while audio is rolling without ever handing the
        // audio thread a buffer whose memory moves.
        preChainBuffer.resize(kMaxPreChainFrames);
        gateBlock.setThresholdDB(-50.0f);
        gateBlock.setAttackMs(5.0f);
        gateBlock.setReleaseMs(150.0f);
        gateBlock.setRangeDB(40.0f);

        compBlock.setThresholdDB(-18.0f);
        compBlock.setRatio(4.0f);
        compBlock.setAttackMs(10.0f);
        compBlock.setReleaseMs(100.0f);
        compBlock.setMakeupGainDB(0.0f);

        wahBlock.setPedalPosition(0.5f);
        wahBlock.setQ(3.0f);

        screamerBlock.setDrive(1.0f);
        screamerBlock.setTone(0.5f);
        screamerBlock.setLevel(0.0f);

        distortionBlock.setDrive(4.0f);
        distortionBlock.setTone(0.5f);
        distortionBlock.setLevel(0.0f);

        cabinetBlock.setMix(1.0f);
        cabinetBlock.setLevel(0.0f);

        namBlock.setInputTrim(0.0f);
        namBlock.setOutputTrim(0.0f);
        namBlock.setMix(1.0f);

        // NAM's own guitar-amp tone stack (core/ToneStack.hpp) plus the A2
        // slimmable size: all four tone knobs neutral (0dB) and Quality at
        // 1.0 (full size) is exactly the state this block had before those
        // controls existed, so nothing changes for a preset/session saved
        // before them - see the kParamNamBass..kParamNamQuality comment in
        // ChainParameters.hpp.
        namBlock.setBassDB(0.0f);
        namBlock.setMidDB(0.0f);
        namBlock.setTrebleDB(0.0f);
        namBlock.setPresenceDB(0.0f);
        namBlock.setQuality(1.0f);

        // Block-processing scratch for run(): the chain's block buffers,
        // NAM's per-block buffers and our own trimmed-input buffer all get
        // sized from the same number. Re-applied on
        // activate()/bufferSizeChanged() below, since a host is free to
        // change its buffer size - or re-activate - after construction.
        //
        // getBufferSize() is 0 while a host hasn't announced anything yet
        // (and for DPF's dummy/meta-data instances), hence the 512 fallback:
        // it covers every buffer size a plugin host realistically hands a
        // chain, and EffectChain/NamBlock each allocate a correct 512-frame
        // default themselves anyway - so this call is only about matching
        // what the host actually said, and a host that never says anything
        // still gets correct (and, at <= 512 frames, fast) processing.
        updateMaxBlockSizes(getBufferSize() != 0 ? getBufferSize() : 512);

        ampBlock.setDriveDB(0.0f);
        ampBlock.setBassDB(0.0f);
        ampBlock.setMidDB(0.0f);
        ampBlock.setTrebleDB(0.0f);
        ampBlock.setPresenceDB(0.0f);
        ampBlock.setVolumeDB(0.0f);

        chorusBlock.setRateHz(1.0f);
        chorusBlock.setDepthMs(5.0f);
        chorusBlock.setMix(0.5f);

        phaserBlock.setRateHz(0.5f);
        phaserBlock.setDepth(0.7f);
        phaserBlock.setMix(0.5f);

        tremoloBlock.setRateHz(5.0f);
        tremoloBlock.setDepth(0.5f);

        delayBlock.setDelayTimeMs(300.0f);
        delayBlock.setFeedback(0.3f);
        delayBlock.setMix(0.3f);

        reverbBlock.setRoomSize(0.5f);
        reverbBlock.setDamping(0.5f);
        reverbBlock.setMix(0.3f);

        autotuneBlock.setKey(0);
        autotuneBlock.setScale(0);
        autotuneBlock.setRetuneSpeed(0.5f);

        deEsserBlock.setFrequency(6000.0f);
        deEsserBlock.setThresholdDB(-30.0f);
        deEsserBlock.setReductionDB(12.0f);

        eqBlock.setLowFreq(100.0f);
        eqBlock.setLowGainDB(0.0f);
        eqBlock.setMidFreq(800.0f);
        eqBlock.setMidGainDB(0.0f);
        eqBlock.setHighFreq(3000.0f);
        eqBlock.setHighGainDB(0.0f);

        doublerBlock.setDelayMs(20.0f);
        doublerBlock.setDepthMs(4.0f);
        doublerBlock.setMix(0.4f);

        harmonizerBlock.setIntervalSemitones(7.0f);
        harmonizerBlock.setMix(0.4f);

        exciterBlock.setFrequency(6000.0f);
        exciterBlock.setAmount(0.3f);

        // Default pedalboard order:
        // Gate(0) -> Comp(1) -> Wah(2) -> Screamer(3) -> Distortion(4) ->
        // Amp(5) -> Cabinet(6) -> NAM(7) -> Autotune(8) -> De-esser(9) ->
        // Doubler(10) -> Harmonizer(11) -> Exciter(12) -> Chorus(13) ->
        // Phaser(14) -> Tremolo(15) -> Delay(16) -> Reverb(17) -> EQ(18).
        // NAM sits right after Cabinet - see kParamNamPosition's case in
        // initParameter() below for why.
        //
        // Autotune/De-esser/Doubler/Harmonizer/Exciter sit *before* the
        // modulation/time-based block (Chorus..Reverb), not after, even
        // though they were added to the codebase later - this isn't just
        // guitar-vs-vocal tidiness, it's a real signal-flow requirement.
        // Autotune tracks pitch by autocorrelation, which reverb/delay
        // tails actively confuse (a smeared, reflected copy of the signal
        // reads as noise to a pitch tracker); Doubler/Harmonizer generate
        // a second voice *from* the input, so if that input already
        // carries reverb/delay, the generated voice carries a smeared
        // copy of it too instead of getting its own clean treatment
        // before the shared ambience is added once, at the end; Exciter
        // adding "air" to an already-reverberant blend colors the reverb
        // tail instead of just brightening the dry voice. None of that
        // is true for EQ - reshaping frequency content doesn't care what
        // fed it - so EQ keeps its own original placement, last in the
        // chain, as a final tone-correction/mastering stage (see its own
        // comment in ChainParameters.hpp). Every position here is still
        // just a starting default - the whole rack is freely reorderable
        // by dragging, this is only about what a fresh instance starts at.
        chain.addBlock(&gateBlock, 0);
        chain.addBlock(&compBlock, 1);
        chain.addBlock(&wahBlock, 2);
        chain.addBlock(&screamerBlock, 3);
        chain.addBlock(&distortionBlock, 4);
        chain.addBlock(&ampBlock, 5);
        chain.addBlock(&cabinetBlock, 6);
        chain.addBlock(&namBlock, 7);
        chain.addBlock(&autotuneBlock, 8);
        chain.addBlock(&deEsserBlock, 9);
        chain.addBlock(&doublerBlock, 10);
        chain.addBlock(&harmonizerBlock, 11);
        chain.addBlock(&exciterBlock, 12);
        chain.addBlock(&chorusBlock, 13);
        chain.addBlock(&phaserBlock, 14);
        chain.addBlock(&tremoloBlock, 15);
        chain.addBlock(&delayBlock, 16);
        chain.addBlock(&reverbBlock, 17);
        chain.addBlock(&eqBlock, 18);
        for (int s = 0; s < kPluginSlotCount; ++s)
        {
            pluginNodes[s].setName("Plugin " + std::to_string(s + 1));
            chain.addBlock(&pluginNodes[s], 19 + s);
            chain.setEnabled(&pluginNodes[s], false);
        }
        chain.rebuildOrder();

        // Amp starts on and un-bypassed (ampOn=true, ampBypass=false above),
        // matching its previous unconditional always-on behavior exactly -
        // see kParamAmpOn/Bypass's comment in ChainParameters.hpp.
        chain.setEnabled(&ampBlock, true);

        // Everything else starts disabled except Screamer, which matches
        // the previous default (a basic overdrive-into-amp sound out of the box).
        chain.setEnabled(&gateBlock, false);
        chain.setEnabled(&compBlock, false);
        chain.setEnabled(&wahBlock, false);
        chain.setEnabled(&screamerBlock, true);
        chain.setEnabled(&distortionBlock, false);
        chain.setEnabled(&cabinetBlock, false);
        chain.setEnabled(&namBlock, false);
        chain.setEnabled(&chorusBlock, false);
        chain.setEnabled(&phaserBlock, false);
        chain.setEnabled(&tremoloBlock, false);
        chain.setEnabled(&delayBlock, false);
        chain.setEnabled(&reverbBlock, false);
        chain.setEnabled(&autotuneBlock, false);
        chain.setEnabled(&deEsserBlock, false);
        chain.setEnabled(&eqBlock, false);
        chain.setEnabled(&doublerBlock, false);
        chain.setEnabled(&harmonizerBlock, false);
        chain.setEnabled(&exciterBlock, false);

        // Defensive initialization: some hosts (notably the JACK/PipeWire
        // standalone target) can start processing audio before they call
        // sampleRateChanged(). Every block that allocates its buffers in
        // setSampleRate() would otherwise start out with empty buffers.
        // Calling it here with whatever sample rate the host reports at
        // construction time guarantees the buffers exist from sample one.
        const double initialSampleRate = getSampleRate();
        updateInputLevelDecay(initialSampleRate);
        gateBlock.setSampleRate(initialSampleRate);
        compBlock.setSampleRate(initialSampleRate);
        wahBlock.setSampleRate(initialSampleRate);
        screamerBlock.setSampleRate(initialSampleRate);
        distortionBlock.setSampleRate(initialSampleRate);
        cabinetBlock.setSampleRate(initialSampleRate);
        namBlock.setSampleRate(initialSampleRate);
        ampBlock.setSampleRate(initialSampleRate);
        chorusBlock.setSampleRate(initialSampleRate);
        phaserBlock.setSampleRate(initialSampleRate);
        tremoloBlock.setSampleRate(initialSampleRate);
        delayBlock.setSampleRate(initialSampleRate);
        reverbBlock.setSampleRate(initialSampleRate);
        autotuneBlock.setSampleRate(initialSampleRate);
        deEsserBlock.setSampleRate(initialSampleRate);
        eqBlock.setSampleRate(initialSampleRate);
        doublerBlock.setSampleRate(initialSampleRate);
        harmonizerBlock.setSampleRate(initialSampleRate);
        exciterBlock.setSampleRate(initialSampleRate);
        for (int s = 0; s < kPluginSlotCount; ++s)
            pluginNodes[s].setSampleRate(initialSampleRate);
        tunerDetector.setSampleRate(initialSampleRate);
    }

    // Re-syncs everything that has to know the host's maximum block size
    // (see updateMaxBlockSizes() below - it is the only place NAM's internal
    // buffers get resized, and it is deliberately off the audio thread for
    // exactly that reason). Called both from activate() and from
    // bufferSizeChanged(), because hosts differ in which of the two they use:
    // some announce a new buffer size only while the plugin is deactivated
    // (DPF's contract for bufferSizeChanged()), others just re-activate it.
    // Also covers the case where the host activates *after* setting a buffer
    // size, i.e. without ever calling bufferSizeChanged() at all.
    void activate() override
    {
        updateMaxBlockSizes(getBufferSize() != 0 ? getBufferSize() : 512);
    }

    void bufferSizeChanged(uint32_t newBufferSize) override
    {
        updateMaxBlockSizes(newBufferSize);
    }

    ampforge::HostedPlugin* getHostedPlugin(int slot) override
    {
        if (slot >= 0 && slot < kPluginSlotCount)
            return pluginNodes[slot].getActivePlugin();
        return nullptr;
    }

    void enqueuePluginParam(int slot, uint32_t paramId, float value) override
    {
        if (slot >= 0 && slot < kPluginSlotCount)
            pluginNodes[slot].enqueueParameterUpdate(paramId, value);
    }

protected:
    const char* getLabel() const override        { return "AmpForge"; }
    const char* getDescription() const override   { return "AmpForge: a full reorderable pedalboard-and-amp chain."; }
    const char* getMaker() const override          { return "Atakan"; }
    const char* getLicense() const override        { return "GPL-3.0-or-later"; }
    uint32_t getVersion() const override            { return d_version(0, 4, 0); }
    int64_t getUniqueId() const override            { return d_cconst('A', 'm', 'p', 'M'); }

    void initParameter(uint32_t index, Parameter& parameter) override
    {
        parameter.hints = kParameterIsAutomatable;

        switch (index)
        {
        // --- Noise Gate ---
        case kParamGateOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Gate On"; parameter.symbol = "gate_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamGatePosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Gate Position"; parameter.symbol = "gate_position";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamGateThreshold:
            parameter.name = "Gate Threshold"; parameter.symbol = "gate_threshold"; parameter.unit = "dB";
            parameter.ranges.def = -50.0f; parameter.ranges.min = -80.0f; parameter.ranges.max = 0.0f;
            break;
        case kParamGateAttack:
            parameter.name = "Gate Attack"; parameter.symbol = "gate_attack"; parameter.unit = "ms";
            parameter.ranges.def = 5.0f; parameter.ranges.min = 0.5f; parameter.ranges.max = 50.0f;
            break;
        case kParamGateRelease:
            parameter.name = "Gate Release"; parameter.symbol = "gate_release"; parameter.unit = "ms";
            parameter.ranges.def = 150.0f; parameter.ranges.min = 10.0f; parameter.ranges.max = 1000.0f;
            break;
        case kParamGateRange:
            parameter.name = "Gate Range"; parameter.symbol = "gate_range"; parameter.unit = "dB";
            parameter.ranges.def = 40.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 80.0f;
            break;

        // --- Input (a fixed pre-chain trim, not one of the reorderable
        // pedals - see ChainParameters.hpp's comment on kParamInputGain) ---
        case kParamInputGain:
            parameter.name = "Input Gain"; parameter.symbol = "input_gain"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -24.0f; parameter.ranges.max = 24.0f;
            break;
        case kParamInputLevel:
            parameter.hints |= kParameterIsOutput;
            parameter.name = "Input Level"; parameter.symbol = "input_level";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Compressor ---
        case kParamCompOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Compressor On"; parameter.symbol = "comp_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamCompPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Compressor Position"; parameter.symbol = "comp_position";
            parameter.ranges.def = 1.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamCompThreshold:
            parameter.name = "Compressor Threshold"; parameter.symbol = "comp_threshold"; parameter.unit = "dB";
            parameter.ranges.def = -18.0f; parameter.ranges.min = -60.0f; parameter.ranges.max = 0.0f;
            break;
        case kParamCompRatio:
            parameter.name = "Compressor Ratio"; parameter.symbol = "comp_ratio"; parameter.unit = ":1";
            parameter.ranges.def = 4.0f; parameter.ranges.min = 1.0f; parameter.ranges.max = 20.0f;
            break;
        case kParamCompAttack:
            parameter.name = "Compressor Attack"; parameter.symbol = "comp_attack"; parameter.unit = "ms";
            parameter.ranges.def = 10.0f; parameter.ranges.min = 0.5f; parameter.ranges.max = 100.0f;
            break;
        case kParamCompRelease:
            parameter.name = "Compressor Release"; parameter.symbol = "comp_release"; parameter.unit = "ms";
            parameter.ranges.def = 100.0f; parameter.ranges.min = 10.0f; parameter.ranges.max = 1000.0f;
            break;
        case kParamCompMakeup:
            parameter.name = "Compressor Makeup Gain"; parameter.symbol = "comp_makeup"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 24.0f;
            break;

        // --- Wah ---
        case kParamWahOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Wah On"; parameter.symbol = "wah_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamWahPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Wah Chain Position"; parameter.symbol = "wah_chain_position";
            parameter.ranges.def = 2.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamWahPedal:
            parameter.name = "Wah Pedal"; parameter.symbol = "wah_pedal";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamWahQ:
            parameter.name = "Wah Resonance"; parameter.symbol = "wah_q";
            parameter.ranges.def = 3.0f; parameter.ranges.min = 0.5f; parameter.ranges.max = 10.0f;
            break;

        // --- Screamer ---
        case kParamScreamerOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Screamer On"; parameter.symbol = "screamer_on";
            parameter.ranges.def = 1.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamScreamerPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Screamer Position"; parameter.symbol = "screamer_position";
            parameter.ranges.def = 3.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamScreamerDrive:
            parameter.name = "Screamer Drive"; parameter.symbol = "screamer_drive"; parameter.unit = "x";
            parameter.ranges.def = 1.0f; parameter.ranges.min = 1.0f; parameter.ranges.max = 20.0f;
            break;
        case kParamScreamerTone:
            parameter.name = "Screamer Tone"; parameter.symbol = "screamer_tone";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.05f; parameter.ranges.max = 1.0f;
            break;
        case kParamScreamerLevel:
            parameter.name = "Screamer Level"; parameter.symbol = "screamer_level"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -24.0f; parameter.ranges.max = 12.0f;
            break;

        // --- Distortion (a harder, more aggressive second gain stage
        // alongside Screamer's overdrive - see core/DistortionBlock.hpp) ---
        case kParamDistortionOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Distortion On"; parameter.symbol = "distortion_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamDistortionPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Distortion Position"; parameter.symbol = "distortion_position";
            parameter.ranges.def = 4.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamDistortionDrive:
            parameter.name = "Distortion Drive"; parameter.symbol = "distortion_drive"; parameter.unit = "x";
            parameter.ranges.def = 4.0f; parameter.ranges.min = 1.0f; parameter.ranges.max = 30.0f;
            break;
        case kParamDistortionTone:
            parameter.name = "Distortion Tone"; parameter.symbol = "distortion_tone";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.05f; parameter.ranges.max = 1.0f;
            break;
        case kParamDistortionLevel:
            parameter.name = "Distortion Level"; parameter.symbol = "distortion_level"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -24.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamDistortionBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Distortion Bypass"; parameter.symbol = "distortion_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Amp ---
        case kParamAmpPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Amp Position"; parameter.symbol = "amp_position";
            parameter.ranges.def = 5.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamAmpDrive:
            parameter.name = "Amp Drive"; parameter.symbol = "amp_drive"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 36.0f;
            break;
        case kParamAmpBass:
            parameter.name = "Amp Bass"; parameter.symbol = "amp_bass"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -12.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamAmpMid:
            parameter.name = "Amp Mid"; parameter.symbol = "amp_mid"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -12.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamAmpTreble:
            parameter.name = "Amp Treble"; parameter.symbol = "amp_treble"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -12.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamAmpVolume:
            parameter.name = "Amp Volume"; parameter.symbol = "amp_volume"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -24.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamAmpOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Amp On"; parameter.symbol = "amp_on";
            parameter.ranges.def = 1.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamAmpBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Amp Bypass"; parameter.symbol = "amp_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Autotune (core/AutotuneBlock.hpp) ---
        case kParamAutotuneOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Autotune On"; parameter.symbol = "autotune_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamAutotunePosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Autotune Position"; parameter.symbol = "autotune_position";
            parameter.ranges.def = 8.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamAutotuneKey:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Autotune Key"; parameter.symbol = "autotune_key";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 11.0f;
            break;
        case kParamAutotuneScale:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Autotune Scale"; parameter.symbol = "autotune_scale";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = float(ampforge::kScaleCount - 1);
            break;
        case kParamAutotuneSpeed:
            parameter.name = "Autotune Speed"; parameter.symbol = "autotune_speed";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamAutotuneBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Autotune Bypass"; parameter.symbol = "autotune_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- De-esser (core/DeEsserBlock.hpp) ---
        case kParamDeEsserOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "De-esser On"; parameter.symbol = "deesser_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamDeEsserPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "De-esser Position"; parameter.symbol = "deesser_position";
            parameter.ranges.def = 9.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamDeEsserFrequency:
            parameter.name = "De-esser Frequency"; parameter.symbol = "deesser_frequency"; parameter.unit = "Hz";
            parameter.ranges.def = 6000.0f; parameter.ranges.min = 1000.0f; parameter.ranges.max = 16000.0f;
            break;
        case kParamDeEsserThreshold:
            parameter.name = "De-esser Threshold"; parameter.symbol = "deesser_threshold"; parameter.unit = "dB";
            parameter.ranges.def = -30.0f; parameter.ranges.min = -60.0f; parameter.ranges.max = 0.0f;
            break;
        case kParamDeEsserReduction:
            parameter.name = "De-esser Reduction"; parameter.symbol = "deesser_reduction"; parameter.unit = "dB";
            parameter.ranges.def = 12.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 24.0f;
            break;
        case kParamDeEsserBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "De-esser Bypass"; parameter.symbol = "deesser_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- EQ (a 3-band parametric equalizer pedal, distinct from Amp's
        // own fixed-frequency Bass/Mid/Treble tone stack - see
        // core/EqBlock.hpp) ---
        case kParamEqOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "EQ On"; parameter.symbol = "eq_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamEqPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "EQ Position"; parameter.symbol = "eq_position";
            parameter.ranges.def = 18.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamEqLowFreq:
            parameter.name = "EQ Low Freq"; parameter.symbol = "eq_low_freq"; parameter.unit = "Hz";
            parameter.ranges.def = 100.0f; parameter.ranges.min = 40.0f; parameter.ranges.max = 500.0f;
            break;
        case kParamEqLowGain:
            parameter.name = "EQ Low Gain"; parameter.symbol = "eq_low_gain"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -15.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamEqMidFreq:
            parameter.name = "EQ Mid Freq"; parameter.symbol = "eq_mid_freq"; parameter.unit = "Hz";
            parameter.ranges.def = 800.0f; parameter.ranges.min = 200.0f; parameter.ranges.max = 5000.0f;
            break;
        case kParamEqMidGain:
            parameter.name = "EQ Mid Gain"; parameter.symbol = "eq_mid_gain"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -15.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamEqHighFreq:
            parameter.name = "EQ High Freq"; parameter.symbol = "eq_high_freq"; parameter.unit = "Hz";
            parameter.ranges.def = 3000.0f; parameter.ranges.min = 1000.0f; parameter.ranges.max = 12000.0f;
            break;
        case kParamEqHighGain:
            parameter.name = "EQ High Gain"; parameter.symbol = "eq_high_gain"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -15.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamEqBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "EQ Bypass"; parameter.symbol = "eq_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Amp Presence (see core/AmpBlock.hpp's comment) ---
        case kParamAmpPresence:
            parameter.name = "Amp Presence"; parameter.symbol = "amp_presence"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 12.0f;
            break;

        // --- Doubler (Automatic Double Tracking - see core/DoublerBlock.hpp) ---
        case kParamDoublerOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Doubler On"; parameter.symbol = "doubler_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamDoublerPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Doubler Position"; parameter.symbol = "doubler_position";
            parameter.ranges.def = 10.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamDoublerDelay:
            parameter.name = "Doubler Delay"; parameter.symbol = "doubler_delay"; parameter.unit = "ms";
            parameter.ranges.def = 20.0f; parameter.ranges.min = 5.0f; parameter.ranges.max = 50.0f;
            break;
        case kParamDoublerDepth:
            parameter.name = "Doubler Depth"; parameter.symbol = "doubler_depth"; parameter.unit = "ms";
            parameter.ranges.def = 4.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamDoublerMix:
            parameter.name = "Doubler Mix"; parameter.symbol = "doubler_mix";
            parameter.ranges.def = 0.4f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamDoublerBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Doubler Bypass"; parameter.symbol = "doubler_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Harmonizer (fixed-interval harmony voice - see
        // core/HarmonizerBlock.hpp) ---
        case kParamHarmonizerOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Harmonizer On"; parameter.symbol = "harmonizer_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamHarmonizerPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Harmonizer Position"; parameter.symbol = "harmonizer_position";
            parameter.ranges.def = 11.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamHarmonizerInterval:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Harmonizer Interval"; parameter.symbol = "harmonizer_interval"; parameter.unit = "st";
            parameter.ranges.def = 7.0f; parameter.ranges.min = -12.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamHarmonizerMix:
            parameter.name = "Harmonizer Mix"; parameter.symbol = "harmonizer_mix";
            parameter.ranges.def = 0.4f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamHarmonizerBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Harmonizer Bypass"; parameter.symbol = "harmonizer_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Exciter (vocal air-band enhancer - see core/ExciterBlock.hpp) ---
        case kParamExciterOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Exciter On"; parameter.symbol = "exciter_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamExciterPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Exciter Position"; parameter.symbol = "exciter_position";
            parameter.ranges.def = 12.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamExciterFrequency:
            parameter.name = "Exciter Frequency"; parameter.symbol = "exciter_frequency"; parameter.unit = "Hz";
            parameter.ranges.def = 6000.0f; parameter.ranges.min = 1500.0f; parameter.ranges.max = 14000.0f;
            break;
        case kParamExciterAmount:
            parameter.name = "Exciter Amount"; parameter.symbol = "exciter_amount";
            parameter.ranges.def = 0.3f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamExciterBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Exciter Bypass"; parameter.symbol = "exciter_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Cabinet (convolves with a loaded speaker-cab impulse
        // response - see core/CabinetBlock.hpp. The IR file path itself
        // is DPF State, not a parameter; see initState() below) ---
        case kParamCabinetOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Cabinet On"; parameter.symbol = "cabinet_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamCabinetPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Cabinet Position"; parameter.symbol = "cabinet_position";
            parameter.ranges.def = 6.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 15.0f;
            break;
        case kParamCabinetMix:
            parameter.name = "Cabinet Mix"; parameter.symbol = "cabinet_mix";
            parameter.ranges.def = 1.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamCabinetLevel:
            parameter.name = "Cabinet Level"; parameter.symbol = "cabinet_level"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -24.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamCabinetBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Cabinet Bypass"; parameter.symbol = "cabinet_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Chorus ---
        case kParamChorusOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Chorus On"; parameter.symbol = "chorus_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamChorusPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Chorus Position"; parameter.symbol = "chorus_position";
            parameter.ranges.def = 13.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamChorusRate:
            parameter.name = "Chorus Rate"; parameter.symbol = "chorus_rate"; parameter.unit = "Hz";
            parameter.ranges.def = 1.0f; parameter.ranges.min = 0.05f; parameter.ranges.max = 5.0f;
            break;
        case kParamChorusDepth:
            parameter.name = "Chorus Depth"; parameter.symbol = "chorus_depth"; parameter.unit = "ms";
            parameter.ranges.def = 5.0f; parameter.ranges.min = 0.5f; parameter.ranges.max = 20.0f;
            break;
        case kParamChorusMix:
            parameter.name = "Chorus Mix"; parameter.symbol = "chorus_mix";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Phaser ---
        case kParamPhaserOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Phaser On"; parameter.symbol = "phaser_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamPhaserPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Phaser Position"; parameter.symbol = "phaser_position";
            parameter.ranges.def = 14.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamPhaserRate:
            parameter.name = "Phaser Rate"; parameter.symbol = "phaser_rate"; parameter.unit = "Hz";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.05f; parameter.ranges.max = 5.0f;
            break;
        case kParamPhaserDepth:
            parameter.name = "Phaser Depth"; parameter.symbol = "phaser_depth";
            parameter.ranges.def = 0.7f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamPhaserMix:
            parameter.name = "Phaser Mix"; parameter.symbol = "phaser_mix";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Tremolo ---
        case kParamTremoloOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Tremolo On"; parameter.symbol = "tremolo_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamTremoloPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Tremolo Position"; parameter.symbol = "tremolo_position";
            parameter.ranges.def = 15.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamTremoloRate:
            parameter.name = "Tremolo Rate"; parameter.symbol = "tremolo_rate"; parameter.unit = "Hz";
            parameter.ranges.def = 5.0f; parameter.ranges.min = 0.5f; parameter.ranges.max = 15.0f;
            break;
        case kParamTremoloDepth:
            parameter.name = "Tremolo Depth"; parameter.symbol = "tremolo_depth";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Delay ---
        case kParamDelayOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Delay On"; parameter.symbol = "delay_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamDelayPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Delay Position"; parameter.symbol = "delay_position";
            parameter.ranges.def = 16.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamDelayTime:
            parameter.name = "Delay Time"; parameter.symbol = "delay_time"; parameter.unit = "ms";
            parameter.ranges.def = 300.0f; parameter.ranges.min = 10.0f; parameter.ranges.max = 1500.0f;
            break;
        case kParamDelayFeedback:
            parameter.name = "Delay Feedback"; parameter.symbol = "delay_feedback";
            parameter.ranges.def = 0.3f; parameter.ranges.min = 0.0f; parameter.ranges.max = 0.95f;
            break;
        case kParamDelayMix:
            parameter.name = "Delay Mix"; parameter.symbol = "delay_mix";
            parameter.ranges.def = 0.3f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Reverb ---
        case kParamReverbOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Reverb On"; parameter.symbol = "reverb_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamReverbPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Reverb Position"; parameter.symbol = "reverb_position";
            parameter.ranges.def = 17.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 18.0f;
            break;
        case kParamReverbRoomSize:
            parameter.name = "Reverb Room Size"; parameter.symbol = "reverb_room_size";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamReverbDamping:
            parameter.name = "Reverb Damping"; parameter.symbol = "reverb_damping";
            parameter.ranges.def = 0.5f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamReverbMix:
            parameter.name = "Reverb Mix"; parameter.symbol = "reverb_mix";
            parameter.ranges.def = 0.3f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Bypass switches ---
        case kParamGateBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Gate Bypass"; parameter.symbol = "gate_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamCompBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Compressor Bypass"; parameter.symbol = "comp_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamWahBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Wah Bypass"; parameter.symbol = "wah_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamScreamerBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Screamer Bypass"; parameter.symbol = "screamer_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamChorusBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Chorus Bypass"; parameter.symbol = "chorus_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamPhaserBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Phaser Bypass"; parameter.symbol = "phaser_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamTremoloBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Tremolo Bypass"; parameter.symbol = "tremolo_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamDelayBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Delay Bypass"; parameter.symbol = "delay_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamReverbBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Reverb Bypass"; parameter.symbol = "reverb_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- Tempo sync (see ChainParameters.hpp's kSyncDivisions) ---
        case kParamDelaySync:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Delay Sync"; parameter.symbol = "delay_sync";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = float(kSyncDivisionCount - 1);
            break;
        case kParamTremoloSync:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Tremolo Sync"; parameter.symbol = "tremolo_sync";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = float(kSyncDivisionCount - 1);
            break;
        case kParamChorusSync:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Chorus Sync"; parameter.symbol = "chorus_sync";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = float(kSyncDivisionCount - 1);
            break;

        case kParamCpuLoad:
            parameter.hints |= kParameterIsOutput;
            parameter.name = "CPU Load"; parameter.symbol = "cpu_load";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        case kParamTunerOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "Tuner On"; parameter.symbol = "tuner_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamTunerFrequency:
            parameter.hints |= kParameterIsOutput;
            parameter.name = "Tuner Frequency"; parameter.symbol = "tuner_frequency"; parameter.unit = "Hz";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 2000.0f;
            break;
        case kParamBufferSize:
            parameter.hints |= kParameterIsOutput;
            parameter.name = "Buffer Size"; parameter.symbol = "buffer_size"; parameter.unit = "smp";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 16384.0f;
            break;
        case kParamSampleRate:
            parameter.hints |= kParameterIsOutput;
            parameter.name = "Sample Rate"; parameter.symbol = "sample_rate"; parameter.unit = "Hz";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 384000.0f;
            break;

        case kParamAmpType:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "Amp Type"; parameter.symbol = "amp_type";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = float(ampforge::kAmpVoicingCount - 1);
            break;

        // --- NAM (a neural-network capture of one specific real amp/pedal/
        // cab, loaded from a .nam file - see core/NamBlock.hpp. The model
        // file path itself is DPF State, not a parameter; see initState()
        // below) ---
        case kParamNamOn:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "NAM On"; parameter.symbol = "nam_on";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamNamPosition:
            parameter.hints |= kParameterIsInteger;
            parameter.name = "NAM Position"; parameter.symbol = "nam_position";
            // Slot 7, right after Cabinet (6) and before Chorus (now 8,
            // shifted up from 7 to make room - see Chorus/Phaser/Tremolo/
            // Delay/Reverb's own position cases above, all bumped by one).
            // NAM is a capture of a real amp/pedal/cab, so it belongs
            // tonally alongside Amp/Cabinet, processing a relatively clean
            // guitar signal - not at the very end of the chain, which
            // would run it on already-modulated/delayed/reverberated
            // audio a real amp capture was never trained on. (An earlier
            // version defaulted NAM to the end (12) purely to avoid
            // reusing Chorus's old slot (7) and the position-collision bug
            // that caused - see EffectChain.hpp/kPedalDefs' history - but
            // that only fixed the crash, not where it tonally belongs; the
            // real fix is this renumbering, which gives every block from
            // Gate(0) through Reverb(12) - 13 blocks total including NAM -
            // its own unique slot again.)
            parameter.ranges.def = 7.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 15.0f;
            break;
        // Display names only: these two are the amp's gain and master volume
        // (input trim into the capture, output level out of it), which is what
        // makes the NAM node read like an amp instead of a capture loader.
        // The *symbols* stay nam_input_trim/nam_output_level on purpose, as do
        // their indices and ranges: some hosts key saved automation and
        // session/project data by symbol, so renaming one would silently
        // orphan everything already saved for it - a label is free to change,
        // an identifier is not. (Same reason the five parameters appended
        // after kParamSampleRate in ChainParameters.hpp went to the end of the
        // enum rather than here.)
        case kParamNamInputTrim:
            parameter.name = "NAM Gain"; parameter.symbol = "nam_input_trim"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -24.0f; parameter.ranges.max = 24.0f;
            break;
        case kParamNamOutputLevel:
            parameter.name = "NAM Master"; parameter.symbol = "nam_output_level"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -24.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamNamMix:
            parameter.name = "NAM Mix"; parameter.symbol = "nam_mix";
            parameter.ranges.def = 1.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        case kParamNamBypass:
            parameter.hints |= kParameterIsBoolean;
            parameter.name = "NAM Bypass"; parameter.symbol = "nam_bypass";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;

        // --- NAM tone stack + A2 quality (see the kParamNamBass ..
        // kParamNamQuality comment in ChainParameters.hpp for why these live
        // at the end of the enum, far from the rest of the NAM block above) ---
        case kParamNamBass:
            parameter.name = "NAM Bass"; parameter.symbol = "nam_bass"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -12.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamNamMid:
            parameter.name = "NAM Mid"; parameter.symbol = "nam_mid"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -12.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamNamTreble:
            parameter.name = "NAM Treble"; parameter.symbol = "nam_treble"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = -12.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamNamPresence:
            // Boost-only (0 = off, the neutral state), exactly like
            // kParamAmpPresence above - a real amp's presence control is a
            // high-shelf lift on top of Treble, not a cut, and keeping the
            // same range/default here is what makes NAM's presence knob read
            // the same as the Amp block's.
            parameter.name = "NAM Presence"; parameter.symbol = "nam_presence"; parameter.unit = "dB";
            parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 12.0f;
            break;
        case kParamNamQuality:
            // A2 slimmable model size: 1.0 = full size, which is the only
            // behaviour that existed before this parameter (so it is also the
            // default, keeping older presets/sessions identical); 0.0 = the
            // smallest/cheapest submodel. No unit - it's a CPU/accuracy
            // trade-off, not a physical quantity. A no-op for every non-A2
            // capture (see NamBlock::hasSlimmableModel()).
            parameter.name = "NAM Quality"; parameter.symbol = "nam_quality";
            parameter.ranges.def = 1.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
            break;
        }

        if (index >= kParamPlugin1On && index <= kParamPlugin8Macro8)
        {
            const int s = static_cast<int>(index - kParamPlugin1On) / 14;
            const int sub = static_cast<int>(index - kParamPlugin1On) % 14;
            const std::string prefix = "Plugin " + std::to_string(s + 1) + " ";
            const std::string symPrefix = "plugin_" + std::to_string(s + 1) + "_";

            switch (sub)
            {
            case 0:
                parameter.hints |= kParameterIsBoolean;
                parameter.name = (prefix + "On").c_str();
                parameter.symbol = (symPrefix + "on").c_str();
                parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
                break;
            case 1:
                parameter.hints |= kParameterIsInteger;
                parameter.name = (prefix + "Position").c_str();
                parameter.symbol = (symPrefix + "position").c_str();
                parameter.ranges.def = 19.0f + static_cast<float>(s); parameter.ranges.min = 0.0f; parameter.ranges.max = 25.0f;
                break;
            case 2:
                parameter.hints |= kParameterIsBoolean;
                parameter.name = (prefix + "Bypass").c_str();
                parameter.symbol = (symPrefix + "bypass").c_str();
                parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
                break;
            case 3:
                parameter.name = (prefix + "Mix").c_str();
                parameter.symbol = (symPrefix + "mix").c_str();
                parameter.ranges.def = 1.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
                break;
            case 4:
                parameter.name = (prefix + "In Trim").c_str();
                parameter.symbol = (symPrefix + "in_trim").c_str();
                parameter.unit = "dB";
                parameter.ranges.def = 0.0f; parameter.ranges.min = -24.0f; parameter.ranges.max = 24.0f;
                break;
            case 5:
                parameter.name = (prefix + "Out Trim").c_str();
                parameter.symbol = (symPrefix + "out_trim").c_str();
                parameter.unit = "dB";
                parameter.ranges.def = 0.0f; parameter.ranges.min = -24.0f; parameter.ranges.max = 24.0f;
                break;
            default: {
                const int m = sub - 6;
                parameter.name = (prefix + "Macro " + std::to_string(m + 1)).c_str();
                parameter.symbol = (symPrefix + "macro_" + std::to_string(m + 1)).c_str();
                parameter.ranges.def = 0.0f; parameter.ranges.min = 0.0f; parameter.ranges.max = 1.0f;
                break;
            }
            }
        }
    }

    float getParameterValue(uint32_t index) const override
    {
        switch (index)
        {
        case kParamGateOn:         return gateOn ? 1.0f : 0.0f;
        case kParamGatePosition:   return gatePosition;
        case kParamGateThreshold:  return gateThreshold;
        case kParamGateAttack:     return gateAttack;
        case kParamGateRelease:    return gateRelease;
        case kParamGateRange:      return gateRange;

        case kParamInputGain:      return inputGain;
        case kParamInputLevel:     return inputLevel;

        case kParamCompOn:         return compOn ? 1.0f : 0.0f;
        case kParamCompPosition:   return compPosition;
        case kParamCompThreshold:  return compThreshold;
        case kParamCompRatio:      return compRatio;
        case kParamCompAttack:     return compAttack;
        case kParamCompRelease:    return compRelease;
        case kParamCompMakeup:     return compMakeup;

        case kParamWahOn:          return wahOn ? 1.0f : 0.0f;
        case kParamWahPosition:    return wahPosition;
        case kParamWahPedal:       return wahPedal;
        case kParamWahQ:           return wahQ;

        case kParamScreamerOn:       return screamerOn ? 1.0f : 0.0f;
        case kParamScreamerPosition: return screamerPosition;
        case kParamScreamerDrive:    return screamerDrive;
        case kParamScreamerTone:     return screamerTone;
        case kParamScreamerLevel:    return screamerLevel;

        case kParamDistortionOn:       return distortionOn ? 1.0f : 0.0f;
        case kParamDistortionPosition: return distortionPosition;
        case kParamDistortionDrive:    return distortionDrive;
        case kParamDistortionTone:     return distortionTone;
        case kParamDistortionLevel:    return distortionLevel;
        case kParamDistortionBypass:   return distortionBypass ? 1.0f : 0.0f;

        case kParamAmpPosition:      return ampPosition;
        case kParamAmpDrive:         return ampDrive;
        case kParamAmpBass:          return ampBass;
        case kParamAmpMid:           return ampMid;
        case kParamAmpTreble:        return ampTreble;
        case kParamAmpVolume:        return ampVolume;
        case kParamAmpOn:            return ampOn ? 1.0f : 0.0f;
        case kParamAmpBypass:        return ampBypass ? 1.0f : 0.0f;

        case kParamAutotuneOn:       return autotuneOn ? 1.0f : 0.0f;
        case kParamAutotunePosition: return autotunePosition;
        case kParamAutotuneKey:      return autotuneKey;
        case kParamAutotuneScale:    return autotuneScale;
        case kParamAutotuneSpeed:    return autotuneSpeed;
        case kParamAutotuneBypass:   return autotuneBypass ? 1.0f : 0.0f;

        case kParamDeEsserOn:        return deEsserOn ? 1.0f : 0.0f;
        case kParamDeEsserPosition:  return deEsserPosition;
        case kParamDeEsserFrequency: return deEsserFrequency;
        case kParamDeEsserThreshold: return deEsserThreshold;
        case kParamDeEsserReduction: return deEsserReduction;
        case kParamDeEsserBypass:    return deEsserBypass ? 1.0f : 0.0f;

        case kParamEqOn:          return eqOn ? 1.0f : 0.0f;
        case kParamEqPosition:    return eqPosition;
        case kParamEqLowFreq:     return eqLowFreq;
        case kParamEqLowGain:     return eqLowGain;
        case kParamEqMidFreq:     return eqMidFreq;
        case kParamEqMidGain:     return eqMidGain;
        case kParamEqHighFreq:    return eqHighFreq;
        case kParamEqHighGain:    return eqHighGain;
        case kParamEqBypass:      return eqBypass ? 1.0f : 0.0f;

        case kParamAmpPresence:   return ampPresence;

        case kParamDoublerOn:       return doublerOn ? 1.0f : 0.0f;
        case kParamDoublerPosition: return doublerPosition;
        case kParamDoublerDelay:    return doublerDelay;
        case kParamDoublerDepth:    return doublerDepth;
        case kParamDoublerMix:      return doublerMix;
        case kParamDoublerBypass:   return doublerBypass ? 1.0f : 0.0f;

        case kParamHarmonizerOn:       return harmonizerOn ? 1.0f : 0.0f;
        case kParamHarmonizerPosition: return harmonizerPosition;
        case kParamHarmonizerInterval: return harmonizerInterval;
        case kParamHarmonizerMix:      return harmonizerMix;
        case kParamHarmonizerBypass:   return harmonizerBypass ? 1.0f : 0.0f;

        case kParamExciterOn:       return exciterOn ? 1.0f : 0.0f;
        case kParamExciterPosition: return exciterPosition;
        case kParamExciterFrequency: return exciterFrequency;
        case kParamExciterAmount:   return exciterAmount;
        case kParamExciterBypass:   return exciterBypass ? 1.0f : 0.0f;

        case kParamCabinetOn:       return cabinetOn ? 1.0f : 0.0f;
        case kParamCabinetPosition: return cabinetPosition;
        case kParamCabinetMix:      return cabinetMix;
        case kParamCabinetLevel:    return cabinetLevel;
        case kParamCabinetBypass:   return cabinetBypass ? 1.0f : 0.0f;

        case kParamChorusOn:         return chorusOn ? 1.0f : 0.0f;
        case kParamChorusPosition:   return chorusPosition;
        case kParamChorusRate:       return chorusRate;
        case kParamChorusDepth:      return chorusDepth;
        case kParamChorusMix:        return chorusMix;

        case kParamPhaserOn:         return phaserOn ? 1.0f : 0.0f;
        case kParamPhaserPosition:   return phaserPosition;
        case kParamPhaserRate:       return phaserRate;
        case kParamPhaserDepth:      return phaserDepth;
        case kParamPhaserMix:        return phaserMix;

        case kParamTremoloOn:        return tremoloOn ? 1.0f : 0.0f;
        case kParamTremoloPosition:  return tremoloPosition;
        case kParamTremoloRate:      return tremoloRate;
        case kParamTremoloDepth:     return tremoloDepth;

        case kParamDelayOn:          return delayOn ? 1.0f : 0.0f;
        case kParamDelayPosition:    return delayPosition;
        case kParamDelayTime:        return delayTime;
        case kParamDelayFeedback:    return delayFeedback;
        case kParamDelayMix:         return delayMix;

        case kParamReverbOn:         return reverbOn ? 1.0f : 0.0f;
        case kParamReverbPosition:   return reverbPosition;
        case kParamReverbRoomSize:   return reverbRoomSize;
        case kParamReverbDamping:    return reverbDamping;
        case kParamReverbMix:        return reverbMix;

        case kParamGateBypass:        return gateBypass ? 1.0f : 0.0f;
        case kParamCompBypass:        return compBypass ? 1.0f : 0.0f;
        case kParamWahBypass:         return wahBypass ? 1.0f : 0.0f;
        case kParamScreamerBypass:    return screamerBypass ? 1.0f : 0.0f;
        case kParamChorusBypass:      return chorusBypass ? 1.0f : 0.0f;
        case kParamPhaserBypass:      return phaserBypass ? 1.0f : 0.0f;
        case kParamTremoloBypass:     return tremoloBypass ? 1.0f : 0.0f;
        case kParamDelayBypass:       return delayBypass ? 1.0f : 0.0f;
        case kParamReverbBypass:      return reverbBypass ? 1.0f : 0.0f;

        case kParamDelaySync:         return delaySync;
        case kParamTremoloSync:       return tremoloSync;
        case kParamChorusSync:        return chorusSync;

        case kParamCpuLoad:           return cpuLoad;

        case kParamTunerOn:           return tunerOn ? 1.0f : 0.0f;
        case kParamTunerFrequency:    return tunerFrequency;
        case kParamBufferSize:        return bufferSize;
        case kParamSampleRate:        return sampleRateValue;

        case kParamAmpType:           return ampType;

        case kParamNamOn:           return namOn ? 1.0f : 0.0f;
        case kParamNamPosition:     return namPosition;
        case kParamNamInputTrim:    return namInputTrim;
        case kParamNamOutputLevel:  return namOutputLevel;
        case kParamNamMix:          return namMix;
        case kParamNamBypass:       return namBypass ? 1.0f : 0.0f;

        case kParamNamBass:         return namBass;
        case kParamNamMid:          return namMid;
        case kParamNamTreble:       return namTreble;
        case kParamNamPresence:     return namPresence;
        case kParamNamQuality:      return namQuality;

        default:
            if (index >= kParamPlugin1On && index <= kParamPlugin8Macro8)
            {
                const int s = static_cast<int>(index - kParamPlugin1On) / 14;
                const int sub = static_cast<int>(index - kParamPlugin1On) % 14;
                switch (sub)
                {
                case 0: return pluginSlots[s].on ? 1.0f : 0.0f;
                case 1: return pluginSlots[s].position;
                case 2: return pluginSlots[s].bypass ? 1.0f : 0.0f;
                case 3: return pluginSlots[s].mix;
                case 4: return pluginSlots[s].inTrim;
                case 5: return pluginSlots[s].outTrim;
                default: return pluginSlots[s].macros[sub - 6];
                }
            }
            return 0.0f;
        }
    }

    void setParameterValue(uint32_t index, float value) override
    {
        switch (index)
        {
        case kParamGateOn:
            gateOn = value > 0.5f; chain.setEnabled(&gateBlock, gateOn && !gateBypass); break;
        case kParamGatePosition:
            gatePosition = value; chain.setPosition(&gateBlock, static_cast<int>(std::round(value))); break;
        case kParamGateThreshold:
            gateThreshold = value; gateBlock.setThresholdDB(value); break;
        case kParamGateAttack:
            gateAttack = value; gateBlock.setAttackMs(value); break;
        case kParamGateRelease:
            gateRelease = value; gateBlock.setReleaseMs(value); break;
        case kParamGateRange:
            gateRange = value; gateBlock.setRangeDB(value); break;

        case kParamInputGain:
            inputGain = value; inputGainLinear = std::pow(10.0f, value / 20.0f); break;
        // kParamInputLevel is output-only - the host never calls setParameterValue() for it.

        case kParamCompOn:
            compOn = value > 0.5f; chain.setEnabled(&compBlock, compOn && !compBypass); break;
        case kParamCompPosition:
            compPosition = value; chain.setPosition(&compBlock, static_cast<int>(std::round(value))); break;
        case kParamCompThreshold:
            compThreshold = value; compBlock.setThresholdDB(value); break;
        case kParamCompRatio:
            compRatio = value; compBlock.setRatio(value); break;
        case kParamCompAttack:
            compAttack = value; compBlock.setAttackMs(value); break;
        case kParamCompRelease:
            compRelease = value; compBlock.setReleaseMs(value); break;
        case kParamCompMakeup:
            compMakeup = value; compBlock.setMakeupGainDB(value); break;

        case kParamWahOn:
            wahOn = value > 0.5f; chain.setEnabled(&wahBlock, wahOn && !wahBypass); break;
        case kParamWahPosition:
            wahPosition = value; chain.setPosition(&wahBlock, static_cast<int>(std::round(value))); break;
        case kParamWahPedal:
            wahPedal = value; wahBlock.setPedalPosition(value); break;
        case kParamWahQ:
            wahQ = value; wahBlock.setQ(value); break;

        case kParamScreamerOn:
            screamerOn = value > 0.5f; chain.setEnabled(&screamerBlock, screamerOn && !screamerBypass); break;
        case kParamScreamerPosition:
            screamerPosition = value; chain.setPosition(&screamerBlock, static_cast<int>(std::round(value))); break;
        case kParamScreamerDrive:
            screamerDrive = value; screamerBlock.setDrive(value); break;
        case kParamScreamerTone:
            screamerTone = value; screamerBlock.setTone(value); break;
        case kParamScreamerLevel:
            screamerLevel = value; screamerBlock.setLevel(value); break;

        case kParamDistortionOn:
            distortionOn = value > 0.5f; chain.setEnabled(&distortionBlock, distortionOn && !distortionBypass); break;
        case kParamDistortionPosition:
            distortionPosition = value; chain.setPosition(&distortionBlock, static_cast<int>(std::round(value))); break;
        case kParamDistortionDrive:
            distortionDrive = value; distortionBlock.setDrive(value); break;
        case kParamDistortionTone:
            distortionTone = value; distortionBlock.setTone(value); break;
        case kParamDistortionLevel:
            distortionLevel = value; distortionBlock.setLevel(value); break;
        case kParamDistortionBypass:
            distortionBypass = value > 0.5f; chain.setEnabled(&distortionBlock, distortionOn && !distortionBypass); break;

        case kParamAmpPosition:
            ampPosition = value; chain.setPosition(&ampBlock, static_cast<int>(std::round(value))); break;
        case kParamAmpDrive:
            ampDrive = value; ampBlock.setDriveDB(value); break;
        case kParamAmpBass:
            ampBass = value; ampBlock.setBassDB(value); break;
        case kParamAmpMid:
            ampMid = value; ampBlock.setMidDB(value); break;
        case kParamAmpTreble:
            ampTreble = value; ampBlock.setTrebleDB(value); break;
        case kParamAmpVolume:
            ampVolume = value; ampBlock.setVolumeDB(value); break;
        case kParamAmpOn:
            ampOn = value > 0.5f; chain.setEnabled(&ampBlock, ampOn && !ampBypass); break;
        case kParamAmpBypass:
            ampBypass = value > 0.5f; chain.setEnabled(&ampBlock, ampOn && !ampBypass); break;

        case kParamAutotuneOn:
            autotuneOn = value > 0.5f; chain.setEnabled(&autotuneBlock, autotuneOn && !autotuneBypass); break;
        case kParamAutotunePosition:
            autotunePosition = value; chain.setPosition(&autotuneBlock, static_cast<int>(std::round(value))); break;
        case kParamAutotuneKey:
            autotuneKey = value; autotuneBlock.setKey(static_cast<int>(std::round(value))); break;
        case kParamAutotuneScale:
            autotuneScale = value; autotuneBlock.setScale(static_cast<int>(std::round(value))); break;
        case kParamAutotuneSpeed:
            autotuneSpeed = value; autotuneBlock.setRetuneSpeed(value); break;
        case kParamAutotuneBypass:
            autotuneBypass = value > 0.5f; chain.setEnabled(&autotuneBlock, autotuneOn && !autotuneBypass); break;

        case kParamDeEsserOn:
            deEsserOn = value > 0.5f; chain.setEnabled(&deEsserBlock, deEsserOn && !deEsserBypass); break;
        case kParamDeEsserPosition:
            deEsserPosition = value; chain.setPosition(&deEsserBlock, static_cast<int>(std::round(value))); break;
        case kParamDeEsserFrequency:
            deEsserFrequency = value; deEsserBlock.setFrequency(value); break;
        case kParamDeEsserThreshold:
            deEsserThreshold = value; deEsserBlock.setThresholdDB(value); break;
        case kParamDeEsserReduction:
            deEsserReduction = value; deEsserBlock.setReductionDB(value); break;
        case kParamDeEsserBypass:
            deEsserBypass = value > 0.5f; chain.setEnabled(&deEsserBlock, deEsserOn && !deEsserBypass); break;

        case kParamEqOn:
            eqOn = value > 0.5f; chain.setEnabled(&eqBlock, eqOn && !eqBypass); break;
        case kParamEqPosition:
            eqPosition = value; chain.setPosition(&eqBlock, static_cast<int>(std::round(value))); break;
        case kParamEqLowFreq:
            eqLowFreq = value; eqBlock.setLowFreq(value); break;
        case kParamEqLowGain:
            eqLowGain = value; eqBlock.setLowGainDB(value); break;
        case kParamEqMidFreq:
            eqMidFreq = value; eqBlock.setMidFreq(value); break;
        case kParamEqMidGain:
            eqMidGain = value; eqBlock.setMidGainDB(value); break;
        case kParamEqHighFreq:
            eqHighFreq = value; eqBlock.setHighFreq(value); break;
        case kParamEqHighGain:
            eqHighGain = value; eqBlock.setHighGainDB(value); break;
        case kParamEqBypass:
            eqBypass = value > 0.5f; chain.setEnabled(&eqBlock, eqOn && !eqBypass); break;

        case kParamAmpPresence:
            ampPresence = value; ampBlock.setPresenceDB(value); break;

        case kParamDoublerOn:
            doublerOn = value > 0.5f; chain.setEnabled(&doublerBlock, doublerOn && !doublerBypass); break;
        case kParamDoublerPosition:
            doublerPosition = value; chain.setPosition(&doublerBlock, static_cast<int>(std::round(value))); break;
        case kParamDoublerDelay:
            doublerDelay = value; doublerBlock.setDelayMs(value); break;
        case kParamDoublerDepth:
            doublerDepth = value; doublerBlock.setDepthMs(value); break;
        case kParamDoublerMix:
            doublerMix = value; doublerBlock.setMix(value); break;
        case kParamDoublerBypass:
            doublerBypass = value > 0.5f; chain.setEnabled(&doublerBlock, doublerOn && !doublerBypass); break;

        case kParamHarmonizerOn:
            harmonizerOn = value > 0.5f; chain.setEnabled(&harmonizerBlock, harmonizerOn && !harmonizerBypass); break;
        case kParamHarmonizerPosition:
            harmonizerPosition = value; chain.setPosition(&harmonizerBlock, static_cast<int>(std::round(value))); break;
        case kParamHarmonizerInterval:
            harmonizerInterval = value; harmonizerBlock.setIntervalSemitones(value); break;
        case kParamHarmonizerMix:
            harmonizerMix = value; harmonizerBlock.setMix(value); break;
        case kParamHarmonizerBypass:
            harmonizerBypass = value > 0.5f; chain.setEnabled(&harmonizerBlock, harmonizerOn && !harmonizerBypass); break;

        case kParamExciterOn:
            exciterOn = value > 0.5f; chain.setEnabled(&exciterBlock, exciterOn && !exciterBypass); break;
        case kParamExciterPosition:
            exciterPosition = value; chain.setPosition(&exciterBlock, static_cast<int>(std::round(value))); break;
        case kParamExciterFrequency:
            exciterFrequency = value; exciterBlock.setFrequency(value); break;
        case kParamExciterAmount:
            exciterAmount = value; exciterBlock.setAmount(value); break;
        case kParamExciterBypass:
            exciterBypass = value > 0.5f; chain.setEnabled(&exciterBlock, exciterOn && !exciterBypass); break;

        case kParamCabinetOn:
            cabinetOn = value > 0.5f; chain.setEnabled(&cabinetBlock, cabinetOn && !cabinetBypass); break;
        case kParamCabinetPosition:
            cabinetPosition = value; chain.setPosition(&cabinetBlock, static_cast<int>(std::round(value))); break;
        case kParamCabinetMix:
            cabinetMix = value; cabinetBlock.setMix(value); break;
        case kParamCabinetLevel:
            cabinetLevel = value; cabinetBlock.setLevel(value); break;
        case kParamCabinetBypass:
            cabinetBypass = value > 0.5f; chain.setEnabled(&cabinetBlock, cabinetOn && !cabinetBypass); break;

        case kParamChorusOn:
            chorusOn = value > 0.5f; chain.setEnabled(&chorusBlock, chorusOn && !chorusBypass); break;
        case kParamChorusPosition:
            chorusPosition = value; chain.setPosition(&chorusBlock, static_cast<int>(std::round(value))); break;
        case kParamChorusRate:
            chorusRate = value; chorusBlock.setRateHz(value); break;
        case kParamChorusDepth:
            chorusDepth = value; chorusBlock.setDepthMs(value); break;
        case kParamChorusMix:
            chorusMix = value; chorusBlock.setMix(value); break;

        case kParamPhaserOn:
            phaserOn = value > 0.5f; chain.setEnabled(&phaserBlock, phaserOn && !phaserBypass); break;
        case kParamPhaserPosition:
            phaserPosition = value; chain.setPosition(&phaserBlock, static_cast<int>(std::round(value))); break;
        case kParamPhaserRate:
            phaserRate = value; phaserBlock.setRateHz(value); break;
        case kParamPhaserDepth:
            phaserDepth = value; phaserBlock.setDepth(value); break;
        case kParamPhaserMix:
            phaserMix = value; phaserBlock.setMix(value); break;

        case kParamTremoloOn:
            tremoloOn = value > 0.5f; chain.setEnabled(&tremoloBlock, tremoloOn && !tremoloBypass); break;
        case kParamTremoloPosition:
            tremoloPosition = value; chain.setPosition(&tremoloBlock, static_cast<int>(std::round(value))); break;
        case kParamTremoloRate:
            tremoloRate = value; tremoloBlock.setRateHz(value); break;
        case kParamTremoloDepth:
            tremoloDepth = value; tremoloBlock.setDepth(value); break;

        case kParamDelayOn:
            delayOn = value > 0.5f; chain.setEnabled(&delayBlock, delayOn && !delayBypass); break;
        case kParamDelayPosition:
            delayPosition = value; chain.setPosition(&delayBlock, static_cast<int>(std::round(value))); break;
        case kParamDelayTime:
            delayTime = value; delayBlock.setDelayTimeMs(value); break;
        case kParamDelayFeedback:
            delayFeedback = value; delayBlock.setFeedback(value); break;
        case kParamDelayMix:
            delayMix = value; delayBlock.setMix(value); break;

        case kParamReverbOn:
            reverbOn = value > 0.5f; chain.setEnabled(&reverbBlock, reverbOn && !reverbBypass); break;
        case kParamReverbPosition:
            reverbPosition = value; chain.setPosition(&reverbBlock, static_cast<int>(std::round(value))); break;
        case kParamReverbRoomSize:
            reverbRoomSize = value; reverbBlock.setRoomSize(value); break;
        case kParamReverbDamping:
            reverbDamping = value; reverbBlock.setDamping(value); break;
        case kParamReverbMix:
            reverbMix = value; reverbBlock.setMix(value); break;

        case kParamGateBypass:
            gateBypass = value > 0.5f; chain.setEnabled(&gateBlock, gateOn && !gateBypass); break;
        case kParamCompBypass:
            compBypass = value > 0.5f; chain.setEnabled(&compBlock, compOn && !compBypass); break;
        case kParamWahBypass:
            wahBypass = value > 0.5f; chain.setEnabled(&wahBlock, wahOn && !wahBypass); break;
        case kParamScreamerBypass:
            screamerBypass = value > 0.5f; chain.setEnabled(&screamerBlock, screamerOn && !screamerBypass); break;
        case kParamChorusBypass:
            chorusBypass = value > 0.5f; chain.setEnabled(&chorusBlock, chorusOn && !chorusBypass); break;
        case kParamPhaserBypass:
            phaserBypass = value > 0.5f; chain.setEnabled(&phaserBlock, phaserOn && !phaserBypass); break;
        case kParamTremoloBypass:
            tremoloBypass = value > 0.5f; chain.setEnabled(&tremoloBlock, tremoloOn && !tremoloBypass); break;
        case kParamDelayBypass:
            delayBypass = value > 0.5f; chain.setEnabled(&delayBlock, delayOn && !delayBypass); break;
        case kParamReverbBypass:
            reverbBypass = value > 0.5f; chain.setEnabled(&reverbBlock, reverbOn && !reverbBypass); break;

        // Switching a Sync knob back to "Free" (index 0) restores that
        // block's own Time/Rate value immediately, instead of leaving it
        // stuck on whatever the synced value happened to be until the
        // user next touches the knob - see applyTempoSync()'s comment.
        case kParamDelaySync:
            delaySync = value;
            if (static_cast<int>(std::round(value)) == 0)
                delayBlock.setDelayTimeMs(delayTime);
            break;
        case kParamTremoloSync:
            tremoloSync = value;
            if (static_cast<int>(std::round(value)) == 0)
                tremoloBlock.setRateHz(tremoloRate);
            break;
        case kParamChorusSync:
            chorusSync = value;
            if (static_cast<int>(std::round(value)) == 0)
                chorusBlock.setRateHz(chorusRate);
            break;

        case kParamTunerOn:
            tunerOn = value > 0.5f;
            if (!tunerOn)
                tunerFrequency = 0.0f; // don't leave a stale reading once the tuner's turned off
            break;

        case kParamAmpType:
            ampType = value;
            ampBlock.setAmpType(static_cast<int>(std::round(value)));
            break;

        case kParamNamOn:
            namOn = value > 0.5f; chain.setEnabled(&namBlock, namOn && !namBypass); break;
        case kParamNamPosition:
            namPosition = value; chain.setPosition(&namBlock, static_cast<int>(std::round(value))); break;
        case kParamNamInputTrim:
            namInputTrim = value; namBlock.setInputTrim(value); break;
        case kParamNamOutputLevel:
            namOutputLevel = value; namBlock.setOutputTrim(value); break;
        case kParamNamMix:
            namMix = value; namBlock.setMix(value); break;
        case kParamNamBypass:
            namBypass = value > 0.5f; chain.setEnabled(&namBlock, namOn && !namBypass); break;
        case kParamNamBass:
            namBass = value; namBlock.setBassDB(value); break;
        case kParamNamMid:
            namMid = value; namBlock.setMidDB(value); break;
        case kParamNamTreble:
            namTreble = value; namBlock.setTrebleDB(value); break;
        case kParamNamPresence:
            namPresence = value; namBlock.setPresenceDB(value); break;
        case kParamNamQuality:
            // Only stored as the value the user asked for - NamBlock applies it
            // at the next block boundary on the audio thread, never here (see
            // NamBlock::setQuality()'s comment: this setter runs on the host's
            // thread, and a submodel switch may not happen mid-block).
            namQuality = value; namBlock.setQuality(value); break;
        }

        if (index >= kParamPlugin1On && index <= kParamPlugin8Macro8)
        {
            const int s = static_cast<int>(index - kParamPlugin1On) / 14;
            const int sub = static_cast<int>(index - kParamPlugin1On) % 14;
            switch (sub)
            {
            case 0:
                pluginSlots[s].on = value > 0.5f;
                chain.setEnabled(&pluginNodes[s], pluginSlots[s].on && !pluginSlots[s].bypass);
                break;
            case 1:
                pluginSlots[s].position = value;
                chain.setPosition(&pluginNodes[s], static_cast<int>(std::round(value)));
                break;
            case 2:
                pluginSlots[s].bypass = value > 0.5f;
                pluginNodes[s].setBypass(pluginSlots[s].bypass);
                chain.setEnabled(&pluginNodes[s], pluginSlots[s].on && !pluginSlots[s].bypass);
                break;
            case 3:
                pluginSlots[s].mix = value;
                pluginNodes[s].setMix(value);
                break;
            case 4:
                pluginSlots[s].inTrim = value;
                pluginNodes[s].setInputTrim(value);
                break;
            case 5:
                pluginSlots[s].outTrim = value;
                pluginNodes[s].setOutputTrim(value);
                break;
            default: {
                const int m = sub - 6;
                pluginSlots[s].macros[m] = value;
                const uint32_t mappedParamId = pluginSlots[s].macroMap[m];
                if (mappedParamId != 0xFFFFFFFF)
                {
                    ampforge::HostedPlugin* p = pluginNodes[s].getActivePlugin();
                    if (p)
                    {
                        ampforge::PluginParamInfo info = p->getParameterInfo(mappedParamId);
                        float realVal = info.minValue + value * (info.maxValue - info.minValue);
                        pluginNodes[s].enqueueParameterUpdate(mappedParamId, realVal);
                    }
                }
                break;
            }
            }
        }
    }

    void sampleRateChanged(double newSampleRate) override
    {
        // Published to the UI through kParamSampleRate (see its comment in
        // ChainParameters.hpp) - the UI's own getSampleRate() isn't
        // guaranteed to be told anything by the host.
        sampleRateValue = static_cast<float>(newSampleRate);

        updateInputLevelDecay(newSampleRate);
        gateBlock.setSampleRate(newSampleRate);
        compBlock.setSampleRate(newSampleRate);
        wahBlock.setSampleRate(newSampleRate);
        screamerBlock.setSampleRate(newSampleRate);
        distortionBlock.setSampleRate(newSampleRate);
        cabinetBlock.setSampleRate(newSampleRate);
        namBlock.setSampleRate(newSampleRate);
        ampBlock.setSampleRate(newSampleRate);
        chorusBlock.setSampleRate(newSampleRate);
        phaserBlock.setSampleRate(newSampleRate);
        tremoloBlock.setSampleRate(newSampleRate);
        delayBlock.setSampleRate(newSampleRate);
        reverbBlock.setSampleRate(newSampleRate);
        autotuneBlock.setSampleRate(newSampleRate);
        deEsserBlock.setSampleRate(newSampleRate);
        eqBlock.setSampleRate(newSampleRate);
        doublerBlock.setSampleRate(newSampleRate);
        harmonizerBlock.setSampleRate(newSampleRate);
        exciterBlock.setSampleRate(newSampleRate);
        for (int s = 0; s < kPluginSlotCount; ++s)
            pluginNodes[s].setSampleRate(newSampleRate);
        tunerDetector.setSampleRate(newSampleRate);

        // The Cabinet block resamples its IR to whatever sample rate was
        // active at load time - if the rate changes later (e.g. the user
        // switches audio interfaces mid-session), re-run the load against
        // the new rate so the IR doesn't stay resampled for a now-stale rate.
        if (!cabinetIRPath.empty())
            cabinetBlock.loadImpulseResponse(cabinetIRPath);
    }

    // Reports the preset names to the host, so they show up in its
    // program/preset dropdown.
    void initProgramName(uint32_t index, String& programName) override
    {
        if (index < kProgramCount)
            programName = kPresets[index].name;
    }

    // Called when the user picks a preset from the host's program list.
    // We simply replay every parameter value from the snapshot through
    // setParameterValue(), the same path used for manual knob turns -
    // that keeps our own member variables and the EffectChain in sync
    // automatically.
    void loadProgram(uint32_t index) override
    {
        if (index >= kProgramCount)
            return;

        const PresetDefinition& preset = kPresets[index];
        for (uint32_t i = 0; i < kParamCount; ++i)
            setParameterValue(i, preset.values[i]);
    }

    // The Cabinet block's loaded IR file path is DPF State rather than a
    // Parameter - a file path isn't a plain automatable float, but it
    // still needs to travel with the DAW session the same way every
    // other value here does. kStateIsFilenamePath tells hosts that
    // support it to offer their own native file picker for this state.
    void initState(uint32_t index, State& state) override
    {
        if (index == 0)
        {
            state.key = kCabinetIRStateKey;
            state.label = "Cabinet IR File";
            state.description = "WAV impulse response file loaded into the Cabinet block's convolution engine.";
            state.hints = kStateIsFilenamePath;
            state.defaultValue = "";
        }
        else if (index == 1)
        {
            // See kPresetImportStateKey's comment in ChainParameters.hpp -
            // this state only exists to get requestStateFile()'s native
            // file dialog; the DSP side never reads or persists its value,
            // ChainUI::stateChanged() does all the actual import work.
            state.key = kPresetImportStateKey;
            state.label = "Import Preset File";
            state.description = "A preset file (as written by AmpForge's own Export) to import.";
            state.hints = kStateIsFilenamePath | kStateIsOnlyForUI;
            state.defaultValue = "";
        }
        else if (index == 2)
        {
            state.key = kNamModelStateKey;
            state.label = "NAM Model File";
            state.description = "A .nam capture file (NeuralAmpModelerCore, A1 or A2) loaded into the NAM block.";
            state.hints = kStateIsFilenamePath;
            state.defaultValue = "";
        }
        else if (index >= 3 && index < 3 + kPluginSlotCount * 5)
        {
            const uint32_t pIdx = index - 3;
            const int s = static_cast<int>(pIdx / 5);
            const int kind = static_cast<int>(pIdx % 5);
            if (kind == 0)
            {
                state.key = kPluginPathStateKeys[s];
                state.label = "Plugin Path";
                state.hints = kStateIsFilenamePath;
                state.defaultValue = "";
            }
            else if (kind == 1)
            {
                state.key = kPluginIdStateKeys[s];
                state.label = "Plugin ID";
                state.defaultValue = "";
            }
            else if (kind == 2)
            {
                state.key = kPluginFormatStateKeys[s];
                state.label = "Plugin Format";
                state.defaultValue = "";
            }
            else if (kind == 3)
            {
                state.key = kPluginMapStateKeys[s];
                state.label = "Plugin Macro Map";
                state.defaultValue = "";
            }
            else if (kind == 4)
            {
                state.key = kPluginBlobStateKeys[s];
                state.label = "Plugin State Blob";
                state.defaultValue = "";
            }
        }
        else if (index == 3 + kPluginSlotCount * 5)
        {
            state.key = kPluginSelectFileStateKey;
            state.label = "Select Plugin File";
            state.description = "Select a plugin bundle (.clap, .vst3, .so, .lv2) from filesystem.";
            state.hints = kStateIsFilenamePath | kStateIsOnlyForUI;
            state.defaultValue = "";
        }
    }

    // Called by the host when it wants to persist our state (saving a
    // session/project). The host may call this from any non-realtime context.
    String getState(const char* key) const override
    {
        if (std::strcmp(key, kCabinetIRStateKey) == 0)
            return String(cabinetIRPath.c_str());
        if (std::strcmp(key, kNamModelStateKey) == 0)
            return String(namModelPath.c_str());

        for (int s = 0; s < kPluginSlotCount; ++s)
        {
            if (std::strcmp(key, kPluginPathStateKeys[s]) == 0)
                return String(pluginSlots[s].path.c_str());
            if (std::strcmp(key, kPluginIdStateKeys[s]) == 0)
                return String(pluginSlots[s].id.c_str());
            if (std::strcmp(key, kPluginFormatStateKeys[s]) == 0)
                return String(pluginSlots[s].format.c_str());
            if (std::strcmp(key, kPluginMapStateKeys[s]) == 0)
                return String(pluginSlots[s].mapJson.c_str());
            if (std::strcmp(key, kPluginBlobStateKeys[s]) == 0)
            {
                ampforge::HostedPlugin* p = const_cast<ampforge::PluginNodeBlock&>(pluginNodes[s]).getActivePlugin();
                if (p)
                {
                    std::string raw = p->saveState();
                    if (!raw.empty())
                    {
                        String b64 = String::asBase64(raw.data(), raw.size());
                        const_cast<PluginSlotState&>(pluginSlots[s]).stateBlob = b64.buffer();
                    }
                }
                return String(pluginSlots[s].stateBlob.c_str());
            }
        }

        return String();
    }

    // Called by the host to restore previously-saved state (or when the UI
    // asks us to load a new file via UI::requestStateFile()). Loading the
    // WAV/NAM file itself is safe to do here even though this isn't the
    // audio thread - see CabinetBlock::loadImpulseResponse()'s own comment
    // (NamBlock::loadModel() follows the identical double-buffer pattern).
    void setState(const char* key, const char* value) override
    {
        if (suppressingStatePush)
            return;

        if (std::strcmp(key, kCabinetIRStateKey) == 0)
        {
            cabinetIRPath = value;
            if (!cabinetIRPath.empty())
            {
                cabinetBlock.loadImpulseResponse(cabinetIRPath);
                suppressingStatePush = true;
                updateStateValue(key, value);
                suppressingStatePush = false;
            }
            return;
        }
        else if (std::strcmp(key, kNamModelStateKey) == 0)
        {
            if (value[0] != '\0' && namBlock.loadModel(value))
            {
                namModelPath = value;
                suppressingStatePush = true;
                updateStateValue(key, value);
                suppressingStatePush = false;
            }
            return;
        }

        for (int s = 0; s < kPluginSlotCount; ++s)
        {
            if (std::strcmp(key, kPluginPathStateKeys[s]) == 0)
            {
                pluginSlots[s].path = value ? value : "";
                loadHostedPlugin(s);
                return;
            }
            if (std::strcmp(key, kPluginIdStateKeys[s]) == 0)
            {
                pluginSlots[s].id = value ? value : "";
                loadHostedPlugin(s);
                return;
            }
            if (std::strcmp(key, kPluginFormatStateKeys[s]) == 0)
            {
                pluginSlots[s].format = value ? value : "";
                loadHostedPlugin(s);
                return;
            }
            if (std::strcmp(key, kPluginMapStateKeys[s]) == 0)
            {
                pluginSlots[s].mapJson = value ? value : "";
                parsePluginMap(s, pluginSlots[s].mapJson);
                return;
            }
            if (std::strcmp(key, kPluginBlobStateKeys[s]) == 0)
            {
                pluginSlots[s].stateBlob = value ? value : "";
                if (!pluginSlots[s].stateBlob.empty())
                {
                    ampforge::HostedPlugin* p = pluginNodes[s].getActivePlugin();
                    if (p)
                    {
                        std::vector<uint8_t> chunk;
                        d_getChunkFromBase64String_impl(chunk, pluginSlots[s].stateBlob.c_str());
                        if (!chunk.empty())
                        {
                            std::string raw(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                            p->loadState(raw);
                        }
                    }
                }
                return;
            }
        }
    }

    void loadHostedPlugin(int s)
    {
        if (pluginSlots[s].path.empty() && pluginSlots[s].id.empty())
            return;

        ampforge::PluginFormat fmt = ampforge::PluginFormat::Unknown;
        if (pluginSlots[s].format == "LV2") fmt = ampforge::PluginFormat::LV2;
        else if (pluginSlots[s].format == "VST3") fmt = ampforge::PluginFormat::VST3;
        else if (pluginSlots[s].format == "CLAP") fmt = ampforge::PluginFormat::CLAP;

        if (fmt == ampforge::PluginFormat::Unknown && !pluginSlots[s].path.empty())
        {
            const std::string& p = pluginSlots[s].path;
            if (p.rfind(".clap") != std::string::npos) fmt = ampforge::PluginFormat::CLAP;
            else if (p.rfind(".vst3") != std::string::npos) fmt = ampforge::PluginFormat::VST3;
            else if (p.rfind(".lv2") != std::string::npos || p.rfind(".lv2/") != std::string::npos) fmt = ampforge::PluginFormat::LV2;
            else
            {
                auto scanned = ampforge::PluginScanner().scanPath(p);
                if (!scanned.empty())
                {
                    fmt = scanned[0].format;
                    if (pluginSlots[s].id.empty()) pluginSlots[s].id = scanned[0].id;
                }
            }
            if (fmt == ampforge::PluginFormat::LV2) pluginSlots[s].format = "LV2";
            else if (fmt == ampforge::PluginFormat::VST3) pluginSlots[s].format = "VST3";
            else if (fmt == ampforge::PluginFormat::CLAP) pluginSlots[s].format = "CLAP";
        }

        if (fmt == ampforge::PluginFormat::Unknown)
            return;

        if (fmt == ampforge::PluginFormat::LV2 && !pluginSlots[s].path.empty())
        {
            std::filesystem::path pp(pluginSlots[s].path);
            std::error_code ec;
            if (pp.extension() == ".ttl" || pp.extension() == ".so")
            {
                if (pp.parent_path().extension() == ".lv2" || std::filesystem::is_regular_file(pp.parent_path() / "manifest.ttl", ec))
                {
                    pluginSlots[s].path = pp.parent_path().string();
                }
            }
        }

        auto p = ampforge::PluginScanner::instantiate(fmt, pluginSlots[s].path, pluginSlots[s].id);
        if (p)
        {
            if (!pluginSlots[s].stateBlob.empty())
            {
                std::vector<uint8_t> chunk;
                d_getChunkFromBase64String_impl(chunk, pluginSlots[s].stateBlob.c_str());
                if (!chunk.empty())
                {
                    std::string raw(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                    p->loadState(raw);
                }
            }
            pluginNodes[s].setName(p->getName());
            pluginNodes[s].setPlugin(std::move(p));

            bool allUnmapped = true;
            for (int m = 0; m < kPluginMacroCount; ++m)
            {
                if (pluginSlots[s].macroMap[m] != 0xFFFFFFFF)
                {
                    allUnmapped = false;
                    break;
                }
            }
            if (allUnmapped && !pluginSlots[s].mapJson.empty())
            {
                parsePluginMap(s, pluginSlots[s].mapJson);
                allUnmapped = false;
            }
            if (allUnmapped)
            {
                ampforge::HostedPlugin* active = pluginNodes[s].getActivePlugin();
                if (active)
                {
                    const uint32_t count = active->getParameterCount();
                    int m = 0;
                    if (active->isModPlugin())
                    {
                        const auto modPorts = active->getModguiPortSymbols();
                        for (const auto& sym : modPorts)
                        {
                            if (m >= kPluginMacroCount) break;
                            for (uint32_t i = 0; i < count; ++i)
                            {
                                auto info = active->getParameterInfo(i);
                                if (info.symbol == sym)
                                {
                                    std::string lower = info.name;
                                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                                    if (lower.find("bypass") == std::string::npos && lower.find("enable") == std::string::npos)
                                    {
                                        pluginSlots[s].macroMap[m++] = info.id;
                                    }
                                    break;
                                }
                            }
                        }
                    }

                    for (uint32_t i = 0; i < count && m < kPluginMacroCount; ++i)
                    {
                        auto info = active->getParameterInfo(i);
                        bool already = false;
                        for (int k = 0; k < m; ++k)
                        {
                            if (pluginSlots[s].macroMap[k] == info.id) { already = true; break; }
                        }
                        if (already) continue;

                        std::string lower = info.name;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if (lower.find("bypass") != std::string::npos || lower.find("enable") != std::string::npos)
                            continue;
                        pluginSlots[s].macroMap[m++] = info.id;
                    }
                }
            }

            applyMacroMappingsToPlugin(s);
        }
    }

    void parsePluginMap(int s, const std::string& mapStr)
    {
        for (int m = 0; m < kPluginMacroCount; ++m)
            pluginSlots[s].macroMap[m] = 0xFFFFFFFF;

        if (mapStr.empty()) return;
        std::istringstream ss(mapStr);
        std::string pair;
        while (std::getline(ss, pair, ','))
        {
            const auto colon = pair.find(':');
            if (colon == std::string::npos) continue;
            try {
                int m = std::stoi(pair.substr(0, colon));
                uint32_t pid = static_cast<uint32_t>(std::stoul(pair.substr(colon + 1)));
                if (m >= 0 && m < kPluginMacroCount)
                    pluginSlots[s].macroMap[m] = pid;
            } catch (...) {}
        }
        applyMacroMappingsToPlugin(s);
    }

    void applyMacroMappingsToPlugin(int s)
    {
        ampforge::HostedPlugin* p = pluginNodes[s].getActivePlugin();
        if (!p) return;
        for (int m = 0; m < kPluginMacroCount; ++m)
        {
            uint32_t pid = pluginSlots[s].macroMap[m];
            if (pid != 0xFFFFFFFF)
            {
                ampforge::PluginParamInfo info = p->getParameterInfo(pid);
                float realVal = info.minValue + pluginSlots[s].macros[m] * (info.maxValue - info.minValue);
                pluginNodes[s].enqueueParameterUpdate(pid, realVal);
            }
        }
    }

    void run(const float** inputs, float** outputs, uint32_t frames) override
    {
        const auto processStart = std::chrono::steady_clock::now();

        bufferSize = static_cast<float>(frames);

        if (delaySync > 0.5f || tremoloSync > 0.5f || chorusSync > 0.5f)
            applyTempoSync();

        const float* in  = inputs[0];
        float*       out = outputs[0];

        // Whether this call may hand the chain a whole block. That needs
        // somewhere to put the Input-Gain-trimmed input - unless Input Gain is
        // at 0dB (a linear gain of exactly 1.0), in which case the chain reads
        // the host's own buffer and needs no scratch at all. Correctness must
        // never depend on the host announcing its buffer size: one that
        // changed it without telling us, or that never called
        // activate()/bufferSizeChanged() at all, can hand us more frames than
        // preChainBuffer holds (kMaxPreChainFrames, fixed at construction), and
        // the per-sample path below is what keeps that case correct instead of
        // writing past the end of the vector. Same audible result either way,
        // just without the speedup. (frames == 0 is harmless in both paths,
        // and EffectChain/NamBlock each degrade on their own if frames is
        // larger than the size *they* were given, so the only buffer this
        // check has to protect is ours.)
        const bool useBlockPath = inputGainLinear == 1.0f || preChainBuffer.size() >= frames;

        // Input Gain is a single scalar, so folding it into the buffer once up
        // front costs one multiply per sample - the same multiply the
        // per-sample loop used to do - and leaves the chain a plain untrimmed
        // block to process.
        const float* chainInput = in;
        if (inputGainLinear != 1.0f && useBlockPath)
        {
            for (uint32_t i = 0; i < frames; ++i)
                preChainBuffer[i] = in[i] * inputGainLinear;
            chainInput = preChainBuffer.data();
        }

        // The input meter and the tuner tap stay per-sample on purpose. They
        // are two cheap operations with no per-call setup, so block
        // processing would buy nothing - and the Input Level meter's exact
        // per-sample fast-attack/slow-decay behaviour (see kInputMeterDecayMs)
        // is something users watch while dialing Input Gain, so keeping the
        // same loop over the same values keeps that reading bit-identical.
        // The chain, by contrast, is where the time goes: NAM's neural
        // network is dramatically cheaper per block than per sample (~1.6 vs
        // ~9.3 us/sample measured on a real A2 capture), and it can only use
        // that if it gets a whole buffer - so it does, right below.
        for (uint32_t i = 0; i < frames; ++i)
        {
            // On the block path the trim already happened above; on the
            // fallback path it stays here, one sample at a time, exactly as
            // this loop always did it - so the meter and the tuner see the
            // same trimmed signal either way.
            const float trimmed = useBlockPath ? chainInput[i] : in[i] * inputGainLinear;

            // Peak with slow decay (see kInputMeterDecayMs) rather than an
            // instant follow, so the Input Level meter reads like a real
            // peak meter instead of flickering with every sample. Clamped
            // to 1.0 - a hair over 0dBFS is still "clipping" either way, and
            // keeping the reported value within the parameter's declared
            // 0..1 range avoids surprising hosts that assume output
            // parameters stay within their min/max.
            const float rectified = std::min(std::fabs(trimmed), 1.0f);
            inputLevel = (rectified > inputLevel) ? rectified : inputLevel * inputLevelDecay;

            // Tuner tap: a copy of the trimmed-but-unprocessed signal, so
            // the reading reflects the guitar's actual pitch regardless of
            // what Distortion/Wah/etc. are doing further down the chain -
            // see kParamTunerOn's comment in ChainParameters.hpp. Only fed
            // while the tuner is actually open, so it costs nothing the
            // rest of the time.
            if (tunerOn)
                tunerDetector.pushSample(trimmed);

            // The fallback path's chain work: the old fully per-sample loop,
            // kept here (rather than in a second copy of the tap code above)
            // so the two paths can't drift apart.
            if (!useBlockPath)
                out[i] = chain.processSample(trimmed);
        }

        // The block path: one call for the whole buffer, which is what lets
        // NamBlock hand NAM whole blocks instead of single frames. Same
        // blocks, same order, same result as processSample() - see
        // EffectChain::processBlock().
        if (useBlockPath)
            chain.processBlock(chainInput, out, frames);

        if (tunerOn)
            tunerFrequency = tunerDetector.getFrequencyHz();

        // CPU Load: how much of this block's real-time budget (frames /
        // sample rate) actually processing it took. Peak-with-decay like
        // Input Level above, but the decay factor is derived here rather
        // than precomputed, since it depends on this call's block size,
        // which hosts are free to vary from call to call (unlike the
        // sample rate, which sampleRateChanged() tells us about up front).
        // Some hosts call run() with frames == 0 (e.g. a parameter-only or
        // latency-compensation flush block) - nothing was actually
        // processed, so leave the meter as-is instead of dividing by a
        // zero blockSec (which would otherwise read back as a spurious
        // 100% load).
        if (frames > 0)
        {
            const float elapsedSec = std::chrono::duration<float>(std::chrono::steady_clock::now() - processStart).count();
            const float blockSec = static_cast<float>(frames) / static_cast<float>(getSampleRate());
            const float rawLoad = std::min(elapsedSec / blockSec, 1.0f);
            const float cpuLoadDecay = std::exp(-blockSec / kCpuMeterDecayTauSec);
            cpuLoad = (rawLoad > cpuLoad) ? rawLoad : cpuLoad * cpuLoadDecay;
        }
    }

private:
    void updateInputLevelDecay(double sampleRate)
    {
        inputLevelDecay = std::exp(-1.0f / (0.001f * kInputMeterDecayMs * static_cast<float>(sampleRate)));
    }

    // Sizes every buffer that depends on how many frames run() may be handed
    // in one call: EffectChain's block scratch, NAM's own per-block buffers,
    // and our trimmed-input buffer (see run()).
    //
    // This is the ONLY place NAM's internal buffers get resized, and it is
    // called exclusively from non-realtime contexts - the constructor,
    // activate() and bufferSizeChanged() - which is exactly why
    // NamBlock::setMaxBlockSize() is allowed to allocate: it grows two
    // std::vectors and Re-Reset()s the loaded model, and Reset() in turn sizes
    // (i.e. allocates) every Eigen matrix and ring buffer NAM owns. None of
    // that may ever happen inside run().
    //
    // All three sizes only ever grow (see EffectChain::setMaxBlockFrames() and
    // NamBlock::setMaxBlockSize()), so a host that repeatedly announces a
    // smaller buffer size costs nothing after the first large one, and nothing
    // can shrink out from under a block that is still using it. A host that
    // never calls either hook is still fine: EffectChain and NamBlock each
    // allocate a correct 512-frame default themselves, and run() checks
    // preChainBuffer's size before using it, so a bigger buffer is handled
    // correctly (just without the block path) rather than overrunning
    // anything.
    void updateMaxBlockSizes(uint32_t frames)
    {
        // 0 means "the host hasn't told us anything yet" (DPF reports that for
        // dummy/meta-data instances, and getBufferSize() can be 0 before the
        // host announces one) - not a real buffer size to size for.
        if (frames == 0)
            return;

        // Nothing here allocates: preChainBuffer was sized in the constructor
        // and the chain's scratch likewise, so all three calls do is raise a
        // cap. That is what makes this safe to reach from a host callback that
        // runs while audio is rolling (and why preChainBuffer must never be
        // resized here - see its own comment).
        chain.setMaxBlockFrames(frames);
        namBlock.setMaxBlockSize(static_cast<int>(frames));
    }

    // Recomputes Delay/Tremolo/Chorus's actual time/rate from the host's
    // current BPM for every block whose Sync knob isn't "Free" (index 0),
    // overriding that block's own Time/Rate parameter for this run() call.
    // Only called when at least one of the three is synced (see run()),
    // since getTimePosition() and the division lookup are pointless
    // overhead otherwise. Falls back to 120 BPM when the host doesn't
    // report a valid tempo (e.g. transport stopped, or a plugin format
    // that doesn't support TimePosition at all) so sync still produces a
    // sensible, stable rate instead of a divide-by-zero.
    void applyTempoSync()
    {
        const TimePosition& time = getTimePosition();
        const double bpm = (time.bbt.valid && time.bbt.beatsPerMinute > 1.0) ? time.bbt.beatsPerMinute : 120.0;
        const float beatMs = 60000.0f / static_cast<float>(bpm); // duration of one quarter note

        const int delayDiv = std::clamp(static_cast<int>(std::round(delaySync)), 0, kSyncDivisionCount - 1);
        if (delayDiv > 0)
        {
            const float ms = std::clamp(beatMs * kSyncDivisions[delayDiv].beatMultiplier, 10.0f, 1500.0f);
            delayBlock.setDelayTimeMs(ms);
        }

        const int tremoloDiv = std::clamp(static_cast<int>(std::round(tremoloSync)), 0, kSyncDivisionCount - 1);
        if (tremoloDiv > 0)
        {
            const float ms = beatMs * kSyncDivisions[tremoloDiv].beatMultiplier;
            tremoloBlock.setRateHz(std::clamp(1000.0f / ms, 0.5f, 15.0f));
        }

        const int chorusDiv = std::clamp(static_cast<int>(std::round(chorusSync)), 0, kSyncDivisionCount - 1);
        if (chorusDiv > 0)
        {
            const float ms = beatMs * kSyncDivisions[chorusDiv].beatMultiplier;
            chorusBlock.setRateHz(std::clamp(1000.0f / ms, 0.05f, 5.0f));
        }
    }


    ampforge::NoiseGateBlock gateBlock;
    ampforge::CompressorBlock compBlock;
    ampforge::WahBlock wahBlock;
    ampforge::ScreamerBlock screamerBlock;
    ampforge::DistortionBlock distortionBlock;
    ampforge::CabinetBlock cabinetBlock;
    ampforge::NamBlock namBlock;
    ampforge::AmpBlock ampBlock;
    ampforge::ChorusBlock chorusBlock;
    ampforge::PhaserBlock phaserBlock;
    ampforge::TremoloBlock tremoloBlock;
    ampforge::DelayBlock delayBlock;
    ampforge::ReverbBlock reverbBlock;
    ampforge::AutotuneBlock autotuneBlock;
    ampforge::DeEsserBlock deEsserBlock;
    ampforge::EqBlock eqBlock;
    ampforge::DoublerBlock doublerBlock;
    ampforge::HarmonizerBlock harmonizerBlock;
    ampforge::ExciterBlock exciterBlock;
    ampforge::EffectChain chain;

    // Holds the Input-Gain-trimmed input for one run() call when run() takes
    // the block path (see its comment). Allocated once in the constructor at
    // kMaxPreChainFrames and *never* resized: run() reads .size()/.data() on the
    // audio thread, and a vector that grew underneath it could free the memory
    // it was writing into (a real hazard when the host announces a new block
    // size while rolling). A buffer bigger than that many frames just takes the
    // per-sample path, which is what the size check in run() already covers.
    static constexpr uint32_t kMaxPreChainFrames = 8192;
    std::vector<float> preChainBuffer;

    // Input - applied directly in run(), ahead of the chain, rather than
    // through an AudioBlock/EffectChain slot, since it's a fixed pre-stage
    // rather than a reorderable pedal (see kParamInputGain's comment).
    float inputGain = 0.0f, inputGainLinear = 1.0f;
    float inputLevel = 0.0f;
    float inputLevelDecay = 0.0f;

    // CPU Load meter - see kParamCpuLoad's comment in ChainParameters.hpp.
    // Same fast-attack/slow-decay shape as the Input Level meter above, so
    // one expensive block still reads clearly instead of instantly falling
    // back to near-zero on the very next (cheaper) block. Unlike
    // inputLevelDecay, its decay factor can't be precomputed once from the
    // sample rate - it depends on the block size too, which hosts are free
    // to change from call to call - so it's recomputed in run() instead
    // (see kCpuMeterDecayTauSec below).
    float cpuLoad = 0.0f;

    // Buffer Size meter - see kParamBufferSize's comment in ChainParameters.hpp.
    float bufferSize = 0.0f;

    // Sample Rate meter - see kParamSampleRate's comment in ChainParameters.hpp.
    float sampleRateValue = 0.0f;

    // Tuner - see kParamTunerOn's comment in ChainParameters.hpp.
    bool tunerOn = false;
    float tunerFrequency = 0.0f;
    ampforge::PitchDetector tunerDetector;

    bool gateOn = false;
    float gatePosition = 0.0f, gateThreshold = -50.0f, gateAttack = 5.0f, gateRelease = 150.0f, gateRange = 40.0f;

    bool compOn = false;
    float compPosition = 1.0f, compThreshold = -18.0f, compRatio = 4.0f, compAttack = 10.0f, compRelease = 100.0f, compMakeup = 0.0f;

    bool wahOn = false;
    float wahPosition = 2.0f, wahPedal = 0.5f, wahQ = 3.0f;

    bool screamerOn = true;
    float screamerPosition = 3.0f, screamerDrive = 1.0f, screamerTone = 0.5f, screamerLevel = 0.0f;

    bool distortionOn = false;
    float distortionPosition = 4.0f, distortionDrive = 4.0f, distortionTone = 0.5f, distortionLevel = 0.0f;

    bool cabinetOn = false;
    float cabinetPosition = 6.0f, cabinetMix = 1.0f, cabinetLevel = 0.0f;
    // The loaded IR's path, carried as DPF State rather than a Parameter -
    // see initState()/getState()/setState() below.
    std::string cabinetIRPath;

    // ampOn defaults true (matching AmpBlock's previous unconditional
    // always-on behavior exactly) - see kParamAmpOn/Bypass's comment in
    // ChainParameters.hpp for why a real On/Bypass pair was added here at
    // all despite that "always-on" history.
    bool ampOn = true;
    float ampPosition = 5.0f, ampDrive = 0.0f, ampBass = 0.0f, ampMid = 0.0f, ampTreble = 0.0f, ampVolume = 0.0f;
    float ampPresence = 0.0f;
    float ampType = 0.0f; // index into ampforge::kAmpVoicings; 0 = Modern

    bool autotuneOn = false;
    float autotunePosition = 13.0f, autotuneKey = 0.0f, autotuneScale = 0.0f, autotuneSpeed = 0.5f;
    bool autotuneBypass = false;

    bool deEsserOn = false;
    float deEsserPosition = 14.0f, deEsserFrequency = 6000.0f, deEsserThreshold = -30.0f, deEsserReduction = 12.0f;
    bool deEsserBypass = false;

    bool eqOn = false;
    float eqPosition = 15.0f;
    float eqLowFreq = 100.0f, eqLowGain = 0.0f;
    float eqMidFreq = 800.0f, eqMidGain = 0.0f;
    float eqHighFreq = 3000.0f, eqHighGain = 0.0f;
    bool eqBypass = false;

    bool doublerOn = false;
    float doublerPosition = 16.0f, doublerDelay = 20.0f, doublerDepth = 4.0f, doublerMix = 0.4f;
    bool doublerBypass = false;

    bool harmonizerOn = false;
    float harmonizerPosition = 17.0f, harmonizerInterval = 7.0f, harmonizerMix = 0.4f;
    bool harmonizerBypass = false;

    bool exciterOn = false;
    float exciterPosition = 18.0f, exciterFrequency = 6000.0f, exciterAmount = 0.3f;
    bool exciterBypass = false;

    bool namOn = false;
    // Position 7 - right after Cabinet, before Chorus - see
    // kParamNamPosition's case in initParameter() above for why. (These
    // C++-side defaults used to be one less than their own parameter's
    // declared range.def for every position below - chorusPosition = 6.0f
    // against a declared default of 7.0f, etc. - which the host masked by
    // always pushing its own declared default at startup; fixed to match
    // exactly while touching these lines anyway, so this doesn't depend on
    // that host behavior.)
    float namPosition = 7.0f, namInputTrim = 0.0f, namOutputLevel = 0.0f, namMix = 1.0f;
    // NAM's tone stack (same units as the Amp block's above) and the A2
    // slimmable size - see the kParamNamBass..kParamNamQuality comment in
    // ChainParameters.hpp. Neutral/1.0 defaults: 0dB on all four bands and
    // full model size is exactly what this block did before those controls
    // existed.
    float namBass = 0.0f, namMid = 0.0f, namTreble = 0.0f, namPresence = 0.0f, namQuality = 1.0f;
    // The loaded .nam model's path, carried as DPF State rather than a
    // Parameter - see initState()/getState()/setState() above.
    std::string namModelPath;
    // Guards setState()'s updateStateValue() calls against DPF's own
    // re-entrancy: Plugin::updateStateValue() is documented to call
    // setState() again itself before notifying the UI (see its doc comment
    // in DistrhoPlugin.hpp), so calling it from inside setState() would
    // otherwise recurse forever. See setState()'s comment for why we call
    // it there at all.
    bool suppressingStatePush = false;

    bool chorusOn = false;
    float chorusPosition = 8.0f, chorusRate = 1.0f, chorusDepth = 5.0f, chorusMix = 0.5f;
    float chorusSync = 0.0f; // index into kSyncDivisions; 0 = Free (off)

    bool phaserOn = false;
    float phaserPosition = 9.0f, phaserRate = 0.5f, phaserDepth = 0.7f, phaserMix = 0.5f;

    bool tremoloOn = false;
    float tremoloPosition = 10.0f, tremoloRate = 5.0f, tremoloDepth = 0.5f;
    float tremoloSync = 0.0f; // index into kSyncDivisions; 0 = Free (off)

    bool delayOn = false;
    float delayPosition = 11.0f, delayTime = 300.0f, delayFeedback = 0.3f, delayMix = 0.3f;
    float delaySync = 0.0f; // index into kSyncDivisions; 0 = Free (off)

    bool reverbOn = false;
    float reverbPosition = 12.0f, reverbRoomSize = 0.5f, reverbDamping = 0.5f, reverbMix = 0.3f;

    // Bypass flags - independent from the *On* flags above. See the
    // comment on the Bypass parameters in ChainParameters.hpp.
    bool gateBypass = false;
    bool compBypass = false;
    bool wahBypass = false;
    bool screamerBypass = false;
    bool chorusBypass = false;
    bool phaserBypass = false;
    bool tremoloBypass = false;
    bool delayBypass = false;
    bool reverbBypass = false;
    bool distortionBypass = false;
    bool cabinetBypass = false;
    bool namBypass = false;
    bool ampBypass = false;

    struct PluginSlotState {
        bool on = false;
        float position = 19.0f;
        bool bypass = false;
        float mix = 1.0f;
        float inTrim = 0.0f;
        float outTrim = 0.0f;
        float macros[kPluginMacroCount] = {0.0f};
        uint32_t macroMap[kPluginMacroCount] = {
            0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
            0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
        };
        std::string path;
        std::string id;
        std::string format;
        std::string mapJson;
        std::string stateBlob;
    };
    PluginSlotState pluginSlots[kPluginSlotCount];
    ampforge::PluginNodeBlock pluginNodes[kPluginSlotCount];

    DISTRHO_DECLARE_NON_COPYABLE(ChainPlugin)
};

Plugin* createPlugin()
{
    return new ChainPlugin();
}

END_NAMESPACE_DISTRHO
