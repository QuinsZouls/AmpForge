# Unit tests for the header-only DSP in core/.
#
# These deliberately don't link DPF or nam_core: everything under core/ is
# header-only and free of plugin-framework dependencies, so the whole
# suite builds in a couple of seconds and runs anywhere a compiler does -
# including CI, where the point is to catch a broken DSP change long
# before anyone builds a plugin and loads it into a DAW.
#
# The exceptions are the plugin-hosting tests (test_plugin_node.cpp and the
# LV2/VST3/CLAP host ones below), which do need dlopen, X11 and the vendored
# CLAP/VST3 headers, and NamBlock, which needs neuralampmodelercore - see the
# second target at the bottom of this file for why that one is split off
# rather than merged in here. Everything else above stays as advertised.

add_executable(ampforge_tests
    main.cpp
    test_biquad.cpp
    test_blocks.cpp
    test_effectchain.cpp
    test_pitchdetector.cpp
    test_tonestack.cpp
    test_plugin_node.cpp
    test_lv2_host.cpp
    test_lv2_chunk_host.cpp
    test_lv2_atom_host.cpp
    test_lv2_rt.cpp
    test_lv2_worker.cpp
    test_clap_host.cpp
    test_vst3_host.cpp
    test_plugin_scanner.cpp
    test_plugin_config.cpp
    test_plugin_gui.cpp
    ../core/host/Lv2Plugin.cpp
    ../core/host/ClapPlugin.cpp
    ../core/host/Vst3Plugin.cpp
    ../core/host/PluginScanner.cpp
    ../core/host/PluginConfig.cpp
    ../core/host/PluginWindowX11.cpp
    "${vst3_pluginterfaces_SOURCE_DIR}/base/funknown.cpp")

target_include_directories(ampforge_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../core"
    "${CMAKE_CURRENT_SOURCE_DIR}/../core/host"
    "${clap_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/vst3_inc"
    "${neuralampmodelercore_SOURCE_DIR}/Dependencies/nlohmann")

target_compile_definitions(ampforge_tests PRIVATE
    TEST_LV2_BUNDLE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_gain.lv2/"
    TEST_LV2_CHUNK_BUNDLE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_chunk.lv2/"
    TEST_LV2_ATOM_BUNDLE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_atom.lv2/"
    TEST_CLAP_PLUGIN_PATH="${CMAKE_CURRENT_BINARY_DIR}/test_clap_gain.clap"
    TEST_VST3_PLUGIN_PATH="${CMAKE_CURRENT_BINARY_DIR}/test_vst3_gain.so"
    AMPFORGE_TEST_HOOKS)

find_package(X11 REQUIRED)
target_link_libraries(ampforge_tests PRIVATE ${CMAKE_DL_LIBS} ${X11_X11_LIB})

# Build sample LV2 test plugin
add_library(test_lv2_gain MODULE test_lv2_gain/gain.c)
target_include_directories(test_lv2_gain PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../core/host")
set_target_properties(test_lv2_gain PROPERTIES
    PREFIX ""
    OUTPUT_NAME "gain"
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_gain.lv2")

add_custom_command(TARGET test_lv2_gain POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lv2_gain/manifest.ttl"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lv2_gain/gain.ttl"
        "${CMAKE_CURRENT_BINARY_DIR}/test_gain.lv2/")

# Build sample LV2 test plugin that requires host features and a fixed-size block
add_library(test_lv2_chunk MODULE test_lv2_chunk/chunk.c)
target_include_directories(test_lv2_chunk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../core/host")
set_target_properties(test_lv2_chunk PROPERTIES
    PREFIX ""
    OUTPUT_NAME "chunk"
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_chunk.lv2")

add_custom_command(TARGET test_lv2_chunk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lv2_chunk/manifest.ttl"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lv2_chunk/chunk.ttl"
        "${CMAKE_CURRENT_BINARY_DIR}/test_chunk.lv2/")

# Build sample LV2 test plugin that declares an atom (event) port
add_library(test_lv2_atom MODULE test_lv2_atom/atom.c)
target_include_directories(test_lv2_atom PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../core/host")
set_target_properties(test_lv2_atom PROPERTIES
    PREFIX ""
    OUTPUT_NAME "atom"
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_atom.lv2")

# Both the binary and the TTLs go into the bundle: lilv reads manifest.ttl from
# the directory the .so lives in, so a missing copy means "plugin not found".
add_custom_command(TARGET test_lv2_atom POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lv2_atom/manifest.ttl"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_lv2_atom/atom.ttl"
        "${CMAKE_CURRENT_BINARY_DIR}/test_atom.lv2/")

# Build sample CLAP test plugin
add_library(test_clap_gain MODULE test_clap_gain/clap_gain.c)
target_include_directories(test_clap_gain PRIVATE "${clap_SOURCE_DIR}/include")
set_target_properties(test_clap_gain PROPERTIES
    PREFIX ""
    OUTPUT_NAME "test_clap_gain"
    SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Build sample VST3 test plugin
add_library(test_vst3_gain MODULE
    test_vst3_gain/vst3_gain.cpp
    "${vst3_pluginterfaces_SOURCE_DIR}/base/funknown.cpp")
target_include_directories(test_vst3_gain PRIVATE "${CMAKE_BINARY_DIR}/vst3_inc")
set_target_properties(test_vst3_gain PROPERTIES
    PREFIX ""
    OUTPUT_NAME "test_vst3_gain"
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_dependencies(ampforge_tests test_lv2_gain test_lv2_chunk test_lv2_atom test_clap_gain test_vst3_gain)

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
#  - ampforge_tests stays fast and keeps its cxx_std_17 (nam_core requires
#    C++20 - see the root CMakeLists.txt's comment on std::clamp - so merging
#    the two would raise the standard for every existing test file for no
#    reason), and it does not gain the network fetch nam_core needs to
#    configure. What it already links (dlopen, X11) comes with the plugin-host
#    tests that pre-date this target, not with anything here.
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
