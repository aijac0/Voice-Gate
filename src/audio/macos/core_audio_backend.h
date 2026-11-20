#pragma once

#include "audio/audio_backend.h"

#include <memory>

namespace voice_gate::audio {

class CoreAudioBackend final : public AudioBackend {
public:
    CoreAudioBackend();
    ~CoreAudioBackend() override;

    [[nodiscard]] DeviceFormats open() override;
    void start(DeviceCallbacks callbacks) override;
    void stop() override;
    [[nodiscard]] AudioTime now() const noexcept override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace voice_gate::audio
