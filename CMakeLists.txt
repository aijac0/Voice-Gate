cmake_minimum_required(VERSION 3.20)
project(VoiceGate LANGUAGES CXX)

include(CTest)
option(VOICE_GATE_ENABLE_WEBRTC "Build the WebRTC Audio Processing integration" OFF)

add_library(voice_gate_audio
    src/audio/audio_backend.cpp
    src/audio/audio_queue.cpp
    src/audio/frame_assembler.cpp
    src/audio/frame_handoff.cpp
    src/audio/pcm.cpp
)
target_include_directories(voice_gate_audio PUBLIC src)
target_compile_features(voice_gate_audio PUBLIC cxx_std_20)
set_target_properties(voice_gate_audio PROPERTIES CXX_EXTENSIONS OFF)

if(VOICE_GATE_ENABLE_WEBRTC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-2=2.1)
    pkg_get_variable(WEBRTC_APM_INCLUDE_ROOT webrtc-audio-processing-2 includedir)
    target_sources(voice_gate_audio PRIVATE src/audio/webrtc_processor.cpp)
    target_include_directories(voice_gate_audio SYSTEM PRIVATE "${WEBRTC_APM_INCLUDE_ROOT}")
    target_link_libraries(voice_gate_audio PRIVATE PkgConfig::WEBRTC_APM)
endif()

if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_library(CORE_AUDIO_FRAMEWORK CoreAudio REQUIRED)
    target_sources(voice_gate_audio PRIVATE src/audio/macos/core_audio_backend.cpp)
    target_link_libraries(voice_gate_audio PRIVATE "${CORE_AUDIO_FRAMEWORK}")

    add_executable(voice_gate_audio_probe tools/audio_probe.cpp)
    target_link_libraries(voice_gate_audio_probe PRIVATE voice_gate_audio)
    set_target_properties(voice_gate_audio_probe PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(BUILD_TESTING)
    add_executable(voice_gate_audio_queue_test tests/audio_queue_test.cpp)
    target_link_libraries(voice_gate_audio_queue_test PRIVATE voice_gate_audio)
    add_test(NAME audio_queue COMMAND voice_gate_audio_queue_test)

    add_executable(voice_gate_frame_assembler_test tests/frame_assembler_test.cpp)
    target_link_libraries(voice_gate_frame_assembler_test PRIVATE voice_gate_audio)
    add_test(NAME frame_assembler COMMAND voice_gate_frame_assembler_test)

    add_executable(voice_gate_frame_handoff_test tests/frame_handoff_test.cpp)
    target_link_libraries(voice_gate_frame_handoff_test PRIVATE voice_gate_audio)
    add_test(NAME frame_handoff COMMAND voice_gate_frame_handoff_test)

    if(VOICE_GATE_ENABLE_WEBRTC)
        add_executable(voice_gate_webrtc_test tests/webrtc_processor_test.cpp)
        target_link_libraries(voice_gate_webrtc_test PRIVATE voice_gate_audio)
        add_test(NAME webrtc_processor COMMAND voice_gate_webrtc_test)
    endif()
endif()
