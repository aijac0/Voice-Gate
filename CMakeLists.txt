cmake_minimum_required(VERSION 3.20)
project(VoiceGate LANGUAGES CXX)

include(CTest)

add_library(voice_gate_audio src/audio/audio_backend.cpp)
target_include_directories(voice_gate_audio PUBLIC src)
target_compile_features(voice_gate_audio PUBLIC cxx_std_20)
set_target_properties(voice_gate_audio PROPERTIES CXX_EXTENSIONS OFF)
