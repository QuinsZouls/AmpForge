#pragma once
#include "AudioBlock.hpp"
#include <atomic>
#include <vector>
#include <algorithm>

/*
 * EffectChain - manages a set of AudioBlocks and the order they should
 * be processed in. Each block occupies a "slot": it can be enabled or
 * disabled, and it has a position (0 = processed first, 1 = next, ...).
 *
 * This is what makes true reordering possible: instead of a hardcoded
 * "Screamer -> Amp -> Delay -> Reverb" sequence in code, the order is
 * data that can change at runtime - later driven by dragging blocks
 * around in the GUI, or by host automation.
 *
 * IMPORTANT: add all blocks with addBlock() first, then call
 * rebuildOrder() once before audio processing starts. After that,
 * setPosition()/setEnabled() are safe to call at any time (including
 * from the audio thread's parameter updates) because no more blocks
 * are being added - the underlying storage no longer resizes, so the
 * pointers we hand out stay valid.
 *
 * Audio can be pushed through in two ways: processSample(), one frame at
 * a time, or processBlock() for a whole host buffer at once (which also
 * needs setMaxBlockFrames() called during that same setup, before audio
 * starts - it allocates). Both run the same blocks in the same order and
 * produce the same result; the block path exists so blocks that are built
 * for block-at-a-time work (NamBlock) can actually use it.
 */

namespace ampforge {

struct ChainSlot
{
    AudioBlock* block = nullptr;
    bool enabled = true;
    int position = 0; // lower = processed earlier
};

class EffectChain
{
public:
    // Hard ceiling on the block-processing scratch, allocated once in the
    // constructor and never resized - same arrangement, and for the same
    // reason, as NamBlock's own scratch: a host can deliver a block-length
    // change while audio is rolling, and a buffer that is always large enough
    // means the audio thread never sees a `std::vector` grow (or, worse, free
    // the memory it is reading). 8192 frames is 32 KB per buffer and far beyond
    // any buffer a host hands a plugin; a larger one simply takes the
    // per-sample fallback below, exactly as it did when the scratch was
    // zero-sized.
    static constexpr uint32_t kMaxScratchFrames = 8192;

    EffectChain()
    {
        scratchA.resize(kMaxScratchFrames);
        scratchB.resize(kMaxScratchFrames);
    }

    // Call this for every block during setup, before rebuildOrder().
    void addBlock(AudioBlock* block, int initialPosition)
    {
        ChainSlot slot;
        slot.block = block;
        slot.position = initialPosition;
        slots.push_back(slot);
    }

    // Call once after all addBlock() calls, and again any time a
    // position changes.
    void rebuildOrder()
    {
        orderedSlots.clear();
        for (auto& slot : slots)
            orderedSlots.push_back(&slot);

        std::sort(orderedSlots.begin(), orderedSlots.end(),
                   [](const ChainSlot* a, const ChainSlot* b)
                   {
                       return a->position < b->position;
                   });
    }

    void setEnabled(AudioBlock* block, bool enabled)
    {
        for (auto& slot : slots)
        {
            if (slot.block == block)
            {
                // Coming back from disabled: clear out whatever sample
                // history the block was left holding (see AudioBlock::
                // reset()'s comment) before it starts processing audio
                // again, so a stale delay/reverb tail from however long
                // ago it was last on can't suddenly reappear now.
                if (enabled && !slot.enabled)
                    block->reset();
                slot.enabled = enabled;
                return;
            }
        }
    }

    void setPosition(AudioBlock* block, int position)
    {
        for (auto& slot : slots)
        {
            if (slot.block == block)
            {
                slot.position = position;
                rebuildOrder();
                return;
            }
        }
    }

    float processSample(float input)
    {
        float x = input;
        for (auto* slot : orderedSlots)
        {
            if (slot->enabled)
                x = slot->block->processSample(x);
        }
        return x;
    }

    // Sizes the two scratch buffers processBlock() ping-pongs through.
    // Call this before audio processing starts (from the same non-realtime
    // setup context as addBlock()/rebuildOrder()) with the largest block
    // size the host can hand us - typically the value the host passed to
    // prepareToPlay().
    //
    // Strictly speaking this allocates nothing and touches no buffer: the
    // scratch was allocated once in the constructor and is never resized, so
    // all this does is raise a cap (atomically, and only ever upwards - a later
    // call with a smaller or equal size is a no-op). That is deliberate, and it
    // is what lets it be safe to call from a host callback that runs while audio
    // is rolling (DPF's LV2 lv2_set_options(), VST2's effSetBlockSize): the
    // audio thread only ever reads this number and indexes buffers whose
    // address never changes, so the acquire/release below is ordering a plain
    // integer rather than pretending to publish a reallocation.
    //
    // Calling it during setup is still the intended use - it is the only way
    // the block path is ever taken.
    //
    // If the host ends up sending more frames than this anyway (buffer
    // size changed, or setMaxBlockFrames() was never called at all),
    // processBlock() doesn't allocate or overflow - it degrades to the
    // per-sample loop for that one call (see its comment).
    void setMaxBlockFrames(uint32_t frames)
    {
        // Clamped to what the fixed scratch holds: a host asking for more than
        // that gets the per-sample fallback in processBlock(), which is the
        // same answer it got when this was a resize.
        frames = std::min(frames, kMaxScratchFrames);
        if (frames <= maxBlockFrames.load(std::memory_order_acquire))
            return;

        maxBlockFrames.store(frames, std::memory_order_release);
    }

