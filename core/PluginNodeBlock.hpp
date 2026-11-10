#pragma once
#include "AudioBlock.hpp"
#include "Denormal.hpp"
#include "host/HostedPlugin.hpp"
#include "host/LockFreeQueue.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ampforge {

class PluginNodeBlock : public AudioBlock {
public:
    static constexpr uint32_t kDefaultMaxBlock = 2048;
    static constexpr size_t kMaxLatency = 8192;

    explicit PluginNodeBlock(const std::string& defaultName = "Plugin")
        : customName(defaultName) {
        dryBuffer.resize(kMaxLatency, 0.0f);
        chunkIn.resize(kDefaultMaxBlock, 0.0f);
        chunkOut.resize(kDefaultMaxBlock, 0.0f);
    }

    ~PluginNodeBlock() override {
        // Swap out active plugin
        HostedPlugin* active = activePlugin.exchange(nullptr, std::memory_order_acq_rel);
        if (active) {
            active->deactivate();
            delete active;
        }
        // Clean up retired plugins
        for (auto& item : retiredPlugins) {
            if (item.first) {
                item.first->deactivate();
                delete item.first;
            }
        }
        retiredPlugins.clear();
    }

    // Must not be called from the audio thread: it waits for the audio callback
    // to leave processBlock() before re-activating the plugin.
    void setSampleRate(double sr) override {
        // One writer at a time. reconfigureGate orders the writer against the
        // *audio* thread; this mutex orders writers against each other. Without
        // it the handshake below tears itself apart: two callers raise the gate,
        // the faster one lowers it again once *its* rebuild is done - while the
        // slower one is still inside deactivate()/activate() - and a block that
        // then passes the lowered gate runs straight into the instance the slower
        // caller is tearing down, i.e. exactly the SIGSEGV the gate exists to
        // prevent.
        std::lock_guard<std::mutex> writer(reconfigureMutex);

        // A rate an earlier call had to give up on is retried first: giving up is
        // the right answer (never force the race), but silently drifting is not
        // (see pendingSampleRate), so the retry comes before anything else.
        const bool settledByRetry = retryPendingSampleRate(sr);

        sampleRate = sr;

        if (!activePlugin.load(std::memory_order_acquire)) {
            // Nothing hosted: there is no rate to hand to anything, and the next
            // setPlugin() activates at `sampleRate` - already the newest request -
            // so a queued rate would only ever be retried for nothing.
            pendingSampleRate = -1.0;
            return;
        }

        // The retry just applied exactly this rate: it *was* this call's
        // handshake, and rebuilding the plugin a second time would only cost
        // another dry gap.
        if (settledByRetry) return;

        if (reactivateAt(sr)) pendingSampleRate = -1.0;
        else pendingSampleRate = sr;
    }

    // Deferred on purpose: processBlock() reads and writes the delay line from
    // the audio thread, so clearing it here would be a data race -
    // ThreadSanitizer reports three distinct ones in
    // plugin_node_concurrent_swap_and_retirement (the std::fill against the delay
    // line's read and against its write, plus dryWritePos), with a partially
    // zeroed delay line - an audible click - or a torn write position as the
    // damage. It is UB even where the damage would be invisible.
    //
    // Clearing it here *when* audioInFlight happens to read 0 does not fix that:
    // a block can count itself in right after that load, and whether it then
    // skips the clearing (flag already consumed) or is the one doing it, it has
    // already passed the point where it would have to wait - it reaches the delay
    // line while this thread is still inside std::fill (measured: 4-5 TSan races
    // per run, i.e. exactly the ones above). Only the audio thread can clear
    // these fields safely, so reset() just requests it and the next block applies
    // it (see applyResetRequest()).
    //
    // Nothing observable changes: reset() is only ever called on the disabled ->
    // enabled transition (see AudioBlock::reset()) and from setPlugin(), and in
    // both cases the block that follows applies the request before it produces a
    // single sample.
    void reset() override {
        resetRequested.store(true, std::memory_order_release);
    }

    const char* getName() const override {
        return customName.c_str();
    }

    void setName(const std::string& name) {
        customName = name;
    }

    void setInputTrim(float db) {
        inputGainLinear = std::pow(10.0f, db / 20.0f);
    }

    void setOutputTrim(float db) {
        outputGainLinear = std::pow(10.0f, db / 20.0f);
    }

    void setMix(float m) {
        mix = std::clamp(m, 0.0f, 1.0f);
    }

    void setBypass(bool b) {
        manualBypass = b;
    }

