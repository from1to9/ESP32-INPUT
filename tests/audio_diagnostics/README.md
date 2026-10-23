# Cached audio diagnostics observer

Run `python3 tests/audio_diagnostics/run.py` from the repository root. Seven
isolated cases compile the actual `app_audio_diagnostics.c` observer and format
comparison code with strict C11 warnings, AddressSanitizer and fail-fast UBSan.
Only the cached media/USB/codec getters and logger are substituted. Exact
compiler-discovered first-party inputs and before/after hashes accompany the
local test evidence. There is no device or network access.

The coordinator calls `app_audio_diagnostics_poll(now_ms)` after PSRAM startup.
The observer copies cached snapshots at most once per 100 ms, logs semantic
state/error changes and emits counter summaries once per ten seconds. No
producer service, audio mutation, HID action, storage or allocation occurs.
It can miss transitions shorter than its sampling interval. Logging has a
cost; physical scheduling/timing effects remain unqualified. At most eight
lines are emitted per admitted observation, including during error flapping.
A monotonic clock regression reports the observer failure once and disables
further observation without changing the audio owner or its policy.

All four audio job slots include state, first-frame status/time, deadline,
terminal reason/error, cleanup and byte/sample/discard counters. Aggregate
audio, speech, USB and codec status remain separate. Unavailable snapshots are
reported as unknown; zero-filled scratch cannot manufacture OFF or success.
The full copied runtime snapshot is held in fixed PSRAM scratch and wiped
before return. Retained observations and logs exclude job/session/peer IDs,
tokens, PCM and speech candidates.

Tests cover unavailable-to-known recovery, getters that leave output untouched,
failed jobs retained after aggregate OFF, first-frame transitions, counter-only
updates waiting for the summary, USB/codec errors and generations, private
metadata wiping, unchanged owner inputs, forbidden mutation calls, throttle
and idle logging bounds, and monotonic-time regression/overflow boundaries.
These are observer guarantees, not proof of microphone capture, speaker sound,
USB host compatibility or the reported physical two-second startup cause.
