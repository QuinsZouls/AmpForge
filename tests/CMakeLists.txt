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
    test_plugin_node.cpp
    test_lv2_host.cpp
    test_clap_host.cpp
    test_vst3_host.cpp
    test_plugin_scanner.cpp
    test_plugin_config.cpp
    ../core/host/Lv2Plugin.cpp
    ../core/host/ClapPlugin.cpp
    ../core/host/Vst3Plugin.cpp
    ../core/host/PluginScanner.cpp
    ../core/host/PluginConfig.cpp
    "${vst3_pluginterfaces_SOURCE_DIR}/base/funknown.cpp")

target_include_directories(ampforge_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../core"
    "${CMAKE_CURRENT_SOURCE_DIR}/../core/host"
    "${clap_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/vst3_inc"
    "${neuralampmodelercore_SOURCE_DIR}/Dependencies/nlohmann")

target_compile_definitions(ampforge_tests PRIVATE
    TEST_LV2_BUNDLE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_gain.lv2/"
    TEST_CLAP_PLUGIN_PATH="${CMAKE_CURRENT_BINARY_DIR}/test_clap_gain.clap"
    TEST_VST3_PLUGIN_PATH="${CMAKE_CURRENT_BINARY_DIR}/test_vst3_gain.so")

target_link_libraries(ampforge_tests PRIVATE ${CMAKE_DL_LIBS})

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

add_dependencies(ampforge_tests test_lv2_gain test_clap_gain test_vst3_gain)

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
