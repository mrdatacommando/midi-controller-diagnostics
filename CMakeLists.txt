# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mark Van de Velde

cmake_minimum_required(VERSION 3.20)

project(MidiScope VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(MidiScope WIN32
    src/main.cpp
    src/midi_engine.cpp
    src/midi_decode.cpp
    src/log_writer.cpp
)

target_compile_definitions(MidiScope PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(MidiScope PRIVATE
    winmm       # MIDI input and output
    comctl32    # list views, status bar
    comdlg32    # save-file dialog
    shlwapi     # path helpers
)

if(MSVC)
    target_compile_options(MidiScope PRIVATE /W4 /permissive- /utf-8)
    # Static CRT: the result is one .exe that runs without the VC++ redist.
    set_property(TARGET MidiScope PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Console harness for the non-UI code. Passive: it listens but never transmits.
add_executable(MidiScopeSelfTest
    tests/selftest.cpp
    src/midi_engine.cpp
    src/midi_decode.cpp
    src/log_writer.cpp
)
target_compile_definitions(MidiScopeSelfTest PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(MidiScopeSelfTest PRIVATE winmm shlwapi)
if(MSVC)
    target_compile_options(MidiScopeSelfTest PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET MidiScopeSelfTest PROPERTY
                 MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()
add_test(NAME selftest COMMAND MidiScopeSelfTest)
