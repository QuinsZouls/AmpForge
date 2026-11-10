#include "test_harness.hpp"
#include "host/Lv2Plugin.hpp"
#include "PluginNodeBlock.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#ifndef TEST_LV2_CHUNK_BUNDLE_DIR
# define TEST_LV2_CHUNK_BUNDLE_DIR "./test_chunk.lv2/"
#endif

// Regression fixture for the LV2 host behaviours the "test-chunk" plugin
// enforces (see tests/test_lv2_chunk/chunk.c):
//
//  1. instantiate() gets real features (urid:map + opts:options carrying
//     buf-size#maxBlockLength as atom:Int). The plugin returns NULL without
//     them, so the assertion is `CHECK(lv2->activate(...))` - lilv's instantiate
//     runs there, not in load() (which only resolves the descriptor).
//  2. run() is never handed more than Lv2Plugin::kMaxRunFrames frames. The
//     plugin processes only that many and zeroes the rest, so any sample the
//     host fails to chunk down and re-feed shows up as a mismatch below.
//  3. The worker extension (work:schedule + LV2_Worker_Interface) is wired: the
//     fixture schedules one message from run(), answers it from work() and
//     checks the answer in work_response(). Nothing else in the suite can see
//     that wiring - without these tests, deleting worker.start(),
//     worker.endCycle() or worker.stop() from Lv2Plugin leaves the whole suite
//     green, and one of those is a use-after-free in production.
namespace {

constexpr const char* kChunkUri = "http://ampforge.org/plugins/test-chunk";

// Sentinel written into the output before processing: distinguishes "the host
// never wrote this sample" from a genuine (wrong) value of 0.
constexpr float kUnwritten = -12345.0f;

// Deterministic ramp: no RNG, so a failing sample index always reproduces.
void fillRamp(std::vector<float>& buffer) {
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = 0.5f * std::sin(static_cast<float>(i) * 0.01f);
    }
}

std::unique_ptr<ampforge::Lv2Plugin> loadChunkPlugin() {
    auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
    if (!lv2->load(kChunkUri, TEST_LV2_CHUNK_BUNDLE_DIR)) return nullptr;
    return lv2;
}

// Bound on the worker-wait loops below. Lv2Worker polls its request ring every
// 2 ms (Lv2Worker::kPollMillis) and a response is only delivered by a *later*
// processBlock, so a round trip needs a handful of blocks with real time between
// them. 20 blocks x 5 ms is an order of magnitude more than that, but bounded on
// purpose: a host that never starts its worker has to fail the CHECKs, never
// hang the suite.
constexpr int kMaxWorkerBlocks = 20;
constexpr auto kWorkerBlockPeriod = std::chrono::milliseconds(5);

// Run one block of the fixture through `plugin`, asserting the audio path is
// untouched by whatever the worker is doing. The fixture outputs exactly
// `in * gain` (gain defaults to 1.0) unless the worker round trip went wrong, in
// which case it outputs silence - so this doubles as the assertion that the host
// delivered the plugin's *own* answer rather than a fabricated one.
void processChunkBlockAndCheck(ampforge::Lv2Plugin& plugin, uint32_t frames) {
    std::vector<float> in(frames), out(frames, kUnwritten);
    fillRamp(in);

    plugin.processBlock(in.data(), out.data(), frames);

    for (uint32_t i = 0; i < frames; ++i) {
        CHECK_NEAR(out[i], in[i], 1e-6f);
    }
}

} // namespace

