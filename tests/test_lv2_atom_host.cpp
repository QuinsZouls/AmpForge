#include "test_harness.hpp"
#include "host/Lv2Plugin.hpp"
#include <cmath>
#include <memory>
#include <vector>

#ifndef TEST_LV2_ATOM_BUNDLE_DIR
# define TEST_LV2_ATOM_BUNDLE_DIR "./test_atom.lv2/"
#endif

// Regression fixture for the LV2 host's atom (event) port support - see
// tests/test_lv2_atom/atom.c and atom.ttl. The fixture declares an
// atom:AtomPort input at index 3 (atom:bufferType atom:Sequence,
// atom:supports midi:MidiEvent) and its run() writes SILENCE instead of passing
// the audio through unless the host:
//
//  1. connected the port at all (`connect_port(3, ...)`), i.e. it was discovered
//     as an event port and not skipped/left null - a null event pointer is the
//     classic "plugin segfaults, or mutes itself" host bug;
//  2. initialised the buffer as a *valid empty atom:Sequence*: atom.type is the
//     URID of atom:Sequence coming from the very urid:map the host handed the
//     plugin, and atom.size is sizeof(LV2_Atom_Sequence_Body) so that
//     lv2_atom_sequence_begin() == lv2_atom_sequence_end(). Zeroed memory (type
//     0, size 0) and the output-capacity size both fail this walk.
//
// Both the audio path and the assertions below are pass-through at the default
// gain of 1.0, so every zeroed sample points straight at the event port.
namespace {

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

} // namespace

AMPFORGE_TEST(lv2_host_connects_and_initializes_atom_ports)
{
    auto& loader = ampforge::LilvLoader::instance();
    if (!loader.isAvailable()) return;

    auto lv2 = std::make_unique<ampforge::Lv2Plugin>();
    // load() only resolves the descriptor and discovers the ports; the event
    // port buffers are created and connected in activate().
    const bool loaded = lv2->load(kAtomUri, TEST_LV2_ATOM_BUNDLE_DIR);
    CHECK(loaded);
    if (!loaded) return;

    const uint32_t frames = 512;  // == Lv2Plugin::kMaxRunFrames, so one chunk per block

    // Two chunks in a row: the host clears the input sequence before every
    // run(), and a host that cleared it wrongly (or re-initialised it as an
    // output sequence) would only show up from the second chunk on.
    auto processTwoBlocksAndAssert = [&]() {
        for (int block = 0; block < 2; ++block) {
            std::vector<float> in(frames), out(frames, kUnwritten);
            fillRamp(in);

            lv2->processBlock(in.data(), out.data(), frames);

            // Pass-through (gain defaults to 1.0): the fixture only emits this
            // when the event port was connected and holds a walkable empty
            // atom:Sequence. Silence here is the fixture's "unusable port" path.
            for (uint32_t i = 0; i < frames; ++i) {
                CHECK_NEAR(out[i], in[i], 1e-6f);
            }
        }
    };

    CHECK(lv2->activate(48000.0, 2048));
    processTwoBlocksAndAssert();

    // Re-activation rebuilds the host's buffers: deactivate() releases the old
    // ones (and the plugin instance they were connected to), so the second
    // instance only sees a usable event port if activate() allocates and
    // re-connects the sequence buffers again. The fixture mutes otherwise, which
    // makes a missing re-connect a plain sample mismatch below.
    lv2->deactivate();
    CHECK(lv2->activate(48000.0, 2048));
    processTwoBlocksAndAssert();
}
