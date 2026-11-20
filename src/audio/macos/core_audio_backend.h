#pragma once

#include "audio/audio_backend.h"

#include <memory>

namespace voice_gate::audio {

enum class CoreAudioOutput { default_device, system_device };
enum class CoreAudioInput { default_device, built_in };

class CoreAudioBackend final : public AudioBackend {
public:
    CoreAudioBackend();
    ~CoreAudioBackend() override;

    [[nodiscard]] DeviceFormats open() override;
    [[nodiscard]] DeviceFormats open(CoreAudioOutput output,
                                     CoreAudioInput input = CoreAudioInput::default_device);
    void start(DeviceCallbacks callbacks) override;
    void stop() override;
    [[nodiscard]] AudioTime now() const noexcept override;
    [[nodiscard]] bool configuration_change_pending() const noexcept;
    [[nodiscard]] bool configuration_changed() const;
    [[nodiscard]] unsigned output_device_id() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace voice_gate::audio
