#include "test_harness.hpp"
#include "host/Lv2Plugin.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
# include <malloc.h>  // _aligned_malloc / _aligned_free
#endif

#ifndef TEST_LV2_ATOM_BUNDLE_DIR
# define TEST_LV2_ATOM_BUNDLE_DIR "./test_atom.lv2/"
#endif

// Real-time safety regression tests for Lv2Plugin::processBlock().
//
// The bug these pin: processBlock() runs on the JACK audio thread, and it used
// to call mapUrid() 13 times per block - each call takes the process-wide URID
// map mutex and builds a temporary std::string, i.e. a heap allocation - and to
// build a fresh std::vector per atom event in the event paths. On a busy machine
// the mutex plus the allocator traffic pushed the block past its deadline and
// the user heard intermittent crunch (xruns).
//
// The fix (parts 1-3 of this series) caches every URID in activate() and moves
// the per-event vectors into pre-sized members, so the audio path no longer
// allocates at all. What follows is the guard that keeps it that way: it counts
// C++ operator new/delete calls on the calling thread while armed and asserts
// the count is zero across processBlock() calls of several block sizes, plus a
// second test that reproduces the original failure scenario (another thread
// hammering the process-wide URID map) to prove the audio path never touches
// that map.
//
// Coverage of the event paths themselves: the fixture (tests/test_lv2_atom)
// now also has an atom OUTPUT port (index 4) that it fills with patch:Set
// events, and AMPFORGE_TEST_HOOKS exposes the two attach flags plus the ring
// seams. That is what makes the harvest path (slot memcpy, patch:Set parse,
// ring pushes, formatFloatFixed6/formatInt32) and the injection path (DSP ring
// drain, scratch build, sequence append) actually *execute* under the guard
// instead of being dead code behind an unattached UI.
//
// The count is an exact integer, not a timing measurement: no latency
// assertions, so the tests cannot go flaky on a loaded CI machine.

