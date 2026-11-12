/*
 * AmpForge test LV2 plugin: the test_lv2_gain gain, plus an LV2 atom (event)
 * input port (index 3) and an LV2 atom (event) output port (index 4). It is the
 * regression fixture for the host's atom-port support.
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
 *  4. Port 4 is an atom:AtomPort *output* (atom:bufferType atom:Sequence). When
 *     the host connected it, run() forges three patch:Set events into it
 *     (float, string and int values - see emit_test_events()). That is what
 *     gives the host's harvest path (slot memcpy, patch:Set parse, ring pushes,
 *     numeric writers) real work to do; without it that code never runs under a
 *     test. The events are written by hand (no forge), so run() stays
 *     allocation-free.
 *
 * Plain C, calloc/free only: it is loaded by the host under test, so it must not
 * pull in a C++ runtime of its own.
 */
#include "lv2/lv2.h"
#include "lv2/atom.h"
#include "lv2/atom-util.h"
#include "lv2/patch.h"
#include "lv2/urid.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

    /* The LV2_Atom_Sequence the host connected to the event OUTPUT port
     * (index 4). NULL until connect_port(4, ...) is called, in which case
     * run() simply emits no events. */
    LV2_Atom_Sequence* events_out;

    /* URIDs the forged patch:Set events are built from, mapped in
     * instantiate() - never in run(), see the comment there. */
    LV2_URID patch_set_urid;
    LV2_URID patch_property_urid;
    LV2_URID patch_value_urid;
    LV2_URID atom_float_urid;
    LV2_URID atom_string_urid;
    LV2_URID atom_int_urid;
    LV2_URID atom_object_urid;
    LV2_URID atom_urid_urid;
    LV2_URID level_urid;
    LV2_URID path_urid;
    LV2_URID meter_urid;
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
    //
    // The patch/atom URIDs below are mapped here for the same reason: run()
    // forges events with them and must not call into the map (mutex + string
    // allocation) on the audio thread.
    if (urid_map && urid_map->map) {
        ta->sequence_urid = urid_map->map(urid_map->handle, LV2_ATOM__Sequence);

        ta->patch_set_urid      = urid_map->map(urid_map->handle, LV2_PATCH__Set);
        ta->patch_property_urid = urid_map->map(urid_map->handle, LV2_PATCH__property);
        ta->patch_value_urid    = urid_map->map(urid_map->handle, LV2_PATCH__value);
        ta->atom_float_urid     = urid_map->map(urid_map->handle, LV2_ATOM__Float);
        ta->atom_string_urid    = urid_map->map(urid_map->handle, LV2_ATOM__String);
        ta->atom_int_urid       = urid_map->map(urid_map->handle, LV2_ATOM__Int);
        ta->atom_object_urid    = urid_map->map(urid_map->handle, LV2_ATOM__Object);
        ta->atom_urid_urid      = urid_map->map(urid_map->handle, LV2_ATOM__URID);
        ta->level_urid          = urid_map->map(urid_map->handle,
                                                "http://ampforge.org/plugins/test-atom#level");
        ta->path_urid           = urid_map->map(urid_map->handle,
                                                "http://ampforge.org/plugins/test-atom#path");
        ta->meter_urid          = urid_map->map(urid_map->handle,
                                                "http://ampforge.org/plugins/test-atom#meter");
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
    case 4: ta->events_out = (LV2_Atom_Sequence*)data; break;
    default: break;
    }
}

/* Append one patch:Set object carrying a single `key -> value` property.
 * `value_type`/`value_size`/`value` describe the property value atom. */
