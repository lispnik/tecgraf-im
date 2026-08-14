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
    test_datatype.cpp)

# The HEIF driver is an optional add-on, so its tests compile only when it is
# built. Everything else in im_tests is unconditional.
if(TARGET im_heif)
    target_sources(im_tests PRIVATE test_heif.cpp)
    target_link_libraries(im_tests PRIVATE im_heif)
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

# One CTest entry per TEST_CASE, so a crash isolates to a single reported
# test rather than an opaque whole-binary failure.
doctest_discover_tests(im_tests)

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