// --- allocation guard -----------------------------------------------------
// Replaces global operator new/delete for the whole test binary, but only
// COUNTS while a thread-local guard is armed (default: off, behaviour-neutral
// for every other test). The guard is armed around processBlock() calls below:
// any C++ allocation on that thread while armed is a real-time violation.
// All plain/array/nothrow/sized/aligned variants are intercepted, which is what
// std::string, std::vector and the pre-fix bug used. Direct C malloc/calloc
// (e.g. inside a plugin's run()) is NOT intercepted: this pins the HOST's C++
// allocations, not arbitrary plugin behaviour.
namespace {
thread_local bool g_guardArmed = false;
thread_local size_t g_guardAllocs = 0;

struct AllocGuard {
    AllocGuard() {
        // Touch the TLS once before arming so a lazy TLS-init allocation (if
        // the platform ever does one) cannot be counted as a violation.
        (void)g_guardArmed;
        g_guardAllocs = 0;
        g_guardArmed = true;
    }
    ~AllocGuard() { g_guardArmed = false; }
    size_t count() const { return g_guardAllocs; }
};

void* countedAlloc(size_t n) {
    if (g_guardArmed) ++g_guardAllocs;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
} // namespace

void* operator new(size_t n) { return countedAlloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void* operator new[](size_t n) { return countedAlloc(n); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    if (g_guardArmed) ++g_guardAllocs;
    return std::malloc(n ? n : 1);
}
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
    if (g_guardArmed) ++g_guardAllocs;
    return std::malloc(n ? n : 1);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
// C++17 aligned variants (portable: _aligned_malloc on Windows, aligned_alloc
// elsewhere; aligned_alloc needs size rounded up to a multiple of alignment).
void* operator new(size_t n, std::align_val_t al) {
    if (g_guardArmed) ++g_guardAllocs;
    const size_t a = static_cast<size_t>(al);
    const size_t sz = ((n ? n : 1) + a - 1) / a * a;
#ifdef _WIN32
    void* p = _aligned_malloc(sz, a);
#else
    void* p = std::aligned_alloc(a, sz);
#endif
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p, std::align_val_t) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void* operator new[](size_t n, std::align_val_t al) { return operator new(n, al); }
void operator delete[](void* p, std::align_val_t al) noexcept { operator delete(p, al); }
// Sized aligned delete: GCC/Clang emit this variant for over-aligned types.
// Without it the default libstdc++ implementation would free() memory that came
// from _aligned_malloc on Windows, i.e. heap corruption.
void operator delete(void* p, size_t, std::align_val_t al) noexcept { operator delete(p, al); }
void operator delete[](void* p, size_t, std::align_val_t al) noexcept { operator delete(p, al); }
// Nothrow aligned variants: same contract as the throwing ones above, except
// that a failure returns nullptr instead of throwing.
void* operator new(size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    if (g_guardArmed) ++g_guardAllocs;
    const size_t a = static_cast<size_t>(al);
    const size_t sz = ((n ? n : 1) + a - 1) / a * a;
#ifdef _WIN32
    return _aligned_malloc(sz, a);
#else
    return std::aligned_alloc(a, sz);
#endif
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void* operator new[](size_t n, std::align_val_t al, const std::nothrow_t& nt) noexcept {
    return operator new(n, al, nt);
}
void operator delete[](void* p, std::align_val_t al, const std::nothrow_t& nt) noexcept {
    operator delete(p, al, nt);
}

namespace {

// Same fixture as test_lv2_atom_host.cpp: a gain of 1.0 plus one atom input
// port. It mutes itself unless the host connected the event port and left a
// valid empty atom:Sequence there, so the pass-through assertions below also
// prove the atom path really ran (and therefore that the allocation-free atom
// code is the code under test, not a skipped branch).
constexpr const char* kAtomUri = "http://ampforge.org/plugins/test-atom";

// Sentinel written into the output before processing: distinguishes "the host
// never wrote this sample" from a genuine (wrong) value of 0.
constexpr float kUnwritten = -12345.0f;

// Deterministic ramp: no RNG, so a failing sample index always reproduces.
void fillRamp(std::vector<float>& buffer) {
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = 0.5f * std::sin(static_cast<float>(i) * 0.01f);
    }
}

// Pass-through check on every `stride`-th sample. Cheap enough to run per
// block, dense enough that a muted or mis-wired fixture cannot slip through.
void checkPassThrough(const std::vector<float>& in, const std::vector<float>& out,
                      uint32_t stride) {
    for (size_t i = 0; i < in.size(); i += stride) {
        CHECK_NEAR(out[i], in[i], 1e-6f);
    }
}

} // namespace

AMPFORGE_TEST(lv2_process_block_is_allocation_free)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
    const bool loaded = lv2->load(kAtomUri, TEST_LV2_ATOM_BUNDLE_DIR);
    CHECK(loaded);
    if (!loaded) return;

    CHECK(lv2->activate(48000.0, 2048));

    // Warm-up, deliberately unguarded: a first run() may lazily initialise
    // something inside the plugin or the host (one-time, non-RT by nature).
    // Only the steady state is what the audio thread has to keep allocation-free.
    {
        std::vector<float> in(512), out(512, kUnwritten);
        fillRamp(in);
        lv2->processBlock(in.data(), out.data(), 512);
    }

    // 512 is exactly one run() chunk; 1024 and 2048 exercise the chunk loop
    // (processBlock() splits anything above kMaxRunFrames == 512).
    const uint32_t blockSizes[] = {512, 1024, 2048};
    for (uint32_t frames : blockSizes) {
        for (int iteration = 0; iteration < 8; ++iteration) {
            std::vector<float> in(frames), out(frames, kUnwritten);
            fillRamp(in);

            // The guard scope covers ONLY the processBlock() call: the vectors
            // above and the assertions below are test-side work that allocates
            // freely, and counting it would make the test meaningless.
            size_t allocs = 0;
            {
                AllocGuard guard;
                lv2->processBlock(in.data(), out.data(), frames);
                allocs = guard.count();
            }

            // The regression assertion. On the pre-fix code this is 13 per
            // run() chunk (one mapUrid() temporary std::string each, plus the
            // per-event vectors), so a single 2048-frame block failed with 52.
            CHECK(allocs == 0);

            // Audio actually ran: the fixture emits silence instead of
            // pass-through when the atom port is unusable, so this doubles as a
            // check that the allocation-free atom path was really exercised.
            checkPassThrough(in, out, 64);
        }
    }

    lv2->deactivate();
    lv2->unload();
}

AMPFORGE_TEST(lv2_process_block_is_allocation_free_under_urid_contention)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    // The audio-thread instance: loaded, activated, warmed up once.
    auto audioPlugin = std::make_unique<ampforge::Lv2Plugin>();
    const bool loaded = audioPlugin->load(kAtomUri, TEST_LV2_ATOM_BUNDLE_DIR);
    CHECK(loaded);
    if (!loaded) return;
    CHECK(audioPlugin->activate(48000.0, 2048));
    {
        std::vector<float> in(512), out(512, kUnwritten);
        fillRamp(in);
        audioPlugin->processBlock(in.data(), out.data(), 512);
    }

