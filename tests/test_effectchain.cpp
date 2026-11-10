#include "test_harness.hpp"

#include "EffectChain.hpp"

/*
 * EffectChain is what makes runtime pedal reordering possible, so these
 * tests pin down the three things it promises: blocks run in ascending
 * position order, disabled blocks are skipped entirely, and a block
 * coming back from disabled gets reset() before it processes again.
 */

namespace {

// Multiplicative and additive blocks are used together so that order is
// observable in the output: (x + 1) * 2 != x * 2 + 1.
class MultiplyBlock : public ampforge::AudioBlock
{
public:
    explicit MultiplyBlock(float factor, const char* blockName = "multiply")
        : gain(factor), name(blockName) {}

    void setSampleRate(double) override {}
    float processSample(float input) override { return input * gain; }
    const char* getName() const override { return name; }
    void reset() override { ++resetCount; }

    float gain;
    const char* name;
    int resetCount = 0;
};

class AddBlock : public ampforge::AudioBlock
{
public:
    explicit AddBlock(float offset, const char* blockName = "add")
        : amount(offset), name(blockName) {}

    void setSampleRate(double) override {}
    float processSample(float input) override { return input + amount; }
    const char* getName() const override { return name; }
    void reset() override { ++resetCount; }

    float amount;
    const char* name;
    int resetCount = 0;
};

} // namespace

AMPFORGE_TEST(effect_chain_runs_blocks_in_ascending_position_order)
{
    // Added out of order on purpose: position, not insertion order,
    // decides what runs first.
    AddBlock addOne(1.0f);
    MultiplyBlock doubleIt(2.0f);

    ampforge::EffectChain chain;
    chain.addBlock(&doubleIt, 1); // second
    chain.addBlock(&addOne, 0);   // first
    chain.rebuildOrder();

    // (x + 1) * 2
    CHECK_NEAR(chain.processSample(3.0f), 8.0f, 1e-6f);
    CHECK_NEAR(chain.processSample(0.0f), 2.0f, 1e-6f);
}

AMPFORGE_TEST(effect_chain_position_changes_take_effect)
{
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);

    ampforge::EffectChain chain;
    chain.addBlock(&doubleIt, 0);
    chain.addBlock(&addOne, 1);
    chain.rebuildOrder();

    // doubleIt then addOne: x * 2 + 1
    CHECK_NEAR(chain.processSample(3.0f), 7.0f, 1e-6f);

    // Swapped: addOne then doubleIt: (x + 1) * 2
    chain.setPosition(&doubleIt, 1);
    chain.setPosition(&addOne, 0);
    CHECK_NEAR(chain.processSample(3.0f), 8.0f, 1e-6f);
}

AMPFORGE_TEST(effect_chain_skips_disabled_blocks)
{
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);

    ampforge::EffectChain chain;
    chain.addBlock(&doubleIt, 0);
    chain.addBlock(&addOne, 1);
    chain.rebuildOrder();

    chain.setEnabled(&doubleIt, false);
    CHECK_NEAR(chain.processSample(3.0f), 4.0f, 1e-6f); // only +1 applies

    chain.setEnabled(&doubleIt, true);
    CHECK_NEAR(chain.processSample(3.0f), 7.0f, 1e-6f);
}

AMPFORGE_TEST(effect_chain_resets_a_block_when_it_is_reenabled)
{
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);

    ampforge::EffectChain chain;
    chain.addBlock(&doubleIt, 0);
    chain.addBlock(&addOne, 1);
    chain.rebuildOrder();

    CHECK(doubleIt.resetCount == 0);

    chain.setEnabled(&doubleIt, false);
    CHECK(doubleIt.resetCount == 0); // disabling must not reset

    chain.setEnabled(&doubleIt, true);
    CHECK(doubleIt.resetCount == 1); // coming back from disabled must reset

    // Enabling an already-enabled block is a no-op, not another reset.
    chain.setEnabled(&doubleIt, true);
    CHECK(doubleIt.resetCount == 1);
}

AMPFORGE_TEST(effect_chain_ignores_blocks_that_were_never_added)
{
    // setEnabled()/setPosition() take a raw pointer, so a block that was
    // never added must simply find no slot rather than corrupt anything.
    MultiplyBlock doubleIt(2.0f);
    MultiplyBlock stranger(3.0f);

    ampforge::EffectChain chain;
    chain.addBlock(&doubleIt, 0);
    chain.rebuildOrder();

    chain.setEnabled(&stranger, false);
    chain.setPosition(&stranger, 5);

    CHECK_NEAR(chain.processSample(1.0f), 2.0f, 1e-6f);
}

