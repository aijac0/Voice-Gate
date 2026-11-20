#include "audio/audio_backend.h"

namespace voice_gate::audio {

std::size_t StreamFormat::bytes_per_sample() const noexcept
{
    switch (encoding) {
    case SampleEncoding::float32:
        return sizeof(float);
    case SampleEncoding::signed16:
        return 2;
    case SampleEncoding::signed32:
        return 4;
    }
    return 0;
}

}  // namespace voice_gate::audio
