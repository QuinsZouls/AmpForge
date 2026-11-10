#pragma once
#include <cstdint>

/*
 * AudioBlock - base interface every module in AmpForge (Amp, Screamer,
 * Delay, Reverb, ...) will derive from.
 *
 * Why do we need this?
 * The design goal is for the user to be able to chain and reorder
 * pedals/amp however they want. The way to support that in code is:
 * make every module fit the same "shape" (interface), then represent
 * the chain as a std::vector<AudioBlock*>. The chain will work like this:
 *
 *   float sample = input;
 *   for (auto* block : chain)
 *       sample = block->processSample(sample);
 *
 * So reordering the chain becomes as simple as reordering the elements
 * of the vector.
 *
 * That per-sample loop is still the primitive - processSample() is what every
 * block implements - and processBlock() is the block-at-a-time entry point
 * built on top of it (see its comment for why it exists).
 */

namespace ampforge {

class AudioBlock
{
public:
    virtual ~AudioBlock() {}

    // Called when the host reports the sample rate (e.g. 44100, 48000).
    // Sample-rate-dependent calculations (like filter coefficients) happen here.
    virtual void setSampleRate(double sampleRate) = 0;

    // Processes a single audio sample and returns the processed result.
    // NOTE: this function runs on the real-time audio thread -
    // no new/malloc, file I/O, or mutex locking is allowed inside it.
    //
    // Still pure virtual, and still the one method every block in this project
    // implements - deliberately, rather than giving it a default that calls
    // processBlock() while processBlock()'s own default calls processSample():
    // those two defaults would recurse forever the first time a block
    // overrode neither, and the failure would be a stack overflow on the audio
    // thread rather than a compile error.
    virtual float processSample(float input) = 0;

    // Processes a whole block of `frames` samples in one call: reads
    // in[0 .. frames) and writes the results to out[0 .. frames). `in` and
    // `out` may be the same buffer (a caller processing in place).
    //
    // Why this exists: NeuralAmpModelerCore (NamBlock) is built to run a
    // block of frames at once - that's where its internal batching gets
    // its efficiency - but every block in this chain has always been driven
    // one frame at a time through processSample(), so NamBlock was calling
    // NAM's process() with num_frames = 1 per sample. Measured on a real
    // capture that costs ~9.5-13 us/sample versus ~1.3-1.8 us/sample at
    // block-sized calls: 6-10x of the block's throughput thrown away, the
    // spread coming from which submodel the capture is running and how large
    // the block is. Handing a block the whole buffer in one call is the only
    // way to get it back. Hosted plugins (PluginNodeBlock) have the same
    // property for the same reason.
    //
    // The default implementation below is deliberately nothing but a loop
    // over processSample(). That is what keeps every other block
    // bit-identical without touching any of them: the same per-sample
    // function runs, in the same order, on the same values, so a chain
    // processed through processBlock() produces exactly what the
    // per-sample loop produced before this method existed. It is also
    // naturally in-place-safe, because iteration i reads in[i] and then
    // writes out[i] - no sample that hasn't been read yet is ever
    // overwritten, even when in == out.
    //
    // Blocks that are already cheap per sample (a gain, an offset, a
    // biquad: a handful of flops, no per-call setup) should NOT override
    // this. The plain loop is already optimal for them, and an override
    // would only be a second copy of the same maths to keep in sync. Only
    // blocks whose per-call overhead is real relative to their per-sample
    // work - NAM's neural network, a hosted plugin, a block-at-a-time
    // convolution - gain anything from overriding it.
    //
    // An override inherits two obligations from that contract:
    //  * it must cope with `in == out`. The chain hands the host's own buffer
    //    straight through when it can (see EffectChain::processBlock()), and
    //    a block that reads `in` only to overwrite it before it has been read
    //    would corrupt the audio, not just reorder it.
    //  * it must not assume frames > 0. Callers are allowed to pass whatever
    //    buffer size the host reports straight through without special-casing
    //    an empty one.
    //
    // Like processSample(), an override runs on the real-time audio
    // thread - it must not allocate, lock, do file I/O, or otherwise
    // block. Any scratch buffer it needs has to be sized beforehand, off
    // the audio thread (see EffectChain::setMaxBlockFrames() and
    // NamBlock::setMaxBlockSize()).
    virtual void processBlock(const float* in, float* out, uint32_t frames)
    {
        for (uint32_t i = 0; i < frames; ++i)
            out[i] = processSample(in[i]);
    }

    // The block's name - will be used in the GUI and preset files later.
    virtual const char* getName() const = 0;

    // Clears whatever sample history a block is holding (a delay/reverb
    // tail, a pitch-shifter's grain buffer, ...) - called whenever the
    // block transitions from disabled back to enabled (see
    // EffectChain::setEnabled()). Blocks whose internal buffer can sit
    // frozen for an arbitrary length of time while disabled - the block
    // is simply skipped in EffectChain::processSample(), so nothing
    // advances or clears it - would otherwise play that stale, possibly
    // long-past audio back out the instant they're switched on again
    // (e.g. re-enabling a Delay whose feedback loop still holds a chunk
    // of a completely different preset's audio). Default no-op: only
    // blocks that actually hold such a buffer (Delay, Reverb, Chorus,
    // Doubler, Harmonizer, Autotune) need to override this.
    virtual void reset() {}
};

} // namespace ampforge
