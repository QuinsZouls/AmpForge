#include "test_harness.hpp"
#include "host/Lv2Worker.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Tests for the standalone LV2 worker driver (core/host/Lv2Worker.hpp).
//
// Everything here is driven by a hand-written LV2_Worker_Interface, so these
// tests need no plugin, no lilv and no fixture bundle: they never skip, and they
// pin down the threading contract (who calls what, on which side of the 2 ms
// poll) rather than any particular plugin's behaviour.
namespace {

// One test's recording fake.
//
// LV2 callbacks carry no user pointer beyond LV2_Handle, so the handle we pass
// to start() *is* the FakeWorker: one instance per test, no globals to reset.
//
// The callbacks run on two threads - work() on the driver's worker thread, and
// work_response()/end_run() on the test's stand-in "audio" thread - so the
// recording is taken under a mutex and read back through snapshot(). That is
// bookkeeping on the fake (a test may take a mutex in work_response(); a real
// plugin must not); the driver's own audio-thread paths stay lock-free, which is
// what the assertions below are about.
struct FakeWorker
{
    // Copied out under the lock, so a test never reads a half-updated log.
    struct Snapshot
    {
        // One character per call, in call order: 'w' = work(),
        // 'r' = work_response(), 'e' = end_run().
        std::string order;
        int workCalls = 0;
        int responseCalls = 0;
        int endRunCalls = 0;
        std::vector<int32_t> inputs;       // what work() was handed
        std::vector<int32_t> transformed;  // inputs + transform, computed by work()
        std::vector<int32_t> responses;    // what work_response() was handed
    };

    // What the fake adds to every message it works on - proves work() received
    // the scheduled bytes rather than something the driver invented.
    int32_t transform = 1;

    Snapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return recorded;
    }

    void recordWork(int32_t input)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++recorded.workCalls;
        recorded.order += 'w';
        recorded.inputs.push_back(input);
        recorded.transformed.push_back(input + transform);
    }

    void recordResponse(int32_t response)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++recorded.responseCalls;
        recorded.order += 'r';
        recorded.responses.push_back(response);
    }

    void recordEndRun()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++recorded.endRunCalls;
        recorded.order += 'e';
    }

private:
    mutable std::mutex mutex;
    Snapshot recorded;
};

