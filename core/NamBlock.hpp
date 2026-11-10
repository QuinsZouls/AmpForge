#pragma once
#include "AudioBlock.hpp"
#include "Denormal.hpp"
#include "ToneStack.hpp"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/*
 * NamBlock - runs the signal through a neural network loaded from a .nam
 * file (NeuralAmpModelerCore, https://github.com/sdatkinson/NeuralAmpModelerCore),
 * a capture of one specific real amp/pedal/cab rather than a hand-tuned
 * emulation. This is a separate, optional block from AmpBlock - the two are
 * independent tone sources the user can mix and match, not alternatives to
 * pick between.
 *
 * Block processing: nam::DSP::process() is designed to run on a block of
 * frames at once, and that is where its internal batching efficiency comes
 * from. Measured on a real A2 capture at 48 kHz on one core: num_frames = 1
 * costs ~9-13 us/sample, while the same model in 64-frame blocks costs
 * ~1.4-1.8 us/sample (1.6 us/sample measured right beside a 12.3 us/sample
 * per-sample run of this very block). Driving it one frame at a time
 * therefore throws away most of NAM's throughput. The cost of a single-frame
 * call is memory-bound rather than a fixed function-call overhead: it
 * re-streams the model's weights and re-touches every layer's buffers to
 * produce one sample, so it grows with how large those buffers are - the same
 * call measured 2.8 us/sample with NAM sized for one frame, 12 us/sample
 * sized for 512 and ~88 us/sample sized for 4096. Which is the other half of
 * why the old Reset(sampleRate, 1) had to go: sizing for one frame is what
 * made block processing impossible in the first place.
 *
 * So this block overrides AudioBlock::processBlock() and hands NAM whole
 * chunks in one process() call; processSample() is implemented as a 1-frame
 * processBlock(), so there is exactly one copy of the maths (trims, tone,
 * mix) and a host that still drives a block one frame at a time gets
 * bit-identical results. The corollary is worth stating plainly: the win
 * only materialises once the caller actually uses processBlock(). Driving
 * this block per sample is *slower* than it was with the one-frame buffers
 * (12.3 vs 2.8 us/sample measured), because NAM is then re-streaming its
 * weights against a block-sized working set - one more reason the chain
 * should drive EffectChain::processBlock() rather than processSample().
 *
 * maxBlockSize is the number of frames NAM may see in a single process()
 * call, and it has to be the same number Reset() was handed: that call is
 * what sizes NAM's internal buffers (per-layer Eigen matrices, per-conv
 * ring buffers), so calling process() with more frames than that would
 * write past the end of them. processBlock() therefore splits whatever it
 * is given into chunks of at most maxBlockSize rather than assuming the
 * host's buffer size and ours agree; setMaxBlockSize() is the off-audio-
 * thread side of that contract (see its own comment).
 *
 * Tone stack: a .nam capture already has its amp's tone stack baked into the
 * recording, so on its own this block would leave the player with no
 * Bass/Mid/Treble/Presence at all. The ToneStack member puts them back -
 * post-model, pre-mix, and using the same filter types/centers/Q as
 * AmpBlock's "Modern" voicing (see core/ToneStack.hpp). With all four knobs
 * at 0dB, both trims at 0dB and Mix at 1 the block is exactly the capture,
 * which is what keeps presets saved before the tone stack existed sounding
 * the way they did.
 *
 * A2 / slimmable size: an A2 capture carries several model sizes in one
 * file (nam::SlimmableModel::SetSlimmableSize, 0..1 = smallest/cheapest to
 * full size). That is A2's headline feature - trade CPU for accuracy - and
 * it is what setQuality() exposes; see there for how a size change is kept
 * off the audio thread's critical path.
 *
 * Loading a .nam file happens on whatever non-audio thread the host calls
 * setState() from (see ChainPlugin.cpp), while audio keeps running on a
 * different thread - handled with the same lock-free double buffer
 * CabinetBlock uses for its impulse response: the new model is fully built,
 * Reset() (which preallocates nam's internal buffers and prewarms, settling
 * initial WaveNet/LSTM state so there's no audible startup transient) and,
 * for an A2 capture, every submodel pre-warmed too, all in the *inactive*
 * slot, then a single atomic store publishes it - the audio thread never
 * sees a half-constructed model.
 *
 * One caveat on that, stated because the sentence above could be read as
 * promising more: the slot the new model is written into is the one the
 * *previous* model lived in, so a load frees it. Two loads arriving inside a
 * single processBlock() window would therefore free a model the audio thread
 * is still running - a real, if narrow, race (a host restoring state while
 * also loading a file, say). It is the same two-slot scheme CabinetBlock has
 * used for its impulse response since before this block existed, so it is
 * inherited rather than introduced here; the fix is a retire list drained on
 * the audio thread, which is a change to both blocks and deliberately not
 * bundled into this one.
 */

namespace ampforge {

class NamBlock : public AudioBlock
{
public:
    // 512 frames covers every buffer size a plugin host realistically hands
    // a chain, so processBlock() is correct even if nobody ever calls
    // setMaxBlockSize() (setMaxBlockFrames() in EffectChain is optional for
    // the same reason). Allocating it here, in the constructor, is the only
    // place it is safe: this is the off-audio-thread setup every block gets.
    static constexpr int kDefaultMaxBlockSize = 512;

    // Hard ceiling on the scratch below, allocated once here in the
    // constructor and never resized. setMaxBlockSize() is reachable from the
    // JACK standalone's process thread (see its comment), and with a fixed
    // buffer the worst that a buffer-size change can cost there is a dropout:
    // there is no reallocation and, because the pointers handed to NAM are
    // written once, no torn read either. 8192 frames is 64 KB both ways and far
    // beyond any buffer a host or a JACK user hands a plugin; anything larger
    // is simply processed in more chunks.
    static constexpr int kMaxScratchFrames = 8192;

    NamBlock()
        : scratchIn(kMaxScratchFrames), scratchOut(kMaxScratchFrames)
    {
        inChannels[0]  = scratchIn.data();
        outChannels[0] = scratchOut.data();
        setMaxBlockSize(kDefaultMaxBlockSize);
    }

    void setSampleRate(double sr) override
    {
        // Refused when it isn't a rate at all: 0 or NaN would reach NAM's own
        // Reset() and produce NaN coefficients (and, for a slimmable model, a
        // container whose children are Resetted at a garbage rate). DPF guards
        // its side, the JACK standalone forwards whatever it is handed.
        if (!(sr > 0.0) || !std::isfinite(sr))
            return;

        sampleRate = sr;
        toneStack.setSampleRate(sr);

        // NAM sizes its internal buffers from this call, so it has to be told
        // the same maxBlockSize processBlock() respects. (This used to be a
        // hardcoded 1, matching the old one-frame-at-a-time process(): the
        // buffers then stayed sized for a single frame, which is both what
        // made block processing impossible and what a stale Reset left behind
        // after a host buffer-size change.)
        //
        // Re-Reset'ing clears the model's history, so a sample-rate change
        // costs a (very short) re-settling transient - it already did before
        // this block was batched, and a rate change mid-session is not a
        // per-buffer event.
        const int active = activeModel.load(std::memory_order_acquire);
        if (active >= 0 && models[static_cast<size_t>(active)])
            models[static_cast<size_t>(active)]->Reset(sampleRate, maxBlockSize.load(std::memory_order_relaxed));
    }

    const char* getName() const override { return "NAM"; }

    // Every setter clamps to the range its parameter declares (see
    // ChainPlugin::initParameter()). Hosts are supposed to do that for us, and
    // most do - but not all of them do it on every path (the JACK standalone
    // forwards whatever value it is handed), and a value far outside the range
    // is not a cosmetic problem here: +512 dB of Presence is a biquad asked for
    // a gain of ~1e26, i.e. instant full-scale noise. A stale preset file
    // written against an older parameter layout is exactly the kind of thing
    // that can deliver one, so the DSP refuses it rather than trusting the
    // caller.
    //
    // NaN gets through std::clamp untouched (all its comparisons are false), so
    // each setter sanitises first: a NaN gain would otherwise persist for the
    // rest of the session - and, in the tone stack, poison a biquad's feedback
    // path. sanitizeParam() is that one place.
    void setInputTrim(float db)  { inputGainLinear  = std::pow(10.0f, sanitizeParam(db, -24.0f, 24.0f) / 20.0f); }
    void setOutputTrim(float db) { outputGainLinear = std::pow(10.0f, sanitizeParam(db, -24.0f, 12.0f) / 20.0f); }
    void setMix(float m)         { mix = sanitizeParam(m, 0.0f, 1.0f, 1.0f); }

    // --- Tone stack: same units, same names as AmpBlock's setters ---
    // Bass/Mid/Treble are +/-12 dB and Presence is boost-only 0..12 dB, matching
    // the parameters and AmpBlock's own tone stack.
    void setBassDB(float db)     { toneStack.setBassDB(sanitizeParam(db, -12.0f, 12.0f)); }
    void setMidDB(float db)      { toneStack.setMidDB(sanitizeParam(db, -12.0f, 12.0f)); }
    void setTrebleDB(float db)   { toneStack.setTrebleDB(sanitizeParam(db, -12.0f, 12.0f)); }
    void setPresenceDB(float db) { toneStack.setPresenceDB(sanitizeParam(db, 0.0f, 12.0f)); }

    // One frame, i.e. a 1-frame block. Kept as an override at all because the
    // chain still has a per-sample path (EffectChain::processSample()) that a
    // host may drive; routing it through processBlock() means the two paths
    // can never drift apart - the same trim/tone/mix maths, in the same order,
    // on the same values.
    float processSample(float input) override
    {
        float output = input;
        processBlock(&input, &output, 1);
        return output;
    }

    // Batched path - see the class comment for why it exists. Nothing here
    // allocates or does I/O: the scratch lives in scratchIn/scratchOut, sized
    // once in the constructor, and `in`/`out` may be the same buffer, since
    // sample i is read before it is written. The one exception is a pending A2
    // size change, which does take ContainerModel's mutex and reset the
    // incoming submodel - see setQuality() for exactly what that costs and why
    // it is the least-bad option.
    void processBlock(const float* in, float* out, uint32_t frames) override
    {
        if (frames == 0)
            return;

        // Publish "audio is in flight" for setMaxBlockSize()'s benefit: that
        // method re-Resets and re-warms the model, which is only safe with this
        // thread stopped. Every plugin format DPF implements guarantees that
        // (it deactivates around bufferSizeChanged()), but the JACK standalone
        // calls it straight from the process thread, so the contract is
        // enforced here rather than merely documented there. Two relaxed
        // stores per block are as cheap as an atomic gets.
        const ProcessingGuard processing(*this);

        // The acquire load is also what makes slimmableModels[] and the rest of
        // the per-slot model state visible/consistent here - see loadModel()'s
        // release store.
        const int active = activeModel.load(std::memory_order_acquire);
        nam::DSP* const model = (active < 0) ? nullptr : models[static_cast<size_t>(active)].get();
        if (!model)
        {
            // Nothing loaded yet (or the load failed): a clean pass-through,
            // deliberately without trims/tone/mix applied - an empty NAM slot
            // has to be transparent, not a silent or a coloured one. Same
            // behaviour as the per-sample path has always had.
            if (out != in)
                std::copy(in, in + frames, out);
            return;
        }

        // A pending A2 size change is applied here, at a block boundary and
        // never in the middle of one: the submodels are different networks, so
        // switching mid-buffer would splice one model's output into the
        // other's. Reading the request once per block is also what keeps the
        // parameter setter - which runs on the host's thread - from having to
        // touch anything the audio thread owns.
        //
        // slimmableModels[] only ever holds a pointer for a capture whose
        // switch is cheap - an A2 "SlimmableContainer" (see loadModel()). That
        // is not the same thing as "the model implements nam::SlimmableModel":
        // NeuralAmpModelerCore's other slimmable shape is a plain "WaveNet"
        // with per-layer "slimmable" config (wavenet/slimmable.h), and its
        // SetSlimmableSize() *rebuilds the whole network* - it copies every
        // weight and constructs a new WaveNet - which on the audio thread is a
        // guaranteed dropout. Such a capture keeps hasSlimmableModel() false
        // and never reaches this branch, which also keeps it in agreement with
        // the UI (both go by the "SlimmableContainer" architecture string).
        nam::SlimmableModel* const slimmable = slimmableModels[static_cast<size_t>(active)];
        if (slimmable)
        {
            const float requested = requestedQuality.load(std::memory_order_relaxed);
            if (requested != appliedQuality)
            {
                slimmable->SetSlimmableSize(static_cast<double>(requested));
                appliedQuality = requested;
            }
        }

        uint32_t done = 0;
        while (done < frames)
        {
            // Never more than maxBlockSize frames per process() call: that is
            // exactly what Reset() sized NAM's internal buffers for.
            // Acquire: paired with the release store at the end of
            // setMaxBlockSize(), which is what guarantees NAM's buffers were
            // already grown when this larger cap becomes visible here.
            const uint32_t chunk = std::min<uint32_t>(frames - done, static_cast<uint32_t>(maxBlockSize.load(std::memory_order_acquire)));
            const float* const chunkIn  = in + done;
            float* const       chunkOut = out + done;

            for (uint32_t i = 0; i < chunk; ++i)
                scratchIn[i] = static_cast<NAM_SAMPLE>(chunkIn[i] * inputGainLinear);

            model->process(inChannels, outChannels, static_cast<int>(chunk));

            for (uint32_t i = 0; i < chunk; ++i)
            {
                // flushDenormal: a model whose output decays toward silence can
                // produce denormals, which x86 then handles through a much
                // slower path even though nothing audible is happening - same
                // reasoning as everywhere else Denormal.hpp is used.
                const float wet = toneStack.process(
                    flushDenormal(static_cast<float>(scratchOut[i])) * outputGainLinear);

                // Mixed against the *untrimmed* input, i.e. the signal as it
                // arrived - reading it before writing this index is what makes
                // an in-place call (in == out) safe.
                chunkOut[i] = chunkIn[i] * (1.0f - mix) + wet * mix;
            }

            done += chunk;
        }
    }

    // Raises the cap processBlock() splits its input by, and re-Resets the
    // loaded model so NAM's own buffers match it. It must be called with the
    // audio thread stopped, because the Reset() at the end of it allocates
    // every Eigen matrix and ring buffer NAM owns; the scratch on our side is
    // fixed-size and is not touched here at all.
    //
    // Where that really holds, and where it does not: for LV2/VST3/CLAP, DPF
    // issues bufferSizeChanged() between deactivate() and activate(), i.e. with
    // processing stopped, which is the case this method is designed for. The
    // JACK standalone's own buffer-size callback is different -
    // DistrhoPluginJACK.cpp's jackBufferSize() calls setBufferSize(nframes,
    // true) straight from the JACK process thread - so a standalone user
    // changing the buffer size at runtime can, in principle, hit the Reset()
    // while audio is flowing. The plugin side keeps that to a minimum by only
    // ever *growing* (a repeat of the same or a smaller size is a no-op, so
    // "host re-announces the size it already used" costs nothing), and a
    // buffer-size change is a deliberate, user-initiated configuration event
    // where a dropout is acceptable - silently running NAM with buffers
    // smaller than the caller's block would not be.
    //
    // Only ever grows, mirroring EffectChain::setMaxBlockFrames(): a later call
    // with a smaller (or equal) size does nothing, so the scratch can never
    // shrink out from under a processBlock() that is still using it and the
    // audio thread is never the one paying for a reallocation. NAM's buffers
    // staying at an old, larger size is harmless - maxBlockSize only ever caps
    // how many frames go into one process() call.
    void setMaxBlockSize(int frames)
    {
        // Refused outright if audio is running: everything below re-Resets and
        // re-warms the model, which touches buffers a processBlock() in flight
        // is reading (for a nam::Buffer-based architecture it can even resize
        // one). DPF deactivates around bufferSizeChanged(), so this only ever
        // trips in the JACK standalone - where the alternative would be a
        // heap corruption, versus not getting the bigger block size until the
        // next time the host is stopped. Cheap to check, so it is checked
        // rather than trusted (see ProcessingGuard).
        if (processingInFlight.load(std::memory_order_relaxed))
            return;

        // Clamped to what the fixed scratch can hold, so processBlock()'s chunk
        // loop can never ask for more frames than the buffers offer.
        frames = std::min(frames, kMaxScratchFrames);
        if (frames <= maxBlockSize.load(std::memory_order_relaxed))
            return;

        // Order matters, and not in the obvious direction: NAM's own buffers
        // are grown *first* and maxBlockSize is published *last*. A host can
        // deliver a block-length change while rolling (DPF's LV2
        // lv2_set_options() handles bufsize:maxBlockLength, and VST2 has
        // effSetBlockSize), so a processBlock() running concurrently must never
        // see a cap larger than the buffers behind it - that is exactly the
        // overrun this whole method exists to prevent. Publishing last with
        // release, and reading it with acquire in processBlock(), is what makes
        // the grown buffers visible before the cap that allows them.
        const int active = activeModel.load(std::memory_order_acquire);
        if (active >= 0 && models[static_cast<size_t>(active)])
        {
            models[static_cast<size_t>(active)]->Reset(sampleRate, frames);

            // Reset() only resizes the *active* submodel - ContainerModel's
            // override deliberately stays on that one (NAM/container.cpp) - so
            // every other submodel is now sized for the old, smaller block.
            // Without re-warming them here, the next Size change made on the
            // audio thread would find an under-sized submodel and grow it
            // (i.e. allocate) mid-block, which is precisely what the whole
            // pre-warm arrangement exists to prevent. This is the same work
            // loadModel() does, on the same kind of (non-realtime) thread.
            nam::SlimmableModel* const slimmable = slimmableModels[static_cast<size_t>(active)];
            if (slimmable)
            {
                warmAllSubmodels(*models[static_cast<size_t>(active)], *slimmable, frames);

                // warmAllSubmodels() walks every size and leaves the container
                // on the *last* one (full size), so the user's setting has to be
                // restored here - exactly as loadModel() does. Skipping this
                // used to lose it silently: appliedQuality still equalled
                // requestedQuality, so the audio thread's comparison never fired
                // again and the model stayed at full size (i.e. the knob lied,
                // and the CPU saving was gone) until the user moved it.
                slimmable->SetSlimmableSize(static_cast<double>(snapQuality(
                    requestedQuality.load(std::memory_order_relaxed))));
            }
        }

        // Published last - see the comment above.
        maxBlockSize.store(frames, std::memory_order_release);
    }

    // A2 slimmable size, 0.0 (smallest/cheapest submodel) .. 1.0 (full size),
    // clamped. No-op for a model that isn't a nam::SlimmableModel - the value
    // is still stored, so setting it before loading an A2 capture (a preset
    // restore, say) does the right thing once that capture is loaded.
    //
    // Applied by the audio thread at the next block boundary rather than here,
    // for two reasons: this setter runs on the host's thread while processBlock()
    // owns the model, and a size change has to be a boundary event (see
    // processBlock()).
    //
    // NeuralAmpModelerCore documents SetSlimmableSize() as thread-safe but NOT
    // real-time safe - it Resets the newly activated submodel, and it takes an
    // internal mutex. Applying it from the audio thread is nonetheless
    // acceptable here because of what loadModel() does while it still owns the
    // model off the audio thread: it pre-warms every submodel (walking
    // GetSlimmableSizeBreakpoints() plus both ends) by running each of them
    // once, which sizes all of their buffers, and then calls
    // SetPrewarmOnReset(false) - which ContainerModel propagates to every
    // submodel it owns. After that a switch is a state clear plus a resize to
    // an already-allocated size: no prewarm, no allocation.
    //
    // What a switch does still cost, stated plainly, because none of it is
    // free and pretending otherwise would be the wrong kind of tidy:
    //  - ContainerModel::SetSlimmableSize() takes _slim_set_mutex, the same
    //    mutex its own Reset() takes. In a plugin that is uncontended in
    //    practice - every Reset() caller (setSampleRate, setMaxBlockSize,
    //    loadModel) is documented as running with the audio thread stopped -
    //    but a JACK standalone user changing the buffer size at runtime is the
    //    one case where the audio thread can wait on a message-thread memset.
    //  - The submodel it activates is Reset() with prewarming off, i.e. its
    //    state is zeroed. The model therefore restarts from silence and
    //    re-ramps over one receptive field (6332 samples, ~132 ms at 48 kHz,
    //    for the A2 capture this was benchmarked on). That is a soft swell
    //    rather than a click - nothing is spliced, the input is continuous -
    //    and it only happens when a user actually moves the Size knob.
    //    Prewarming instead would move 9-18 ms of work onto the audio thread,
    //    which is the worse trade; a crossfade would need both submodels
    //    running at once, i.e. double the CPU, for the same 132 ms.
    void setQuality(float q01)
    {
        // Fallback 1.0, not 0.0: of the two ends, full size is the one that
        // means "unchanged" - 0.0 would silently drop a user onto the smallest,
        // cheapest submodel (the exact trap ChainParameters.hpp warns about for
        // a zero-initialised Quality).
        const float clamped = sanitizeParam(q01, 0.0f, 1.0f, 1.0f);
        // Snapped here, on the caller's thread, so the audio-thread comparison
        // only changes when a real submodel switch is due (see snapQuality()).
        requestedQuality.store(snapQuality(clamped), std::memory_order_relaxed);
    }

    // Whether the loaded capture is an A2 one (i.e. carries more than one model
    // size). False until a model is loaded and for every A1/.nam architecture
    // that doesn't make it a cheap switch (see loadModel()). Note the UI does
    // not read this - it decides whether to enable the Size knob from the .nam
    // file's architecture string itself, because the DSP and UI are separate
    // binaries on LV2 and the state-push that would have carried this is a
    // no-op on VST3/CLAP (see ChainParameters.hpp). Kept as the authoritative
    // answer for tests and diagnostics; the two must agree, and both go by
    // "SlimmableContainer".
    bool hasSlimmableModel() const
    {
        const int active = activeModel.load(std::memory_order_acquire);
        return active >= 0 && slimmableModels[static_cast<size_t>(active)] != nullptr;
    }

    // The sample rate the .nam file itself declares (most captures assume one),
    // or -1.0 if it doesn't say - NAM_UNKNOWN_EXPECTED_SAMPLE_RATE. Reported for
    // tests/diagnostics; the sidebar shows the same number by reading it out of
    // the file on the UI side (see above for why the UI can't just ask).
    // -1.0 until a model has been loaded.
    double getModelSampleRate() const
    {
        const int active = activeModel.load(std::memory_order_acquire);
        return (active < 0) ? -1.0 : modelSampleRates[static_cast<size_t>(active)];
    }

    // Parses a .nam file and, on success, hot-swaps it in for the next
    // processBlock()/processSample() call. Safe to call from any non-audio
    // thread while audio keeps running (see the class comment above). Returns
    // false (leaving whatever was loaded before untouched) if the file can't be
    // read or parsed.
    bool loadModel(const std::string& path)
    {
        std::unique_ptr<nam::DSP> model;
        nam::dspData config;
        try
        {
            // Explicit std::filesystem::path: nam::get_dsp() is overloaded for
            // both a file path and a raw nlohmann::json config, and a plain
            // std::string converts implicitly to either - ambiguous otherwise.
            // The dspData overload also hands back the model's declared
            // architecture ("WaveNet", "LSTM", ...) and sample rate for display
            // purposes - in particular, this is how the UI can confirm an A2
            // capture actually loaded as A2 rather than falling back to A1
            // parsing.
            model = nam::get_dsp(std::filesystem::path(path), config);
        }
        catch (const std::exception& e)
        {
            lastError = e.what();
            return false;
        }
        if (!model)
        {
            lastError = "get_dsp() returned no model (unrecognized/unsupported .nam file)";
            return false;
        }

        // Mono only, checked before anything else touches the model.
        //
        // NAM's .nam format allows in_channels/out_channels > 1 (linear.cpp,
        // convnet.cpp, lstm.cpp and wavenet/model.cpp all read them from the
        // file), and a container's submodels carry their own. This block is a
        // mono block - the chain is mono, and it hands NAM the two one-element
        // pointer arrays below - so a 2-channel capture would have NAM read
        // input[1] and write output[1] straight past the end of this object.
        // There is no safe "best effort" here either: a channel-count mismatch
        // is a heap corruption, not a wrong-sounding model. Refuse it and say
        // why, which is what lastError is for.
        const int numIn  = model->NumInputChannels();
        const int numOut = model->NumOutputChannels();
        if (numIn != 1 || numOut != 1)
        {
            lastError = "unsupported .nam channel count (" + std::to_string(numIn) +
                        " in / " + std::to_string(numOut) +
                        " out); AmpForge runs mono models only";
            return false;
        }

        // ...and the same check for an A2 container's *submodels*, which the
        // two calls above cannot see: ContainerModel's own DSP is constructed
        // as DSP(1, 1, ...) no matter what it contains (NAM/container.cpp), and
        // it forwards process()'s pointer arrays straight to whichever submodel
        // is active. A file whose submodels are stereo therefore reports 1/1
        // here and then overruns the same two one-element arrays at processing
        // time. The submodel configs are only in the raw JSON, so they are
        // checked there.
        if (config.architecture == "SlimmableContainer")
        {
            const std::string badChannels = findNonMonoSubmodel(config.config);
            if (!badChannels.empty())
            {
                lastError = "unsupported .nam channel count in a submodel (" + badChannels +
                            "); AmpForge runs mono models only";
                return false;
            }
        }

        // Preallocates nam's internal buffers - must happen here, off the audio
        // thread, since process() would otherwise do this lazily (and
        // expensively) on its first call. maxBlockSize, not 1: the buffers
        // sized by this call are exactly the ones a single process() is later
        // allowed to fill (see the class comment).
        //
        // Note this is only *sufficient* for the architectures whose Reset()
        // actually sizes their buffers - WaveNet, LSTM. Linear and ConvNet
        // derive from nam::Buffer, which grows its buffers lazily on first
        // process() instead, because their GetPrewarmSamples() is 0 and
        // prewarm() therefore returns immediately. warmAllSubmodels() below
        // runs each submodel once for exactly that reason.
        model->Reset(sampleRate, maxBlockSize.load(std::memory_order_relaxed));

        // Cast once, here, and cache the result per slot: the audio thread must
        // not run a dynamic_cast.
        //
        // Only a "SlimmableContainer" capture may be treated as size-switchable
        // from the audio thread, and *that*, not "implements
        // nam::SlimmableModel", is the test on purpose. NeuralAmpModelerCore
        // has two slimmable shapes: the A2 container (several complete
        // submodels, so a switch only activates an already-built one) and a
        // plain "WaveNet" with per-layer "slimmable" config
        // (NAM/wavenet/slimmable.h), whose SetSlimmableSize() *rebuilds the
        // entire network* - copies every weight, constructs a new WaveNet,
        // allocates every layer buffer. Doing that inside processBlock() would
        // be a guaranteed dropout, so that shape is left alone entirely: it
        // reports hasSlimmableModel() false (which also matches what the UI
        // shows, since the UI goes by the same architecture string) and the
        // Size parameter is simply inert for it.
        nam::SlimmableModel* const slimmable =
            (config.architecture == "SlimmableContainer")
                ? dynamic_cast<nam::SlimmableModel*>(model.get())
                : nullptr;
        // Which slot this new model is going into. Everything per-model below
        // is written into it and only then published, by the release store at
        // the end; the audio thread reaches it all through the acquire load of
        // activeModel.
        const int active = activeModel.load(std::memory_order_acquire);
        const size_t next = (active < 0) ? 0 : (1 - static_cast<size_t>(active));

        if (slimmable)
        {
            // Snapshot the breakpoints into this slot's fixed-size table, before
            // the slot can be published and before anything can ask for a
            // snapped value. Not a std::vector: snapQuality() is reached from
            // setQuality(), which on LV2 runs inside run(), so the audio thread
            // reads this table - and a vector being reassigned on the message
            // thread would be a data race on its size/data pointer. Per slot
            // because that also removes the *element* race: the audio thread
            // reads the published slot's table, and loadModel() only ever
            // writes the other one. (Compared with calling
            // GetSlimmableSizeBreakpoints(), which returns a fresh vector every
            // call, this also stops loadModel() allocating.)
            const std::vector<double> breakpoints = slimmable->GetSlimmableSizeBreakpoints();
            const size_t count = std::min(breakpoints.size(), kMaxBreakpoints);
            for (size_t i = 0; i < count; ++i)
                slotBreakpoints[next][i] = breakpoints[i];
            slotBreakpointCounts[next] = count;

            warmAllSubmodels(*model, *slimmable, maxBlockSize.load(std::memory_order_relaxed));

            // Leave the model at the size the user asked for, in case setQuality()
            // already ran (a preset restored before its .nam file, say). With the
            // default 1.0 this is the last switch above's state, so it is a
            // no-op; otherwise it is the same state clear a runtime switch does,
            // just happening at load time where nothing is real-time.
            slimmable->SetSlimmableSize(static_cast<double>(snapQualityTo(
                next, requestedQuality.load(std::memory_order_relaxed))));
        }
        else
        {
            // No grid for whatever goes in this slot: an A1 capture loaded after
            // an A2 one must not keep snapping its (inert) Size value onto the
            // previous file's breakpoints.
            slotBreakpointCounts[next] = 0;
        }

        architecture = config.architecture;

        // A successful load clears whatever the previous failed attempt left
        // behind. Without this, getLastError() kept reporting the *previous*
        // failure forever: the UI's failure toast is driven off "this load
        // failed", and a stale message there would claim a model that loaded
        // perfectly well had failed (or show the wrong reason for a later
        // failure of a different kind).
        lastError.clear();

        // Per-slot, written before the model is published - the release store
        // below is what makes them visible to the audio thread's acquire load.
        modelSampleRates[next] = config.expected_sample_rate;
        slimmableModels[next]  = slimmable;
        models[next] = std::move(model);

        // Single store, release: everything the audio thread can reach through
        // the new model is in place before it can see it.
        activeModel.store(static_cast<int>(next), std::memory_order_release);
        return true;
    }

    // True once a model has actually been loaded (as opposed to the empty,
    // pass-through starting state).
    bool hasModel() const
    {
        return activeModel.load(std::memory_order_acquire) >= 0;
    }

    // The loaded model's architecture name ("WaveNet", "LSTM", ...), as reported
    // by the .nam file itself - empty until a model has actually been loaded.
    // Purely informational (tests/diagnostics - the sidebar reads the file
    // itself); doesn't affect processing, since nam::DSP already dispatches to
    // the right implementation internally.
    const std::string& getArchitecture() const { return architecture; }

    // Why the most recent loadModel() call failed (exception message or
    // "no model" reason) - empty if the last call succeeded or none has been
    // made yet. Purely for the UI's failure toast (see ChainPlugin::setState());
    // doesn't affect processing.
    const std::string& getLastError() const { return lastError; }

    // AudioBlock::reset() - called when a block goes from disabled back to
    // enabled (see EffectChain::setEnabled()). Only the tone stack's filter
    // state is cleared here; the NAM model is deliberately NOT re-Reset.
    //
    // Why not: Reset() zeroes every internal buffer NAM owns - conv ring
    // buffers and per-layer Eigen matrices, several hundred KB for an A2
    // capture - and, with prewarming on (the default for every non-slimmable
    // capture; loadModel() only turns it off for A2 containers), it also runs a
    // full prewarm, measured at ~9-18 ms here against ~0.02 ms without it.
    // setEnabled() is reached from setParameterValue(), which LV2 calls from
    // inside run(): a re-enable would be hundreds of KB of memset plus,
    // potentially, thousands of buffers' worth of real-time budget, on the audio
    // thread. (It would not, contrary to what this comment used to claim,
    // reallocate: Reset() at the sample rate and block size already in use is a
    // same-size resize, which Eigen early-outs.)
    //
    // The consequence, stated honestly: the model keeps whatever history it had
    // when it was switched off, so the first samples after re-enabling splice in
    // up to one receptive field of pre-disable signal. On the A2 capture used to
    // benchmark this, that field is 6332 samples (~132 ms at 48 kHz) - longer
    // than "a small residue", and audible as a brief tail of the old audio. It
    // is still the lesser evil compared with Delay/Reverb's frozen-buffer case
    // (nothing here was left mid-feedback; it is the same performance, just
    // paused), and the clean fixes - a cross-fade over the first block after a
    // re-enable, or keeping the model running on silence while bypassed - are
    // deliberately out of scope here rather than overlooked.
    void reset() override { toneStack.reset(); }

private:
    // Sets processingInFlight for the duration of one processBlock() call, so
    // the setup entry points (setMaxBlockSize(), and by extension
    // loadModel()'s sibling Reset paths) can tell whether "the audio thread is
    // stopped" - the assumption they are written around - actually holds.
    // Relaxed is enough: it is a "is anyone in there right now" flag, not a
    // channel for publishing data.
    struct ProcessingGuard
    {
        explicit ProcessingGuard(const NamBlock& b) : block(b)
        {
            block.processingInFlight.store(true, std::memory_order_relaxed);
        }
        ~ProcessingGuard() { block.processingInFlight.store(false, std::memory_order_relaxed); }
        ProcessingGuard(const ProcessingGuard&) = delete;
        ProcessingGuard& operator=(const ProcessingGuard&) = delete;

        const NamBlock& block;
    };

    // Clamp to the parameter's declared range, refusing anything non-finite.
    // std::clamp alone is not enough: every comparison against a NaN is false,
    // so a NaN would come out the other side unchanged and then stick - a NaN
    // input gain is NaN audio for the rest of the session, and a NaN tone
    // value poisons a biquad's feedback path. Hosts and corrupt preset files
    // both get here in practice (ChainUI's preset parser uses strtof, which
    // accepts the literal "nan").
    static float sanitizeParam(float value, float lo, float hi)
    {
        return std::isfinite(value) ? std::clamp(value, lo, hi) : 0.0f;
    }
    // Overload with an explicit fallback for parameters whose neutral value
    // isn't zero (Quality's is 1.0 - falling back to 0.0f there would mean
    // "the smallest, cheapest submodel", which is the wrong kind of surprise
    // for a corrupt value).
    static float sanitizeParam(float value, float lo, float hi, float fallback)
    {
        return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    }

    // Description of the first non-mono submodel in an A2 container's config,
    // or an empty string when every submodel is mono. Reads the raw
    // "config" JSON because that is the only place a submodel's channel count
    // exists (see the call site); the shape is
    // {"submodels":[{"max_value":0.5,"model":{"config":{"in_channels":..}}},
    // ...]} and every field is optional, defaulting to 1 - which is what
    // NAM's own parsers do (NAM/linear.cpp, convnet.cpp, lstm.cpp,
    // wavenet/model.cpp all read it with a default of 1).
    //
    // Deliberately tolerant: a config it can't make sense of reports "mono"
    // rather than failing the load, because anything genuinely malformed is
    // rejected by get_dsp() itself a few lines above, with a better message.
    static std::string findNonMonoSubmodel(const nlohmann::json& config)
    {
        if (!config.is_object())
            return {};

        const auto submodels = config.find("submodels");
        if (submodels == config.end() || !submodels->is_array())
            return {};

        for (const auto& submodel : *submodels)
        {
            if (!submodel.is_object())
                continue;
            const auto model = submodel.find("model");
            if (model == submodel.end() || !model->is_object())
                continue;
            const auto submodelConfig = model->find("config");
            if (submodelConfig == model->end() || !submodelConfig->is_object())
                continue;

            const auto channel = [&submodelConfig](const char* key) {
                const auto it = submodelConfig->find(key);
                return (it != submodelConfig->end() && it->is_number_integer()) ? it->get<int>() : 1;
            };
            const int in  = channel("in_channels");
            const int out = channel("out_channels");
            if (in != 1 || out != 1)
                return std::to_string(in) + " in / " + std::to_string(out) + " out";
        }
        return {};
    }

    // Pre-warms, sizes and precomputes *every* submodel of an A2 container -
    // the one piece of setup that makes a later SetSlimmableSize() call from
    // the audio thread cheap, so it runs on every non-realtime path that can
    // invalidate it: loadModel() (a new model) and setMaxBlockSize() (a bigger
    // block, which Reset() only applies to the active submodel).
    //
    // Three things happen here, and all of them matter:
    //   1. every submodel is Reset() at the current maxBlockSize by
    //      SetSlimmableSize() (via ContainerModel, which resets the child it is
    //      about to activate);
    //   2. every submodel is then actually *run* over one block of silence.
    //      Reset() alone is not enough: WaveNet sizes its ring buffers in
    //      Reset(), but Linear and ConvNet derive from nam::Buffer, whose
    //      buffers grow lazily inside process() - and their GetPrewarmSamples()
    //      is 0, so prewarm() returns immediately and sizes nothing. Without
    //      this step the first process() after a switch to one of those would
    //      allocate on the audio thread, which is the thing this method exists
    //      to prevent;
    //   3. prewarming is turned off afterwards, so the Reset() that
    //      SetSlimmableSize() performs on the submodel it activates is only a
    //      state clear (measured: ~9-18 ms with the prewarm on, ~0.02 ms
    //      without, for a real A2 capture). ContainerModel overrides
    //      SetPrewarmOnReset() to propagate the flag to all its children.
    //
    // Walking `0.0` as well as the breakpoints is deliberate: a breakpoint is
    // a submodel's *upper* bound, so the smallest submodel is only selected
    // below the first one - iterating breakpoints plus 1.0 alone would leave
    // exactly the one a user reaching for "less CPU" lands on cold.
    // `frames` is passed in rather than read from maxBlockSize: setMaxBlockSize()
    // calls this *before* publishing the new cap (it has to - see the ordering
    // comment there), so the member still holds the old, smaller size at that
    // point and warming at it would leave every submodel under-sized.
    void warmAllSubmodels(nam::DSP& model, nam::SlimmableModel& slimmable, int frames)
    {
        // Prewarm off *first*, before any of the Resets below: it is what keeps
        // them cheap here too (the state-clearing Reset is unavoidable, the
        // 9-18 ms warmup is not, and these run one per submodel).
        model.SetPrewarmOnReset(false);

        std::fill_n(scratchIn.data(), static_cast<size_t>(frames), NAM_SAMPLE(0));
        std::fill_n(scratchOut.data(), static_cast<size_t>(frames), NAM_SAMPLE(0));

        std::vector<double> sizes;
        sizes.push_back(0.0);
        for (double breakpoint : slimmable.GetSlimmableSizeBreakpoints())
            sizes.push_back(breakpoint);
        sizes.push_back(1.0);
        for (double size : sizes)
        {
            slimmable.SetSlimmableSize(size);
            // One block on silence, output discarded: this is what forces a
            // nam::Buffer-based submodel to size its buffers, and it costs one
            // block per submodel instead of the thousands prewarm() would.
            model.process(inChannels, outChannels, frames);
        }

        // The loop above left scratchOut holding that silence; nothing reads it
        // before processBlock() fills it, but clearing it again keeps the class
        // honest about a "warm" buffer never being mistaken for audio.
        std::fill_n(scratchOut.data(), static_cast<size_t>(frames), NAM_SAMPLE(0));
    }

    // Snaps a 0..1 quality request onto the submodel it actually selects, so
    // that (a) appliedQuality only changes when a switch really happens - a
    // host automating or ramping the parameter inside one submodel's range
    // would otherwise re-trigger the comparison constantly - and (b) a
    // restored session/preset lands on the same value it was saved with
    // regardless of float rounding. Semantics match
    // ContainerModel::_get_index_for_slimmable_size(): a submodel covers
    // values *below* its breakpoint, and the last one covers everything else.
    //
    // Uses the *active* slot's grid, which is what the audio thread's
    // comparison (and the parameter setter that feeds it) is talking about.
    float snapQuality(float q01) const
    {
        const int active = activeModel.load(std::memory_order_acquire);
        return (active < 0) ? q01 : snapQualityTo(static_cast<size_t>(active), q01);
    }

    // snapQuality() against an explicit slot - used by loadModel() for the slot
    // it is about to publish, since that model's grid is the one the requested
    // size has to be snapped against (the previously active one may be a
    // different file entirely).
    float snapQualityTo(size_t slot, float q01) const
    {
        // Nothing slimmable in that slot (or the model isn't a container): the
        // value is inert, keep it as asked so a session round-trips unchanged.
        const size_t count = slotBreakpointCounts[slot];
        if (count == 0)
            return q01;

        // Which submodel does `q01` select, and what is that submodel's *lower*
        // bound? The lower bound, not the upper one: a submodel covers values
        // strictly below its breakpoint (ContainerModel: `val < max_value`), so
        // returning the upper bound would hand back a value that selects the
        // *next* submodel - exactly the off-by-one that would turn "smallest"
        // into "largest".
        const double q = static_cast<double>(q01);
        for (size_t i = 0; i < count; ++i)
            if (q < slotBreakpoints[slot][i])
                return (i == 0) ? 0.0f : static_cast<float>(slotBreakpoints[slot][i - 1]);
        return 1.0f; // the last submodel's range runs up to (and including) 1.0
    }

    double sampleRate = 44100.0;
    float inputGainLinear  = 1.0f;
    float outputGainLinear = 1.0f;
    float mix = 1.0f;
    std::string architecture;
    std::string lastError;
    ToneStack toneStack;

    std::array<std::unique_ptr<nam::DSP>, 2> models;
    std::atomic<int> activeModel{-1};

    // The breakpoints of the *currently loaded* slimmable model, captured at
    // load time so snapQuality() (and therefore the audio thread) never has to
    // call back into NAM. Shared by both model slots on purpose: a switch only
    // ever happens against the active model, and loadModel() refreshes this
    // before publishing the new one.
    //
    // Fixed-size and *per slot*, because the audio thread reads this table: on
    // LV2, setQuality() is reached from inside run(), and a std::vector being
    // reassigned by loadModel() at that moment would be a data race on its
    // size/data pointer. Per slot removes the element race too - loadModel()
    // writes the grid of the slot it is about to publish, while a running
    // processBlock() (or setQuality()) can only be looking at the active one.
    // The counts need no atomics: a slot's count is written before the release
    // store that publishes its model, and read after the acquire load of it.
    //
    // The one window left is theoretical and worth naming anyway: a reader that
    // loaded the active index, then stalled long enough for two more loads to
    // cycle back round to that same slot, would see it being rewritten. The
    // cost is a wrongly snapped Size value for one block, never memory unsafety
    // (the arrays are fixed-size), and it takes two state imports inside one
    // audio callback to hit.
    //
    // 32 is far more submodels than any A2 capture has (a handful); anything
    // beyond it is clamped, which only costs a less precise Size knob, never
    // correctness.
    static constexpr size_t kMaxBreakpoints = 32;
    std::array<std::array<double, kMaxBreakpoints>, 2> slotBreakpoints{};
    std::array<size_t, 2> slotBreakpointCounts{};

    // Per-slot companions to models[], written in loadModel() before the
    // publishing store and read after the matching acquire load - so no
    // separate atomics (or locks) are needed to hand them to the audio thread.
    // Caching the cast keeps dynamic_cast out of the audio thread;
    // modelSampleRates comes from the file's own header.
    std::array<nam::SlimmableModel*, 2> slimmableModels{nullptr, nullptr};
    std::array<double, 2> modelSampleRates{-1.0, -1.0};

    // The A2 size the parameter setter asked for (written on the host's thread)
    // and the one the audio thread has actually applied. Plain float, not
    // atomic: it is owned exclusively by the audio thread. After a model swap
    // it can briefly disagree with the newly published model's size, which at
    // worst costs one redundant SetSlimmableSize() - a no-op when the size
    // already matches (ContainerModel's own fast path).
    std::atomic<float> requestedQuality{1.0f};
    float appliedQuality = 1.0f;

    // Block-processing scratch, allocated once in the constructor at
    // kMaxScratchFrames and used only by processBlock(). NAM_SAMPLE is float in
    // this build (nam_core is compiled with NAM_SAMPLE_FLOAT - see
    // CMakeLists.txt), but the buffers are typed NAM_SAMPLE so they stay
    // correct if that ever changes. inChannels/outChannels are set from them
    // once and then only read, which is what keeps them race-free against
    // setMaxBlockSize().
    //
    // Atomic, acquire/release rather than relaxed, because it is not just a
    // counter: the store in setMaxBlockSize() happens *after* NAM's own buffers
    // were grown to match, and that ordering is what stops a concurrently
    // running processBlock() from chunking by a cap its model cannot yet take.
    // The scratch it caps is fixed-size (see above), so only the model side
    // rides on that ordering.
    std::atomic<int> maxBlockSize{kDefaultMaxBlockSize};

    // True while a processBlock() call is on the stack - see ProcessingGuard
    // and setMaxBlockSize(). mutable because the guard only ever holds a const
    // reference to the block (it has no other business with it), and this is
    // precisely the kind of flag that the logically-const audio path sets.
    mutable std::atomic<bool> processingInFlight{false};
    std::vector<NAM_SAMPLE> scratchIn;
    std::vector<NAM_SAMPLE> scratchOut;
    NAM_SAMPLE* inChannels[1]  = { nullptr };
    NAM_SAMPLE* outChannels[1] = { nullptr };
};

} // namespace ampforge
