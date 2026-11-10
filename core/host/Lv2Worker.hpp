#pragma once

/*
 * LV2 worker driver: host-side machinery for plugins that use
 * <http://lv2plug.in/ns/ext/worker> (the `work:schedule` feature).
 *
 * The worker extension splits plugin work in two halves that run on different
 * sides of the real-time boundary:
 *
 *   - run()  (audio thread) may call host->schedule_work() to ask for work to be
 *     done later, and gets the answers back as work_response()/end_run() calls
 *     during a subsequent run();
 *   - work() (background thread) does the actual work - loading a file, running
 *     a heavy computation - and may reply with a response.
 *
 * This header is the reusable, plugin-agnostic half of that: a lock-free byte
 * ring for the two directions, and a class that owns the background thread and
 * pumps both sides. It knows nothing about lilv, atoms or Lv2Plugin, so it can
 * be unit-tested on its own (see tests/test_lv2_worker.cpp) - the same reasoning
 * that keeps LockFreeQueue.hpp dependency-free.
 *
 * Threading classes, stated once so the per-method notes can be short:
 *
 *   audio thread : ByteRing::push (request direction), Lv2Worker::schedule(),
 *                  Lv2Worker::endCycle()
 *   worker thread: ByteRing::peek / release (request direction) and the push
 *                  side of the response ring, i.e. Lv2Worker::threadMain()
 *   non-RT       : Lv2Worker::start(), Lv2Worker::stop() and, inside them,
 *                  ByteRing::allocate() / ByteRing::reset()
 *
 * Nothing on the audio-thread paths allocates, takes a mutex or makes a system
 * call; the request ring is a single-producer/single-consumer ring and the one
 * cross-thread producer case (a plugin restoring state off the audio thread) is
 * serialised with an atomic_flag that fails fast instead of blocking. The rings
 * are sized by the first start() and only reset() from then on, so a restart
 * can never free a buffer the audio thread happens to be inside.
 */

#include "lv2/lv2.h"
#include "lv2/worker.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace ampforge {

// Lock-free SPSC byte ring with message framing.
//
// Every message becomes one record in the ring:
//
//     [ uint32 size ][ 4 pad ][ payload ... ][ padding to 8 bytes ]
//
// Both ends index the ring with monotonic byte counters (head/tail) that are
// masked when indexing, so a record may wrap around the end of the buffer; the
// counters themselves never wrap in practice (size_t, 256 KB ring) and the
// "bytes in flight" difference is what tells us the free space and the
// emptiness. Framing plus 8-byte-aligned records means a message can be copied
// in as two memcpys (head, tail of the wrap) and read back the same way.
//
// allocate() is the only allocating call: after it the buffer never grows, so
// push()/peek()/release() are allocation-free and safe on the audio thread.
class ByteRing {
public:
    // The size field itself, and the record's framing overhead.
    static constexpr size_t kSizeFieldBytes = sizeof(uint32_t);
    // 8, not 4, even though only the size field is written: the record is
    // padded to kAlignment anyway, so the four spare bytes cost nothing already
    // paid for, and they are what puts the payload on an 8-byte boundary. A
    // plugin is entitled to cast the payload straight to its own message struct,
    // which is undefined behaviour if the bytes start four past an aligned
    // address - and with a 4-byte header every other record (payload sizes vary)
    // would land there, since the record start is only guaranteed 8-aligned.
    static constexpr size_t kHeaderBytes = 8;
    // The alignment every record starts at.
    static constexpr size_t kAlignment = 8;
    // Smallest capacity allocate() will honour (one maximal record must fit).
    static constexpr size_t kMinCapacity = 64;

    ByteRing() = default;