/*
 * Block processing (EffectChain::processBlock()).
 *
 * The promise being tested is that the block path is the *same* chain, not
 * a second, subtly different one: same blocks, same order, same skipping of
 * disabled slots, same result. So most of these tests build the reference
 * output by calling processSample() in a loop and then require the block
 * path to match it bit-exactly - the comparison is `==` rather than
 * CHECK_NEAR on purpose, because the whole point is that existing blocks
 * (which only implement processSample()) go through AudioBlock's default
 * processBlock() loop and must not drift by even one ULP.
 *
 * The blocks used here are stateless (gain / offset), which is what makes
 * it legitimate to run both paths on the same chain object and compare:
 * nothing carries over from one call to the next.
 */

namespace {

// A ramp is enough to make every frame produce a different value, so an
// off-by-one in the buffer offsets can't hide behind repeated samples.
std::vector<float> makeRamp(std::size_t frames, float start, float step)
{
    std::vector<float> buffer(frames);
    for (std::size_t i = 0; i < frames; ++i)
        buffer[i] = start + step * static_cast<float>(i);
    return buffer;
}

// The reference: what the chain has always produced, one sample at a time.
std::vector<float> runPerSample(ampforge::EffectChain& chain, const std::vector<float>& input)
{
    std::vector<float> output(input.size());
    for (std::size_t i = 0; i < input.size(); ++i)
        output[i] = chain.processSample(input[i]);
    return output;
}

/*
 * Wraps another block and records how the chain drove it, so a test can
 * observe the two things the output samples alone cannot show:
 *
 *  - perSampleCalls / blockCallFrames: which path the chain picked. When
 *    processBlock() has to fall back to the per-sample loop (frames bigger
 *    than the scratch buffers), the inner block's processBlock() must
 *    never be reached - and for a stateless gain block the output would
 *    come out right either way, so this is the only way to see it.
 *
 *  - aliasedCalls: whether the chain handed the block the same buffer for
 *    input and output. Not a nicety - in-place *is* allowed (see
 *    AudioBlock::processBlock()'s contract: "in and out may be the same
 *    buffer", because the host calls us in place), so the chain aliases them
 *    on purpose when there is exactly one enabled block, and that has to keep
 *    working. Recording it is how a test can tell *which* of the two shapes
 *    the chain chose for a given case: the gain/offset blocks in this file are
 *    stateless and derive each output sample from the same input sample, so
 *    they can't tell two scratch buffers from one shared buffer, and a chain
 *    that started aliasing when it must not (two enabled blocks, i.e. a
 *    block reading an intermediate another block already wrote) would pass
 *    every comparison against processSample() and still break.
 */
class ObservedBlock : public ampforge::AudioBlock
{
public:
    explicit ObservedBlock(ampforge::AudioBlock& innerBlock)
        : inner(innerBlock) {}

    void setSampleRate(double sampleRate) override { inner.setSampleRate(sampleRate); }

    float processSample(float input) override
    {
        ++perSampleCalls;
        return inner.processSample(input);
    }

    const char* getName() const override { return inner.getName(); }
    void reset() override { inner.reset(); }

    void processBlock(const float* in, float* out, uint32_t frames) override
    {
        blockCallFrames.push_back(frames);
        if (in == out)
            ++aliasedCalls;
        inner.processBlock(in, out, frames);
    }

    void clearObservations()
    {
        perSampleCalls = 0;
        aliasedCalls = 0;
        blockCallFrames.clear();
    }

    ampforge::AudioBlock& inner;
    int perSampleCalls = 0;
    int aliasedCalls = 0;
    std::vector<uint32_t> blockCallFrames;
};

// Three enabled slots of alternating kind: a gain, an offset, and a gain
// again, so a mistake in how results are handed from one slot to the next
// shows up (getting the ping-pong buffers backwards, or dropping the last
// block's output, changes the numbers).
void addThreeGainAndOffsetBlocks(ampforge::EffectChain& chain,
                                 MultiplyBlock& doubleIt,
                                 AddBlock& addOne,
                                 MultiplyBlock& halveIt)
{
    chain.addBlock(&doubleIt, 0);
    chain.addBlock(&addOne, 1);
    chain.addBlock(&halveIt, 2);
    chain.rebuildOrder();
}

} // namespace

AMPFORGE_TEST(effect_chain_process_block_matches_per_sample)
{
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);
    MultiplyBlock halveIt(0.5f);

    ampforge::EffectChain chain;
    addThreeGainAndOffsetBlocks(chain, doubleIt, addOne, halveIt);

    const std::vector<float> input = makeRamp(64, -1.0f, 0.03125f);
    const std::vector<float> expected = runPerSample(chain, input);

    // 64 frames, so the block path has to bounce through the scratch
    // buffers twice and still land the final result in `out`.
    chain.setMaxBlockFrames(64);
    std::vector<float> output(input.size(), 0.0f);
    chain.processBlock(input.data(), output.data(), static_cast<uint32_t>(input.size()));

    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(output[i] == expected[i]);
}

