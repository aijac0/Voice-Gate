# Voice Gate

Voice Gate is a local app for talking with GPT Live. When a conversation goes quiet, it closes the connection and stops streaming audio.

During the pause, Voice Gate checks for sound on your device. When it detects sound again, it reconnects and resumes streaming audio so you can pick up where you left off.

GPT Live bills for connected time, including silence, so disconnecting during pauses can reduce idle costs.

Voice Gate is in active design, implementation, and testing.

## Build and test

Use CMake 3.20 or later with a C++20 compiler. To select a different compiler, set `CXX` before configuring a fresh build directory.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

On macOS, run `./build/voice_gate_audio_probe` to play a quiet test tone and check microphone capture and device timestamps. The probe reports timing and peak level.

## Verify with WebRTC AudioProcessing

Install Meson, Ninja, and pkg-config. The dependency script builds WebRTC AudioProcessing 2.1 and its pinned Abseil dependency from checksum-verified archives.

```sh
./tools/build_webrtc_apm.sh
export PKG_CONFIG_PATH="$PWD/build/webrtc-apm/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake -S . -B build-webrtc -DVOICE_GATE_ENABLE_WEBRTC=ON
cmake --build build-webrtc
ctest --test-dir build-webrtc --output-on-failure
./build-webrtc/voice_gate_audio_probe --verify-webrtc
```

On macOS, add `--restart` to run a second Core Audio session or `--switch-output` to check distinct default and system output routes.

Use `--built-in-mic` with `--verify-webrtc` to select the physical microphone for the probe when macOS defaults to a virtual input.
