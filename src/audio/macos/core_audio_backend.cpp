#include "audio/macos/core_audio_backend.h"

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace voice_gate::audio {
namespace {

using Property = AudioObjectPropertyAddress;

void check(OSStatus status, const char* operation)
{
    if (status != noErr) {
        throw std::runtime_error(std::string(operation) + " (Core Audio status " +
                                 std::to_string(status) + ")");
    }
}

template <typename Value>
Value read_property(AudioObjectID object, Property property, const char* operation)
{
    Value value{};
    UInt32 size = sizeof(value);
    check(AudioObjectGetPropertyData(object, &property, 0, nullptr, &size, &value), operation);
    if (size != sizeof(value)) {
        throw std::runtime_error(std::string(operation) + " returned an unexpected size");
    }
    return value;
}

AudioDeviceID default_device(AudioObjectPropertySelector selector)
{
    const auto device = read_property<AudioDeviceID>(
        kAudioObjectSystemObject,
        {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        "Read default audio device");
    if (device == kAudioObjectUnknown) {
        throw std::runtime_error("No default audio device is available");
    }
    return device;
}

SampleEncoding sample_encoding(const AudioStreamBasicDescription& stream)
{
    const auto flags = stream.mFormatFlags;
    const bool big_endian = (flags & kAudioFormatFlagIsBigEndian) != 0;
    if (stream.mFormatID != kAudioFormatLinearPCM ||
        (flags & kAudioFormatFlagIsPacked) == 0 ||
        big_endian != (std::endian::native == std::endian::big) ||
        stream.mFramesPerPacket != 1 || !std::isfinite(stream.mSampleRate) ||
        stream.mSampleRate <= 0 || stream.mChannelsPerFrame == 0) {
        throw std::runtime_error("Core Audio device must supply packed native-endian PCM");
    }
    if ((flags & kAudioFormatFlagIsFloat) && stream.mBitsPerChannel == 32) {
        return SampleEncoding::float32;
    }
    if (flags & kAudioFormatFlagIsSignedInteger) {
        if (stream.mBitsPerChannel == 16) {
            return SampleEncoding::signed16;
        }
        if (stream.mBitsPerChannel == 32) {
            return SampleEncoding::signed32;
        }
    }
    throw std::runtime_error("Unsupported Core Audio sample encoding");
}

struct DeviceLayout {
    StreamFormat format{};
    std::vector<unsigned> buffer_channels;
    bool operator==(const DeviceLayout&) const = default;
};

std::size_t max_frames(AudioDeviceID device)
{
    const auto range = read_property<AudioValueRange>(
        device, {kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal,
                 kAudioObjectPropertyElementMain},
        "Read Core Audio buffer frame-size range");
    if (!std::isfinite(range.mMaximum) || range.mMaximum < 1 ||
        range.mMaximum > std::numeric_limits<UInt32>::max()) {
        throw std::runtime_error("Invalid Core Audio buffer frame-size range");
    }
    const Property variable_size{kAudioDevicePropertyUsesVariableBufferFrameSizes,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    const auto variable_max = AudioObjectHasProperty(device, &variable_size)
        ? read_property<UInt32>(device, variable_size, "Read Core Audio variable frame limit")
        : 0;
    return std::max(static_cast<std::size_t>(std::ceil(range.mMaximum)),
                    static_cast<std::size_t>(variable_max));
}

DeviceLayout read_layout(AudioDeviceID device, AudioObjectPropertyScope scope)
{
    const Property property{kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    check(AudioObjectGetPropertyDataSize(device, &property, 0, nullptr, &size),
          "Read audio stream list size");
    std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
    if (streams.empty()) {
        throw std::runtime_error("The selected audio device has no streams in this direction");
    }
    check(AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, streams.data()),
          "Read audio stream list");
    streams.resize(size / sizeof(AudioStreamID));

    DeviceLayout layout;
    for (const auto stream : streams) {
        const auto description = read_property<AudioStreamBasicDescription>(
            stream,
            {kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal,
             kAudioObjectPropertyElementMain},
            "Read Core Audio stream format");
        const auto encoding = sample_encoding(description);
        if (layout.format.channels == 0) {
            layout.format.sample_rate = description.mSampleRate;
            layout.format.encoding = encoding;
        } else if (layout.format.sample_rate != description.mSampleRate ||
                   layout.format.encoding != encoding) {
            throw std::runtime_error("The selected device uses different PCM formats per stream");
        }
        const bool planar = (description.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
        const unsigned channels_per_buffer = planar ? 1 : description.mChannelsPerFrame;
        if (description.mBytesPerFrame != channels_per_buffer * layout.format.bytes_per_sample()) {
            throw std::runtime_error("Unsupported Core Audio frame stride");
        }
        layout.buffer_channels.insert(layout.buffer_channels.end(),
                                      planar ? description.mChannelsPerFrame : 1,
                                      channels_per_buffer);
        layout.format.channels += description.mChannelsPerFrame;
    }
    if (layout.format.channels == 0) {
        throw std::runtime_error("The audio stream list changed while opening the device");
    }
    return layout;
}

AudioTime host_time(UInt64 ticks) noexcept
{
    return AudioTime(static_cast<AudioTime::rep>(AudioConvertHostTimeToNanos(ticks)));
}

template <typename Byte, typename BufferList>
std::optional<DeviceBlock<Byte>> make_block(const DeviceLayout& layout,
                                           std::span<BufferView<Byte>> views,
                                           BufferList* buffers,
                                           const AudioTimeStamp* timestamp) noexcept
{
    if (!buffers || buffers->mNumberBuffers != views.size() || views.empty()) {
        return std::nullopt;
    }
    std::size_t frames = 0;
    for (std::size_t index = 0; index < views.size(); ++index) {
        const auto& buffer = buffers->mBuffers[index];
        const auto channels = layout.buffer_channels[index];
        const auto stride = channels * layout.format.bytes_per_sample();
        if (!buffer.mData || buffer.mNumberChannels != channels ||
            buffer.mDataByteSize % stride != 0) {
            return std::nullopt;
        }
        const auto buffer_frames = buffer.mDataByteSize / stride;
        if (index == 0) {
            frames = buffer_frames;
        } else if (frames != buffer_frames) {
            return std::nullopt;
        }
        views[index] = {std::span(static_cast<Byte*>(buffer.mData), buffer.mDataByteSize), channels};
    }
    if (frames == 0) {
        return std::nullopt;
    }
    std::optional<AudioTime> first_sample;
    if (timestamp && (timestamp->mFlags & kAudioTimeStampHostTimeValid)) {
        first_sample = host_time(timestamp->mHostTime);
    }
    return DeviceBlock<Byte>{views, frames, first_sample};
}

}  // namespace

struct CoreAudioBackend::State {
    struct Registration {
        State* state{};
        AudioDeviceID device{kAudioObjectUnknown};
        bool input{};
        bool output{};
        AudioDeviceIOProcID handle{};
        bool running{};
    };

    static OSStatus io(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList* input,
                       const AudioTimeStamp* input_time, AudioBufferList* output,
                       const AudioTimeStamp* output_time, void* context) noexcept
    {
        auto& registration = *static_cast<Registration*>(context);
        State& state = *registration.state;
        if (!state.accepting.load()) {
            return noErr;
        }
        state.in_flight.fetch_add(1);
        if (state.accepting.load()) {
            if (registration.input && state.callbacks.capture) {
                const auto block = make_block(state.input_layout, std::span(state.input_views),
                                              input, input_time);
                if (block) {
                    state.callbacks.capture(state.callbacks.context, *block);
                }
            }
            if (registration.output && state.callbacks.render) {
                const auto block = make_block(state.output_layout, std::span(state.output_views),
                                              output, output_time);
                if (block) {
                    state.callbacks.render(state.callbacks.context, *block);
                }
            }
        }
        state.in_flight.fetch_sub(1);
        return noErr;
    }

    DeviceLayout input_layout;
    DeviceLayout output_layout;
    std::vector<BufferView<const std::byte>> input_views;
    std::vector<BufferView<std::byte>> output_views;
    std::array<Registration, 2> registrations{};
    DeviceCallbacks callbacks{};
    std::atomic<bool> accepting{false};
    std::atomic<unsigned> in_flight{0};
    bool opened{};
};

CoreAudioBackend::CoreAudioBackend() : state_(std::make_unique<State>())
{
}

CoreAudioBackend::~CoreAudioBackend()
{
    try {
        stop();
    } catch (...) {
        // A failed unregister can leave Core Audio holding the callback context.
        // Stop has detached the caller's callbacks before returning the error.
        static_cast<void>(state_.release());
    }
}

DeviceFormats CoreAudioBackend::open()
{
    stop();
    auto& state = *state_;
    state.opened = false;
    const auto input = default_device(kAudioHardwarePropertyDefaultInputDevice);
    const auto output = default_device(kAudioHardwarePropertyDefaultOutputDevice);
    state.input_layout = read_layout(input, kAudioObjectPropertyScopeInput);
    state.output_layout = read_layout(output, kAudioObjectPropertyScopeOutput);
    state.input_views.resize(state.input_layout.buffer_channels.size());
    state.output_views.resize(state.output_layout.buffer_channels.size());
    state.registrations[0] = {&state, input, true, input == output};
    state.registrations[1] = {};
    if (input != output) {
        state.registrations[1] = {&state, output, false, true};
    }
    const auto input_max_frames = max_frames(input);
    const auto output_max_frames = input == output ? input_max_frames : max_frames(output);
    state.opened = true;
    return {state.input_layout.format, state.output_layout.format,
            input_max_frames, output_max_frames};
}

void CoreAudioBackend::start(DeviceCallbacks callbacks)
{
    auto& state = *state_;
    if (!state.opened || state.registrations[0].handle || state.registrations[1].handle) {
        throw std::logic_error("Open the Core Audio backend and stop any active run before starting");
    }
    state.callbacks = callbacks;
    try {
        for (auto& registration : state.registrations) {
            if (registration.device != kAudioObjectUnknown) {
                check(AudioDeviceCreateIOProcID(registration.device, State::io, &registration,
                                                &registration.handle),
                      "Register the Core Audio callback");
            }
        }
        state.accepting.store(true);
        for (auto& registration : state.registrations) {
            if (registration.handle) {
                check(AudioDeviceStart(registration.device, registration.handle),
                      "Start the Core Audio device");
                registration.running = true;
            }
        }
    } catch (...) {
        stop();
        throw;
    }
}

void CoreAudioBackend::stop()
{
    auto& state = *state_;
    state.accepting.store(false);
    OSStatus first_error = noErr;
    for (auto registration = state.registrations.rbegin();
         registration != state.registrations.rend(); ++registration) {
        if (registration->running) {
            const auto status = AudioDeviceStop(registration->device, registration->handle);
            if (status == noErr) {
                registration->running = false;
            } else if (first_error == noErr) {
                first_error = status;
            }
        }
        if (registration->handle && !registration->running) {
            const auto status = AudioDeviceDestroyIOProcID(registration->device, registration->handle);
            if (status == noErr) {
                registration->handle = nullptr;
            } else if (first_error == noErr) {
                first_error = status;
            }
        }
    }
    while (state.in_flight.load() != 0) {
        std::this_thread::yield();
    }
    state.callbacks = {};
    check(first_error, "Stop or unregister the Core Audio callback");
}

AudioTime CoreAudioBackend::now() const noexcept
{
    return host_time(AudioGetCurrentHostTime());
}

bool CoreAudioBackend::configuration_changed() const
{
    const auto& state = *state_;
    const auto input = default_device(kAudioHardwarePropertyDefaultInputDevice);
    const auto output = default_device(kAudioHardwarePropertyDefaultOutputDevice);
    const auto opened_output = state.registrations[0].output
        ? state.registrations[0].device
        : state.registrations[1].device;
    return input != state.registrations[0].device || output != opened_output ||
           read_layout(input, kAudioObjectPropertyScopeInput) != state.input_layout ||
           read_layout(output, kAudioObjectPropertyScopeOutput) != state.output_layout;
}

}  // namespace voice_gate::audio
