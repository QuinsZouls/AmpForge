#include "test_harness.hpp"

#include "NamBlock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

/*
 * Tests for NamBlock - the one block under core/ that isn't header-only: it
 * loads a model from a .nam file and runs audio through
 * NeuralAmpModelerCore, so covering it means linking nam_core (see
 * tests/CMakeLists.txt for why that is a second test target instead of more
 * files in ampforge_tests).
 *
 * Every fixture is generated at runtime into a temporary directory rather
 * than checked in as a binary .nam file. Two reasons: the on-disk format is
 * plain JSON (version / architecture / config / weights / sample_rate), so a
 * fixture written out here is readable next to the assertions that use it -
 * you can see what the model *does* instead of trusting a blob - and a
 * generated model can be given a deliberately trivial impulse response. The
 * models below are one-tap Linear filters (output = gain * input), which
 * makes every expected value in these tests something you can work out in
 * your head: what is under test here is NamBlock's batching, its A2
 * slimmable-size handling and its tone-stack/mix wiring, not NAM's own
 * arithmetic (that has its own tests upstream).
 */

namespace {

constexpr double kSampleRate = 48000.0;

// --- aligned allocation (portable form) -----------------------------------
//
// std::aligned_alloc is C11, not C++17: libstdc++ happens to expose it on
// Linux, MinGW's does not, which is exactly how the Windows CI build broke on
// it. Over-allocating and stashing the real pointer in the slot just before the
// aligned one is the portable form, and it keeps the pair honest - the aligned
// operators below allocate and free through this alone, so a pointer never
// crosses between it and plain malloc/free.
void* alignedAllocate(std::size_t size, std::size_t alignment)
{
    // Rounded up to a power of two and never smaller than a pointer: both the
    // slack and the mask below need that, and std::align_val_t is only ever a
    // power of two to begin with.
    std::size_t align = alignment;
    if (align < sizeof(void*))
        align = sizeof(void*);
    while (align & (align - 1))
        ++align;

    void* const raw = std::malloc(size + align - 1 + sizeof(void*));
    if (!raw)
        return nullptr;

    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    const std::uintptr_t aligned = (start + align - 1) & ~static_cast<std::uintptr_t>(align - 1);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void alignedFree(void* p)
{
    if (!p)
        return;
    std::free(reinterpret_cast<void**>(p)[-1]);
}

// --- allocation counting --------------------------------------------------
//
// Global operator new/delete are replaced in this translation unit so a test
// can ask "did the audio path allocate?" and get a number instead of an
// assumption (see namblock_process_block_does_not_allocate). This is the one
// test executable that links nam_core, so the override can't affect anything
// else.
//
// Deliberately a plain counter rather than a hook with conditions: it counts
// every allocation in this TU's binary, and the tests that care take a delta
// around the call they are interested in. Nothing in the harness allocates per
// CHECK, so the deltas are meaningful; the aligned forms are overridden too
// because Eigen and NAM's buffers go through them.
std::atomic<long> gAllocationCount{0};

} // namespace

// The replacement operators below are malloc-backed, which is the whole point
// (they are a counter, not a allocator), but gcc inlines them into every call
// site and then reports each `free()` in operator delete as freeing a pointer
// from `operator new` -Wmismatched-new-delete: 50-odd warnings, all false,
// all from this block. Silenced here rather than project-wide so the rest of
// the build keeps whatever diagnostics it has.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size)
{
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return operator new(size); }

void* operator new(std::size_t size, std::align_val_t alignment)
{
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
    // Always through alignedAllocate(), never through a plain malloc even when
    // the alignment would happen to be satisfied by one: the matching aligned
    // delete below has to be able to tell how the pointer was obtained, and the
    // only way to guarantee that is to keep the two forms symmetric.
    if (void* p = alignedAllocate(size ? size : 1, static_cast<std::size_t>(alignment)))
        return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return operator new(size, alignment);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { alignedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { alignedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { alignedFree(p); }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

long allocationCount() { return gAllocationCount.load(std::memory_order_relaxed); }

#if defined(AMPFORGE_COUNTS_MALLOC)
// Eigen (and therefore every NAM layer buffer) allocates with malloc, not
// operator new - see tests/CMakeLists.txt, which adds the --wrap flags that
// route these here. Counted into the same total, so the "did the audio path
// allocate?" assertions cover both.
//
// Only malloc is counted: calloc/realloc are wrapped so their bookkeeping stays
// consistent (a realloc that grows a buffer is an allocation, and Eigen's
// aligned path can realloc), but free is wrapped only to keep the pair
// symmetric - it never affects the count.
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __real_calloc(std::size_t, std::size_t);
extern "C" void* __real_realloc(void*, std::size_t);
extern "C" void  __real_free(void*);

extern "C" void* __wrap_malloc(std::size_t size)
{
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
    return __real_malloc(size);
}

extern "C" void* __wrap_calloc(std::size_t count, std::size_t size)
{
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
    return __real_calloc(count, size);
}

extern "C" void* __wrap_realloc(void* p, std::size_t size)
{
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
    return __real_realloc(p, size);
}

extern "C" void __wrap_free(void* p) { __real_free(p); }
#endif

// The tolerance for every "this constant should come out as that constant"
// check. It is not zero because of the tone stack: it sits in the signal path
// even at 0dB, and its four biquads are unity in *exact* arithmetic only.
// Both things cost accuracy here:
//  - their denominator nearly cancels at DC (1 + a1 + a2 is ~2e-4 of a0 for the
//    120 Hz LowShelf), so a coefficient rounding of one part in 1e8 comes out
//    as ~1e-4 relative error in the passband,
//  - the filters still have to settle: the poles sit at r ~ 0.99, i.e. a time
//    constant of ~90 frames at 48 kHz, so a block or two of constant input is
//    not yet the steady state.
// 1e-3 absolute on a 0.2 signal is a 0.5% window: comfortably above the ~3e-5
// measured error, and still 150x smaller than the 0.15 gap between the two A2
// submodels the tests below are telling apart.
constexpr double kConstantTolerance = 1e-3;

// --- .nam fixture generation ---------------------------------------------

// A one-tap Linear model: y[n] = gain * x[n].
//
// receptive_field 2 with weights {gain, 0} is NAM's on-disk way of spelling
// that. Linear stores the impulse response *reversed* (NAM/linear.cpp:
// _weight(i) = weights[receptive_field - 1 - i]), so the last weight is the
// tap applied to the newest input sample and the one before it multiplies
// the previous sample - hence {gain, 0} = "current sample only". bias false
// keeps the output exactly gain * x, with no constant term.
std::string linearModelJson(double gain)
{
    std::ostringstream json;
    json << R"({"version":"0.7.0","architecture":"Linear",)"
         << R"("config":{"receptive_field":2,"bias":false},)"
         << R"("weights":[)" << gain << R"(,0.0],"sample_rate":48000})";
    return json.str();
}

// An A2 "SlimmableContainer": one .nam file holding several model sizes, each
// with its own weights (that is the whole point of A2 - trade CPU for
// accuracy). `max_value` is *not* a gain: it is the upper bound of the
// slimmable-size range that selects this submodel (see
// ContainerModel::_get_index_for_slimmable_size), so the first submodel
// serves sizes below its max_value and the last one is the fallback.
//
// Two submodels, with deliberately different gains so a test can tell which
// one is running from the output alone:
//   size in [0.0, 0.5)  -> submodel 0, y = 0.5 * x
//   size in [0.5, 1.0]  -> submodel 1, y = 2.0 * x
std::string slimmableContainerJson(double smallGain, double largeGain)
{
    return R"({"version":"0.7.0","architecture":"SlimmableContainer",)"
           R"("config":{"submodels":[)"
               R"({"max_value":0.5,"model":)"
        + linearModelJson(smallGain) + R"(},)"
               R"({"max_value":1.0,"model":)"
        + linearModelJson(largeGain) + R"(}]},)"
           R"("weights":[],"sample_rate":48000})";
}

// A two-channel Linear model, i.e. a .nam file NAM will happily build - the
// format allows in_channels/out_channels > 1 (see NAM/linear.cpp's
// parse_config_json) - but which this block cannot run: it is a mono block and
// hands NAM one-element pointer arrays. Named with the shape it is, because the
// point of the test that uses it is that this must be *rejected* rather than
// "mostly work": passing a stereo model a mono buffer is a heap overflow, not a
// wrong-sounding capture.
std::string stereoLinearModelJson(double gain)
{
    std::ostringstream json;
    // Note the weights are still only receptive_field long: NAM's Linear
    // requires exactly that many (NAM/linear.cpp), and the channel count is
    // what makes it stereo - each channel simply gets its own copy of the same
    // impulse response.
    json << R"({"version":"0.7.0","architecture":"Linear",)"
         << R"("config":{"receptive_field":2,"bias":false,)"
         << R"("in_channels":2,"out_channels":2},)"
         << R"("weights":[)" << gain << R"(,0.0],"sample_rate":48000})";
    return json.str();
}

// An A2 container whose *submodels* are stereo. This is the case a plain
// NumInputChannels() check cannot catch: ContainerModel's own DSP is built as
// DSP(1, 1, ...) whatever it holds (NAM/container.cpp) and forwards process()'s
// pointer arrays straight to the active submodel, so the container reports mono
// while the submodel it activates is not.
std::string containerWithStereoSubmodelJson()
{
    return R"({"version":"0.7.0","architecture":"SlimmableContainer",)"
           R"("config":{"submodels":[)"
               R"({"max_value":0.5,"model":)" + linearModelJson(1.0) + R"(},)"
               R"({"max_value":1.0,"model":)" + stereoLinearModelJson(1.0) + R"(}]},)"
           R"("weights":[],"sample_rate":48000})";
}

// Three submodels, one per breakpoint, with gains far enough apart that the
// output alone says which one ran:
//   size in [0.0, 0.25) -> 1.0x
//   size in [0.25, 0.5) -> 2.0x
//   size in [0.5, 1.0]  -> 4.0x
// Needed to test the *mid-range* mapping: with two submodels a request of 0.4
// and one of 0.6 can't be told apart from a broken snapQuality() that returns
// the upper bound, which is exactly the off-by-one that function exists to
// avoid.
std::string threeSubmodelContainerJson()
{
    return R"({"version":"0.7.0","architecture":"SlimmableContainer",)"
           R"("config":{"submodels":[)"
               R"({"max_value":0.25,"model":)" + linearModelJson(1.0) + R"(},)"
               R"({"max_value":0.5,"model":)"  + linearModelJson(2.0) + R"(},)"
               R"({"max_value":1.0,"model":)"  + linearModelJson(4.0) + R"(}]},)"
           R"("weights":[],"sample_rate":48000})";
}

std::filesystem::path makeUniqueTempDir()
{
    // Timestamp plus a counter: ctest may run test executables in parallel on
    // CI, and a fixed directory name would let one run's cleanup delete
    // another run's fixtures (or fail on an already-existing path).
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();

    std::ostringstream name;
    name << "ampforge_nam_tests_" << stamp << '_' << counter++;

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name.str();
    std::filesystem::create_directories(dir);
    return dir;
}

// Owns a temporary directory of generated .nam files for the duration of one
// test and removes it afterwards, including on an early return. Nothing is
// written into the source tree, so a failed run leaves no fixtures behind for
// the next run to trip over (and .gitignore needs no entry for them).
class NamFixtures
{
public:
    NamFixtures()
    : dir(makeUniqueTempDir())
    {
    }

    ~NamFixtures()
    {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }

    NamFixtures(const NamFixtures&) = delete;
    NamFixtures& operator=(const NamFixtures&) = delete;

    // Writes `contents` to `fileName` and returns the full path, ready to hand
    // to NamBlock::loadModel().
    std::string write(const std::string& fileName, const std::string& contents) const
    {
        const std::filesystem::path path = dir / fileName;
        std::ofstream file(path, std::ios::binary);
        file << contents;
        return path.string();
    }

    // A1 identity model: output == input, so a failed expectation can't be
    // blamed on the model's own gain.
    std::string a1Identity() const { return write("a1_identity.nam", linearModelJson(1.0)); }

    // A2 model whose smallest submodel halves the signal and whose full-size
    // one doubles it - see slimmableContainerJson().
    std::string a2Slimmable() const { return write("a2_slimmable.nam", slimmableContainerJson(0.5, 2.0)); }

    // A2 model with three submodels (1x / 2x / 4x) - see
    // threeSubmodelContainerJson().
    std::string a2ThreeSizes() const { return write("a2_three.nam", threeSubmodelContainerJson()); }

    // A two-channel capture, which loadModel() must refuse.
    std::string a1Stereo() const { return write("a1_stereo.nam", stereoLinearModelJson(1.0)); }

    // A container whose second submodel is stereo - also refused, even though
    // the container itself reports 1 in / 1 out.
    std::string a2StereoSubmodel() const
    {
        return write("a2_stereo_submodel.nam", containerWithStereoSubmodelJson());
    }

private:
    std::filesystem::path dir;
};

// --- driving a block ------------------------------------------------------

// A steady sine, the probe signal every "does the audio actually change" test
// uses. Amplitude stays well under 1.0 so nothing clips on the way out.
std::vector<float> sine(std::size_t frames, double frequencyHz, float amplitude = 0.25f)
{
    const double omega = 2.0 * M_PI * frequencyHz / kSampleRate;

    std::vector<float> signal(frames);
    for (std::size_t n = 0; n < frames; ++n)
        signal[n] = static_cast<float>(amplitude * std::sin(omega * static_cast<double>(n)));
    return signal;
}

// Drives the block through processBlock() in `chunk`-frame calls (the last one
// possibly shorter) and returns everything it produced.
std::vector<float> processInChunks(ampforge::NamBlock& block, const std::vector<float>& input, std::size_t chunk)
{
    std::vector<float> output(input.size());
    for (std::size_t done = 0; done < input.size(); done += chunk)
    {
        const std::size_t frames = std::min(chunk, input.size() - done);
        block.processBlock(input.data() + done, output.data() + done, static_cast<uint32_t>(frames));
    }
    return output;
}

// The same input, one frame at a time through processSample() - the path a
// host that ignores processBlock() still takes.
std::vector<float> processPerSample(ampforge::NamBlock& block, const std::vector<float>& input)
{
    std::vector<float> output(input.size());
    for (std::size_t i = 0; i < input.size(); ++i)
        output[i] = block.processSample(input[i]);
    return output;
}

// Largest absolute difference between two equally long buffers. Used instead
// of CHECK_NEAR in a loop so a mismatch reports one failure with the worst
// sample and its index, rather than hundreds of failures.
float maxDifference(const std::vector<float>& a, const std::vector<float>& b, std::size_t* worstIndex = nullptr)
{
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
    {
        const float difference = std::fabs(a[i] - b[i]);
        if (difference > worst || std::isnan(difference))
        {
            worst = difference;
            if (worstIndex != nullptr)
                *worstIndex = i;
        }
    }
    return worst;
}

// RMS of the second half of a buffer: the first half is treated as the settle
// window, so a filter's start-up transient can't skew the measurement.
double secondHalfRMS(const std::vector<float>& signal)
{
    const std::size_t start = signal.size() / 2;

    double sum = 0.0;
    for (std::size_t i = start; i < signal.size(); ++i)
        sum += static_cast<double>(signal[i]) * static_cast<double>(signal[i]);

    const std::size_t count = signal.size() - start;
    return (count == 0) ? 0.0 : std::sqrt(sum / static_cast<double>(count));
}

} // namespace

// --- a) A1 model loading --------------------------------------------------

AMPFORGE_TEST(namblock_a1_model_loads_and_reports_its_architecture)
{
    // The empty starting state first: a block with no .nam file has to say so,
    // because the UI uses exactly these three answers to decide whether to
    // draw a model name, a Quality control and a sample-rate warning.
    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(!block.hasModel());
    CHECK(block.getArchitecture().empty());
    CHECK_NEAR(block.getModelSampleRate(), -1.0, 1e-9); // NAM's "unknown" marker
    CHECK(!block.hasSlimmableModel());
    CHECK(block.getLastError().empty());

    NamFixtures fixtures;
    CHECK(block.loadModel(fixtures.a1Identity()));

    CHECK(block.hasModel());
    // The architecture string comes from the file's own header, which is how
    // the UI can confirm an A2 capture really loaded as A2 instead of
    // silently falling back to A1 parsing.
    CHECK(block.getArchitecture() == "Linear");
    // A plain A1 Linear model does not implement nam::SlimmableModel, so the
    // Quality control must be reported as meaningless for it.
    CHECK(!block.hasSlimmableModel());
    CHECK_NEAR(block.getModelSampleRate(), 48000.0, 1e-9);
    CHECK(block.getLastError().empty());
}

// --- b) A2 model loading --------------------------------------------------

AMPFORGE_TEST(namblock_a2_model_loads_and_reports_slimmable)
{
    NamFixtures fixtures;

    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a2Slimmable()));

    CHECK(block.hasModel());
    CHECK(block.getArchitecture() == "SlimmableContainer");
    // The load path casts the model to nam::SlimmableModel once, off the audio
    // thread, and caches it - this is the flag that proves that cast found the
    // A2 container rather than an A1 model.
    CHECK(block.hasSlimmableModel());
    // The container's own declared rate, which is what the UI compares against
    // the host's to warn about a capture played back at the wrong rate.
    CHECK_NEAR(block.getModelSampleRate(), 48000.0, 1e-9);
    CHECK(block.getLastError().empty());
}

// --- c) A2 slimmable size actually switches submodels ---------------------

AMPFORGE_TEST(namblock_a2_quality_switches_submodel)
{
    NamFixtures fixtures;

    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a2Slimmable()));

    // Two blocks, so the measurement has a settle window. The biquads' poles
    // sit at r ~ 0.99 (a ~90-frame time constant at 48 kHz), so a single
    // 64-frame block is not yet steady state - the first block of constant
    // input is the transient, the second one is what gets asserted.
    constexpr std::size_t kFrames = 64;
    constexpr float kInput = 0.1f;
    std::vector<float> input(kFrames, kInput);

    auto runOneBlock = [&](ampforge::NamBlock& block) {
        std::vector<float> output(kFrames, 0.0f);
        block.processBlock(input.data(), output.data(), static_cast<uint32_t>(kFrames));
        return output.back();
    };

    // Quality 1.0 is the default and the full-size submodel (the one whose
    // max_value is 1.0), i.e. gain 2.0.
    block.setQuality(1.0f);
    runOneBlock(block);
    CHECK_NEAR(runOneBlock(block), 0.2, kConstantTolerance);

    // Quality 0.0 selects the smallest submodel (gain 0.5). The setter only
    // stores the request - it is applied by the audio thread at the next
    // block boundary, because the submodels are different networks and
    // splicing one into the middle of a buffer would be audible - so this
    // processBlock() is what performs the switch.
    block.setQuality(0.0f);
    CHECK_NEAR(runOneBlock(block), 0.05, kConstantTolerance);
    // ...and the one after it, now that the newly activated submodel and the
    // tone stack have both settled: a switch that only worked for one block
    // (or that left the block reading from a stale buffer) would show up here.
    CHECK_NEAR(runOneBlock(block), 0.05, kConstantTolerance);

    // ...and back, to prove the switch is not a one-way latch.
    block.setQuality(1.0f);
    runOneBlock(block);
    CHECK_NEAR(runOneBlock(block), 0.2, kConstantTolerance);
}

