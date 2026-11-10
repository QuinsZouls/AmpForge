/*
 * AmpForge test LV2 plugin: the test_lv2_gain gain, plus an LV2 atom (event)
 * input port. It is the regression fixture for the host's atom-port support.
 *
 * WHY it looks like this - it is a regression fixture, so its oddities are
 * deliberate:
 *
 *  1. Port 3 is declared as atom:AtomPort with atom:bufferType atom:Sequence
 *     and atom:supports midi:MidiEvent (see atom.ttl). That is what makes the
 *     host classify it as an *event* port instead of audio or control, so
 *     discoverPorts() puts it in Lv2Plugin::atomInPorts and activate() has to
 *     hand it one of the kAtomBufferBytes sequence buffers.
 *
 *  2. run() only touches that buffer when the host made the port usable, i.e.
 *     all three of:
 *       - connect_port(3, ...) was called at all (otherwise the pointer here is
 *         null - the NBA-style "my event port is a null pointer" crash),
 *       - the buffer's atom.type resolves - through the very urid:map feature
 *         the host handed us in instantiate() - to atom:Sequence,
 *       - the sequence walks as an *empty* sequence with
 *         lv2_atom_sequence_begin()/lv2_atom_sequence_end(): begin == end and
 *         atom.size == sizeof(LV2_Atom_Sequence_Body). A host that leaves
 *         atom.size at 0 fails this (begin is body+1 while end is body), and so
 *         does one that writes the *output* capacity
 *         (kAtomBufferBytes - sizeof(LV2_Atom)) into an input sequence, which
 *         puts end far past begin.
 *     Any failure means "unusable" -> run() outputs SILENCE rather than
 *     passing the audio through. That is the point: a host that never connects
 *     or never initialises the event port gets silence, which a test notices
 *     instantly, instead of plausible-but-wrong audio.
 *
 *     Muting instead of refusing instantiate() is deliberate too: a missing
 *     urid:map does not make this plugin unloadable, it just makes the event
 *     port unusable - the "audio still flows" path a real plugin needs to
 *     survive on a host that does not support the extension.
 *
 *  3. The audio path is the plain gain from test_lv2_gain (default 1.0), so with
 *     a healthy event port out == in and any zero in the output is a
 *     mis-handled atom port, not a DSP surprise.
 *
 * Plain C, calloc/free only: it is loaded by the host under test, so it must not
 * pull in a C++ runtime of its own.
 */
#include "lv2/lv2.h"
#include "lv2/atom.h"
#include "lv2/atom-util.h"
#include "lv2/urid.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const float* in;
    float* out;
    const float* gain;

    /* The LV2_Atom_Sequence the host connected to the event port (index 3).
     * NULL means connect_port() was never called for it. */
    const LV2_Atom_Sequence* events;

    /* atom:Sequence as resolved by the host's urid:map, or 0 when the host
     * passed no map (or the map failed) - in which case atom.type cannot be
     * verified and the port counts as unusable. */
    LV2_URID sequence_urid;
} TestAtom;

/* True when the event port buffer is a walkable, empty atom:Sequence. */
static int atom_input_usable(const TestAtom* ta) {
    if (!ta->events) return 0;              /* connect_port(3, ...) never called */
    if (ta->sequence_urid == 0) return 0;   /* no urid:map -> type unresolvable */

    const LV2_Atom_Sequence* seq = ta->events;
    if (seq->atom.type != ta->sequence_urid) return 0;  /* not an atom:Sequence */

    /* This fixture expects zero events, so the body is just the header: anything
     * else (0 from an uninitialised buffer, the output-capacity value, a
     * half-written sequence) is not iterable the way the host contract says. */
    if (seq->atom.size != (uint32_t)sizeof(LV2_Atom_Sequence_Body)) return 0;

    const LV2_Atom_Event* first = lv2_atom_sequence_begin(&seq->body);
    const LV2_Atom_Event* last = lv2_atom_sequence_end(&seq->body, seq->atom.size);
    return first == last;
}

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {
    (void)descriptor; (void)rate; (void)bundle_path;

    const LV2_URID_Map* urid_map = NULL;
    // A host that supports no features passes a one-element array holding NULL,
    // so this loop simply does not run.
    if (features) {
        for (const LV2_Feature* const* f = features; *f; ++f) {
            if ((*f)->URI && strcmp((*f)->URI, LV2_URID__map) == 0) {
                urid_map = (const LV2_URID_Map*)(*f)->data;
                break;
            }
        }
    }

    TestAtom* ta = (TestAtom*)calloc(1, sizeof(TestAtom));
    if (!ta) return NULL;

    // Mapped here, not in run(): the map feature is not realtime safe, and the
    // URID has to be the one the host wrote into the buffer, so it must come
    // from this same map. No map means run() mutes (see atom_input_usable).
    if (urid_map && urid_map->map) {
        ta->sequence_urid = urid_map->map(urid_map->handle, LV2_ATOM__Sequence);
    }
    return (LV2_Handle)ta;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    TestAtom* ta = (TestAtom*)instance;
    if (!ta) return;
    switch (port) {
    case 0: ta->in = (const float*)data; break;
    case 1: ta->out = (float*)data; break;
    case 2: ta->gain = (const float*)data; break;
    case 3: ta->events = (const LV2_Atom_Sequence*)data; break;
    default: break;
    }
}

static void run(LV2_Handle instance, uint32_t sample_count) {
    TestAtom* ta = (TestAtom*)instance;
    if (!ta || !ta->in || !ta->out) return;

    // Silence when the event port is unusable - see the file comment. Writing
    // zeros (rather than leaving the buffer alone) makes the failing host
    // produce an obvious mismatch instead of whatever the output already held.
    if (!atom_input_usable(ta)) {
        for (uint32_t i = 0; i < sample_count; ++i) {
            ta->out[i] = 0.0f;
        }
        return;
    }

    const float g = ta->gain ? *ta->gain : 1.0f;
    for (uint32_t i = 0; i < sample_count; ++i) {
        ta->out[i] = ta->in[i] * g;
    }
}

static void cleanup(LV2_Handle instance) {
    free(instance);
}

static const LV2_Descriptor descriptor = {
    "http://ampforge.org/plugins/test-atom",
    instantiate,
    connect_port,
    NULL, // activate
    run,
    NULL, // deactivate
    cleanup,
    NULL  // extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}
