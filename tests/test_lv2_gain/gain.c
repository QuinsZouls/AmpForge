#include "lv2/lv2.h"
#include <stdlib.h>

typedef struct {
    const float* in;
    float* out;
    const float* gain;
} TestGain;

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {
    (void)descriptor; (void)rate; (void)bundle_path; (void)features;
    TestGain* tg = (TestGain*)calloc(1, sizeof(TestGain));
    return (LV2_Handle)tg;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    TestGain* tg = (TestGain*)instance;
    switch (port) {
    case 0: tg->in = (const float*)data; break;
    case 1: tg->out = (float*)data; break;
    case 2: tg->gain = (const float*)data; break;
    default: break;
    }
}

static void run(LV2_Handle instance, uint32_t sample_count) {
    TestGain* tg = (TestGain*)instance;
    if (!tg || !tg->in || !tg->out || !tg->gain) return;
    const float g = *tg->gain;
    for (uint32_t i = 0; i < sample_count; ++i) {
        tg->out[i] = tg->in[i] * g;
    }
}

static void cleanup(LV2_Handle instance) {
    free(instance);
}

static const LV2_Descriptor descriptor = {
    "http://ampforge.org/plugins/test-gain",
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