// --- d) block path vs per-sample path -------------------------------------

AMPFORGE_TEST(namblock_block_and_per_sample_paths_agree)
{
    NamFixtures fixtures;
    const std::string model = fixtures.a2Slimmable();

    const std::vector<float> input = sine(1000, 220.0);

    // Two instances, not one used twice: the model is stateful (Linear keeps
    // an input history, a WaveNet keeps per-layer ring buffers), so running
    // the same instance over the signal a second time would start from
    // wherever the first pass left it and the two outputs would differ for
    // reasons that have nothing to do with batching.
    ampforge::NamBlock blockDriven;
    blockDriven.setSampleRate(kSampleRate);
    CHECK(blockDriven.loadModel(model));

    ampforge::NamBlock sampleDriven;
    sampleDriven.setSampleRate(kSampleRate);
    CHECK(sampleDriven.loadModel(model));

    const std::vector<float> blockOutput = processInChunks(blockDriven, input, 64);
    const std::vector<float> sampleOutput = processPerSample(sampleDriven, input);

    CHECK(blockOutput.size() == input.size());
    CHECK(sampleOutput.size() == input.size());

    // processSample() is a 1-frame processBlock(), so the two paths run the
    // same trim/tone/mix arithmetic on the same values in the same order and
    // should agree to the last bit; the tolerance is only there so a future
    // model whose block maths re-associates the sum (or an FMA contraction
    // that differs between the two loop shapes) reports as a small numeric
    // difference rather than an instant failure.
    std::size_t worst = 0;
    const float difference = maxDifference(blockOutput, sampleOutput, &worst);
    CHECK(difference <= 1e-7f);
    if (difference > 1e-7f)
        std::printf("    worst sample %zu: block %g vs per-sample %g\n", worst,
                    static_cast<double>(blockOutput[worst]), static_cast<double>(sampleOutput[worst]));

    // A comparison of two silent buffers would pass for the wrong reason.
    CHECK(secondHalfRMS(blockOutput) > 0.01);
}

