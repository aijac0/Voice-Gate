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
#include <utility>
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

bool device_disappeared(OSStatus status) noexcept
{
    return status == kAudioHardwareBadObjectError || status == kAudioHardwareBadDeviceError;
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

AudioDeviceID built_in_input()
{
    const Property devices_property{kAudioHardwarePropertyDevices,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    check(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices_property, 0,
                                        nullptr, &size),
          "Read the Core Audio device list size");
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (!devices.empty()) {
        check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices_property, 0,
                                         nullptr, &size, devices.data()),
              "Read the Core Audio device list");
        devices.resize(size / sizeof(AudioDeviceID));
    }
    const Property transport{kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal,
                             kAudioObjectPropertyElementMain};
    const Property streams{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput,
                           kAudioObjectPropertyElementMain};
    for (const auto device : devices) {
        UInt32 transport_type = 0;
        UInt32 transport_size = sizeof(transport_type);
        UInt32 stream_size = 0;
        if (AudioObjectGetPropertyData(device, &transport, 0, nullptr, &transport_size,
                                       &transport_type) == noErr &&
            transport_type == kAudioDeviceTransportTypeBuiltIn &&
            AudioObjectGetPropertyDataSize(device, &streams, 0, nullptr, &stream_size) == noErr &&
            stream_size > 0) {
            return device;
        }
    }
    throw std::runtime_error("No built-in Core Audio input device is available");
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
    std::vector<AudioStreamID> streams;
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
    layout.streams = std::move(streams);
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
    struct PropertyWatch {
        AudioObjectID object;
        Property address;
        bool installed{};
    };

    struct Registration {
        State* state{};
        AudioDeviceID device{kAudioObjectUnknown};
        bool input{};
        bool output{};
        AudioDeviceIOProcID handle{};
        bool running{};
    };

    static OSStatus on_configuration_change(AudioObjectID, UInt32, const Property*,
                                            void* context) noexcept
    {
        static_cast<State*>(context)->configuration_invalid.store(true);
        return noErr;
    }

    [[nodiscard]] bool forwarding_audio() const noexcept
    {
        return accepting.load() && !configuration_invalid.load();
    }

    static OSStatus io(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList* input,
                       const AudioTimeStamp* input_time, AudioBufferList* output,
                       const AudioTimeStamp* output_time, void* context) noexcept
    {
        auto& registration = *static_cast<Registration*>(context);
        State& state = *registration.state;
        if (!state.forwarding_audio()) {
            return noErr;
        }
        state.in_flight.fetch_add(1);
        if (state.forwarding_audio()) {
            if (registration.input && state.callbacks.capture) {
                const auto block = make_block(state.input_layout, std::span(state.input_views),
                                              input, input_time);
                if (block && state.forwarding_audio()) {
                    state.callbacks.capture(state.callbacks.context, *block);
                }
            }
            if (registration.output && state.callbacks.render) {
                const auto block = make_block(state.output_layout, std::span(state.output_views),
                                              output, output_time);
                if (block && state.forwarding_audio()) {
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
    std::vector<PropertyWatch> watches;
    DeviceCallbacks callbacks{};
    std::atomic<bool> accepting{false};
    std::atomic<bool> configuration_invalid{false};
    std::atomic<unsigned> in_flight{0};
    bool opened{};
    bool follows_default_input{true};
    AudioObjectPropertySelector output_selector{kAudioHardwarePropertyDefaultOutputDevice};
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
    return open(CoreAudioOutput::default_device);
}

DeviceFormats CoreAudioBackend::open(CoreAudioOutput route, CoreAudioInput input_route)
{
    stop();
    auto& state = *state_;
    state.opened = false;
    state.follows_default_input = input_route == CoreAudioInput::default_device;
    const auto input = state.follows_default_input
        ? default_device(kAudioHardwarePropertyDefaultInputDevice)
        : built_in_input();
    state.output_selector = route == CoreAudioOutput::system_device
        ? kAudioHardwarePropertyDefaultSystemOutputDevice
        : kAudioHardwarePropertyDefaultOutputDevice;
    const auto output = default_device(state.output_selector);
    state.input_layout = read_layout(input, kAudioObjectPropertyScopeInput);
    state.output_layout = read_layout(output, kAudioObjectPropertyScopeOutput);
    state.input_views.resize(state.input_layout.buffer_channels.size());
    state.output_views.resize(state.output_layout.buffer_channels.size());
    state.registrations[0] = {&state, input, true, input == output};
    state.registrations[1] = {};
    if (input != output) {
        state.registrations[1] = {&state, output, false, true};
    }
    state.watches = {
        {kAudioObjectSystemObject, {state.output_selector,
                                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}},
        {input, {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput,
                 kAudioObjectPropertyElementMain}},
        {output, {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                  kAudioObjectPropertyElementMain}},
    };
    if (state.follows_default_input) {
        state.watches.push_back({kAudioObjectSystemObject,
                                 {kAudioHardwarePropertyDefaultInputDevice,
                                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}});
    }
    for (const auto* layout : {&state.input_layout, &state.output_layout}) {
        for (const auto stream : layout->streams) {
            state.watches.push_back({stream, {kAudioStreamPropertyVirtualFormat,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain}});
        }
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
    const bool watches_installed = std::any_of(state.watches.begin(), state.watches.end(),
        [](const State::PropertyWatch& watch) { return watch.installed; });
    if (!state.opened || state.registrations[0].handle ||
        state.registrations[1].handle || watches_installed) {
        throw std::logic_error("Open the Core Audio backend and stop any active run before starting");
    }
    state.callbacks = callbacks;
    state.configuration_invalid.store(false);
    try {
        for (auto& watch : state.watches) {
            check(AudioObjectAddPropertyListener(watch.object, &watch.address,
                                                 State::on_configuration_change, &state),
                  "Watch Core Audio device and stream changes");
            watch.installed = true;
        }
        if (configuration_changed()) {
            throw std::runtime_error("Core Audio configuration changed during startup");
        }
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
            } else if (device_disappeared(status)) {
                registration->running = false;
                registration->handle = nullptr;
            } else if (first_error == noErr) {
                first_error = status;
            }
        }
        if (registration->handle && !registration->running) {
            const auto status = AudioDeviceDestroyIOProcID(registration->device, registration->handle);
            if (status == noErr || device_disappeared(status)) {
                registration->handle = nullptr;
            } else if (first_error == noErr) {
                first_error = status;
            }
        }
    }
    for (auto& watch : state.watches) {
        if (watch.installed) {
            const auto status = AudioObjectRemovePropertyListener(
                watch.object, &watch.address, State::on_configuration_change, &state);
            if (status == noErr || device_disappeared(status)) {
                watch.installed = false;
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
    if (configuration_change_pending()) {
        return true;
    }
    const auto& state = *state_;
    try {
        const auto input = state.follows_default_input
            ? default_device(kAudioHardwarePropertyDefaultInputDevice)
            : state.registrations[0].device;
        const auto output = default_device(state.output_selector);
        return input != state.registrations[0].device || output != output_device_id() ||
               read_layout(input, kAudioObjectPropertyScopeInput) != state.input_layout ||
               read_layout(output, kAudioObjectPropertyScopeOutput) != state.output_layout;
    } catch (const std::runtime_error&) {
        // A device can disappear before the new default route is available.
        return true;
    }
}

bool CoreAudioBackend::configuration_change_pending() const noexcept
{
    return state_->configuration_invalid.load();
}

unsigned CoreAudioBackend::output_device_id() const noexcept
{
    const auto& registrations = state_->registrations;
    return registrations[0].output ? registrations[0].device : registrations[1].device;
}

}  // namespace voice_gate::audio
