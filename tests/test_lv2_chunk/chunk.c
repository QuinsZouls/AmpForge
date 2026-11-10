/*
 * AmpForge test LV2 plugin: a gain that can only process `maxBlockLength`
 * samples per run() call, that refuses to instantiate without the host features
 * a real-world plugin needs, and that uses the worker extension.
 *
 * WHY it looks like this - it is a regression fixture, so its oddities are
 * deliberate:
 *
 *  1. It *requires* urid:map, opts:options and buf-size:boundedBlockLength, the
 *     way DPF-based plugins do (they map every URI they touch and size their
 *     internal scratch buffers from buf-size#maxBlockLength). instantiate()
 *     therefore returns NULL unless the host hands over all three features *and*
 *     a maxBlockLength option of atom:Int type. A host that passes nullptr/short
 *     feature lists - as AmpForge did before - cannot even load this plugin,
 *     which is exactly the bug this test pins down.
 *
 *  2. It encodes the host contract "advertised maxBlockLength == largest run()
 *     call": maxBlockLength must be in (0, TEST_HOST_MAX_RUN_FRAMES], and run()
 *     (point 3) never processes more than that. The upper bound and the
 *     boundedBlockLength requirement are what make an *over*-advertising host
 *     fail loudly: a plugin is entitled to allocate against the advertised
 *     bound, so a host that promises more than it will ever pass (or passes
 *     more than it promised) is the bug, not this fixture. Missing or
 *     oddly-typed options are rejected for the same reason - a plugin cannot
 *     guess a buffer size it was never given.
 *
 *  3. run() mimics plugins with a fixed internal block size: they process at
 *     most `maxBlock` frames and write zeros for the rest, because their
 *     internal buffers are sized against the advertised maxBlockLength. A host
 *     that hands such a plugin a bigger block gets silence for the tail. Writing
 *     zeros (instead of leaving stale data) makes a non-chunking host fail
 *     loudly: the output simply does not match the input.
 *
 *  4. It also *requires* buf-size:sequenceSize and buf-size:nominalBlockLength,
 *     with the same strictness as maxBlockLength. An atom-aware plugin sizes its
 *     event buffers from sequenceSize (the number of bytes an LV2_Atom_Sequence
 *     port really has room for) and its typical working block from
 *     nominalBlockLength. A host that leaves them out makes the plugin guess -
 *     over-allocating, or writing past what the host's event buffer holds - and
 *     a host that advertises nominal > max makes it over-allocate against a
 *     promise that the very next run() breaks. Both are host bugs, so both are
 *     refused here instead of being clamped and hidden.
 *
 *  5. It requires the worker extension (work:schedule) and implements the plugin
 *     half of it: the first run() schedules one message, work() answers it
 *     through the respond callback it was handed, and work_response() checks the
 *     answer. That is what makes the host's worker wiring observable at all -
 *     without a plugin that both needs the feature and answers messages, a host
 *     that never starts its worker thread (or never pumps the responses) looks
 *     perfectly healthy. The host must advertise work:schedule, because a plugin
 *     that needs non-RT work would otherwise have to do it inline in run()
 *     (glitch) or refuse to load.
 *
 *     The values are fixed on purpose so the fixture stays deterministic: 40 is
 *     scheduled, work() replies with 41, and only 41 counts as a correct
 *     response. A host that fabricates a reply (echoing the request, or looping
 *     bytes back on its own) therefore produces 40, which is caught. Exactly one
 *     message is scheduled per instance, so the chunk tests below are unaffected.
 *
 *  6. FAILURE CHANNEL: if the round trip goes wrong (work() got the wrong
 *     payload, respond() failed, or a response arrived carrying the wrong value)
 *     run() outputs SILENCE and never recovers. Same trick as
 *     tests/test_lv2_atom/atom.c uses for its atom port: a host that mishandles
 *     the extension produces an obvious mismatch instead of plausible audio.
 *     The healthy path (out == in * gain) is untouched, which is what the
 *     existing chunk tests assert.
 *
 * Plain C, calloc/free only: it is loaded by the host under test, so it must not
 * pull in a C++ runtime of its own.
 */