LV2_Worker_Status fakeWork(LV2_Handle instance, LV2_Worker_Respond_Function, LV2_Worker_Respond_Handle,
                           uint32_t size, const void* data)
{
    int32_t value = 0;
    if (data != nullptr && size == sizeof value) std::memcpy(&value, data, sizeof value);
    static_cast<FakeWorker*>(instance)->recordWork(value);
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status fakeWorkResponse(LV2_Handle instance, uint32_t size, const void* body)
{
    int32_t value = 0;
    if (body != nullptr && size == sizeof value) std::memcpy(&value, body, sizeof value);
    static_cast<FakeWorker*>(instance)->recordResponse(value);
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status fakeEndRun(LV2_Handle instance)
{
    static_cast<FakeWorker*>(instance)->recordEndRun();
    return LV2_WORKER_SUCCESS;
}

const LV2_Worker_Interface kFakeInterface = {fakeWork, fakeWorkResponse, fakeEndRun};

// What the respond-path fake adds to the message before replying with it.
constexpr int32_t kRespondDelta = 100;

// A plugin that replies the way real ones do: through the respond callback it
// was handed, not through a ResponseFn hook. The reply deliberately differs from
// the input, so "the response is the transformed value" cannot be satisfied by
// anything that echoes the request.
LV2_Worker_Status respondingWork(LV2_Handle instance, LV2_Worker_Respond_Function respond,
                  LV2_Worker_Respond_Handle handle, uint32_t size, const void* data)
{
    int32_t value = 0;
    if (data != nullptr && size == sizeof value) std::memcpy(&value, data, sizeof value);
    static_cast<FakeWorker*>(instance)->recordWork(value);

    if (respond == nullptr) return LV2_WORKER_ERR_UNKNOWN;
    const int32_t reply = value + kRespondDelta;
    return respond(handle, sizeof reply, &reply);
}

const LV2_Worker_Interface kRespondingInterface = {respondingWork, fakeWorkResponse, fakeEndRun};

// A plugin that refuses the message outright.
LV2_Worker_Status failingWork(LV2_Handle instance, LV2_Worker_Respond_Function,
                              LV2_Worker_Respond_Handle, uint32_t size, const void* data)
{
    int32_t value = 0;
    if (data != nullptr && size == sizeof value) std::memcpy(&value, data, sizeof value);
    static_cast<FakeWorker*>(instance)->recordWork(value);
    return LV2_WORKER_ERR_UNKNOWN;
}

const LV2_Worker_Interface kFailingInterface = {failingWork, fakeWorkResponse, fakeEndRun};

// The echo the driver used to perform by default. It is now an explicit opt-in:
// install it where "a response arrived" has to be observable as "the request
// bytes came back".
bool loopbackResponse(void*, uint32_t inSize, const void* inData, std::vector<uint8_t>& out)
{
    out.clear();
    if (inData == nullptr || inSize == 0) return false;
    const uint8_t* bytes = static_cast<const uint8_t*>(inData);
    out.assign(bytes, bytes + inSize);
    return true;
}

// pumpUntil()'s bound. 500 x 2 ms is a second of wall clock - orders of
// magnitude more than the worker's 2 ms poll needs - but bounded on purpose: a
// broken driver has to fail the CHECKs below, never hang the suite.
constexpr int kMaxPumpIterations = 500;

// What the audio thread does once per block: pump endCycle() until `done()`.
// Returns whether it became true.
template <typename Done>
bool pumpUntil(ampforge::Lv2Worker& worker, Done done)
{
    for (int i = 0; i < kMaxPumpIterations; ++i)
    {
        worker.endCycle();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Wait for `done()` without calling endCycle() at all, for the tests that pin
// what one endCycle() does: any pumping there would consume the very state the
// assertion is about.
template <typename Done>
bool waitUntil(Done done)
{
    for (int i = 0; i < kMaxPumpIterations; ++i)
    {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Occurrences of `c` in `log` at or after `from`.
int countFrom(const std::string& log, size_t from, char c)
{
    int count = 0;
    for (size_t i = from; i < log.size(); ++i)
        if (log[i] == c) ++count;
    return count;
}

// The audio-thread-side handler: schedule an int32 through the driver.
LV2_Worker_Status scheduleInt(ampforge::Lv2Worker& worker, int32_t value)
{
    return worker.schedule(sizeof value, &value);
}

} // namespace

// One message, one work() call, one work_response(), one end_run() after it.
AMPFORGE_TEST(lv2_worker_round_trip)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kFakeInterface, &fake);
    CHECK(worker.isRunning());

    // The driver no longer echoes a request back on its own - a plugin that
    // replies has to call respond() - so this test opts into the echo explicitly
    // to have something to check work_response() against.
    worker.setResponseFn(&loopbackResponse, nullptr);

    constexpr int32_t kScheduled = 41;
    CHECK(scheduleInt(worker, kScheduled) == LV2_WORKER_SUCCESS);
    CHECK(worker.requestsScheduled() == 1);

    CHECK(pumpUntil(worker, [&fake] { return fake.snapshot().responseCalls > 0; }));

    const FakeWorker::Snapshot seen = fake.snapshot();
    CHECK(seen.workCalls == 1);
    CHECK(seen.responseCalls == 1);
    CHECK(worker.responsesDelivered() == 1);

    // work() saw the scheduled bytes and did its job on them...
    CHECK(seen.inputs.size() == 1);
    CHECK(seen.inputs.size() == 1 && seen.inputs[0] == kScheduled);
    CHECK(seen.transformed.size() == 1 && seen.transformed[0] == kScheduled + 1);

    // ...while the response is the echo this test installed: the bytes that went
    // in come back out unchanged.
    CHECK(seen.responses.size() == 1);
    CHECK(seen.responses.size() == 1 && seen.responses[0] == kScheduled);

    // Call order: exactly one work_response(), and exactly one end_run() after
    // it - LV2 requires end_run() to close the cycle the responses arrived in.
    const size_t responsePos = seen.order.find('r');
    CHECK(responsePos != std::string::npos);
    CHECK(countFrom(seen.order, 0, 'r') == 1);
    CHECK(countFrom(seen.order, responsePos + 1, 'e') == 1);
    CHECK(!seen.order.empty() && seen.order.back() == 'e');

    worker.stop();
    CHECK(!worker.isRunning());
}

// A message that cannot fit is refused, not truncated or blocked on.
AMPFORGE_TEST(lv2_worker_reports_no_space)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kFakeInterface, &fake);

    std::vector<uint8_t> oversized(ampforge::Lv2Worker::kRingBytes + 8, 0x5a);
    CHECK(worker.schedule(static_cast<uint32_t>(oversized.size()), oversized.data()) ==
          LV2_WORKER_ERR_NO_SPACE);
    // Nothing was accepted, so nothing counts as scheduled.
    CHECK(worker.requestsScheduled() == 0);

    // The rejection left the ring untouched: a fitting message still goes through.
    CHECK(scheduleInt(worker, 7) == LV2_WORKER_SUCCESS);
    CHECK(worker.requestsScheduled() == 1);

    worker.stop();

    // The other boundary of the same ring: a record that wraps around the end of
    // the buffer. A 256 KB ring never wraps in a test this short, so drive
    // ByteRing directly with a 64-byte one - 40-byte records (8-byte header +
    // 32-byte payload, already a multiple of the 8-byte alignment) put the second
    // record across the wrap.
    ampforge::ByteRing ring;
    ring.allocate(64);
    CHECK(ring.capacity() == 64);

    std::vector<uint8_t> first(32);
    std::vector<uint8_t> second(32);
    for (size_t i = 0; i < first.size(); ++i)
    {
        first[i] = static_cast<uint8_t>(i);
        second[i] = static_cast<uint8_t>(255 - i);
    }

    CHECK(ring.push(first.data(), static_cast<uint32_t>(first.size())));
    uint32_t size = 0;
    const uint8_t* payload = ring.peek(size);
    CHECK(payload != nullptr && size == first.size() &&
          std::memcmp(payload, first.data(), first.size()) == 0);
    // The payload of every record must land on an 8-byte boundary, so a plugin
    // may cast it straight to its own message struct: that is what the padding in
    // the record header buys.
    CHECK(reinterpret_cast<uintptr_t>(payload) % ampforge::ByteRing::kAlignment == 0);
    ring.release();

    CHECK(ring.push(second.data(), static_cast<uint32_t>(second.size())));
    payload = ring.peek(size);  // stitched out of the wrapped halves
    CHECK(payload != nullptr && size == second.size() &&
          std::memcmp(payload, second.data(), second.size()) == 0);
    CHECK(reinterpret_cast<uintptr_t>(payload) % ampforge::ByteRing::kAlignment == 0);
    ring.release();

    CHECK(ring.peek(size) == nullptr);  // both records consumed

    // reset() drops what is queued but keeps the capacity, which is what lets a
    // restart reuse the same allocation (Lv2Worker::start()).
    CHECK(ring.push(first.data(), static_cast<uint32_t>(first.size())));
    ring.reset();
    CHECK(ring.peek(size) == nullptr);
    CHECK(ring.capacity() == 64);
    CHECK(ring.push(second.data(), static_cast<uint32_t>(second.size())));
    payload = ring.peek(size);
    CHECK(payload != nullptr && size == second.size() &&
          std::memcmp(payload, second.data(), second.size()) == 0);
    ring.release();
}

// A size field that cannot describe a record is not trusted: it is the length a
// plugin's work() would memcpy with, so a bogus one is dropped with the ring
// rather than handed out.
AMPFORGE_TEST(lv2_worker_corrupt_size_is_dropped)
{
    ampforge::ByteRing ring;
    ring.allocate(64);
    CHECK(ring.capacity() == 64);

    // Two 24-byte records (8-byte header + 16-byte payload) both fit, so the
    // oldest one is still queued when the second goes in behind it.
    std::vector<uint8_t> first(16, 0x11);
    std::vector<uint8_t> second(16, 0x22);
    CHECK(ring.push(first.data(), static_cast<uint32_t>(first.size())));
    CHECK(ring.push(second.data(), static_cast<uint32_t>(second.size())));

    // Corrupt the oldest record's size field where it lies: the payload peek()
    // hands out starts exactly kHeaderBytes after it, so that pointer is the way
    // into the ring from a test - the same "drive ByteRing directly" trick the
    // wrap case above uses, with a const_cast on top because peek() only lends
    // out a read-only view. Nothing past the end of the buffer is touched: the
    // header of the first record is the buffer's first byte.
    uint32_t size = 0;
    const uint8_t* payload = ring.peek(size);
    CHECK(payload != nullptr && size == first.size());

    // One byte more than the biggest payload the ring can hold, i.e. the first
    // length that is impossible rather than merely large.
    const uint32_t bogus =
        static_cast<uint32_t>(ring.capacity() - ampforge::ByteRing::kHeaderBytes + 1);
    uint8_t* sizeField = const_cast<uint8_t*>(payload) - ampforge::ByteRing::kHeaderBytes;
    std::memcpy(sizeField, &bogus, sizeof bogus);

    // The record cannot be legitimate - its payload would end past the buffer -
    // so the whole ring goes and nothing is handed out.
    CHECK(ring.peek(size) == nullptr);

    // Dropped, not wedged: the counters are back at "empty", so a fresh push is
    // readable again straight away.
    CHECK(ring.push(second.data(), static_cast<uint32_t>(second.size())));
    payload = ring.peek(size);
    CHECK(payload != nullptr && size == second.size());
    CHECK(payload != nullptr &&
          std::memcmp(payload, second.data(), second.size()) == 0);
    ring.release();
    CHECK(ring.peek(size) == nullptr);

    // An empty ring has no size field to validate, and reset() keeps that true.
    ring.reset();
    CHECK(ring.peek(size) == nullptr);
}

// No interface means no thread, and the audio-thread entry points stay harmless.
AMPFORGE_TEST(lv2_worker_without_interface_is_inert)
{
    ampforge::Lv2Worker worker;
    worker.start(nullptr, nullptr);
    CHECK(!worker.isRunning());

    // A host that accepted work:schedule must not report failure just because
    // the plugin turned out to have no worker: the message is accepted and
    // dropped, per the extension's "will be handled eventually" contract.
    const int32_t value = 3;
    CHECK(worker.schedule(sizeof value, &value) == LV2_WORKER_SUCCESS);
    CHECK(worker.requestsScheduled() == 0);

    worker.endCycle();  // no interface: must be a no-op, not a crash
    CHECK(worker.responsesDelivered() == 0);

    worker.stop();
    worker.stop();
    CHECK(!worker.isRunning());
}

// stop() joins and can be repeated; the audio-thread paths stay inert afterwards.
AMPFORGE_TEST(lv2_worker_stop_is_idempotent)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kFakeInterface, &fake);
    CHECK(worker.isRunning());

    worker.stop();
    CHECK(!worker.isRunning());
    worker.stop();  // second stop must not deadlock or double-join
    CHECK(!worker.isRunning());

    // A run() racing the shutdown still calls these: they must not crash and
    // must not reach into the plugin any more. schedule() no longer answers
    // "success" here: the worker is gone, so a success would promise a delivery
    // that cannot happen.
    CHECK(scheduleInt(worker, 99) == LV2_WORKER_ERR_UNKNOWN);
    worker.endCycle();
    worker.endCycle();

    const FakeWorker::Snapshot seen = fake.snapshot();
    CHECK(seen.workCalls == 0);  // no thread left to run work()
    CHECK(seen.responseCalls == 0);
    CHECK(worker.responsesDelivered() == 0);
}