AMPFORGE_TEST(effect_chain_process_block_skips_disabled_blocks)
{
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);
    MultiplyBlock halveIt(0.5f);

    ampforge::EffectChain chain;
    addThreeGainAndOffsetBlocks(chain, doubleIt, addOne, halveIt);
    chain.setMaxBlockFrames(32);

    const std::vector<float> input = makeRamp(32, 0.0f, 0.0625f);

    chain.setEnabled(&addOne, false);
    const std::vector<float> expected = runPerSample(chain, input); // x * 2 * 0.5 == x

    std::vector<float> output(input.size(), 0.0f);
    chain.processBlock(input.data(), output.data(), static_cast<uint32_t>(input.size()));

    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(output[i] == expected[i]);
}

AMPFORGE_TEST(effect_chain_process_block_works_in_place)
{
    // Real hosts hand us the same pointer for in and out. The intermediates
    // live in the chain's scratch buffers, so `out` (== `in` here) is only
    // written once every read of the input has already happened - but only
    // if processBlock() actually sequences it that way, which is the thing
    // under test.
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);
    MultiplyBlock halveIt(0.5f);

    ampforge::EffectChain chain;
    addThreeGainAndOffsetBlocks(chain, doubleIt, addOne, halveIt);
    chain.setMaxBlockFrames(48);

    const std::vector<float> input = makeRamp(48, -0.75f, 0.03125f);
    const std::vector<float> expected = runPerSample(chain, input);

    std::vector<float> buffer = input; // deliberately the same vector for in and out
    chain.processBlock(buffer.data(), buffer.data(), static_cast<uint32_t>(buffer.size()));

    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(buffer[i] == expected[i]);
}

AMPFORGE_TEST(effect_chain_process_block_falls_back_when_larger_than_max_block_frames)
{
    // The host sent more frames than setMaxBlockFrames() prepared for (or
    // never called it). The chain must not allocate or run off the end of
    // its scratch buffers - it processes the call per sample instead, which
    // is correct (just slower) and needs no scratch at all.
    //
    // Checking the numbers alone would be too weak here: an implementation
    // that ignores maxBlockFrames and processes the block anyway would
    // still come up with the right samples, it would just corrupt memory
    // doing it. So a recording block reports *which* path was taken - the
    // per-sample loop means processBlock() on that block is never reached.
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);
    MultiplyBlock halveIt(0.5f);
    ObservedBlock observedHalve(halveIt);

    ampforge::EffectChain chain;
    chain.addBlock(&doubleIt, 0);
    chain.addBlock(&addOne, 1);
    chain.addBlock(&observedHalve, 2);
    chain.rebuildOrder();
    chain.setMaxBlockFrames(16);

    const std::vector<float> input = makeRamp(256, -1.0f, 0.0078125f);
    const std::vector<float> expected = runPerSample(chain, input);

    // runPerSample() above already drove the observed block per sample, so
    // start from a clean slate.
    observedHalve.clearObservations();

    std::vector<float> output(input.size(), 0.0f);
    chain.processBlock(input.data(), output.data(), static_cast<uint32_t>(input.size()));

    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(output[i] == expected[i]);

    CHECK(observedHalve.blockCallFrames.empty()); // never got a block: fell back
    CHECK(observedHalve.perSampleCalls == static_cast<int>(input.size()));

    // Sized for the same block, the block path is taken instead - otherwise
    // the assertions above would also hold for a chain that always fell
    // back, i.e. one where block processing never works at all.
    chain.setMaxBlockFrames(256);
    observedHalve.clearObservations();
    chain.processBlock(input.data(), output.data(), static_cast<uint32_t>(input.size()));

    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(output[i] == expected[i]);
    CHECK(observedHalve.blockCallFrames.size() == 1);
    CHECK(observedHalve.blockCallFrames[0] == 256);
    CHECK(observedHalve.perSampleCalls == 0);
}