// --- e) a host buffer larger than maxBlockSize is chunked correctly -------

AMPFORGE_TEST(namblock_host_buffer_larger_than_max_block_size_is_chunked_correctly)
{
    // NAM's internal buffers are sized by Reset() from maxBlockSize, so
    // handing process() more frames than that in one call would write past
    // them; processBlock() therefore splits whatever the host gives it into
    // chunks of at most maxBlockSize. This test is what proves that split is
    // transparent (no duplicated or dropped frames at the seam).
    //
    // NamBlock's maxBlockSize only ever grows - setMaxBlockSize() ignores a
    // smaller value on purpose, so the scratch buffers can't shrink out from
    // under an in-flight processBlock() - which makes the default
    // kDefaultMaxBlockSize (512) the smallest chunk size any test can ask
    // for. A 1000-frame call therefore spans exactly two internal chunks,
    // 512 + 488, and the reference below is fed those same two sizes
    // explicitly.
    NamFixtures fixtures;
    const std::string model = fixtures.a2Slimmable();
    constexpr std::size_t kFrames = 1000;

    ampforge::NamBlock oneCall;
    oneCall.setSampleRate(kSampleRate);
    CHECK(oneCall.loadModel(model));

    ampforge::NamBlock splitCalls;
    splitCalls.setSampleRate(kSampleRate);
    CHECK(splitCalls.loadModel(model));

    const std::vector<float> input = sine(kFrames, 220.0);
    std::vector<float> oneCallOutput(kFrames, 0.0f);
    oneCall.processBlock(input.data(), oneCallOutput.data(), static_cast<uint32_t>(kFrames));

    // Fed as the host would feed a 512-frame buffer twice: the second call is
    // the 488-frame tail, so each call is a single chunk for the block and
    // nothing internal ever has to be split.
    const std::vector<float> splitOutput = processInChunks(splitCalls, input, 512);

    CHECK(oneCallOutput.size() == splitOutput.size());

    // Bit-for-bit: an unsplit call and the same frames delivered as separate
    // calls must produce identical audio, which is only true if the chunk
    // loop advances both its input and output pointers by the chunk it just
    // did (the seam between 512 and 488 is where that either works or does
    // not).
    std::size_t worst = 0;
    const float difference = maxDifference(oneCallOutput, splitOutput, &worst);
    CHECK(difference == 0.0f);
    if (difference != 0.0f)
        std::printf("    worst sample %zu: one call %g vs split calls %g\n", worst,
                    static_cast<double>(oneCallOutput[worst]), static_cast<double>(splitOutput[worst]));

    // Without this, a "no audio at all" implementation would pass the
    // comparison above.
    CHECK(secondHalfRMS(oneCallOutput) > 0.01);
}