    // NON-RT: size the ring for `bytes` of payload. The capacity is rounded up
    // to a power of two (the masking above needs that) and the scratch buffer
    // peek() may need is reserved here, so peek() never allocates either.
    void allocate(size_t bytes) {
        size_t c = kMinCapacity;
        while (c < bytes) c <<= 1;
        if (c < kMinCapacity) c = kMinCapacity;  // overflow guard for huge input

        cap = c;
        buf.assign(cap, 0);
        // Upper bound on a wrapped payload: it can never exceed the capacity.
        scratch.assign(cap, 0);
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const { return cap; }

    // NON-RT: forget every queued record without touching the buffer, so the
    // allocations survive a stop()/start() pair (see Lv2Worker::start()).
    void reset() {
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
    }

    // Producer: copy `size` bytes as one record, or nothing at all.
    //
    // Returns false when the record would not fit (including when `size` alone
    // is bigger than capacity()), in which case the ring is left untouched so
    // the caller can simply retry later. Never allocates, never locks, never
    // makes a system call: this is called on the audio thread.
    bool push(const void* data, uint32_t size) {
        const size_t record = recordSize(size);
        if (record > cap) return false;  // also covers cap == 0 (not allocated)

        const size_t h = head.load(std::memory_order_relaxed);
        const size_t t = tail.load(std::memory_order_acquire);
        if (cap - (h - t) < record) return false;  // full: all-or-nothing

        size_t pos = h & (cap - 1);
        writeAt(pos, &size, kSizeFieldBytes);
        pos = (pos + kHeaderBytes) & (cap - 1);
        if (size != 0 && data != nullptr) writeAt(pos, data, size);
        // The four spare header bytes and the tail padding are never read back,
        // so they are left as they are.

        // Release: the consumer may now see the header and payload we wrote.
        head.store(h + record, std::memory_order_release);
        return true;
    }

    // Consumer: point at the oldest record's payload, or nullptr when empty.
    //
    // The returned pointer stays valid until release() is called. A payload that
    // does not wrap is returned in place; a wrapped one is stitched together
    // once into `scratch` (reserved by allocate()) because the streaming LV2
    // contract wants a contiguous buffer. `scratch` is reused for every wrapped
    // message, which is why the pointer is only valid until the next peek().
    //
    // The size read out of the ring is validated before it is trusted: a length
    // no record could have means the whole ring is dropped as corrupt (see
    // below), so `size` never points past the buffer.
    const uint8_t* peek(uint32_t& size) const {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t h = head.load(std::memory_order_acquire);
        if (t == h) return nullptr;

        size = readSizeAt(t & (cap - 1));
        if (size > cap - kHeaderBytes) {
            // A record bigger than the ring can only come from a corrupted or
            // resurrected frame (e.g. a producer racing a reset()), and the size
            // is what the plugin's work() would trust for its memcpy. Nothing
            // useful is behind it, so drop the ring instead of handing out a
            // length that points past the buffer.
            tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
            return nullptr;
        }
        const size_t payloadPos = (t + kHeaderBytes) & (cap - 1);
        if (payloadPos + size <= cap) return buf.data() + payloadPos;  // in place

        const size_t firstPart = cap - payloadPos;
        std::memcpy(scratch.data(), buf.data() + payloadPos, firstPart);
        std::memcpy(scratch.data() + firstPart, buf.data(), size - firstPart);
        return scratch.data();
    }

    // Consumer: drop the record peek() returned. Advances tail by the record
    // size, so the producer can reuse those bytes.
    void release() {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t h = head.load(std::memory_order_acquire);
        if (t == h) return;  // nothing peeked: keep tail in step with head

        const size_t record = recordSize(readSizeAt(t & (cap - 1)));
        // Release: everything we read from it is finished with.
        tail.store(t + record, std::memory_order_release);
    }

private:
    // `size` rounded up so the next record still starts 8-byte aligned.
    static size_t recordSize(uint32_t size) {
        const size_t raw = kHeaderBytes + static_cast<size_t>(size);
        return (raw + (kAlignment - 1)) & ~(kAlignment - 1);
    }

    // Copy `n` bytes at buffer position `pos`, wrapping around the end.
    void writeAt(size_t pos, const void* src, size_t n) {
        const size_t firstPart = (n < cap - pos) ? n : cap - pos;
        std::memcpy(buf.data() + pos, src, firstPart);
        if (n > firstPart) {
            std::memcpy(buf.data(), static_cast<const uint8_t*>(src) + firstPart,
                        n - firstPart);
        }
    }

    // Read the size field - the only part of the header that is written - which
    // may span the wrap point.
    uint32_t readSizeAt(size_t pos) const {
        uint8_t raw[kSizeFieldBytes] = {};
        const size_t firstPart = (kSizeFieldBytes < cap - pos) ? kSizeFieldBytes : cap - pos;
        std::memcpy(raw, buf.data() + pos, firstPart);
        if (kSizeFieldBytes > firstPart) {
            std::memcpy(raw + firstPart, buf.data(), kSizeFieldBytes - firstPart);
        }
        uint32_t size = 0;
        std::memcpy(&size, raw, sizeof size);
        return size;
    }

    std::vector<uint8_t> buf;
    size_t cap = 0;  // power of two, 0 until allocate()
    // `scratch` is only written by peek() on the consumer side, hence its
    // const-ness: the class is conceptually one producer + one consumer, and
    // peek() must not be able to resize it.
    mutable std::vector<uint8_t> scratch;
    std::atomic<size_t> head{0};
    // `tail` is the consumer's counter, so only release() moves it forwards -
    // except when peek() drops a corrupt ring, which is why it is mutable: the
    // read side has to be able to throw away the whole buffer it is looking at.
    mutable std::atomic<size_t> tail{0};
};

// Drives one plugin instance's LV2_Worker_Interface.
//
// Owner's contract (what a caller has to do, and what this class does in turn):
//
//   * non-RT thread: start(iface, handle) after instantiate(), stop() before
//     cleanup. `stop()` joins the thread and is idempotent;
//   * audio thread: pass `schedule()` to the plugin as the work:schedule
//     feature's callback, and call endCycle() once after every run();
//   * worker thread (internal): work() is called with the bytes schedule()
//     copied in, and the response is queued back for endCycle() to hand to
//     work_response().
//
// Polling rather than signalling is deliberate. Waking a thread from schedule()
// would need a mutex + condition variable or an eventfd - either of which is a
// lock or a system call on the audio thread - so instead the worker thread
// sleeps 2 ms between drains, which bounds the latency of a state restore at a
// cost the loop can well afford (LV2 worker traffic is rare and non-RT).
class Lv2Worker {
public:
    // Ring sizes: generous enough for state chunks (a preset/IR blob) while
    // staying at half a megabyte of total buffer - allocated once per plugin.
    static constexpr size_t kRingBytes = 256 * 1024;
    // Worker thread poll interval, see the "polling rather than signalling" note.
    static constexpr int kPollMillis = 2;

