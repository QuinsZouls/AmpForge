#include "test_harness.hpp"

#include "EffectChain.hpp"
#include "ScreamerBlock.hpp"
#include "NoiseGateBlock.hpp"
#include <cmath>

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

AMPFORGE_TEST(effect_chain_process_block_matches_process_sample_bit_for_bit)
{
    MultiplyBlock doubleIt1(2.0f);
    AddBlock addOne1(1.0f);

    ampforge::EffectChain chain1;
    chain1.addBlock(&doubleIt1, 0);
    chain1.addBlock(&addOne1, 1);
    chain1.rebuildOrder();

    MultiplyBlock doubleIt2(2.0f);
    AddBlock addOne2(1.0f);

    ampforge::EffectChain chain2;
    chain2.addBlock(&doubleIt2, 0);
    chain2.addBlock(&addOne2, 1);
    chain2.rebuildOrder();

    constexpr uint32_t kFrames = 256;
    float inBuf[kFrames];
    float sampleOut[kFrames];
    float blockOut[kFrames];

    for (uint32_t i = 0; i < kFrames; ++i)
    {
        inBuf[i] = std::sin(static_cast<float>(i) * 0.1f);
        sampleOut[i] = chain1.processSample(inBuf[i]);
        blockOut[i] = inBuf[i];
    }

    chain2.processBlock(blockOut, kFrames);

    for (uint32_t i = 0; i < kFrames; ++i)
    {
        CHECK(sampleOut[i] == blockOut[i]);
    }
}

AMPFORGE_TEST(effect_chain_stateful_dsp_process_block_matches_process_sample)
{
    ampforge::NoiseGateBlock gate1;
    ampforge::ScreamerBlock screamer1;
    gate1.setSampleRate(44100.0);
    screamer1.setSampleRate(44100.0);

    ampforge::EffectChain chain1;
    chain1.addBlock(&gate1, 0);
    chain1.addBlock(&screamer1, 1);
    chain1.rebuildOrder();

    ampforge::NoiseGateBlock gate2;
    ampforge::ScreamerBlock screamer2;
    gate2.setSampleRate(44100.0);
    screamer2.setSampleRate(44100.0);

    ampforge::EffectChain chain2;
    chain2.addBlock(&gate2, 0);
    chain2.addBlock(&screamer2, 1);
    chain2.rebuildOrder();

    constexpr uint32_t kFrames = 512;
    float inBuf[kFrames];
    float sampleOut[kFrames];
    float blockOut[kFrames];

    for (uint32_t i = 0; i < kFrames; ++i)
    {
        inBuf[i] = 0.5f * std::sin(static_cast<float>(i) * 0.05f);
        sampleOut[i] = chain1.processSample(inBuf[i]);
        blockOut[i] = inBuf[i];
    }

    chain2.processBlock(blockOut, kFrames);

    for (uint32_t i = 0; i < kFrames; ++i)
    {
        // Must be bit-for-bit identical
        CHECK(sampleOut[i] == blockOut[i]);
    }
}