    // The contender: a second instance of the same plugin, activated and
    // deactivated in a loop. Every activate() maps ~20 URIs through the
    // process-wide map (mutex + std::string), which is exactly the contention
    // that used to make the audio thread allocate and stall mid-block.
    auto backgroundPlugin = std::make_unique<ampforge::Lv2Plugin>();
    const bool backgroundLoaded = backgroundPlugin->load(kAtomUri, TEST_LV2_ATOM_BUNDLE_DIR);
    CHECK(backgroundLoaded);

    std::atomic<bool> stop{false};
    std::atomic<int> contenderActivateFailures{0};
    std::thread contender;
    if (backgroundLoaded) {
        contender = std::thread([&] {
            // No CHECK here: the harness failure counter is not atomic, so all
            // assertions stay on the main thread.
            while (!stop.load(std::memory_order_relaxed)) {
                if (!backgroundPlugin->activate(48000.0, 2048)) {
                    contenderActivateFailures.fetch_add(1, std::memory_order_relaxed);
                }
                backgroundPlugin->deactivate();
            }
        });
    }

    const uint32_t frames = 1024;  // two run() chunks, so the chunk loop is in play
    std::vector<float> in(frames), out(frames, kUnwritten);

    for (int iteration = 0; iteration < 200; ++iteration) {
        fillRamp(in);
        std::fill(out.begin(), out.end(), kUnwritten);

        size_t allocs = 0;
        {
            AllocGuard guard;
            audioPlugin->processBlock(in.data(), out.data(), frames);
            allocs = guard.count();
        }

        // With the fix the audio path never touches the URID map, so this stays
        // 0 no matter what the contender is doing. Before the fix it was 26 per
        // block (13 mapUrid() calls per chunk), and the mutex made it worse.
        CHECK(allocs == 0);

        // Pass-through on the first and last iteration only: enough to prove
        // the audio path is healthy while the contention is running, without
        // spending the loop's time on assertions.
        if (iteration == 0 || iteration == 199) {
            checkPassThrough(in, out, 64);
        }

        // Give the contender a chance to run: without this the main loop can
        // finish first and the contention would never be exercised.
        if ((iteration & 15) == 15) std::this_thread::yield();
    }

    stop.store(true);
    if (contender.joinable()) contender.join();

    // The contention only means something if the contender actually ran:
    // every activate() maps ~20 URIs through the global mutex. A failing
    // activate would make this test pass vacuously.
    CHECK(contenderActivateFailures.load() == 0);

    // Still correct once the contention has stopped: the audio instance was not
    // disturbed by it.
    fillRamp(in);
    std::fill(out.begin(), out.end(), kUnwritten);
    audioPlugin->processBlock(in.data(), out.data(), frames);
    checkPassThrough(in, out, 64);

