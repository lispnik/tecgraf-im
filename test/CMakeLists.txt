# Test suite. Reached from the top-level CMakeLists only when BUILD_TESTING
# is ON, so a plain library build stays unaffected.
#
# doctest is vendored under doctest/ (single header + the upstream CMake
# integration scripts) rather than fetched, so the Windows job does not need
# another vcpkg port and no CI job needs network access at configure time.

include(${CMAKE_CURRENT_SOURCE_DIR}/doctest/doctest.cmake)

# ----------------------------------------------------------------------
# Unit tests
# ----------------------------------------------------------------------

add_executable(im_tests
    test_main.cpp
    test_attrib.cpp
    test_bin.cpp
    test_binfile.cpp
    test_convert.cpp
    test_format.cpp
    test_misc.cpp
    test_datatype.cpp)

# Capture is an optional add-on too, and unlike the others its tests are worth
# having even where the backend is the do-nothing stub: the device-list
# contract is the same on every platform, and the case that matters most --
# the pixel conversion -- runs on synthetic buffers with no camera at all.
if(TARGET im_capture)
    target_sources(im_tests PRIVATE test_capture.cpp)
    target_link_libraries(im_tests PRIVATE im_capture)
endif()

# The HEIF driver is an optional add-on, so its tests compile only when it is
# built. Everything else in im_tests is unconditional.
if(TARGET im_heif)
    target_sources(im_tests PRIVATE test_heif.cpp)
    target_link_libraries(im_tests PRIVATE im_heif)
endif()

# Likewise for libim_process (IM_BUILD_PROCESS). Deliberately the plain
# variant and not im_process_omp: the point operations hand a thread index to
# their callbacks, so the OpenMP build would make call counts and visit order
# depend on the host's core count.
if(TARGET im_process)
    target_sources(im_tests PRIVATE test_process.cpp test_process2.cpp
                                   test_plus.cpp test_golden.cpp
                                   test_transform.cpp)
    target_link_libraries(im_tests PRIVATE im_process)
endif()

# The Fourier cases in test_transform.cpp need libim_fftw3, which is its own
# option (IM_BUILD_FFTW3). Everything else in that file is in libim_process
# and compiles either way, so the cases are guarded by a define rather than
# the whole file being conditional.
if(TARGET im_fftw3)
    target_link_libraries(im_tests PRIVATE im_fftw3)
    target_compile_definitions(im_tests PRIVATE IM_TEST_HAS_FFTW3)
endif()

target_include_directories(im_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(im_tests PRIVATE im)

# Scratch directory for the format round-trip test. Inside the build tree, so
# a test run never writes into the source checkout.
set(IM_TEST_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/test-output)
file(MAKE_DIRECTORY ${IM_TEST_OUTPUT_DIR})
target_compile_definitions(im_tests
    PRIVATE IM_TEST_OUTPUT_DIR="${IM_TEST_OUTPUT_DIR}"
            IM_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# Point LeakSanitizer at the suppression file whenever the sanitizers are on,
# so a local run behaves like CI. ci-sanitizers.yml sets LSAN_OPTIONS in the
# job environment, which does nothing for a developer following the flow
# documented in CLAUDE.md (-DIM_ENABLE_SANITIZERS=ON, then ctest): they hit
# the x265 encoder leak that test/lsan.supp exists to silence, and see red
# locally against green in CI with no hint why.
#
# Linux only, because Darwin has no LeakSanitizer -- ASan there aborts every
# binary the moment detect_leaks is asked for.
#
# Passed through doctest_discover_tests rather than set_tests_properties:
# discovery happens at build time, so the tests do not exist yet as far as
# this file is concerned. The workflow keeps its own LSAN_OPTIONS for runs
# that invoke the binary directly instead of going through ctest; the two
# name the same file.
set(_im_test_properties "")
if(IM_ENABLE_SANITIZERS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_im_test_properties PROPERTIES ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
endif()

# One CTest entry per TEST_CASE, so a crash isolates to a single reported
# test rather than an opaque whole-binary failure.
doctest_discover_tests(im_tests ${_im_test_properties})

# The forked crash probes that used to live here (test_attrib_isolated.cpp)
# are gone: the two cases they covered -- a negative element count reaching
# imAttribNode -- no longer crash, so they are ordinary assertions in
# test_attrib.cpp now. Reinstate the pattern if another bug turns up that can
# only be observed by watching a process die.

# ----------------------------------------------------------------------
# Lua binding smoke test
# ----------------------------------------------------------------------

if(IM_BUILD_LUA)
    # Prefer an interpreter next to the LUA_LIBRARY actually linked against,
    # so a build pinned to lua@5.4 is not smoke-tested with a stray lua5.5
    # from PATH.
    if(LUA_LIBRARY)
        get_filename_component(_lua_lib_dir "${LUA_LIBRARY}" DIRECTORY)
        get_filename_component(_lua_prefix "${_lua_lib_dir}" DIRECTORY)
    endif()

    find_program(IM_LUA_INTERPRETER
        NAMES lua lua5.4 lua5.3 lua5.2
        HINTS "${_lua_prefix}/bin")

    if(IM_LUA_INTERPRETER)
        # Modules beyond core imlua exist only when their add-on was built.
        set(_lua_modules "")
        foreach(m imlua_process imlua_jp2 imlua_fftw3 imlua_heif)
            if(TARGET ${m})
                list(APPEND _lua_modules ${m})
            endif()
        endforeach()

        add_test(NAME lua.smoke
            COMMAND ${IM_LUA_INTERPRETER}
                    ${CMAKE_CURRENT_SOURCE_DIR}/smoke.lua
                    $<TARGET_FILE_DIR:imlua>
                    ${CMAKE_SHARED_LIBRARY_SUFFIX}
                    ${_lua_modules})
    else()
        message(STATUS "Lua interpreter not found; skipping lua.smoke test")
    endif()
endif()