// FIFO, both ways: several requests keep their order through the rings.
AMPFORGE_TEST(lv2_worker_many_messages_preserve_order)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kFakeInterface, &fake);
    // Opt into the echo again: without it work() never answers, so there would be
    // nothing to check the order of on the way back.
    worker.setResponseFn(&loopbackResponse, nullptr);

    constexpr int32_t kMessageCount = 50;
    for (int32_t i = 0; i < kMessageCount; ++i)
        CHECK(scheduleInt(worker, i) == LV2_WORKER_SUCCESS);
    CHECK(worker.requestsScheduled() == static_cast<uint32_t>(kMessageCount));

    CHECK(pumpUntil(worker, [&fake] {
        return fake.snapshot().responseCalls == static_cast<int>(kMessageCount);
    }));

    const FakeWorker::Snapshot seen = fake.snapshot();
    CHECK(seen.workCalls == static_cast<int>(kMessageCount));
    CHECK(seen.responseCalls == static_cast<int>(kMessageCount));
    CHECK(worker.responsesDelivered() == static_cast<uint32_t>(kMessageCount));

    CHECK(seen.responses.size() == static_cast<size_t>(kMessageCount));
    for (int32_t i = 0; i < kMessageCount; ++i)
    {
        const size_t index = static_cast<size_t>(i);
        CHECK(index < seen.responses.size() && seen.responses[index] == i);
    }

    worker.stop();
}