    audioPlugin->deactivate();
    audioPlugin->unload();
    if (backgroundLoaded) {
        backgroundPlugin->deactivate();
        backgroundPlugin->unload();
    }
}
// --- harvest path ---------------------------------------------------------
//
// Why this test exists: the fixture's atom OUTPUT port (index 4) plus the
// attach seams make processBlock()'s harvest loop actually run - the per-event
// slot memcpy, the patch:Set object walk, the ring pushes and the numeric
// writers (formatFloatFixed6/formatInt32). Before this, no UI was ever attached
// and the fixture had no output port, so that whole branch was dead code under
// test: the allocation guard above only ever pinned the URID cache.
//
// The three forged events also pin the formatting end-to-end: a float 0.5 must
// come out as "0.500000" (fixed 6 decimals, locale-free), an int 42 as "42",
// and a string verbatim.
AMPFORGE_TEST(lv2_harvest_is_allocation_free)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
    const bool loaded = lv2->load(kAtomUri, TEST_LV2_ATOM_BUNDLE_DIR);
    CHECK(loaded);
    if (!loaded) return;

    CHECK(lv2->activate(48000.0, 2048));

    // Both sinks on: the UI ring takes every harvested atom, the patch ring
    // takes every parsed patch:Set. This is what a real UI would do.
    lv2->testSetNativeUiAttached(true);
    lv2->testSetPatchSinkOpen(true);

    // Warm-up, deliberately unguarded (same reasoning as the tests above), then
    // drain both rings so the per-block size assertions start from empty.
    {
        std::vector<float> in(512), out(512, kUnwritten);
        fillRamp(in);
        lv2->processBlock(in.data(), out.data(), 512);
    }
    bool ok = false;
    while (lv2->testUiRingSize() > 0) (void)lv2->testPopUiEvent(ok);
    while (lv2->testPatchRingSize() > 0) (void)lv2->testPopPatchNote(ok);

    const uint32_t frames = 1024;  // two run() chunks: the harvest runs twice
    for (int iteration = 0; iteration < 8; ++iteration) {
        std::vector<float> in(frames), out(frames, kUnwritten);
        fillRamp(in);

        // Guard scope covers ONLY processBlock(): the vectors, the ring drains
        // and the string comparisons below are test-side work.
        size_t allocs = 0;
        {
            AllocGuard guard;
            lv2->processBlock(in.data(), out.data(), frames);
            allocs = guard.count();
        }
        CHECK(allocs == 0);

        // Audio still flows: the fixture only passes audio through when the
        // input sequence is a valid empty one, which it is here (nothing was
        // injected), so the harvest path ran alongside a healthy audio path.
        checkPassThrough(in, out, 64);

        // The harvest really produced events - otherwise the guard above would
        // be watching a loop that never entered.
        CHECK(lv2->testUiRingSize() > 0);
        CHECK(lv2->testPatchRingSize() > 0);

        if (iteration == 0) {
            // The UI-ring events must carry the eventTransfer format URID:
            // a regression once left protocol at 0, which a native UI reads
            // as "control port, one float" and misparses the atom payload.
            bool uok = false;
            auto uev = lv2->testPopUiEvent(uok);
            CHECK(uok);
            CHECK(uev.protocol != 0);
            CHECK(uev.size > 0);

            // The fixture forges exactly three patch:Set events, in this order.
            // These allocations are test-side, outside the guard.
            auto note = lv2->testPopPatchNote(ok);
            CHECK(ok);
            CHECK(std::string(note.value, note.valueLen) == "0.500000");
            note = lv2->testPopPatchNote(ok);
            CHECK(ok);
            CHECK(std::string(note.value, note.valueLen) == "/test/ir.wav");
            note = lv2->testPopPatchNote(ok);
            CHECK(ok);
            CHECK(std::string(note.value, note.valueLen) == "42");
        }

        // Drain both rings fully: a full ring would start dropping and the
        // "> 0" assertions above would stop meaning anything.
        while (lv2->testUiRingSize() > 0) (void)lv2->testPopUiEvent(ok);
        while (lv2->testPatchRingSize() > 0) (void)lv2->testPopPatchNote(ok);
    }

    // Nothing was dropped: the harvest never overflowed a ring, i.e. the
    // producers did their work without hitting the drop paths.
    CHECK(lv2->uiEventsDropped() == 0);
    CHECK(lv2->patchNotesDropped() == 0);

    lv2->testSetNativeUiAttached(false);
    lv2->testSetPatchSinkOpen(false);
    lv2->deactivate();
    lv2->unload();
}