#include "lv2/lv2.h"
#include "lv2/atom.h"
#include "lv2/buf-size.h"
#include "lv2/options.h"
#include "lv2/urid.h"
#include "lv2/worker.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The largest maxBlockLength this fixture accepts, i.e. the largest run() call
 * the host may ever make. Must stay in sync with Lv2Plugin::kMaxRunFrames
 * (core/host/Lv2Plugin.hpp): advertising more than this means the host promises
 * a fixed block bound it does not honour, and instantiate() fails. */
#define TEST_HOST_MAX_RUN_FRAMES 512

/* The worker round trip: what run() schedules, what work() replies with, and
 * therefore what work_response() insists on seeing. Distinctive on purpose - a
 * host that echoes the request back (40) instead of delivering the plugin's own
 * answer (41) is fabricating a response, and the fixture refuses it. */
#define TEST_WORK_PAYLOAD 40
#define TEST_WORK_REPLY   41

typedef struct {
    const float* in;
    float* out;
    const float* gain;

    /* Frames this instance is willing to process in a single run() call, taken
     * from the host's buf-size#maxBlockLength option. */
    int32_t max_block;

    /* The other two buf-size options, kept for the same reason: the fixture's
     * view of what the host promised has to be inspectable from a debugger (a
     * future assertion about event-buffer capacity would read sequence_size). */
    int32_t nominal_block;
    int32_t sequence_size;

    /* The guard in instantiate() is what enforces the feature; remembering it
     * here keeps the fixture's view of the host inspectable (a debugger, or a
     * future assertion about what this instance was handed). */
    int has_bounded_block_length;

    /* work:schedule, taken from the feature array in instantiate(). instantiate()
     * refuses to return an instance without it, so it is never NULL after that. */
    const LV2_Worker_Schedule* schedule;

    /* 0 = nothing scheduled yet, 1 = scheduled successfully, -1 = the host
     * refused the message. Never retried: one message per instance keeps the
     * fixture deterministic. */
    int work_requested;

    /* 0 = still waiting for work_response(), 1 = it carried TEST_WORK_REPLY,
     * -1 = the round trip is broken (wrong payload, respond() failed, or a
     * response with the wrong value). Once -1, run() is silent for good. */
    int response_ok;

    /* Successful round trips, i.e. work_response() calls that carried
     * TEST_WORK_REPLY. Should end up at exactly 1 per instance. */
    int round_trips;

    /* How many times the host closed a cycle. Observable proof that the host
     * calls end_run() after every run(), not only after a busy one. */
    int end_run_calls;
} TestChunk;

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {
    (void)descriptor; (void)rate; (void)bundle_path;

    const LV2_URID_Map* urid_map = NULL;
    const LV2_Options_Option* options = NULL;
    const LV2_Worker_Schedule* schedule = NULL;
    int has_bounded = 0;

    // The feature array is NULL terminated; a host that supports no features
    // passes a one-element array holding NULL, so this loop simply does not run.
    if (features) {
        for (const LV2_Feature* const* f = features; *f; ++f) {
            if (!(*f)->URI) continue;
            if (strcmp((*f)->URI, LV2_URID__map) == 0) {
                urid_map = (const LV2_URID_Map*)(*f)->data;
            } else if (strcmp((*f)->URI, LV2_OPTIONS__options) == 0) {
                options = (const LV2_Options_Option*)(*f)->data;
            } else if (strcmp((*f)->URI, LV2_BUF_SIZE__boundedBlockLength) == 0) {
                has_bounded = 1;
            } else if (strcmp((*f)->URI, LV2_WORKER__schedule) == 0) {
                schedule = (const LV2_Worker_Schedule*)(*f)->data;
            }
        }
    }

    // All of these are hard requirements here: this is what proves the host
    // supplies them. No map means no URI can be resolved, no options means no
    // block size, without boundedBlockLength a plugin may not assume any fixed
    // upper bound on a run() call at all - which is precisely the assumption this
    // fixture is built on - and without work:schedule a plugin that needs
    // non-RT work has nothing to hand it to (see point 5 in the file comment).
    if (!urid_map || !urid_map->map || !options || !has_bounded) return NULL;
    if (!schedule || !schedule->schedule_work) return NULL;

    const LV2_URID key_max_block =
        urid_map->map(urid_map->handle, LV2_BUF_SIZE__maxBlockLength);
    const LV2_URID key_nominal_block =
        urid_map->map(urid_map->handle, LV2_BUF_SIZE__nominalBlockLength);
    const LV2_URID key_sequence_size =
        urid_map->map(urid_map->handle, LV2_BUF_SIZE__sequenceSize);
    const LV2_URID type_int = urid_map->map(urid_map->handle, LV2_ATOM__Int);
    if (key_max_block == 0 || key_nominal_block == 0 || key_sequence_size == 0 ||
        type_int == 0) {
        return NULL;
    }

    // Options are a zeroed-entry-terminated array. Anything but an Int of the
    // right size is a host bug, and this fixture exists to catch those too - so
    // every one of the three options below is validated identically instead of
    // trusting whichever ones happen to be found.
    int32_t max_block = 0;
    int32_t nominal_block = 0;
    int32_t sequence_size = 0;
    int found = 0;

    for (const LV2_Options_Option* o = options; o->key != 0; ++o) {
        if (o->type != type_int || o->size != sizeof(int32_t) || !o->value) {
            // Only the three keys we read are held to this; a host is free to
            // put float options (sampleRate) in the same array.
            if (o->key != key_max_block && o->key != key_nominal_block &&
                o->key != key_sequence_size) {
                continue;
            }
            return NULL;
        }
        if (o->key == key_max_block) {
            max_block = *(const int32_t*)o->value;
            found |= 1;
        } else if (o->key == key_nominal_block) {
            nominal_block = *(const int32_t*)o->value;
            found |= 2;
        } else if (o->key == key_sequence_size) {
            sequence_size = *(const int32_t*)o->value;
            found |= 4;
        }
    }
    if (found != 7) return NULL;
    if (max_block <= 0) return NULL;

    // The host contract: "advertised maxBlockLength == largest run() call". An
    // advertisement above TEST_HOST_MAX_RUN_FRAMES is an over-promise (the host
    // will never send that many frames, so the plugin wastes that memory and,
    // worse, may size internal blocks the host then thrashes), so refuse it
    // rather than silently clamp and hide the host bug.
    if (max_block > TEST_HOST_MAX_RUN_FRAMES) return NULL;

    // Same contract for the other two: a sequence with no room is unusable, and
    // a nominal block outside (0, maxBlockLength] is a promise the next run()
    // breaks (0 or negative cannot be allocated against; above max is *more*
    // than the host will ever pass). Both are over/under-advertisements, i.e.
    // the host bug this fixture is here to catch.
    if (sequence_size <= 0) return NULL;
    if (nominal_block <= 0 || nominal_block > max_block) return NULL;

    TestChunk* tc = (TestChunk*)calloc(1, sizeof(TestChunk));
    if (!tc) return NULL;
    tc->max_block = max_block;
    tc->nominal_block = nominal_block;
    tc->sequence_size = sequence_size;
    tc->has_bounded_block_length = has_bounded;
    tc->schedule = schedule;
    return (LV2_Handle)tc;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    TestChunk* tc = (TestChunk*)instance;
    if (!tc) return;
    switch (port) {
    case 0: tc->in = (const float*)data; break;
    case 1: tc->out = (float*)data; break;
    case 2: tc->gain = (const float*)data; break;
    default: break;
    }
}

