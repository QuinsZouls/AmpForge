#pragma once
#include "AudioBlock.hpp"
#include "Denormal.hpp"
#include "host/HostedPlugin.hpp"
#include "host/LockFreeQueue.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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

    void setSampleRate(double sr) override {
        sampleRate = sr;
        HostedPlugin* p = activePlugin.load(std::memory_order_acquire);
        if (p) {
            p->deactivate();
            p->activate(sampleRate, maxBlockSize);
        }
    }

    void reset() override {
        std::fill(dryBuffer.begin(), dryBuffer.end(), 0.0f);
        dryWritePos = 0;
        consecutiveBadBlocks = 0;
        autoBypassed.store(false, std::memory_order_release);
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
        if (newPlugin) {
            if (!newPlugin->activate(sampleRate, maxBlockSize)) {
                lastError = "Failed to activate plugin";
                return false;
            }
        }

        reapRetired();

        HostedPlugin* next = newPlugin.release();
        HostedPlugin* old = activePlugin.exchange(next, std::memory_order_acq_rel);

        if (old) {
            const uint64_t epoch = audioEpoch.load(std::memory_order_acquire);
            std::lock_guard<std::mutex> lock(retireMutex);
            retiredPlugins.push_back({old, epoch});
        }

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
        processBlock(&s, 1);
        return s;
    }

    void processBlock(float* buffer, uint32_t frames) override {
        if (frames == 0) return;

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

        HostedPlugin* plugin = activePlugin.load(std::memory_order_acquire);
        if (!plugin || manualBypass || autoBypassed.load(std::memory_order_relaxed)) {
            return; // pass-through
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
                chunkIn[i] = buffer[offset + i] * inputGainLinear;
            }

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
                    // Pass dry remainder through
                    return;
                }
            } else if (consecutiveBadBlocks > 0) {
                --consecutiveBadBlocks;
            }

            // Mix wet & dry with latency compensation
            for (uint32_t i = 0; i < chunkFrames; ++i) {
                const float dryIn = buffer[offset + i];

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
                buffer[offset + i] = flushDenormal(mixed);
            }

            offset += chunkFrames;
        }
    }

    const std::string& getLastError() const { return lastError; }

private:
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

    // Retired plugins awaiting deletion
    std::mutex retireMutex;
    std::vector<std::pair<HostedPlugin*, uint64_t>> retiredPlugins;

    // Lock-free parameter queue
    SpscRingBuffer<PluginParamUpdate, 512> paramQueue;
};

} // namespace ampforge
