# Spoken-letter policy tests

Run `python3 tests/spoken_letters/run.py`. A strict C11 address/undefined-sanitizer
build runs the production window/frame policy with a controlled model boundary.
Tests cover every Alpha-Zulu/zero-nine command ID, signed PCM decoding, candidate
confirmation identity, duplicate effects, old jobs/generations, continuous
samples, format changes, malformed results, ambiguous ties, model failures,
exact timeouts, cancellation and clock regression. These tests do not claim
speech recognition accuracy: the target runtime uses actual ESP-SR AFE/MultiNet
and requires a target build plus later acoustic qualification.

The same runner also executes the production firmware worker with SDK/model
boundaries replaced. It checks grammar installation, private AFE lifecycle,
bounded copied-frame admission, terminal queue clearing, cancellation while
MultiNet is running, confirmation once, stale cancellation isolation, queue-age
expiry, overflow identity/reason, normal capture closure retaining a candidate,
strong cancellation clearing it, old-job closure isolation, in-flight closure
and missing-model/PSRAM failures. It does not
substitute those boundaries in the firmware build.
