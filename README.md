# Voice Gate

Voice Gate is a local app for talking with GPT Live. When a conversation goes quiet, it closes the connection and stops streaming audio.

During the pause, Voice Gate checks for sound on your device. When it detects sound again, it reconnects and resumes streaming audio so you can pick up where you left off.

GPT Live bills for connected time, including silence, so disconnecting during pauses can reduce idle costs.

Sprint One implements the macOS audio capture and output reference prototype.

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

Add `--restart` to the last command to run a second Core Audio session with the same frame format. When the Mac has distinct default and system output devices, use `--switch-output` to verify both routes. The probe retains the startup WebRTC sample rate and channel counts through either transition.

Use `--built-in-mic` with `--verify-webrtc` to select the physical microphone for the probe when macOS defaults to a virtual input.

The shared C++ code in `src/audio` copies audio into bounded queues, assembles timed frames, and submits the playback reference before microphone audio. The macOS backend in `src/audio/macos` handles device formats, callbacks, and shutdown. The WebRTC tests use the module with its audio effects disabled.
