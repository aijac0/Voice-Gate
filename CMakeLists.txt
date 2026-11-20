cmake_minimum_required(VERSION 3.20)
project(VoiceGate LANGUAGES CXX)

include(CTest)

add_library(voice_gate_audio src/audio/audio_backend.cpp src/audio/pcm.cpp)
target_include_directories(voice_gate_audio PUBLIC src)
target_compile_features(voice_gate_audio PUBLIC cxx_std_20)
set_target_properties(voice_gate_audio PROPERTIES CXX_EXTENSIONS OFF)

if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_library(CORE_AUDIO_FRAMEWORK CoreAudio REQUIRED)
    target_sources(voice_gate_audio PRIVATE src/audio/macos/core_audio_backend.cpp)
    target_link_libraries(voice_gate_audio PRIVATE "${CORE_AUDIO_FRAMEWORK}")

    add_executable(voice_gate_audio_probe tools/audio_probe.cpp)
    target_link_libraries(voice_gate_audio_probe PRIVATE voice_gate_audio)
    set_target_properties(voice_gate_audio_probe PROPERTIES CXX_EXTENSIONS OFF)
endif()