// --- f) no model loaded = bare pass-through -------------------------------

AMPFORGE_TEST(namblock_without_a_model_is_a_bare_passthrough)
{
    // An empty NAM slot has to be transparent - not silent, and not coloured
    // by settings that happen to be sitting in the block. So every control
    // that *would* change the signal is deliberately moved off its default
    // here: if processBlock() applied any of them without a model, the
    // outputs below would not match the input.
    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    block.setInputTrim(20.0f);
    block.setOutputTrim(-20.0f);
    block.setMix(0.0f);
    block.setBassDB(12.0f);
    block.setTrebleDB(-12.0f);

    const std::vector<float> input = sine(256, 220.0);
    std::vector<float> output(input.size(), 0.0f);

    block.processBlock(input.data(), output.data(), static_cast<uint32_t>(input.size()));

    bool identical = true;
    for (std::size_t i = 0; i < input.size(); ++i)
        identical = identical && (output[i] == input[i]);
    CHECK(identical);

    // In place (in == out) too: hosts may hand the same buffer as both, and
    // the pass-through has to stay correct there as well.
    std::vector<float> inPlace = input;
    block.processBlock(inPlace.data(), inPlace.data(), static_cast<uint32_t>(inPlace.size()));
    CHECK(inPlace == input);

    // A zero-frame call is legal (AudioBlock::processBlock documents it) and
    // must not touch anything: the block has no model, but the check is that
    // the early-out happens before any buffer is written at all.
    std::vector<float> untouched = input;
    block.processBlock(untouched.data(), untouched.data(), 0);
    CHECK(untouched == input);
}