    // Block-based counterpart of processSample(): runs `frames` samples
    // through the enabled slots in order, once per block instead of once
    // per sample. The point is to let a block that is designed to work on
    // a whole buffer at a time - NamBlock's neural network is the one that
    // motivated this - do so; everything else just inherits
    // AudioBlock::processBlock()'s per-sample loop and keeps behaving
    // exactly as before. Unlike processSample(), `in` and `out` are
    // separate pointers and may be the same buffer (in-place call).
    //
    // How the buffers are used: the intermediates live in scratchA/
    // scratchB, and each step writes into the buffer the previous step
    // did *not* just read from, so no block ever reads and writes the same
    // buffer. The final step writes straight into `out`, which is why
    // there is no trailing copy in the common case: nothing after it can
    // need the intermediate value, so copying `out` over itself would be
    // pure waste (with exactly one enabled block, the one block simply
    // writes `out` directly). `out` - which may alias `in` - is therefore
    // only written on the last step, and `in` is only read by the first
    // one, so an in-place call is safe even though the chain needs
    // somewhere to put intermediates.
    //
    // Cases worth spelling out:
    //   * frames == 0                -> nothing is touched at all.
    //   * no enabled blocks          -> `in` is copied to `out` (this is
    //                                   the one case that does need a
    //                                   copy; skipping it would leave
    //                                   whatever stale audio the host's
    //                                   output buffer happened to hold).
    //   * frames > maxBlockFrames    -> the scratch buffers are too small.
    //                                   Resizing here would allocate on
    //                                   the audio thread and running the
    //                                   block path anyway would write past
    //                                   the end of them, so this call
    //                                   falls back to the per-sample loop.
    //                                   Same audible result, just slower -
    //                                   the safe answer to host
    //                                   misbehaviour (or to a missing
    //                                   setMaxBlockFrames() call).
    void processBlock(const float* in, float* out, uint32_t frames)
    {
        if (frames == 0)
            return;

        if (frames > maxBlockFrames.load(std::memory_order_acquire))
        {
            for (uint32_t i = 0; i < frames; ++i)
                out[i] = processSample(in[i]);
            return;
        }

        // Count the enabled slots first: the loop below needs to know which
        // step is the last one so it can write `out` directly instead of
        // bouncing through scratch and copying afterwards. Two passes over
        // a handful of pointers is free compared to the audio it processes.
        uint32_t enabledCount = 0;
        for (auto* slot : orderedSlots)
        {
            if (slot->enabled)
                ++enabledCount;
        }

        // Disabled slots are skipped exactly like processSample() does, so
        // toggling a pedal sounds the same whichever path the host drives.
        if (enabledCount == 0)
        {
            std::copy(in, in + frames, out);
            return;
        }

        float* const scratch[2] = { scratchA.data(), scratchB.data() };
        const float* src = in;
        uint32_t step = 0;
        uint32_t remaining = enabledCount;
        for (auto* slot : orderedSlots)
        {
            if (!slot->enabled)
                continue;

            --remaining;
            float* const dst = (remaining == 0) ? out : scratch[step % 2];
            slot->block->processBlock(src, dst, frames);

            src = dst;
            ++step;
        }
    }

private:
    std::vector<ChainSlot> slots;
    std::vector<ChainSlot*> orderedSlots;

    // Block-processing scratch: allocated once at kMaxScratchFrames in the
    // constructor and never resized, so both pointers stay valid for the
    // lifetime of the chain. maxBlockFrames only says how much of them the
    // host's buffer may fill, and it is atomic because setMaxBlockFrames()
    // can be reached from a host callback that runs while audio does (see its
    // comment).
    //
    // Note this makes EffectChain non-copyable and non-movable (a std::atomic
    // member deletes both) - nothing in the project copies one, and it owns
    // scratch memory plus raw block pointers, so copying was never meaningful.
    std::vector<float> scratchA;
    std::vector<float> scratchB;
    std::atomic<uint32_t> maxBlockFrames{0};
};

} // namespace ampforge
