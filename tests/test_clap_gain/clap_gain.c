#include <clap/clap.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t* host;
    double gain;
} MyClapPlugin;

static const clap_plugin_descriptor_t s_desc = {
    CLAP_VERSION,
    "org.ampforge.test-clap-gain",
    "AmpForge CLAP Test Gain",
    "AmpForge",
    "https://ampforge.org",
    "",
    "",
    "0.1.0",
    "A simple test gain plugin",
    (const char*[]) { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL }
};

static bool clap_init(const clap_plugin_t* plugin) {
    (void)plugin;
    return true;
}

static void clap_destroy(const clap_plugin_t* plugin) {
    free((void*)plugin);
}

static bool clap_activate(const clap_plugin_t* plugin,
                          double sample_rate,
                          uint32_t min_frames_count,
                          uint32_t max_frames_count) {
    (void)plugin; (void)sample_rate; (void)min_frames_count; (void)max_frames_count;
    return true;
}

static void clap_deactivate(const clap_plugin_t* plugin) {
    (void)plugin;
}

static bool clap_start_processing(const clap_plugin_t* plugin) {
    (void)plugin;
    return true;
}

static void clap_stop_processing(const clap_plugin_t* plugin) {
    (void)plugin;
}

static void clap_reset(const clap_plugin_t* plugin) {
    (void)plugin;
}

// Params extension
static uint32_t clap_params_count(const clap_plugin_t* plugin) {
    (void)plugin;
    return 1;
}

static bool clap_params_get_info(const clap_plugin_t* plugin, uint32_t index, clap_param_info_t* info) {
    (void)plugin;
    if (index != 0) return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    strncpy(info->name, "Gain", sizeof(info->name) - 1);
    info->min_value = 0.0;
    info->max_value = 4.0;
    info->default_value = 1.0;
    return true;
}

static bool clap_params_get_value(const clap_plugin_t* plugin, clap_id id, double* value) {
    MyClapPlugin* self = (MyClapPlugin*)plugin;
    if (id == 0) {
        *value = self->gain;
        return true;
    }
    return false;
}

static bool clap_params_value_to_text(const clap_plugin_t* plugin, clap_id id, double value, char* out, uint32_t out_len) {
    (void)plugin; (void)id; (void)value; (void)out; (void)out_len;
    return false;
}

static bool clap_params_text_to_value(const clap_plugin_t* plugin, clap_id id, const char* text, double* out_value) {
    (void)plugin; (void)id; (void)text; (void)out_value;
    return false;
}

static void clap_params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t* out) {
    (void)plugin; (void)in; (void)out;
}

static const clap_plugin_params_t s_plugin_params = {
    clap_params_count,
    clap_params_get_info,
    clap_params_get_value,
    clap_params_value_to_text,
    clap_params_text_to_value,
    clap_params_flush
};

// Audio ports extension
static uint32_t clap_audio_ports_count(const clap_plugin_t* plugin, bool is_input) {
    (void)plugin; (void)is_input;
    return 1;
}

static bool clap_audio_ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input, clap_audio_port_info_t* info) {
    (void)plugin;
    if (index != 0) return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
    strncpy(info->name, is_input ? "Input" : "Output", sizeof(info->name) - 1);
    info->channel_count = 1;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t s_plugin_audio_ports = {
    clap_audio_ports_count,
    clap_audio_ports_get
};

static const void* clap_get_extension(const clap_plugin_t* plugin, const char* id) {
    (void)plugin;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &s_plugin_params;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &s_plugin_audio_ports;
    return NULL;
}

static clap_process_status clap_process(const clap_plugin_t* plugin, const clap_process_t* process) {
    MyClapPlugin* self = (MyClapPlugin*)plugin;

    // Handle input parameter events
    if (process->in_events) {
        uint32_t num_events = process->in_events->size(process->in_events);
        for (uint32_t i = 0; i < num_events; ++i) {
            const clap_event_header_t* hdr = process->in_events->get(process->in_events, i);
            if (hdr && hdr->space_id == CLAP_CORE_EVENT_SPACE_ID && hdr->type == CLAP_EVENT_PARAM_VALUE) {
                const clap_event_param_value_t* ev = (const clap_event_param_value_t*)hdr;
                if (ev->param_id == 0) {
                    self->gain = ev->value;
                }
            }
        }
    }

    if (process->audio_inputs_count > 0 && process->audio_outputs_count > 0) {
        const float* in = process->audio_inputs[0].data32[0];
        float* out = process->audio_outputs[0].data32[0];
        const float g = (float)self->gain;
        for (uint32_t i = 0; i < process->frames_count; ++i) {
            out[i] = in[i] * g;
        }
    }

    return CLAP_PROCESS_CONTINUE;
}

static void clap_on_main_thread(const clap_plugin_t* plugin) {
    (void)plugin;
}

static const clap_plugin_t* create_plugin(const clap_plugin_factory_t* factory,
                                          const clap_host_t* host,
                                          const char* plugin_id) {
    (void)factory;
    if (strcmp(plugin_id, s_desc.id) != 0) return NULL;

    MyClapPlugin* p = (MyClapPlugin*)calloc(1, sizeof(MyClapPlugin));
    p->plugin.desc = &s_desc;
    p->plugin.plugin_data = p;
    p->plugin.init = clap_init;
    p->plugin.destroy = clap_destroy;
    p->plugin.activate = clap_activate;
    p->plugin.deactivate = clap_deactivate;
    p->plugin.start_processing = clap_start_processing;
    p->plugin.stop_processing = clap_stop_processing;
    p->plugin.reset = clap_reset;
    p->plugin.process = clap_process;
    p->plugin.get_extension = clap_get_extension;
    p->plugin.on_main_thread = clap_on_main_thread;

    p->host = host;
    p->gain = 1.0;

    return &p->plugin;
}

static uint32_t get_plugin_count(const clap_plugin_factory_t* factory) {
    (void)factory;
    return 1;
}

static const clap_plugin_descriptor_t* get_plugin_descriptor(const clap_plugin_factory_t* factory, uint32_t index) {
    (void)factory;
    return (index == 0) ? &s_desc : NULL;
}

static const clap_plugin_factory_t s_factory = {
    get_plugin_count,
    get_plugin_descriptor,
    create_plugin
};

static bool entry_init(const char* path) {
    (void)path;
    return true;
}

static void entry_deinit(void) {}

static const void* entry_get_factory(const char* factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) {
        return &s_factory;
    }
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION,
    entry_init,
    entry_deinit,
    entry_get_factory
};