// --- g) Mix and the tone stack are actually wired into the audio ----------

AMPFORGE_TEST(namblock_mix_and_tone_stack_change_the_audio)
{
    NamFixtures fixtures;
    const std::string model = fixtures.a2Slimmable();

    // Mix at 0 must return the *dry* input. The full-size A2 submodel doubles
    // the signal, so "the model ran but its output was not mixed in" is a 2x
    // difference here rather than a rounding error - which is why the fixture
    // is a gain-2 model and not the identity one.
    {
        ampforge::NamBlock block;
        block.setSampleRate(kSampleRate);
        CHECK(block.loadModel(model));
        block.setQuality(1.0f);
        block.setMix(0.0f);

        const std::vector<float> input = sine(512, 220.0);
        const std::vector<float> output = processInChunks(block, input, 64);

        bool dry = true;
        for (std::size_t i = 0; i < input.size(); ++i)
            dry = dry && (output[i] == input[i]);
        CHECK(dry);
    }

    // Same as above but with Mix at 1: the wet path, i.e. 2x the input.
    {
        ampforge::NamBlock block;
        block.setSampleRate(kSampleRate);
        CHECK(block.loadModel(model));
        block.setQuality(1.0f);

        const std::vector<float> input = sine(512, 220.0);
        const std::vector<float> output = processInChunks(block, input, 64);

        CHECK_NEAR(secondHalfRMS(output) / secondHalfRMS(input), 2.0, 1e-3);
    }

    // The tone stack, measured on a 60 Hz sine - well below Bass's 120 Hz
    // shelf center, so a +12 dB Bass setting has to show up as a real boost
    // in the output. Two instances so neither run starts from the other's
    // filter history; each one's first half is discarded as the settle
    // window.
    struct ToneCase { float bassDB; };
    const ToneCase cases[] = { { 0.0f }, { 12.0f } };
    double rms[2] = { 0.0, 0.0 };

    for (int c = 0; c < 2; ++c)
    {
        ampforge::NamBlock block;
        block.setSampleRate(kSampleRate);
        CHECK(block.loadModel(model));
        block.setQuality(1.0f);
        block.setBassDB(cases[c].bassDB);

        const std::vector<float> input = sine(4800, 60.0);
        rms[c] = secondHalfRMS(processInChunks(block, input, 64));
    }

    CHECK(rms[0] > 0.01); // the flat case is not silent, so the ratio below means something

    // Expressed in dB: the boost must be plainly audible (well above the
    // ~0.1 dB a rounding difference would give) and still a shelf, not a
    // runaway (a broken Q or an unclamped gain would overshoot 12 dB).
    const double boostDB = 20.0 * std::log10(rms[1] / rms[0]);
    CHECK(boostDB > 6.0);
    CHECK(boostDB < 16.0);
}