static void activate(LV2_Handle instance) { (void)instance; }

/* Plugin half of the worker extension. Runs on the host's background thread, so
 * it must not touch the audio buffers - it only reads its message and answers. */
static LV2_Worker_Status work(LV2_Handle instance,
                              LV2_Worker_Respond_Function respond,
                              LV2_Worker_Respond_Handle handle,
                              uint32_t size,
                              const void* data) {
    TestChunk* tc = (TestChunk*)instance;
    if (!tc) return LV2_WORKER_ERR_UNKNOWN;

    int32_t value = 0;
    if (!data || size != sizeof(int32_t)) {
        tc->response_ok = -1;
        return LV2_WORKER_ERR_UNKNOWN;
    }
    memcpy(&value, data, sizeof value);

    // Anything but the payload run() actually scheduled means the host mangled
    // the message on the way here - a broken round trip, not a valid request.
    if (value != TEST_WORK_PAYLOAD) {
        tc->response_ok = -1;
        return LV2_WORKER_ERR_UNKNOWN;
    }
    if (!respond) {
        tc->response_ok = -1;
        return LV2_WORKER_ERR_UNKNOWN;
    }

    const int32_t reply = value + 1;  // TEST_WORK_REPLY
    if (respond(handle, sizeof reply, &reply) != LV2_WORKER_SUCCESS) {
        tc->response_ok = -1;
        return LV2_WORKER_ERR_UNKNOWN;
    }
    return LV2_WORKER_SUCCESS;
}

