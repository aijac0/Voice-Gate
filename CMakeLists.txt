cmake_minimum_required(VERSION 3.20)
project(VoiceGate LANGUAGES CXX)

include(CTest)
option(VOICE_GATE_ENABLE_WEBRTC "Build the WebRTC Audio Processing integration" OFF)
option(VOICE_GATE_ENABLE_WEBRTC_MEDIA "Build the native WebRTC media transport" OFF)

add_library(voice_gate_audio
    src/audio/audio_backend.cpp
    src/audio/audio_pipeline.cpp
    src/audio/audio_queue.cpp
    src/audio/frame_assembler.cpp
    src/audio/frame_handoff.cpp
    src/audio/local_activation.cpp
    src/audio/pcm.cpp
)
target_include_directories(voice_gate_audio PUBLIC src)
target_compile_features(voice_gate_audio PUBLIC cxx_std_20)
set_target_properties(voice_gate_audio PROPERTIES CXX_EXTENSIONS OFF)

add_library(voice_gate_segment src/segment/segment_controller.cpp)
target_link_libraries(voice_gate_segment PUBLIC voice_gate_audio)
set_target_properties(voice_gate_segment PROPERTIES CXX_EXTENSIONS OFF)

if(VOICE_GATE_ENABLE_WEBRTC_MEDIA)
    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(CURL REQUIRED)
    find_package(LibDataChannel 0.24.6 EXACT CONFIG REQUIRED)
    find_package(Opus 1.6.1 EXACT CONFIG REQUIRED)
    find_package(nlohmann_json 3.12.0 EXACT CONFIG REQUIRED)
    add_library(voice_gate_webrtc_media src/transport/gpt_live.cpp src/transport/webrtc_peer.cpp)
    target_link_libraries(voice_gate_webrtc_media PUBLIC voice_gate_audio
                          PRIVATE LibDataChannel::LibDataChannel Opus::opus CURL::libcurl
                                  nlohmann_json::nlohmann_json)
    set_target_properties(voice_gate_webrtc_media PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(VOICE_GATE_ENABLE_WEBRTC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing-2=2.1)
    target_sources(voice_gate_audio PRIVATE src/audio/webrtc_processor.cpp)
    target_include_directories(voice_gate_audio SYSTEM PRIVATE "${WEBRTC_APM_INCLUDEDIR}")
    target_link_libraries(voice_gate_audio PRIVATE PkgConfig::WEBRTC_APM)
endif()

if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_library(CORE_AUDIO_FRAMEWORK CoreAudio REQUIRED)
    target_sources(voice_gate_audio PRIVATE src/audio/macos/core_audio_backend.cpp)
    target_link_libraries(voice_gate_audio PRIVATE "${CORE_AUDIO_FRAMEWORK}")

    add_executable(voice_gate_audio_probe tools/audio_probe.cpp)
    target_link_libraries(voice_gate_audio_probe PRIVATE voice_gate_audio)
    set_target_properties(voice_gate_audio_probe PROPERTIES CXX_EXTENSIONS OFF)
    if(VOICE_GATE_ENABLE_WEBRTC)
        find_library(CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
        target_compile_definitions(voice_gate_audio_probe PRIVATE VOICE_GATE_ENABLE_WEBRTC)
        target_link_libraries(voice_gate_audio_probe PRIVATE "${CORE_FOUNDATION_FRAMEWORK}")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(voice_gate_audio_queue_test tests/audio_queue_test.cpp)
    target_link_libraries(voice_gate_audio_queue_test PRIVATE voice_gate_audio)
    add_test(NAME audio_queue COMMAND voice_gate_audio_queue_test)

    add_executable(voice_gate_audio_pipeline_test tests/audio_pipeline_test.cpp)
    target_link_libraries(voice_gate_audio_pipeline_test PRIVATE voice_gate_audio)
    add_test(NAME audio_pipeline COMMAND voice_gate_audio_pipeline_test)

    add_executable(voice_gate_frame_assembler_test tests/frame_assembler_test.cpp)
    target_link_libraries(voice_gate_frame_assembler_test PRIVATE voice_gate_audio)
    add_test(NAME frame_assembler COMMAND voice_gate_frame_assembler_test)

    add_executable(voice_gate_frame_handoff_test tests/frame_handoff_test.cpp)
    target_link_libraries(voice_gate_frame_handoff_test PRIVATE voice_gate_audio)
    add_test(NAME frame_handoff COMMAND voice_gate_frame_handoff_test)

    add_executable(voice_gate_local_activation_test tests/local_activation_test.cpp)
    target_link_libraries(voice_gate_local_activation_test PRIVATE voice_gate_audio)
    add_test(NAME local_activation COMMAND voice_gate_local_activation_test)

    add_executable(voice_gate_segment_test tests/segment_controller_test.cpp)
    target_link_libraries(voice_gate_segment_test PRIVATE voice_gate_segment)
    add_test(NAME segment_controller COMMAND voice_gate_segment_test)

    if(VOICE_GATE_ENABLE_WEBRTC_MEDIA)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_executable(voice_gate_webrtc_loopback_test tests/webrtc_loopback_test.cpp)
        target_link_libraries(voice_gate_webrtc_loopback_test PRIVATE voice_gate_webrtc_media
                              voice_gate_segment LibDataChannel::LibDataChannel)
        add_test(NAME webrtc_loopback COMMAND voice_gate_webrtc_loopback_test)
        set_tests_properties(webrtc_loopback PROPERTIES TIMEOUT 45)

        add_executable(voice_gate_signaling_client_test tests/signaling_client_test.cpp)
        target_link_libraries(voice_gate_signaling_client_test PRIVATE voice_gate_webrtc_media)
        add_test(NAME signaling_client
                 COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/server/run_client_test.py"
                         $<TARGET_FILE:voice_gate_signaling_client_test>)
        set_tests_properties(signaling_client PROPERTIES TIMEOUT 30)
    endif()

    if(VOICE_GATE_ENABLE_WEBRTC)
        add_executable(voice_gate_webrtc_test tests/webrtc_processor_test.cpp)
        target_link_libraries(voice_gate_webrtc_test PRIVATE voice_gate_audio)
        add_test(NAME webrtc_processor COMMAND voice_gate_webrtc_test)
    endif()
endif()