    // Response hook, called on the worker thread right after iface->work()
    // succeeded and *only* when the plugin did not reply through the respond
    // callback it was handed - i.e. it is the "the plugin replies out of band"
    // escape hatch, not the normal path.
    //
    // Fill `out` with the response payload and return true; return false to say
    // "this call produced no response" (nothing is then queued). With no hook
    // installed the default is also "no response": a plugin that wants to answer
    // uses respond(), which is what every real implementation does. The old
    // default looped the request bytes back into work_response(), which handed
    // the plugin a reply it never produced - measured as 1064 fabricated bytes on
    // a real hexforge instance - and only ever "worked" because the tests wanted
    // an echo. A caller that wants that echo (the tests do) can install one
    // itself, as in tests/test_lv2_worker.cpp.
    using ResponseFn = bool (*)(void* user, uint32_t inSize, const void* inData,
                                std::vector<uint8_t>& out);

    Lv2Worker() = default;
    ~Lv2Worker() { stop(); }

    Lv2Worker(const Lv2Worker&) = delete;
    Lv2Worker& operator=(const Lv2Worker&) = delete;

    // NON-RT: (re)start driving `iface` for `pluginHandle`.
    //
    // Restarts from scratch, so it doubles as "reconfigure": any thread is
    // joined first. A null interface is legal - it means the plugin has no
    // worker after all, so no thread is spawned (isRunning() stays false) but
    // schedule() still succeeds and drops what it is given, which is what the
    // LV2 spec asks of a host that accepted the feature.
    void start(const LV2_Worker_Interface* iface, LV2_Handle pluginHandle) {
        stop();

        // Allocate once per worker, then only reset the counters. A host that
        // re-activates calls start() again while a previous run() may still be
        // inside push()/peek(), and allocating here would free the buffer under
        // the audio thread (ThreadSanitizer: a write of size 8 in allocate()
        // against a read in push()). Zeroing the counters drops whatever was in
        // flight during the restart instead, which is exactly the "restarts from
        // scratch" semantics documented above.
        if (requestRing.capacity() == 0) requestRing.allocate(kRingBytes);
        else requestRing.reset();
        if (responseRing.capacity() == 0) responseRing.allocate(kRingBytes);
        else responseRing.reset();

        workerIface.store(iface, std::memory_order_relaxed);
        workerHandle.store(pluginHandle, std::memory_order_release);
        scheduledRequests.store(0, std::memory_order_relaxed);
        deliveredResponses.store(0, std::memory_order_relaxed);
        failedWorkCount.store(0, std::memory_order_relaxed);
        // One value for both answers schedule() needs, set before the thread
        // exists: "a worker will run this" (kRunning) or "the plugin has none"
        // (kNoWorker, the case documented as "schedule() succeeds and drops the
        // message"). Neither may be mistaken for the torn-down state below.
        state.store(iface != nullptr ? kRunning : kNoWorker, std::memory_order_release);

        if (iface == nullptr) return;  // nothing to drive

        stopFlag.store(false, std::memory_order_relaxed);
        thread = std::thread(&Lv2Worker::threadMain, this);
    }