AMPFORGE_TEST(lv2_host_provides_features_and_chunks_run)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
    // load() only resolves the descriptor; it succeeds with or without features.
    const bool loaded = lv2->load(kChunkUri, TEST_LV2_CHUNK_BUNDLE_DIR);
    CHECK(loaded);
    if (!loaded) return;

    CHECK(lv2->getName() == "AmpForge Test Chunk");
    CHECK(lv2->getParameterCount() == 1);
    CHECK(lv2->getParameterInfo(0).symbol == "gain");

    // The feature assertion: instantiate() runs here and refuses to return an
    // instance unless the host passed urid:map *and* opts:options with a usable
    // buf-size#maxBlockLength (atom:Int). A host that hands over nullptr/short
    // feature lists - as AmpForge did - fails right here.
    //
    // Max block 2048 while the plugin only takes maxBlockLength (512) frames per
    // run() call: the host has to chunk. 2048 is also the largest block below.
    CHECK(lv2->activate(48000.0, 2048));

    // Pure pass-through (gain defaults to 1.0): every sample must come back
    // untouched. Frames beyond the first 512 only survive if the host chunks
    // run() - the plugin writes zeros for them otherwise.
    const uint32_t frameCounts[] = { 100, 512, 1000, 2048 };
    for (uint32_t frames : frameCounts) {
        std::vector<float> in(frames), out(frames, kUnwritten);
        fillRamp(in);

        lv2->processBlock(in.data(), out.data(), frames);

        for (uint32_t i = 0; i < frames; ++i) {
            CHECK_NEAR(out[i], in[i], 1e-6f);
        }
    }

    // Gain 2.0 proves the chunks really went through the plugin's own DSP
    // instead of some host-side copy of the input: out == 2 * in only if every
    // chunk was multiplied.
    lv2->setParameterValue(0, 2.0f);
    CHECK_NEAR(lv2->getParameterValue(0), 2.0f, 1e-5f);

    const uint32_t frames = 1024;
    std::vector<float> in(frames), out(frames, kUnwritten);
    fillRamp(in);
    lv2->processBlock(in.data(), out.data(), frames);

    for (uint32_t i = 0; i < frames; ++i) {
        CHECK_NEAR(out[i], 2.0f * in[i], 1e-6f);
    }
}

AMPFORGE_TEST(plugin_node_end_to_end_feeds_whole_block)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = loadChunkPlugin();
    CHECK(lv2 != nullptr);
    if (!lv2) return;

    // The user-visible path: the "Plugin" node in the pedalboard. Mix defaults
    // to 1.0 and both trims to 0 dB, so at gain 1.0 the node is a pass-through
    // and anything but the input coming out is a host bug - this is the case
    // that used to output half silence.
    ampforge::PluginNodeBlock node;
    node.setSampleRate(48000.0);
    CHECK(node.setPlugin(std::move(lv2)));

    const uint32_t frames = 1024;
    std::vector<float> in(frames), out(frames, kUnwritten);

    // A few blocks in a row: the node reuses its scratch buffers between calls,
    // so a partially written chunk would also show up as stale data here.
    for (int block = 0; block < 4; ++block) {
        fillRamp(in);
        std::fill(out.begin(), out.end(), kUnwritten);

        node.processBlock(in.data(), out.data(), frames);

        for (uint32_t i = 0; i < frames; ++i) {
            CHECK_NEAR(out[i], in[i], 1e-5f);
        }
    }
}
// Regression for the two host behaviours the chunked run() rewrite changed and
// that `lv2_host_provides_features_and_chunks_run` above cannot see, because it
// always activates with the biggest block it later processes (2048):
//
//  1. processBlock() now loops over *all* the frames it was given. It used to
//     compute `processFrames = min(frames, activeMaxBlock)` and work on that
//     only, so a host block bigger than the activated max block left the tail of
//     the output completely untouched: the caller got whatever its buffer
//     already held past activeMaxBlock. Activating with a small block and then
//     passing four times as many frames is the only way to reach that path.
//  2. activate() after deactivate() rebuilds the LV2 feature/option storage.
//     clearHostFeatures() empties `options`/`features` and resets
//     optionsFeature.data to nullptr on the way out of deactivate(), so a second
//     instance only gets a usable urid:map + opts:options + boundedBlockLength
//     list if activate() calls buildHostFeatures() again - and the fixture
//     refuses to instantiate otherwise, which makes a missing rebuild a plain
//     `CHECK(lv2->activate(...))` failure.
AMPFORGE_TEST(lv2_host_processes_more_frames_than_active_max_block)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = loadChunkPlugin();
    CHECK(lv2 != nullptr);
    if (!lv2) return;

    // Small activated block (512 == Lv2Plugin::kMaxRunFrames here). The host
    // advertises maxBlockLength 512 to the plugin, so run() may never see more,
    // but processBlock() must still cover every frame the caller handed it.
    CHECK(lv2->activate(48000.0, 512));

    const uint32_t frames = 2048;  // 4x activeMaxBlock - the old code stopped at 512.
    std::vector<float> in(frames), out(frames, kUnwritten);
    fillRamp(in);

    lv2->processBlock(in.data(), out.data(), frames);

    // Pass-through (gain defaults to 1.0). Frames past 512 stay at the sentinel
    // if the host truncates instead of looping, so this assertion pins the loop.
    for (uint32_t i = 0; i < frames; ++i) {
        CHECK_NEAR(out[i], in[i], 1e-6f);
    }

    // Deactivate/re-activate with a bigger max block: deactivate() releases the
    // feature/option arrays, and this second instance is the one that would see
    // a dangling or empty option pointer if the host failed to rebuild them.
    lv2->deactivate();
    CHECK(lv2->activate(48000.0, 2048));

    std::fill(out.begin(), out.end(), kUnwritten);
    fillRamp(in);

    lv2->processBlock(in.data(), out.data(), frames);

    for (uint32_t i = 0; i < frames; ++i) {
        CHECK_NEAR(out[i], in[i], 1e-6f);
    }
}