// A plugin that answers through respond(): work_response() must see the plugin's
// own bytes, not an echo of the request. This is the path real plugins take, and
// the one the round-trip test's ResponseFn hook deliberately bypasses.
AMPFORGE_TEST(lv2_worker_responds_through_respond_callback)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kRespondingInterface, &fake);

    constexpr int32_t kScheduled = 7;
    CHECK(scheduleInt(worker, kScheduled) == LV2_WORKER_SUCCESS);
    CHECK(pumpUntil(worker, [&fake] { return fake.snapshot().responseCalls > 0; }));

    const FakeWorker::Snapshot seen = fake.snapshot();
    CHECK(seen.workCalls == 1);
    CHECK(seen.responseCalls == 1);
    CHECK(worker.responsesDelivered() == 1);

    // The transformed value, i.e. exactly what respond() was handed - if the
    // request were looped back this would be kScheduled instead.
    CHECK(seen.responses.size() == 1 && seen.responses[0] == kScheduled + kRespondDelta);

    // Exactly one response, closed by exactly one end_run(): the respond() path
    // must not produce a second, ResponseFn-less "reply" on top of it.
    const size_t responsePos = seen.order.find('r');
    CHECK(responsePos != std::string::npos);
    CHECK(countFrom(seen.order, 0, 'r') == 1);
    CHECK(countFrom(seen.order, responsePos + 1, 'e') == 1);

    worker.stop();
}