static int append_patch_set(LV2_Atom_Sequence* seq, uint32_t capacity, const TestAtom* ta,
                            LV2_URID key, LV2_URID value_type, uint32_t value_size,
                            const void* value) {
    /* patch:Set body: two properties, each padded to 64 bits. prop1 carries
     * an atom:URID (4 bytes), prop2 carries the value atom. The object body
     * size must be the TRUE padded layout - the host walks properties and
     * steps events with lv2_atom_pad_size(), so an under-declared atom.size
     * desynchronises the sequence walk. */
    const uint32_t prop1_size = ((uint32_t)sizeof(LV2_Atom_Property_Body) +
                                 (uint32_t)sizeof(LV2_URID) + 7u) & ~7u;
    const uint32_t prop2_size = ((uint32_t)sizeof(LV2_Atom_Property_Body) +
                                 value_size + 7u) & ~7u;
    const uint32_t obj_size = (uint32_t)sizeof(LV2_Atom_Object_Body) +
                              prop1_size + prop2_size;
    /* An event's slot is LV2_Atom_Event (time union + the atom header) plus
     * the padded object body. The atom header is ALREADY part of
     * LV2_Atom_Event - adding sizeof(LV2_Atom) again would make every slot
     * 8 bytes too wide and desynchronise lv2_atom_sequence_next(). */
    const uint32_t padded = (uint32_t)sizeof(LV2_Atom_Event) +
                            ((obj_size + 7u) & ~7u);
    if (capacity - seq->atom.size < padded) return 0;










    uint8_t* p = (uint8_t*)&seq->body + seq->atom.size;
    LV2_Atom_Event* ev = (LV2_Atom_Event*)p;
    ev->time.frames = 0;
    ev->body.size = obj_size;
    ev->body.type = ta->atom_object_urid;

    /* The object body starts right after the event's atom header, i.e. at
     * &ev->body + sizeof(LV2_Atom) - NOT at (ev + 1), which would skip the
     * header and shift every property by 8 bytes. */
    LV2_Atom_Object_Body* body = (LV2_Atom_Object_Body*)(&ev->body + 1);
    body->id = 0;
    body->otype = ta->patch_set_urid;

    /* patch:property -> atom:URID. NOTE: LV2_Atom_Property_Body already ends
     * with the value's LV2_Atom header, so the value bytes start at (prop + 1). */
    LV2_Atom_Property_Body* prop = (LV2_Atom_Property_Body*)(body + 1);
    prop->key = ta->patch_property_urid;
    prop->context = 0;
    prop->value.size = (uint32_t)sizeof(LV2_URID);
    prop->value.type = ta->atom_urid_urid;
    memcpy((uint8_t*)(prop + 1), &key, sizeof(LV2_URID));

    /* patch:value -> the value atom, padded to 64 bits like every property. */
    prop = (LV2_Atom_Property_Body*)((uint8_t*)prop +
                                     ((sizeof(LV2_Atom_Property_Body) + sizeof(LV2_URID) + 7u) & ~7u));
    prop->key = ta->patch_value_urid;
    prop->context = 0;
    prop->value.size = value_size;
    prop->value.type = value_type;
    if (value_size) memcpy((uint8_t*)(prop + 1), value, value_size);

    seq->atom.size += padded;
    return 1;
}

/* Emit three patch:Set events (float #level=0.5, string #path="/test/ir.wav",
 * int #meter=42) into the output sequence so the host's harvest path has real
 * work to do. Output-sequence contract: atom.size is the CAPACITY the host
 * published; this leaves it at the written data size.
 *
 * The events are written by hand rather than with lv2_atom_forge_*: the
 * layout below is explicit about the two things the host's sequence walk
 * depends on (each property padded to 64 bits, each event slot =
 * LV2_Atom_Event + padded body), which is exactly what this fixture exists
 * to pin. The forge would produce the same bytes; hand-writing keeps the
 * fixture dependency-free and makes the padding arithmetic visible. */
static void emit_test_events(TestAtom* ta) {
    LV2_Atom_Sequence* seq = ta->events_out;
    if (!seq || ta->sequence_urid == 0) return;
    if (seq->atom.type != ta->sequence_urid) return;
    if (seq->atom.size < (uint32_t)sizeof(LV2_Atom_Sequence_Body)) return;

    /* atom.size is the capacity the host published, not the used length: this
     * fixture writes the sequence from scratch every run(). */
    const uint32_t capacity = seq->atom.size;
    seq->atom.size = (uint32_t)sizeof(LV2_Atom_Sequence_Body);
    const float level = 0.5f;
    const int32_t meter = 42;
    static const char path[] = "/test/ir.wav";

    if (!append_patch_set(seq, capacity, ta, ta->level_urid,
                          ta->atom_float_urid, (uint32_t)sizeof(float), &level)) return;
    if (!append_patch_set(seq, capacity, ta, ta->path_urid,
                          ta->atom_string_urid, (uint32_t)sizeof(path), path)) return;
    if (!append_patch_set(seq, capacity, ta, ta->meter_urid,
                          ta->atom_int_urid, (uint32_t)sizeof(int32_t), &meter)) return;
}

static void run(LV2_Handle instance, uint32_t sample_count) {
    TestAtom* ta = (TestAtom*)instance;
    if (!ta || !ta->in || !ta->out) return;

    // Emit the harvest-path test events first: they must flow even when the
    // input carries injected events, which makes the input sequence non-empty
    // and sends the audio path down the mute branch below.
    emit_test_events(ta);

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