// The worker round trip actually runs.
//
// The fixture's run() schedules exactly one message (carrying 40) on its first
// block, its work() - called by the host's *background* thread - replies with 41
// through the respond callback, and its work_response() only accepts that 41.
// The two counters below are what make the host's wiring observable:
//
//  * workerRequestsScheduled() >= 1 fails unless schedule() reached the driver,
//    i.e. unless the work:schedule feature was handed to the plugin *and* the
//    audio-thread path into it works. It also fails if worker.start() is never
//    called: schedule() before start() is refused ("nothing will ever run this"),
//    so nothing is counted.
//  * workerResponsesDelivered() >= 1 fails unless a *later* processBlock pumped
//    the response ring (worker.endCycle()), i.e. unless the host actually closes
//    its cycles. Starting the worker without ever pumping it leaves the reply
//    stuck in the ring exactly the same way.
//
// The audio assertions ride along for free: the fixture mutes whenever the round
// trip goes wrong, so "still exactly in * gain" is also "the host did not
// fabricate or mangle the reply".
AMPFORGE_TEST(lv2_chunk_host_worker_round_trip)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = loadChunkPlugin();
    CHECK(lv2 != nullptr);
    if (!lv2) return;

    CHECK(lv2->activate(48000.0, 1024));

    // Nothing has run yet, so nothing can have been scheduled: the counters start
    // at zero per worker.start() (i.e. per activate()).
    CHECK(lv2->workerRequestsScheduled() == 0);
    CHECK(lv2->workerResponsesDelivered() == 0);

    constexpr uint32_t kFrames = 256;

    bool roundTripped = false;
    for (int block = 0; block < kMaxWorkerBlocks; ++block) {
        processChunkBlockAndCheck(*lv2, kFrames);

        if (lv2->workerResponsesDelivered() >= 1) {
            roundTripped = true;
            break;
        }
        // Real time, not a spin: the worker only drains its request ring on its
        // 2 ms timer, and the response is only handed to the plugin by the next
        // processBlock's endCycle().
        std::this_thread::sleep_for(kWorkerBlockPeriod);
    }

    CHECK(roundTripped);
    CHECK(lv2->workerRequestsScheduled() >= 1);
    CHECK(lv2->workerResponsesDelivered() >= 1);
    // The fixture answers once and is never asked again (one schedule per
    // instance), so the delivered count can only ever be the one reply.
    CHECK(lv2->workerResponsesDelivered() <= lv2->workerRequestsScheduled());
}