    bool isBypassed() const {
        return manualBypass || autoBypassed.load(std::memory_order_relaxed);
    }

    bool isAutoBypassed() const {
        return autoBypassed.load(std::memory_order_relaxed);
    }

    void clearAutoBypass() {
        consecutiveBadBlocks = 0;
        autoBypassed.store(false, std::memory_order_release);
    }

    bool hasPlugin() const {
        return activePlugin.load(std::memory_order_relaxed) != nullptr;
    }

    HostedPlugin* getActivePlugin() {
        return activePlugin.load(std::memory_order_acquire);
    }

    const HostedPlugin* getActivePlugin() const {
        return activePlugin.load(std::memory_order_acquire);
    }

    // Called from UI or non-audio thread to queue a parameter update
    bool enqueueParameterUpdate(uint32_t paramId, float value) {
        PluginParamUpdate u;
        u.id = paramId;
        u.value = value;
        return paramQueue.push(u);
    }

    // Hot-swap a new plugin in. Safe to call from non-audio threads while
    // audio runs concurrently.
    bool setPlugin(std::unique_ptr<HostedPlugin> newPlugin) {
        // setPlugin() is a non-RT writer too - not for the pointer swap (that is
        // atomic and needs no help), but because the queued-rate retry below goes
        // through the very same gate handshake setSampleRate() uses. Same rule,
        // same order: reconfigureMutex, then retireMutex (see reactivateAt()).
        std::lock_guard<std::mutex> writer(reconfigureMutex);

        // Same retry as setSampleRate(): this is the other non-RT entry point a
        // queued rate can be applied from, and the live instance is the one that
        // needs it if the swap below fails.
        retryPendingSampleRate(sampleRate);

        if (newPlugin) {
            if (!newPlugin->activate(sampleRate, maxBlockSize)) {
                lastError = "Failed to activate plugin";
                return false;
            }
        }

        reapRetired();

        HostedPlugin* next = newPlugin.release();
        HostedPlugin* old = nullptr;
        {
            // The exchange and the retire-push are one step with respect to the
            // re-configuring writer: reactivateAt() holds this mutex while it
            // calls deactivate()/activate() on the instance it loaded from
            // activePlugin, so the instance it is holding can neither be swapped
            // out nor reaped (deleted) underneath it. Epoch read inside the lock,
            // i.e. *after* the swap: any block that loaded `old` before it is
            // either still counted in audioInFlight (so reapRetired() waits) or
            // has already bumped the epoch, and requiring a *later* epoch below
            // keeps that true.
            const uint64_t epoch = audioEpoch.load(std::memory_order_acquire);
            std::lock_guard<std::mutex> lock(retireMutex);
            old = activePlugin.exchange(next, std::memory_order_acq_rel);
            if (old) retiredPlugins.push_back({old, epoch});
        }

        // The instance just installed was activated at `sampleRate`, which is
        // always the newest requested rate, so nothing is left to retry.
        pendingSampleRate = -1.0;

        reset();
        return true;
    }