// --- h) reset() clears the tone stack -------------------------------------

AMPFORGE_TEST(namblock_reset_clears_the_tone_stack)
{
    NamFixtures fixtures;

    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a1Identity()));
    // Boosted, so the filters have state worth holding: with all four bands
    // at 0 dB their ring-down would be almost nothing and the test would be
    // weak.
    block.setBassDB(12.0f);
    block.setTrebleDB(12.0f);

    // Push real audio through first, so the four biquads end up with a
    // non-zero history.
    const std::vector<float> signal = sine(2000, 220.0);
    const std::vector<float> beforeReset = processInChunks(block, signal, 64);
    CHECK(secondHalfRMS(beforeReset) > 0.01); // not vacuous: the block was doing something

    // reset() is what a block gets when it goes from disabled back to
    // enabled. It clears the tone stack only (the NAM model is deliberately
    // not re-Reset - see NamBlock::reset()), so silence in must give exact
    // silence out: if a stale filter history survived, its ring-down would
    // be audible here.
    block.reset();
    const std::vector<float> silence(512, 0.0f);
    const std::vector<float> afterReset = processInChunks(block, silence, 64);

    bool exactlySilent = true;
    for (float sample : afterReset)
        exactlySilent = exactlySilent && (sample == 0.0f);
    CHECK(exactlySilent);
}

// --- i) a failed load leaves the previous model in place ------------------

AMPFORGE_TEST(namblock_failed_load_keeps_the_previous_model)
{
    NamFixtures fixtures;

    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a1Identity()));
    CHECK(block.hasModel());
    CHECK(block.getArchitecture() == "Linear");

    // 1) Not JSON at all: the parser throws, loadModel() catches, and the
    // block must keep playing whatever it had before rather than dropping to
    // a pass-through in the middle of a session.
    const std::string garbage = fixtures.write("garbage.nam", "this is not a .nam file {{{{");
    CHECK(!block.loadModel(garbage));
    CHECK(block.hasModel());
    CHECK(block.getArchitecture() == "Linear");
    CHECK(!block.getLastError().empty());

    // 2) Valid JSON, but an architecture NAM has no parser registered for -
    // the other way a real file can be rejected (e.g. a capture from a newer
    // trainer). Same contract: the old model stays, the error fills in.
    const std::string wrongArchitecture = fixtures.write(
        "wrong_architecture.nam",
        R"({"version":"0.7.0","architecture":"NotAnArchitecture",)"
        R"("config":{},"weights":[],"sample_rate":48000})");
    CHECK(!block.loadModel(wrongArchitecture));
    CHECK(block.hasModel());
    CHECK(block.getArchitecture() == "Linear");
    CHECK(!block.getLastError().empty());

    // 3) A path that doesn't exist behaves the same way.
    CHECK(!block.loadModel((std::filesystem::temp_directory_path() / "ampforge-no-such-model.nam").string()));
    CHECK(block.hasModel());
    CHECK(block.getArchitecture() == "Linear");

    // And the surviving model still processes: a failed load must not leave
    // the audio path broken (a half-published buffer would silence the block
    // instead). The identity model makes the expected output the input.
    const std::vector<float> input = sine(256, 220.0);
    const std::vector<float> output = processInChunks(block, input, 64);
    CHECK(secondHalfRMS(output) > 0.01);
    CHECK(maxDifference(output, input) < 0.01f);
}