// A refused message is dropped and counted, but it does not change what the host
// calls: worker.h requires end_run() after every run(), and a plugin cannot act
// on a request it is never told was dropped, so there is nothing to retry.
AMPFORGE_TEST(lv2_worker_failed_work_still_ends_run)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kFailingInterface, &fake);

    CHECK(scheduleInt(worker, 5) == LV2_WORKER_SUCCESS);
    CHECK(worker.requestsScheduled() == 1);

    // Wait for the refusal without calling endCycle() at all, so the cycle below
    // is unambiguously "the one that saw the failure".
    CHECK(waitUntil([&worker] { return worker.failedWork() == 1; }));

    const FakeWorker::Snapshot failed = fake.snapshot();
    CHECK(failed.workCalls == 1);
    CHECK(failed.responseCalls == 0);  // no invented reply for a refused message
    CHECK(worker.responsesDelivered() == 0);
    CHECK(failed.endRunCalls == 0);  // no cycle has run yet

    // The cycle that sees the failure still closes the run. The previous contract
    // was the opposite (end_run() suppressed for one cycle); it bought nothing,
    // because the plugin is never told the request was dropped, and it risked
    // losing the state publication end_run() exists for.
    worker.endCycle();
    CHECK(fake.snapshot().endRunCalls == 1);

    worker.endCycle();
    CHECK(fake.snapshot().endRunCalls == 2);

    // The count is a report, not an input: it survived both cycles.
    CHECK(worker.failedWork() == 1);
    CHECK(fake.snapshot().order == "wee");

    worker.stop();
}

// worker.h: end_run() MUST be called after every run(), responses or not - it is
// where a plugin publishes state it computed on the worker thread.
AMPFORGE_TEST(lv2_worker_end_run_after_quiet_cycle)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kFakeInterface, &fake);
    CHECK(fake.snapshot().endRunCalls == 0);

    worker.endCycle();
    CHECK(fake.snapshot().endRunCalls == 1);
    worker.endCycle();
    CHECK(fake.snapshot().endRunCalls == 2);

    // Nothing was scheduled and nothing came back: end_run() is all that happened.
    const FakeWorker::Snapshot seen = fake.snapshot();
    CHECK(seen.workCalls == 0);
    CHECK(seen.responseCalls == 0);
    CHECK(worker.responsesDelivered() == 0);
    CHECK(seen.order == "ee");

    worker.stop();
}