    void reapRetired() {
        std::lock_guard<std::mutex> lock(retireMutex);
        if (retiredPlugins.empty()) return;

        const uint64_t currentEpoch = audioEpoch.load(std::memory_order_acquire);
        const int inFlight = audioInFlight.load(std::memory_order_acquire);

        // Only safely delete when no audio thread is currently inside processBlock
        if (inFlight == 0) {
            auto it = retiredPlugins.begin();
            while (it != retiredPlugins.end()) {
                if (currentEpoch > it->second) {
                    if (it->first) {
                        it->first->deactivate();
                        delete it->first;
                    }
                    it = retiredPlugins.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    float processSample(float input) override {
        float s = input;
        processBlock(&s, &s, 1);
        return s;
    }

    // `in` and `out` may alias (the chain calls this in place when it can), so
    // every read below reads `in` and every write writes `out` - never one
    // buffer for both. The dry-pass-through helper is what keeps the three
    // early returns honest: with separate buffers, simply returning would leave
    // whatever the host's output buffer already held in `out`, i.e. a block of
    // stale audio instead of the dry signal.
    void processBlock(const float* in, float* out, uint32_t frames) override {
        if (frames == 0) return;

        auto passThroughFrom = [&](uint32_t from) {
            if (out != in)
                std::copy(in + from, in + frames, out + from);
        };

        // Track in-flight audio processing
        audioInFlight.fetch_add(1, std::memory_order_acq_rel);
        struct FlightGuard {
            std::atomic<int>& inFlight;
            std::atomic<uint64_t>& epoch;
            ~FlightGuard() {
                inFlight.fetch_sub(1, std::memory_order_acq_rel);
                epoch.fetch_add(1, std::memory_order_release);
            }
        } guard{audioInFlight, audioEpoch};

        // Pairs with the fence in reactivateAt(). Together they order "count
        // myself in" against "read the gate" and "raise the gate" against "read
        // the count", which is what makes the handshake complete on a weakly
        // ordered CPU too. Cheap (a compiler barrier on x86) and once per block.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // See reconfigureGate: a block that arrives while the plugin is being
        // rebuilt must not touch it. Dry pass-through, not silence, and not a
        // spin - the audio thread must never wait on a non-RT thread.
        if (reconfigureGate.load(std::memory_order_seq_cst)) {
            passThroughFrom(0);
            return;
        }

        // A reset() asked for from a non-RT thread is applied here, before this
        // block touches the delay line: that is what keeps reset() from writing
        // fields this thread reads and writes (see applyResetRequest()).
        applyResetRequest();

        HostedPlugin* plugin = activePlugin.load(std::memory_order_acquire);
        if (!plugin || manualBypass || autoBypassed.load(std::memory_order_relaxed)) {
            passThroughFrom(0); // pass-through
            return;
        }

        // Drain up to 64 queued parameter updates per block
        PluginParamUpdate update;
        int updatesApplied = 0;
        while (updatesApplied < 64 && paramQueue.pop(update)) {
            plugin->setParameterValue(update.id, update.value);
            ++updatesApplied;
        }

        const uint32_t latency = std::min(plugin->getLatency(), static_cast<uint32_t>(kMaxLatency - 1));

        // Process in chunks up to maxBlockSize
        uint32_t offset = 0;
        while (offset < frames) {
            const uint32_t chunkFrames = std::min(frames - offset, maxBlockSize);

            // Prepare chunk input with input trim
            for (uint32_t i = 0; i < chunkFrames; ++i) {
                chunkIn[i] = in[offset + i] * inputGainLinear;
            }

            // Clear the scratch output first: `chunkOut` is reused across
            // blocks, and a plugin may write only part of the block it is given
            // (fixed internal block size, or silent/skipped output). Any tail
            // it leaves untouched must come out as silence, not as the samples
            // left over from the previous block.
            std::fill(chunkOut.begin(), chunkOut.begin() + chunkFrames, 0.0f);

            // Call hosted plugin
            plugin->processBlock(chunkIn.data(), chunkOut.data(), chunkFrames);

            // Validate chunk output (NaN/Inf protection)
            bool blockHasBadSample = false;
            for (uint32_t i = 0; i < chunkFrames; ++i) {
                const float s = chunkOut[i];
                if (std::isnan(s) || std::isinf(s)) {
                    blockHasBadSample = true;
                    chunkOut[i] = 0.0f;
                }
            }

            if (blockHasBadSample) {
                ++consecutiveBadBlocks;
                if (consecutiveBadBlocks >= 5) {
                    autoBypassed.store(true, std::memory_order_release);
                    // Pass the rest dry, from this chunk's first frame on: the
                    // frames of it already written below stay as they are only
                    // if `out` already held dry audio, which it does not.
                    passThroughFrom(offset);
                    return;
                }
            } else if (consecutiveBadBlocks > 0) {
                --consecutiveBadBlocks;
            }

            // Mix wet & dry with latency compensation
            for (uint32_t i = 0; i < chunkFrames; ++i) {
                const float dryIn = in[offset + i];

                // Write to circular dry delay line
                dryBuffer[dryWritePos] = dryIn;

                // Read delayed dry signal
                float delayedDry;
                if (latency == 0) {
                    delayedDry = dryIn;
                } else {
                    const size_t readPos = (dryWritePos + kMaxLatency - latency) % kMaxLatency;
                    delayedDry = dryBuffer[readPos];
                }

                dryWritePos = (dryWritePos + 1) % kMaxLatency;

                const float wet = chunkOut[i] * outputGainLinear;
                const float mixed = (1.0f - mix) * delayedDry + mix * wet;
                out[offset + i] = flushDenormal(mixed);
            }

            offset += chunkFrames;
        }
    }

    const std::string& getLastError() const { return lastError; }

private:
    // Re-activate the hosted plugin at `rate` behind the gate.
    //
    // Re-activation tears the hosted plugin down and rebuilds it (for LV2 it also
    // restarts its worker thread), so it cannot run while the audio callback is
    // inside processBlock() using the buffers about to be freed. Same reasoning
    // as reapRetired() waiting before it deletes a retired plugin - but waiting is
    // only half of it: the gate is raised *first*, so a block entering after this
    // point passes through dry instead of running the plugin we are about to tear
    // down, which is what closes the window between "the writer sampled
    // audioInFlight" and "the block counted itself in".
    //
    // Returns false when the audio thread could not be caught idle within the
    // bound, in which case nothing was touched. Non-RT only, and only while
    // holding reconfigureMutex (one writer at a time).
    bool reactivateAt(double rate) {
        HostedPlugin* p = activePlugin.load(std::memory_order_acquire);
        if (!p) return true;  // nothing hosted: nothing to rebuild

        reconfigureGate.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!waitForAudioIdle()) {
            // A wedged (or debugger-paused) audio thread: skip the change rather
            // than force it. Lowering the gate is what makes that skip harmless -
            // and the caller is what keeps it from becoming a permanent lie about
            // the rate (see pendingSampleRate).
            reconfigureGate.store(false, std::memory_order_seq_cst);
            return false;
        }

        {
            // retireMutex around the whole "look at the active plugin and touch
            // it" region: setPlugin() swaps activePlugin and pushes the old
            // instance onto retiredPlugins under this same mutex, and
            // reapRetired() deletes it from there as soon as audioInFlight has
            // been 0 across an epoch change - which the gated pass-through above
            // brings about almost immediately, since those blocks still bump
            // audioEpoch. Reloading the pointer would not help; the window is
            // after the load, not in it.
            //
            // Lock order: reconfigureMutex -> retireMutex, here and in
            // setPlugin() (which takes retireMutex twice: once inside
            // reapRetired(), once for the swap - sequentially, never nested with
            // another lock). Nothing takes them the other way round, and the
            // audio thread takes neither: processBlock() only touches the atomic
            // gate, the atomic pointer, the SPSC parameter queue and atomics. So
            // neither the handshake nor this mutex can ever make a callback wait,
            // and there is no cycle to deadlock on.
            std::lock_guard<std::mutex> lock(retireMutex);
            p = activePlugin.load(std::memory_order_acquire);  // may have changed
            if (p) {
                p->deactivate();
                p->activate(rate, maxBlockSize);
            }
        }

        // The dry path is continuous across the rebuild, not restarted mid-flight:
        // the gated blocks above returned dry *without* advancing the delay line,
        // so with latency > 0 and mix < 1 the blocks after the gate drops would
        // read pre-gate samples - a step in the dry signal. Clearing it (what
        // setPlugin() does via reset()) restarts the dry path from a known point
        // instead. Safe to write here: the gate is still up and no callback is in
        // flight, and a gated block returns before it reaches the delay line.
        std::fill(dryBuffer.begin(), dryBuffer.end(), 0.0f);
        dryWritePos = 0;

        reconfigureGate.store(false, std::memory_order_seq_cst);
        return true;
    }

    // Apply `pendingSampleRate` if one is queued. Returns true when the plugin is
    // left running at exactly `rate` because of it, i.e. when the retry *was* the
    // handshake the caller was about to do anyway.
    bool retryPendingSampleRate(double rate) {
        if (pendingSampleRate < 0.0) return false;
        const double pending = pendingSampleRate;
        if (!reactivateAt(pending)) return false;
        pendingSampleRate = -1.0;
        return pending == rate;
    }

    // Apply a pending reset(), on the audio thread. The exchange is what makes it
    // single-winner: a block that finds the flag already taken has nothing left to
    // do, and two blocks can never clear the same fields at the same time.
    void applyResetRequest() {
        if (!resetRequested.exchange(false, std::memory_order_acq_rel)) return;
        std::fill(dryBuffer.begin(), dryBuffer.end(), 0.0f);
        dryWritePos = 0;
        consecutiveBadBlocks = 0;
        autoBypassed.store(false, std::memory_order_release);
    }

    // Spin (bounded) until no audio callback is in flight.
    //
    // This is the *waiting* half of the writer/processBlock() handshake:
    // on its own, sampling this counter leaves a window - a block can enter right
    // after the sample - which reconfigureGate is what closes. The gate is what
    // makes waiting sufficient, in other words, and this is what makes it
    // necessary: without the wait, a block that was already inside processBlock()
    // when the gate went up would still be running the plugin.
    //
    // A running stream is idle between callbacks, so in practice this returns on
    // the first or second poll. The bound only exists so a wedged - or
    // debugger-paused - audio thread cannot hang the caller (UI); the caller then
    // simply skips the change instead of forcing a race. Same condition
    // reapRetired() waits for before it deletes a retired plugin.
    bool waitForAudioIdle(int maxMillis = 500) {
        constexpr int kStepMicros = 100;
        for (int waitedMicros = 0; waitedMicros < maxMillis * 1000; waitedMicros += kStepMicros) {
            if (audioInFlight.load(std::memory_order_acquire) == 0) return true;
            std::this_thread::sleep_for(std::chrono::microseconds(kStepMicros));
        }
        return audioInFlight.load(std::memory_order_acquire) == 0;
    }

    std::string customName;
    std::string lastError;
    double sampleRate = 44100.0;
    uint32_t maxBlockSize = kDefaultMaxBlock;

    float inputGainLinear = 1.0f;
    float outputGainLinear = 1.0f;
    float mix = 1.0f;
    bool manualBypass = false;
    std::atomic<bool> autoBypassed{false};
    int consecutiveBadBlocks = 0;

    // Latency delay line
    std::vector<float> dryBuffer;
    size_t dryWritePos = 0;

    // Intermediate chunk buffers
    std::vector<float> chunkIn;
    std::vector<float> chunkOut;

    // Lock-free plugin instance management
    std::atomic<HostedPlugin*> activePlugin{nullptr};
    std::atomic<int> audioInFlight{0};
    std::atomic<uint64_t> audioEpoch{0};

    // Re-configuration gate. reactivateAt() (reached from setSampleRate() and from
    // the pending-rate retry in setPlugin()) has to deactivate and rebuild the
    // hosted plugin - freeing buffers a run() in flight is using - so the writer
    // raises this gate *before* it looks at audioInFlight, and processBlock()
    // reads it *after* it has counted itself in. That ordering is what makes the
    // handshake complete: either the audio thread sees the gate and passes the
    // block through dry (never touching the plugin), or the writer sees the
    // in-flight count and waits for that block to finish. Sampling the counter
    // alone was not enough - a block could enter right after the sample and run
    // into the freed instance (measured: SIGSEGV in Lv2Plugin::processBlock).
    //
    // Only re-configuring a *live* plugin needs this. The setPlugin() *swap*
    // deliberately ignores the gate: it exchanges the pointer and retires the old
    // plugin, so a block already running keeps using an instance nobody has freed
    // yet, and reapRetired() only deletes it once audioInFlight has been 0 across
    // a whole epoch - the swap is safe by design, the gate would only cost a dry
    // block.
    //
    // ~PluginNodeBlock() does not need it either: it runs when the node is being
    // destroyed, i.e. after the audio thread that called processBlock() has been
    // stopped or joined. reset() does not take it for a different reason: it
    // writes nothing at all any more, it only sets resetRequested and lets the
    // audio thread do the clearing (see applyResetRequest()).
    std::atomic<bool> reconfigureGate{false};

    // Set by reset(), picked up by the audio thread before the next block touches
    // the delay line. What it guards - the dry line, dryWritePos and
    // consecutiveBadBlocks - is plain data, so a caller writing it while a
    // callback runs is a data race: UB regardless of how visible the damage is.
    std::atomic<bool> resetRequested{false};

    // One writer at a time: serialises setSampleRate() and setPlugin(), the two
    // non-RT entry points that raise reconfigureGate. Lock order everywhere is
    // reconfigureMutex -> retireMutex; the audio thread takes neither.
    std::mutex reconfigureMutex;

    // The rate the hosted plugin *should* be running at but is not, because the
    // handshake gave up (waitForAudioIdle() timed out on a wedged or
    // debugger-paused audio thread). Skipping the change is right - the
    // alternative is forcing the race - but leaving the plugin at the old rate
    // while the rest of the chain runs at the new one is not: an IR/cab block or
    // an oversampled amp then runs at the wrong rate (wrong pitch, wrong curves)
    // with nothing surfacing it. So the request is kept and retried at the next
    // non-RT entry point (setSampleRate(), setPlugin()) until one succeeds.
    // -1.0 means "nothing pending". Only the writers above touch it, all under
    // reconfigureMutex, so it needs no atomics of its own.
    double pendingSampleRate = -1.0;

    // Retired plugins awaiting deletion. Only the non-RT side takes this - the
    // audio thread must stay lock-free, which is why retirement works by epoch
    // rather than by mutual exclusion - and reactivateAt() reuses it as "this
    // instance cannot be retired while I am touching it".
    std::mutex retireMutex;
    std::vector<std::pair<HostedPlugin*, uint64_t>> retiredPlugins;

    // Lock-free parameter queue
    SpscRingBuffer<PluginParamUpdate, 512> paramQueue;
};

} // namespace ampforge