AMPFORGE_TEST(effect_chain_process_block_never_aliases_a_blocks_input_and_output)
{
    // The middle block is the interesting one: the first block reads the
    // host's buffer and the last one writes `out`, so only an intermediate
    // step can end up being handed the buffer it just wrote itself.
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);
    MultiplyBlock halveIt(0.5f);
    ObservedBlock observedAdd(addOne);

    ampforge::EffectChain chain;
    chain.addBlock(&doubleIt, 0);
    chain.addBlock(&observedAdd, 1);
    chain.addBlock(&halveIt, 2);
    chain.rebuildOrder();
    chain.setMaxBlockFrames(64);

    const std::vector<float> input = makeRamp(64, -1.0f, 0.03125f);
    const std::vector<float> expected = runPerSample(chain, input);
    observedAdd.clearObservations();

    // Separate in and out buffers: no block in the chain may be handed the
    // same buffer for both, because the intermediates are supposed to live
    // in the chain's scratch buffers and be handed over as distinct
    // buffers - a block doing whole-buffer work is not obliged to be
    // in-place safe.
    std::vector<float> output(input.size(), 0.0f);
    chain.processBlock(input.data(), output.data(), static_cast<uint32_t>(input.size()));

    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(output[i] == expected[i]);
    CHECK(observedAdd.blockCallFrames.size() == 1);
    CHECK(observedAdd.aliasedCalls == 0);

    // An in-place call from the host (in == out) is a different matter: the
    // chain may not read `in` after writing `out`, so with a single enabled
    // block it can pass the host's already-aliased pair straight through -
    // that is exactly what processBlock()'s contract allows ("in and out
    // may be the same buffer"), and it is why the one-block case costs no
    // copy at all.
    ampforge::EffectChain single;
    single.addBlock(&observedAdd, 0);
    single.rebuildOrder();
    single.setMaxBlockFrames(64);
    observedAdd.clearObservations();

    std::vector<float> inPlace = input;
    single.processBlock(inPlace.data(), inPlace.data(), static_cast<uint32_t>(inPlace.size()));
    CHECK(observedAdd.blockCallFrames.size() == 1); // one call, not a per-sample fallback
    CHECK(observedAdd.aliasedCalls == 1);           // and no pointless scratch detour

    // ... while a multi-block in-place call must not alias, because the
    // intermediates in between still need the input.
    std::vector<float> inPlaceChain = input;
    observedAdd.clearObservations();
    chain.processBlock(inPlaceChain.data(), inPlaceChain.data(),
                       static_cast<uint32_t>(inPlaceChain.size()));

    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(inPlaceChain[i] == expected[i]);
    CHECK(observedAdd.aliasedCalls == 0);
}

AMPFORGE_TEST(effect_chain_process_block_with_zero_frames_touches_nothing)
{
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);
    ObservedBlock observedAdd(addOne);

    ampforge::EffectChain chain;
    chain.addBlock(&doubleIt, 0);
    chain.addBlock(&observedAdd, 1);
    chain.rebuildOrder();
    chain.setMaxBlockFrames(64);

    std::vector<float> input = makeRamp(8, 1.0f, 1.0f);
    const std::vector<float> inputBefore = input;

    // A sentinel in every slot: an empty block must leave it untouched
    // rather than, say, falling through into the block path and running the
    // chain over zero frames - which would still copy `in` and so would
    // look harmless for this buffer, but only because the input happens to
    // be a valid 8-frame ramp. Running the blocks at all on an empty buffer
    // is what must not happen (their per-sample paths never run, so nothing
    // is advanced, but the copy into `out` would still clobber it), so the
    // sentinel plus the observed block's call count together pin it down.
    observedAdd.clearObservations();

    std::vector<float> output(8, -123.0f);
    chain.processBlock(input.data(), output.data(), 0);

    for (std::size_t i = 0; i < output.size(); ++i)
        CHECK(output[i] == -123.0f);
    CHECK(observedAdd.blockCallFrames.empty()); // no block was asked to run
    CHECK(observedAdd.perSampleCalls == 0);

    // And the input must be left alone too, obviously - checked so a
    // future "write in place then copy" rewrite can't quietly break it.
    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(input[i] == inputBefore[i]);
}

AMPFORGE_TEST(effect_chain_process_block_copies_input_when_no_blocks_are_enabled)
{
    MultiplyBlock doubleIt(2.0f);
    AddBlock addOne(1.0f);

    ampforge::EffectChain chain;
    chain.addBlock(&doubleIt, 0);
    chain.addBlock(&addOne, 1);
    chain.rebuildOrder();
    chain.setMaxBlockFrames(64);
    chain.setEnabled(&doubleIt, false);
    chain.setEnabled(&addOne, false);

    const std::vector<float> input = makeRamp(64, -1.0f, 0.03125f);

    std::vector<float> output(input.size(), 0.0f);
    chain.processBlock(input.data(), output.data(), static_cast<uint32_t>(input.size()));

    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(output[i] == input[i]);

    // Same thing with in == out: the copy must not corrupt the signal
    // (std::copy with overlapping, identical source and destination).
    std::vector<float> inPlace = input;
    chain.processBlock(inPlace.data(), inPlace.data(), static_cast<uint32_t>(inPlace.size()));

    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK(inPlace[i] == input[i]);
}