    // NON-RT: stop the worker thread and forget the plugin. Idempotent.
    //
    // Marking the state stopped first also makes the audio-thread entry points
    // inert, so a stray schedule()/endCycle() from a run() racing the shutdown is
    // a no-op instead of a call into a plugin that may already be cleaned up. It
    // is the flag schedule() reads, and it has to say "torn down" before the
    // interface below disappears: that is what tells it the difference between
    // "no worker" (success, dropped) and "nothing will ever run it" (an error).
    void stop() {
        stopFlag.store(true, std::memory_order_relaxed);
        state.store(kStopped, std::memory_order_release);
        if (thread.joinable()) thread.join();
        workerIface.store(nullptr, std::memory_order_release);
        workerHandle.store(nullptr, std::memory_order_relaxed);
        failedWorkCount.store(0, std::memory_order_relaxed);
    }

    bool isRunning() const { return state.load(std::memory_order_acquire) == kRunning; }

    // Audio thread: queue one message for work().
    //
    // Exactly the work:schedule callback. Called from run() every time the
    // plugin wants the worker, and occasionally from a non-RT thread (state
    // restore), so the producer side is guarded by `pushLock`: the loser of the
    // test_and_set returns LV2_WORKER_ERR_NO_SPACE straight away - the plugin
    // retries on its next run() - rather than blocking the audio thread.
    LV2_Worker_Status schedule(uint32_t size, const void* data) {
        // One load, one decision: the answer has to come from a single
        // observation of the worker, which is exactly why the state is one value
        // and not the interface pointer plus a flag.
        const int current = state.load(std::memory_order_acquire);
        if (current == kStopped) {
            // Torn down (stop() was called): nothing will ever run this message,
            // so answering "success" would promise a delivery that cannot happen.
            return LV2_WORKER_ERR_UNKNOWN;
        }
        if (current == kNoWorker) {
            // No worker: honour the "eventually handles it" contract by not
            // reporting an error, and drop the message (there is no thread).
            return LV2_WORKER_SUCCESS;
        }

        if (pushLock.test_and_set(std::memory_order_acquire)) {
            return LV2_WORKER_ERR_NO_SPACE;  // another producer is mid-push
        }

        const bool pushed = requestRing.push(data, size);
        pushLock.clear(std::memory_order_release);
        if (!pushed) return LV2_WORKER_ERR_NO_SPACE;

        scheduledRequests.fetch_add(1, std::memory_order_relaxed);
        return LV2_WORKER_SUCCESS;
    }