// stop() means "nothing will ever run this", so schedule() says so instead of
// reporting the success it reports when the plugin simply has no worker.
AMPFORGE_TEST(lv2_worker_rejects_after_stop)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kFakeInterface, &fake);
    CHECK(scheduleInt(worker, 11) == LV2_WORKER_SUCCESS);
    const uint32_t scheduledBeforeStop = worker.requestsScheduled();
    CHECK(scheduledBeforeStop == 1);
    worker.stop();

    CHECK(scheduleInt(worker, 12) == LV2_WORKER_ERR_UNKNOWN);
    CHECK(worker.requestsScheduled() == scheduledBeforeStop);

    // The documented exception, unchanged: a plugin with no worker at all still
    // gets SUCCESS and its message dropped, because the alternative is a plugin
    // retrying a request that can never be accepted. Such a message is not
    // counted either - there is no thread that could ever pick it up.
    worker.start(nullptr, nullptr);
    CHECK(!worker.isRunning());
    const int32_t value = 13;
    CHECK(worker.schedule(sizeof value, &value) == LV2_WORKER_SUCCESS);
    CHECK(worker.requestsScheduled() == 0);

    worker.stop();
}

// A restart reuses the ring allocation and only resets the counters, so a message
// queued before stop() can never resurface in the next generation.
AMPFORGE_TEST(lv2_worker_restart_does_not_leak)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kFakeInterface, &fake);

    CHECK(scheduleInt(worker, 100) == LV2_WORKER_SUCCESS);
    worker.stop();  // whether work() got to it before the join does not matter

    worker.start(&kFakeInterface, &fake);
    worker.setResponseFn(&loopbackResponse, nullptr);
    const FakeWorker::Snapshot baseline = fake.snapshot();

    // Several full cycles after the restart: the pre-restart message is invisible
    // (its record was dropped by the reset), so it produces no work(), no
    // work_response() and no delivery - not even if work() already ran on it
    // before the join, because its response went with the same reset.
    for (int i = 0; i < 20; ++i)
    {
        worker.endCycle();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const FakeWorker::Snapshot quiet = fake.snapshot();
    CHECK(quiet.workCalls == baseline.workCalls);
    CHECK(quiet.responseCalls == 0);
    CHECK(worker.responsesDelivered() == 0);

    // The same allocation still works, and is still single-producer: one message
    // scheduled after the restart is delivered exactly once.
    constexpr int32_t kAfterRestart = 7;
    CHECK(scheduleInt(worker, kAfterRestart) == LV2_WORKER_SUCCESS);
    CHECK(pumpUntil(worker, [&fake] { return fake.snapshot().responseCalls == 1; }));

    const FakeWorker::Snapshot seen = fake.snapshot();
    CHECK(seen.workCalls == baseline.workCalls + 1);
    CHECK(!seen.inputs.empty() && seen.inputs.back() == kAfterRestart);
    CHECK(seen.responses.size() == 1 && seen.responses[0] == kAfterRestart);
    CHECK(countFrom(seen.order, 0, 'r') == 1);
    CHECK(worker.responsesDelivered() == 1);

    worker.stop();
}

// The default is "no response": a work() that returns SUCCESS without calling
// respond() must produce nothing, not an echo of the request. The old loopback
// handed plugins a reply they never wrote (1064 fabricated bytes measured on a
// real hexforge instance), and it is exactly what a ResponseFn opt-in is for.
AMPFORGE_TEST(lv2_worker_no_response_by_default)
{
    FakeWorker fake;
    ampforge::Lv2Worker worker;
    worker.start(&kFakeInterface, &fake);  // work() succeeds, never responds

    constexpr int32_t kScheduled = 3;
    CHECK(scheduleInt(worker, kScheduled) == LV2_WORKER_SUCCESS);

    // Enough cycles for the message to be worked and any response to come back;
    // work() being recorded is what proves the message really was processed.
    for (int i = 0; i < 10; ++i)
    {
        worker.endCycle();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const FakeWorker::Snapshot seen = fake.snapshot();
    CHECK(seen.workCalls == 1);
    CHECK(seen.responseCalls == 0);
    CHECK(seen.responses.empty());
    CHECK(worker.responsesDelivered() == 0);

    // The cycle still closed, though: end_run() is not tied to a response having
    // been sent.
    CHECK(seen.endRunCalls == 10);

    worker.stop();
}