// The worker counters stay consistent with each other.
//
// This used to be called ..._round_trip_is_not_fabricated, which over-claimed:
// the fixture always answers through the respond() callback, so restoring
// Lv2Worker's old "echo the request back" default does NOT fail anything here
// (mutation-proven). What this test really pins is the arithmetic between the two
// counters - never more responses delivered than requests scheduled, and no work()
// call refused on the way - while a real round trip runs.
//
// The "the host never invents a reply" rule is pinned by
// lv2_worker_no_response_by_default in tests/test_lv2_worker.cpp, which drives
// Lv2Worker directly with no respond() and asserts that nothing is delivered.
AMPFORGE_TEST(lv2_chunk_host_worker_counts_are_consistent)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = loadChunkPlugin();
    CHECK(lv2 != nullptr);
    if (!lv2) return;

    CHECK(lv2->activate(48000.0, 1024));

    constexpr uint32_t kFrames = 256;

    bool sawResponse = false;
    for (int block = 0; block < kMaxWorkerBlocks; ++block) {
        processChunkBlockAndCheck(*lv2, kFrames);

        // Checked after every block, not only at the end: "more responses than
        // requests" can only ever be a transient state, so a final-counters-only
        // assertion would miss it.
        CHECK(lv2->workerResponsesDelivered() <= lv2->workerRequestsScheduled());

        if (lv2->workerResponsesDelivered() >= 1) {
            sawResponse = true;
            break;
        }
        std::this_thread::sleep_for(kWorkerBlockPeriod);
    }

    CHECK(sawResponse);
    CHECK(lv2->workerRequestsScheduled() >= 1);
    // No refused work() call: the bytes reached the worker thread intact, which
    // is what makes the counted round trip a real one instead of an error path
    // that happens not to crash.
    CHECK(lv2->workerFailedWork() == 0);
}

// Deactivating with work still pending must be safe - and must leave the plugin
// re-activatable.
//
// This is the behavioural pin for the stop()-before-instance_free() ordering in
// Lv2Plugin::deactivate(): the block below queues a message, and the worker
// thread is polling every 2 ms, so at the moment deactivate() runs its work() call
// is either in flight or about to start. Without worker.stop() there,
// instance_free() frees the plugin while work() is about to run on it - a
// use-after-free that neither crashes reliably nor fails any other test. The
// assertion is therefore "no crash, no hang, and the next instance still works",
// including its own fresh round trip.
AMPFORGE_TEST(lv2_chunk_host_deactivate_with_work_pending)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = loadChunkPlugin();
    CHECK(lv2 != nullptr);
    if (!lv2) return;

    CHECK(lv2->activate(48000.0, 1024));

    // Exactly one block: the fixture schedules its message from inside run() and
    // processBlock() returns immediately afterwards, well inside the 2 ms the
    // worker sleeps between drains.
    constexpr uint32_t kFrames = 256;
    {
        std::vector<float> in(kFrames), out(kFrames, kUnwritten);
        fillRamp(in);
        lv2->processBlock(in.data(), out.data(), kFrames);
        for (uint32_t i = 0; i < kFrames; ++i) {
            CHECK_NEAR(out[i], in[i], 1e-6f);
        }
    }
    CHECK(lv2->workerRequestsScheduled() == 1);

    lv2->deactivate();

    // Second instance: deactivate() released the feature/option storage and the
    // worker, so activate() has to rebuild all of it - and the fresh instance
    // schedules its own message, which has to come back through the fresh
    // worker thread.
    CHECK(lv2->activate(48000.0, 1024));

    bool roundTripped = false;
    for (int block = 0; block < kMaxWorkerBlocks; ++block) {
        processChunkBlockAndCheck(*lv2, kFrames);

        if (lv2->workerResponsesDelivered() >= 1) {
            roundTripped = true;
            break;
        }
        std::this_thread::sleep_for(kWorkerBlockPeriod);
    }

    CHECK(roundTripped);
    CHECK(lv2->workerResponsesDelivered() <= lv2->workerRequestsScheduled());
}