    // Audio thread: deliver whatever the worker produced, once per process cycle.
    //
    // Drains the response ring into work_response(), then closes the cycle with
    // end_run() - worker.h says the plugin's end_run "MUST be called ...
    // after every run(), regardless of whether or not any responses were sent
    // that cycle". It is also where plugins publish their freshly computed state,
    // so nothing may gate it: not "something happened this cycle" (the old
    // behaviour, which dropped those publications on quiet cycles) and not "a
    // work() call failed" either, because the plugin is never told about a
    // dropped request and can therefore not retry anything - skipping the call
    // would only risk losing a publication or leaving a DPF-style in-flight flag
    // set, while calling it costs one function pointer call. No allocation, no
    // locks, no system calls; a no-op when there is no worker.
    void endCycle() {
        // Handle first: a stop() landing between the two loads would otherwise
        // leave work_response()/end_run() being handed a null handle.
        const LV2_Handle handle = workerHandle.load(std::memory_order_acquire);
        if (handle == nullptr) return;

        const LV2_Worker_Interface* iface = workerIface.load(std::memory_order_acquire);
        if (iface == nullptr) return;

        uint32_t size = 0;
        while (const uint8_t* payload = responseRing.peek(size)) {
            if (iface->work_response != nullptr) {
                iface->work_response(handle, size, payload);
            }
            responseRing.release();
            deliveredResponses.fetch_add(1, std::memory_order_relaxed);
        }

        if (iface->end_run != nullptr) iface->end_run(handle);
    }

    // Test/diagnostics only: how many requests were accepted, how many
    // responses were handed to work_response(), and how many work() calls the
    // plugin refused. The refusal count is a report, not an input: a refused
    // message is dropped and the cycle still ends (see endCycle()), so nothing
    // the host calls depends on it.
    uint32_t responsesDelivered() const {
        return deliveredResponses.load(std::memory_order_relaxed);
    }
    uint32_t requestsScheduled() const {
        return scheduledRequests.load(std::memory_order_relaxed);
    }
    uint32_t failedWork() const { return failedWorkCount.load(std::memory_order_relaxed); }

    // Non-RT (or matched by a plugin's own internal sync): customise how a
    // response is produced. See ResponseFn; nullptr (the default) means a
    // message work() did not answer with respond() produces no response at all.
    void setResponseFn(ResponseFn fn, void* user) {
        responseUser.store(user, std::memory_order_relaxed);
        responseFn.store(fn, std::memory_order_release);
    }

private:
    // One work() call's context, so the C respond callback can find both the
    // owner and whether the plugin actually replied.
    struct WorkCall {
        Lv2Worker* self;
        bool responded;
    };

    // The respond callback handed to work(). Real plugins (DPF ones, most
    // notably) reply through this rather than through the return value; pushing
    // straight into the response ring is exactly the protocol the spec
    // describes, and it keeps a plugin that ignores our ResponseFn working.
    static LV2_Worker_Status respondThunk(LV2_Worker_Respond_Handle handle,
                                          uint32_t size, const void* data) {
        WorkCall* call = static_cast<WorkCall*>(handle);
#ifndef NDEBUG
        // Only the worker thread may push into the response ring: SPSC holds.
        // A plugin calling respond() from a thread of its own would make the
        // ring multi-producer, which the design forbids - catch it here in debug
        // builds instead of letting it corrupt the ring silently.
        assert(call->self->workerThreadId == std::this_thread::get_id());
#endif
        call->responded = true;
        return call->self->responseRing.push(data, size) ? LV2_WORKER_SUCCESS
                                                         : LV2_WORKER_ERR_NO_SPACE;
    }