// --- injection path -------------------------------------------------------
//
// Why this test exists: the DSP ring drain (UI->audio thread) only runs when
// something was pushed into it, and nothing in the tests ever pushed - so the
// scratch build, the sequence append and the unmatched-port skip were dead code
// under the guard. testPushDspEvent() is the producer seam (same code path as
// uiWriteFunction, mutex included, on the test thread).
AMPFORGE_TEST(lv2_injection_is_allocation_free)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
    const bool loaded = lv2->load(kAtomUri, TEST_LV2_ATOM_BUNDLE_DIR);
    CHECK(loaded);
    if (!loaded) return;

    CHECK(lv2->activate(48000.0, 2048));

    // Warm-up, unguarded (see above).
    {
        std::vector<float> in(512), out(512, kUnwritten);
        fillRamp(in);
        lv2->processBlock(in.data(), out.data(), 512);
    }

    // Minimal float atom: injection only needs a valid LV2_Atom header, the
    // fixture ignores the CONTENT of input events.
    struct { LV2_Atom atom; float value; } evt;
    evt.atom.size = sizeof(float);
    evt.atom.type = 1;  // arbitrary URID: injection does not validate types
    evt.value = 0.75f;

    const uint32_t frames = 512;  // one run() chunk
    std::vector<float> in(frames), out(frames, kUnwritten);

    // --- matched port: the event is drained and appended to port 3 ---------
    CHECK(lv2->testPushDspEvent(3, &evt, sizeof(evt)));  // port 3 = atom input
    CHECK(lv2->testDspRingSize() == 1);

    fillRamp(in);
    std::fill(out.begin(), out.end(), kUnwritten);
    size_t allocs = 0;
    {
        AllocGuard guard;
        lv2->processBlock(in.data(), out.data(), frames);
        allocs = guard.count();
    }
    CHECK(allocs == 0);
    CHECK(lv2->testDspRingSize() == 0);  // consumed

    // NO pass-through assertion here: the injected event makes the input
    // sequence non-empty and the fixture deliberately mutes on non-empty input
    // (that is its usability contract). Audio correctness is covered by the
    // harvest test above.

    // --- unmatched port: skipped, never injected into port 0 ---------------
    CHECK(lv2->testPushDspEvent(99, &evt, sizeof(evt)));
    CHECK(lv2->testDspRingSize() == 1);

    fillRamp(in);
    std::fill(out.begin(), out.end(), kUnwritten);
    allocs = 0;
    {
        AllocGuard guard;
        lv2->processBlock(in.data(), out.data(), frames);
        allocs = guard.count();
    }
    CHECK(allocs == 0);
    // The unmatched portIndex is skipped (sentinel), not injected into port 0:
    // the ring is drained all the same.
    CHECK(lv2->testDspRingSize() == 0);

    // --- back to clean: input empty again, pass-through restored -----------
    fillRamp(in);
    std::fill(out.begin(), out.end(), kUnwritten);
    lv2->processBlock(in.data(), out.data(), frames);
    checkPassThrough(in, out, 64);

    // The wrong-port event above was rejected by the injection sentinel and
    // counted (the valid one was delivered): exactly one drop.
    CHECK(lv2->dspEventsDropped() == 1);

    lv2->deactivate();
    lv2->unload();
}
// --- drop paths -----------------------------------------------------------
//
// Every rejection - producer-side oversize, full ring, and the consumer's
// malformed/unmatched/over-capacity checks - must be counted, so a field
// report of "the UI event I sent never arrived" is one accessor away. This
// test drives each path through the seams and checks the counters.
AMPFORGE_TEST(lv2_event_drop_paths_are_counted)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
    const bool loaded = lv2->load(kAtomUri, TEST_LV2_ATOM_BUNDLE_DIR);
    CHECK(loaded);
    if (!loaded) return;
    CHECK(lv2->activate(48000.0, 2048));

    // A minimal well-formed float atom, and an oversized payload.
    struct { LV2_Atom atom; float value; } evt;
    evt.atom.size = sizeof(float);
    evt.atom.type = 1;  // arbitrary URID: injection does not validate types
    evt.value = 0.75f;
    std::vector<uint8_t> big(9000, 0);  // > kMaxDspEventBytes (8176)

    // 1. Producer-side oversize: rejected up front, counted, never queued.
    CHECK(!lv2->testPushDspEvent(3, big.data(), static_cast<uint32_t>(big.size())));
    CHECK(lv2->dspEventsDropped() == 1);
    CHECK(lv2->testDspRingSize() == 0);

    // 2. Consumer-side malformed: a payload smaller than an LV2_Atom header
    //    is accepted by the producer seam (no minimum there) and rejected by
    //    the injection drain, which must count it.
    const uint32_t tiny = 4;
    CHECK(lv2->testPushDspEvent(3, &evt, tiny));
    {
        std::vector<float> in(512), out(512, kUnwritten);
        fillRamp(in);
        AllocGuard guard;
        lv2->processBlock(in.data(), out.data(), 512);
        CHECK(guard.count() == 0);
    }
    CHECK(lv2->testDspRingSize() == 0);
    CHECK(lv2->dspEventsDropped() == 2);

    // 3. Full ring: 8 slots. The 9th push fails and is counted; nothing is
    //    lost silently.
    for (int i = 0; i < 8; ++i) {
        CHECK(lv2->testPushDspEvent(3, &evt, sizeof(evt)));
    }
    CHECK(!lv2->testPushDspEvent(3, &evt, sizeof(evt)));
    CHECK(lv2->dspEventsDropped() == 3);
    CHECK(lv2->testDspRingSize() == 8);

    // Drain them: all 8 are deliverable, so the counter must not move.
    {
        std::vector<float> in(512), out(512, kUnwritten);
        fillRamp(in);
        AllocGuard guard;
        lv2->processBlock(in.data(), out.data(), 512);
        CHECK(guard.count() == 0);
    }
    CHECK(lv2->testDspRingSize() == 0);
    CHECK(lv2->dspEventsDropped() == 3);

    lv2->deactivate();
    lv2->unload();
}