// --- j) a multi-channel .nam file is refused, not "mostly run" -------------
//
// NAM's format allows in_channels/out_channels > 1, and the parser builds such
// a model without complaint. NamBlock is mono - the chain is mono, and it hands
// NAM two one-element pointer arrays - so running one would have NAM read
// input[1] and write output[1] past the end of the block. That is heap
// corruption, so the only acceptable behaviour is to refuse the load and say
// why, exactly like any other unusable file.
AMPFORGE_TEST(namblock_rejects_multi_channel_model)
{
    NamFixtures fixtures;

    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a1Identity()));
    CHECK(block.getArchitecture() == "Linear");

    CHECK(!block.loadModel(fixtures.a1Stereo()));
    CHECK(!block.getLastError().empty());
    // The message has to be actionable, not just "failed": the whole reason
    // this is a refusal rather than a fallback is that the user has to know
    // the file is stereo.
    CHECK(block.getLastError().find("channel") != std::string::npos);

    // The container case: the file's *submodels* are stereo, which the
    // container's own NumInputChannels() cannot report (it is hard-coded to
    // 1/1). Without reading the raw JSON this loads and then stomps memory the
    // first time a stereo submodel is activated.
    CHECK(!block.loadModel(fixtures.a2StereoSubmodel()));
    CHECK(block.getLastError().find("channel") != std::string::npos);
    CHECK(block.hasModel());
    CHECK(block.getArchitecture() == "Linear");

    // Same contract as every other failed load: the previous model survives and
    // still processes (see namblock_failed_load_keeps_the_previous_model).
    CHECK(block.hasModel());
    CHECK(block.getArchitecture() == "Linear");
    const std::vector<float> input = sine(256, 220.0);
    const std::vector<float> output = processInChunks(block, input, 64);
    CHECK(maxDifference(output, input) < 0.01f);
}

// --- k) a block-size change must not lose the requested A2 size -----------
//
// Regression: warmAllSubmodels() walks every size (so every submodel gets sized
// and warmed) and leaves the container on the *last*, full-size one.
// setMaxBlockSize() calls it whenever the host announces a bigger buffer, so
// without re-applying the user's setting afterwards the model silently jumped
// to full size - and stayed there, because appliedQuality still equalled
// requestedQuality and the audio thread's comparison therefore never fired
// again. The Size knob then disagreed with what was actually running until the
// user moved it, which is the one thing a size control must never do.
AMPFORGE_TEST(namblock_block_size_growth_keeps_the_requested_size)
{
    NamFixtures fixtures;

    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a2Slimmable()));

    block.setQuality(0.0f); // smallest submodel: 0.5x
    const std::vector<float> input = sine(512, 220.0);
    const std::vector<float> before = processInChunks(block, input, 64);
    CHECK_NEAR(secondHalfRMS(before) / secondHalfRMS(input), 0.5, 1e-3);

    // What a host does when the user raises the buffer size. The audio thread
    // applies nothing here (setQuality() wasn't called again) - the model has
    // to come out of setMaxBlockSize() still on the small submodel.
    block.setMaxBlockSize(2048);

    const std::vector<float> after = processInChunks(block, input, 64);
    CHECK_NEAR(secondHalfRMS(after) / secondHalfRMS(input), 0.5, 1e-3);

    // And the size switch must still work *after* the growth, i.e. the
    // re-apply left both the container and appliedQuality in a sane state
    // rather than pinning everything to 0.5x.
    block.setQuality(1.0f);
    const std::vector<float> full = processInChunks(block, input, 64);
    CHECK_NEAR(secondHalfRMS(full) / secondHalfRMS(input), 2.0, 1e-3);
}

// --- l) the mid-range size mapping picks the right submodel ---------------
//
// snapQuality()'s job is that a request lands on the submodel it actually
// selects and that the value it returns selects that same submodel again (a
// submodel covers values *below* its breakpoint). Getting the bound wrong is an
// off-by-one that turns "a bit smaller" into "the smallest", so the mid-range
// case is tested explicitly, on a fixture whose three submodels are 4x apart.
AMPFORGE_TEST(namblock_quality_mid_range_selects_the_matching_submodel)
{
    NamFixtures fixtures;

    // 0.4 selects the second submodel (0.25 <= 0.4 < 0.5), which is 2x.
    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a2ThreeSizes()));
    block.setQuality(0.4f);

    const std::vector<float> input = sine(512, 220.0);
    CHECK_NEAR(secondHalfRMS(processInChunks(block, input, 64)) / secondHalfRMS(input), 2.0, 1e-3);

    // Just below the first breakpoint is the smallest submodel, just above the
    // second is the largest - the two boundaries the snap has to get right.
    block.setQuality(0.2f);
    CHECK_NEAR(secondHalfRMS(processInChunks(block, input, 64)) / secondHalfRMS(input), 1.0, 1e-3);

    block.setQuality(0.6f);
    CHECK_NEAR(secondHalfRMS(processInChunks(block, input, 64)) / secondHalfRMS(input), 4.0, 1e-3);

    // A request of exactly 1.0 is the last submodel's range (its breakpoint is
    // inclusive), not an out-of-range value.
    block.setQuality(1.0f);
    CHECK_NEAR(secondHalfRMS(processInChunks(block, input, 64)) / secondHalfRMS(input), 4.0, 1e-3);
}

// --- m) a size switch after a sample-rate change still works --------------
//
// setSampleRate() re-Resets the active model, and warmAllSubmodels() runs at the
// current maxBlockSize - so this is the case where the sample rate, the block
// size and the submodel bookkeeping all have to still agree afterwards. Cheap
// to get wrong (Reset() with a stale rate or size leaves the container's
// children sized for the old configuration) and silent when it is.
AMPFORGE_TEST(namblock_size_switch_works_after_a_sample_rate_change)
{
    NamFixtures fixtures;

    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a2Slimmable()));

    block.setSampleRate(44100.0);
    block.setQuality(0.0f);

    // 64-frame chunks at 220 Hz: the frequency is irrelevant to a one-tap
    // model, so the expected gain is the same as at 48 kHz.
    const std::vector<float> input = sine(512, 220.0);
    CHECK_NEAR(secondHalfRMS(processInChunks(block, input, 64)) / secondHalfRMS(input), 0.5, 1e-3);

    block.setQuality(1.0f);
    CHECK_NEAR(secondHalfRMS(processInChunks(block, input, 64)) / secondHalfRMS(input), 2.0, 1e-3);
}

