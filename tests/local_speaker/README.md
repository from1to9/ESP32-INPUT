# Opt-in boot speaker discriminator

`CONFIG_APP_LOCAL_SPEAKER_TEST` defaults off. An explicitly selected diagnostic
image plays one finite local tone before the ordinary media owner starts:
48 kHz mono PCM16LE, 1 kHz, peak 4096 (0.125 FS), requested driver volume 80,
300 frames of 960 bytes (three seconds of generated PCM). It uses the real
board adapter and codec, without USB PCM input or microphone capture. It does
not exercise the audio engine or implement the public remote test-tone API.

Any physical button press or lock skips/cancels the test. Startup shares a
two-second deadline; each write is bounded to 100 ms and the output window to
three seconds from the unmute request. Mute and stop each have a 500 ms deadline. Exact asynchronous
arguments and frame bytes stay unchanged while pending. A short/error write
ends playback; normal media startup requires confirmed stop. The boot test
does not register a second driver owner or persist volume/preferences.

The final log distinguishes bytes generated, bytes submitted once per logical
frame, and validated bytes reported by board write callbacks, including a
partial/error report. Retries increase `write_calls`, not `submitted_bytes`.
These counters do not establish DMA consumption, analog output or audibility.
PCM scratch is static and erased after shutdown. The diagnostic callback is
detached before handover; normal initialization reuses the existing board
worker. Diagnostic generation 1 precedes normal engine route generation 2.

Run `python3 tests/local_speaker/run.py --output-dir <new-local-directory>`.
The sanitizer suite composes the actual test owner, board driver, pinned
ES8311 driver and real engine for handover. SDK boundaries supply scheduling,
I2C, GPIO and I2S results. It verifies waveform bytes, lock/cancel, timeouts,
partial/errors, one-run behavior, callback detachment and normal generation-2
prepare. It never accesses a device or qualifies physical sound.
