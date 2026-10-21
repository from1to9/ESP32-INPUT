# Device restart worker boundary tests

Run `python3 tests/device_restart/run.py`. It compiles the production worker
under strict C11 warnings and Address/Undefined Behavior sanitizers, replacing
only the task, monotonic timer and `esp_restart` boundaries. No device or running
process is restarted by this suite.

Cases prove that initialization and request admission have no restart effect;
the 200ms reply interval, finite deadline, fresh coordinator authority and actual
maintenance evidence gate dispatch; stale IDs, cancellation and clock rollback
fail closed; and a returning restart boundary remains uncertain instead of
claiming a later boot was observed. The coordinator independently proves input
release and media hardware-off before authorizing this worker.
