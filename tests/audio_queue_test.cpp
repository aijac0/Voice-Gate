#include "audio/audio_queue.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

using namespace voice_gate::audio;
using namespace std::chrono_literals;

int main()
{
    AudioQueue reference({48000, 2, SampleEncoding::signed16}, 8, 1, 12);
    std::array<std::int16_t, 3> left{123, -42, 0};
    std::array<std::int16_t, 3> right{-19, 422, 0};
    const std::array buffers{
        BufferView<std::byte>{std::as_writable_bytes(std::span(left)), 1},
        BufferView<std::byte>{std::as_writable_bytes(std::span(right)), 1}};
    if (!reference.push(OutputBlock{buffers, 3, 101ms}) ||
        reference.push(OutputBlock{buffers, 3, 102ms})) {
        std::cerr << "A full queue must preserve its unread block.\n";
        return 1;
    }
    left.fill(0);
    right.fill(0);
    const std::array<std::int16_t, 6> expected{123, -19, -42, 422, 0, 0};
    const auto block = reference.front();
    if (!block || block->frames != 3 || block->generation != 12 ||
        block->first_sample != 101ms || block->samples.size() != sizeof(expected) ||
        std::memcmp(block->samples.data(), expected.data(), sizeof(expected)) != 0) {
        std::cerr << "Playback reference must preserve the exact submitted samples and silence.\n";
        return 1;
    }
    reference.pop();
    const std::array<std::int16_t, 4> microphone{3, -2, 0, 1};
    const std::array input{
        BufferView<const std::byte>{std::as_bytes(std::span(microphone)), 1}};
    AudioQueue capture({44100, 1, SampleEncoding::signed16}, 4, 2, 13);
    if (reference.front() || capture.push(InputBlock{input, 5, 125ms}) ||
        !capture.push(InputBlock{input, 4, std::nullopt})) {
        std::cerr << "Capture must reject over-capacity blocks and preserve missing timing.\n";
        return 1;
    }
    const auto captured = capture.front();
    return captured && captured->frames == 4 && !captured->first_sample &&
                   captured->generation == 13
               ? 0
               : 1;
}
