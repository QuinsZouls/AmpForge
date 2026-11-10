# Unit tests for the header-only DSP in core/.
#
# These deliberately don't link DPF or nam_core: everything under core/ is
# header-only and free of plugin-framework dependencies, so the whole
# suite builds in a couple of seconds and runs anywhere a compiler does -
# including CI, where the point is to catch a broken DSP change long
# before anyone builds a plugin and loads it into a DAW.

add_executable(ampforge_tests
    main.cpp
    test_biquad.cpp
    test_blocks.cpp
    test_effectchain.cpp
    test_pitchdetector.cpp
    test_tonestack.cpp)

target_include_directories(ampforge_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../core")

target_compile_features(ampforge_tests PRIVATE cxx_std_17)

# The Windows CI cross-compiles this and runs it under Wine, where the MinGW
# runtime DLLs (libgcc_s_seh-1.dll, libstdc++-6.dll) aren't on the loader
# path: the run dies with "Importing dlls ... failed" before a single test
# executes. Static linking makes the binary self-contained, so it runs from
# any directory - on the CI runner and on a plain Windows machine alike.
# Scoped to the tests on purpose: the plugins keep their normal dynamic
# runtime, which is what a host expects to load.
if (MINGW)
    target_link_options(ampforge_tests PRIVATE -static)
endif ()

add_test(NAME ampforge_core_tests COMMAND ampforge_tests)

# --- Second target for the one block that isn't header-only ---
#
# NamBlock (core/NamBlock.hpp) includes NeuralAmpModelerCore's own headers and
# links the nam_core OBJECT library, so its tests need Eigen + NAM's sources
# compiled and the network fetch of the NAM repo. That is exactly what
# ampforge_tests above is built to avoid, and the split is worth keeping:
#
#  - ampforge_tests stays a couple of seconds and dependency-free, which is
#    what makes `cmake -DAMPFORGE_BUILD_TESTS=ON && ctest` viable on a fresh
#    clone and what lets it keep its cxx_std_17 (nam_core requires C++20 - see
#    the root CMakeLists.txt's comment on std::clamp - so merging the two
#    would raise the standard for every existing test file for no reason).
#  - NamBlock is the block that changed the most recently (block batching, A2
#    slimmable size, tone stack), which is precisely the code that must not be
#    left uncovered just because covering it costs a fetch.
#
# The fixtures are generated as small JSON .nam files at run time (see
# test_namblock.cpp), so nothing binary lands in the repo.
add_executable(ampforge_nam_tests
    main.cpp
    test_namblock.cpp)

target_include_directories(ampforge_nam_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../core")

# Linking nam_core is what brings everything else with it, and doing it via the
# target rather than re-listing NAM's sources is deliberate: nam_core is an
# OBJECT library (see the root CMakeLists.txt), so its .o files are passed
# straight through into this executable - including every architecture's
# self-registering config parser, which a real static library would let the
# linker drop as "unreferenced" - and its PUBLIC include dirs,
# NAM_SAMPLE_FLOAT / NAM_ENABLE_A2_FAST defines and cxx_std_20 all reach the
# tests. That means these tests exercise the same float sample type and the
# same A2 fast path the plugin is built with, instead of a near-enough
# re-listing of the same flags.
target_link_libraries(ampforge_nam_tests PRIVATE nam_core)

# Same reasoning as ampforge_tests' own -static above: the Windows CI
# cross-compiles this and runs it under Wine, where the MinGW runtime DLLs
# aren't on the loader path.
if (MINGW)
    target_link_options(ampforge_nam_tests PRIVATE -static)
endif ()

# Count malloc() too, not just operator new: the allocation test in
# test_namblock.cpp is there to prove NamBlock's audio path never allocates, and
# Eigen - which NAM uses for every layer buffer - allocates through
# malloc/handmade_aligned_malloc, bypassing operator new entirely. Without this
# the test would be blind to exactly the WaveNet buffers it exists to watch.
#
# GNU ld's --wrap is what makes it possible: the test defines __wrap_malloc et
# al., which count and then call __real_malloc. Guarded because it is a GNU-ld
# feature (lld has it too; macOS's ld does not), and the test's assertions fall
# back to operator-new-only counting where it is missing - via the
# AMPFORGE_COUNTS_MALLOC define rather than a silent difference in coverage.
if (NOT APPLE AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_link_options(ampforge_nam_tests PRIVATE
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=free")
    target_compile_definitions(ampforge_nam_tests PRIVATE AMPFORGE_COUNTS_MALLOC)
endif ()

add_test(NAME ampforge_nam_tests COMMAND ampforge_nam_tests)