/* Back on the audio thread, inside the host's cycle pump (run() context). Only a
 * response carrying TEST_WORK_REPLY - the value work() itself replied with -
 * counts: 40 here would mean the host echoed the request instead of delivering
 * the plugin's answer. */
static LV2_Worker_Status work_response(LV2_Handle instance, uint32_t size, const void* body) {
    TestChunk* tc = (TestChunk*)instance;
    if (!tc) return LV2_WORKER_ERR_UNKNOWN;

    int32_t value = 0;
    if (!body || size != sizeof(int32_t)) {
        tc->response_ok = -1;
        return LV2_WORKER_ERR_UNKNOWN;
    }
    memcpy(&value, body, sizeof value);

    if (value == TEST_WORK_REPLY) {
        tc->response_ok = 1;
        ++tc->round_trips;
        return LV2_WORKER_SUCCESS;
    }
    tc->response_ok = -1;  // wrong value, and never recovered from
    return LV2_WORKER_ERR_UNKNOWN;
}

/* LV2 asks the host to call this after every run(), whether or not a response
 * arrived, so counting the calls is the cheapest honest implementation. */
static LV2_Worker_Status end_run(LV2_Handle instance) {
    TestChunk* tc = (TestChunk*)instance;
    if (!tc) return LV2_WORKER_ERR_UNKNOWN;
    ++tc->end_run_calls;
    return LV2_WORKER_SUCCESS;
}

static void run(LV2_Handle instance, uint32_t sample_count) {
    TestChunk* tc = (TestChunk*)instance;
    if (!tc || !tc->in || !tc->out || !tc->gain) return;

    // Ask the host for the round trip exactly once, on the first block that
    // actually runs. Once per instance (never retried, even on failure) so the
    // chunk tests keep seeing the same audio on every block.
    if (sample_count > 0 && tc->work_requested == 0) {
        const int32_t payload = TEST_WORK_PAYLOAD;
        const LV2_Worker_Status status =
            tc->schedule->schedule_work(tc->schedule->handle, sizeof payload, &payload);
        tc->work_requested = (status == LV2_WORKER_SUCCESS) ? 1 : -1;
    }

    // Failure channel: a broken round trip mutes the output for good. Silence
    // (rather than the gain path) is what makes a host that mishandles the
    // worker extension fail loudly instead of sounding plausible.
    if (tc->response_ok < 0) {
        for (uint32_t i = 0; i < sample_count; ++i) {
            tc->out[i] = 0.0f;
        }
        return;
    }

    const float g = *tc->gain;
    const uint32_t n = (sample_count < (uint32_t)tc->max_block)
                           ? sample_count
                           : (uint32_t)tc->max_block;

    for (uint32_t i = 0; i < n; ++i) {
        tc->out[i] = tc->in[i] * g;
    }
    // The tail we refuse to process is silence, not leftover data: a host that
    // does not chunk run() then produces obvious gaps instead of plausible
    // (but stale) audio.
    for (uint32_t i = n; i < sample_count; ++i) {
        tc->out[i] = 0.0f;
    }
}

static void deactivate(LV2_Handle instance) { (void)instance; }

static void cleanup(LV2_Handle instance) {
    free(instance);
}

/* The interface the host resolves through extension_data(LV2_WORKER__interface)
 * and drives from its worker thread. Every member of this build's
 * LV2_Worker_Interface is set - a NULL end_run() would hide whether the host
 * closes its cycles, and NULL work_response() would leave the response ring with
 * nowhere to go. */
static const LV2_Worker_Interface worker_interface = {
    work,
    work_response,
    end_run
};

static const void* extension_data(const char* uri) {
    if (uri && strcmp(uri, LV2_WORKER__interface) == 0) {
        return &worker_interface;
    }
    return NULL;
}

static const LV2_Descriptor descriptor = {
    "http://ampforge.org/plugins/test-chunk",
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}