    // Queue a response for a message the plugin did not answer through
    // respond(), i.e. only the ResponseFn path. Without one there is nothing to
    // send: the host must not invent a reply the plugin never produced.
    void emitResponse(uint32_t inSize, const void* inData, std::vector<uint8_t>& out) {
        const ResponseFn fn = responseFn.load(std::memory_order_acquire);
        if (fn == nullptr) return;  // default: no response

        out.clear();
        void* user = responseUser.load(std::memory_order_relaxed);
        if (!fn(user, inSize, inData, out)) return;  // "no response"
        if (!out.empty()) {
            responseRing.push(out.data(), static_cast<uint32_t>(out.size()));
        }
    }

    // Worker thread. Non-RT, so a std::vector for the response payload is fine.
    void threadMain() {
        const LV2_Worker_Interface* iface = workerIface.load(std::memory_order_acquire);
        const LV2_Handle handle = workerHandle.load(std::memory_order_relaxed);
        if (iface == nullptr) return;

        // Recorded for respondThunk()'s debug-only same-thread check; only this
        // thread ever reads it.
        workerThreadId = std::this_thread::get_id();

        std::vector<uint8_t> out;  // reusable response payload, this thread only

        while (!stopFlag.load(std::memory_order_relaxed)) {
            uint32_t size = 0;
            while (const uint8_t* payload = requestRing.peek(size)) {
                WorkCall call{this, false};
                const LV2_Worker_Status status =
                    iface->work != nullptr
                        ? iface->work(handle, &Lv2Worker::respondThunk, &call, size, payload)
                        : LV2_WORKER_ERR_UNKNOWN;

                // Payload is still peeked here, so the response can copy from it.
                if (status != LV2_WORKER_SUCCESS) {
                    // The plugin refused the message, so there is no response to
                    // deliver. Only counted, and never acted on: the request is
                    // already gone and the plugin is not told, so a plugin cannot
                    // retry anything and endCycle() still has to close the run -
                    // see failedWork() and endCycle().
                    failedWorkCount.fetch_add(1, std::memory_order_relaxed);
                } else if (!call.responded) {
                    emitResponse(size, payload, out);
                }
                requestRing.release();
            }

            // Always sleep between drains: it bounds the wake-up latency (see
            // the class comment) and keeps a plugin that floods requests from
            // spinning this thread hot.
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMillis));
        }
    }

    ByteRing requestRing;   // audio thread -> worker thread
    ByteRing responseRing;  // worker thread -> audio thread

    std::atomic<const LV2_Worker_Interface*> workerIface{nullptr};
    std::atomic<LV2_Handle> workerHandle{nullptr};
    std::atomic<bool> stopFlag{false};

    // What the audio thread may do with a scheduled message, as one value: the
    // interface pointer and the "is a worker there" answer have to be consistent,
    // and two independent atomics cannot promise that (a racing stop() could be
    // seen as "accepting" but with the interface already cleared, which returned
    // a success for a message nothing would ever run).
    enum WorkerState : int { kStopped = 0, kNoWorker = 1, kRunning = 2 };
    std::atomic<int> state{kStopped};

    // Producer guard for requestRing: the audio thread is the normal producer,
    // but a plugin restoring state may call schedule() from a non-RT thread, so
    // the producer side is not strictly single. A flag (never a mutex) keeps the
    // audio path lock-free and fails fast instead of blocking.
    std::atomic_flag pushLock = ATOMIC_FLAG_INIT;

    std::atomic<uint32_t> scheduledRequests{0};
    std::atomic<uint32_t> deliveredResponses{0};

    // Set by threadMain() when work() refuses a message, read by failedWork()
    // only: it is how a test or a diagnostics view sees a plugin refusing what it
    // is given, and no host call changes because of it.
    std::atomic<uint32_t> failedWorkCount{0};

    // Set by threadMain() before its loop and read only from there (by
    // respondThunk()'s debug-only SPSC check), hence not an atomic.
    std::thread::id workerThreadId{};

    std::atomic<ResponseFn> responseFn{nullptr};
    std::atomic<void*> responseUser{nullptr};

    std::thread thread;
};

} // namespace ampforge