// --- n) processBlock() does not allocate ---------------------------------
//
// The class comment promises the audio thread never allocates, and for the
// path that matters most - a pending A2 size switch - that promise depends on
// warmAllSubmodels() having sized *every* submodel up front. It is easy to
// break silently (a submodel whose Reset() doesn't size it, a switch that
// touches a cold one) and impossible to notice by listening, so it is counted
// rather than trusted: global operator new is overridden below and every call
// made while a flag is set is tallied.
//
// Both allocation routes are counted where the platform allows it: operator new
// here, and malloc/calloc/realloc through GNU ld's --wrap (see
// tests/CMakeLists.txt), because Eigen - which NAM uses for every layer buffer -
// allocates with malloc and would otherwise be invisible to this test. Without
// --wrap the assertions still hold for the std::vector-backed paths (nam::Buffer
// -based architectures) but not for WaveNet's Eigen buffers, which is why the
// wrap flags are there rather than optional.
//
// The point is not to prove the absence of allocation in some absolute sense,
// but to catch the regression where a size switch starts allocating NAM's
// buffers on the audio thread.
AMPFORGE_TEST(namblock_process_block_does_not_allocate)
{
    NamFixtures fixtures;

    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a2Slimmable()));

    const std::vector<float> input = sine(4096, 220.0);
    std::vector<float> output(input.size());

    // Counted from the very first block after the load, deliberately: the
    // promise is that loadModel() preallocated *everything* (nam::Buffer-based
    // architectures grow their buffers lazily inside process(), which is why
    // warmAllSubmodels() runs each submodel once), not just that the steady
    // state is clean.
    block.setQuality(0.0f); // a pending switch, applied at the next block boundary

    const long before = allocationCount();
    for (std::size_t done = 0; done < input.size(); done += 512)
        block.processBlock(input.data() + done, output.data() + done, 512);
    const long allocations = allocationCount() - before;

    CHECK(allocations == 0);

    // ...and the switch really did happen, so "no allocations" isn't the
    // trivial result of nothing having changed.
    CHECK_NEAR(secondHalfRMS(output) / secondHalfRMS(input), 0.5, 1e-2);

    // The other half: a *host* growing its buffer mid-session, then the user
    // moving Size. setMaxBlockSize() re-Resets and re-warms every submodel, so
    // the blocks that follow must still be allocation-free - that is the case
    // the warming exists for, and it is the one a future change to
    // warmAllSubmodels() would quietly break.
    block.setMaxBlockSize(4096);
    block.setQuality(1.0f);

    std::vector<float> bigInput = sine(4096, 220.0);
    std::vector<float> bigOutput(bigInput.size());

    const long beforeGrowth = allocationCount();
    block.processBlock(bigInput.data(), bigOutput.data(), 4096); // applies the pending switch
    for (int repeat = 0; repeat < 8; ++repeat)
        block.processBlock(bigInput.data(), bigOutput.data(), 4096);
    const long growthAllocations = allocationCount() - beforeGrowth;

    CHECK(growthAllocations == 0);
    CHECK_NEAR(secondHalfRMS(bigOutput) / secondHalfRMS(bigInput), 2.0, 1e-2);
}

// --- o) a container with an unusable submodel fails cleanly --------------
//
// An A2 file is a container of *nested* models, so it has more ways to be
// broken than a flat one: the top-level JSON can be perfectly valid and still
// contain a submodel whose own config is nonsense. NAM's container builds each
// submodel eagerly, so the failure happens inside the parse - the contract this
// pins down is that it surfaces as a plain failed load (lastError set, previous
// model still running) and not as an exception escaping loadModel() or as a
// half-built container being published.
AMPFORGE_TEST(namblock_container_with_a_broken_submodel_is_rejected)
{
    NamFixtures fixtures;

    ampforge::NamBlock block;
    block.setSampleRate(kSampleRate);
    CHECK(block.loadModel(fixtures.a1Identity()));
    CHECK(block.hasModel());

    // Two submodels: the first is fine, the second asks for an architecture
    // NAM has no parser for. A container is only as loadable as its worst
    // submodel, because nothing can run a size the container can't build.
    const std::string broken =
        R"({"version":"0.7.0","architecture":"SlimmableContainer",)"
        R"("config":{"submodels":[)"
            R"({"max_value":0.5,"model":)" + linearModelJson(1.0) + R"(},)"
            R"({"max_value":1.0,"model":)"
                R"({"version":"0.7.0","architecture":"NotAnArchitecture",)"
                R"("config":{},"weights":[],"sample_rate":48000})"
            R"(}]},)"
        R"("weights":[],"sample_rate":48000})";

    CHECK(!block.loadModel(fixtures.write("a2_broken_submodel.nam", broken)));
    CHECK(!block.getLastError().empty());

    // Nothing was published, so the block is still on the model it had.
    CHECK(block.hasModel());
    CHECK(block.getArchitecture() == "Linear");
    CHECK(!block.hasSlimmableModel());

    const std::vector<float> input = sine(256, 220.0);
    const std::vector<float> output = processInChunks(block, input, 64);
    CHECK(maxDifference(output, input) < 0.01f);

    // And the Size parameter is inert rather than dangerous: a value that no
    // loaded model understands must not switch anything or corrupt the block.
    block.setQuality(0.0f);
    const std::vector<float> afterQuality = processInChunks(block, input, 64);
    CHECK(maxDifference(afterQuality, input) < 0.01f);
}
