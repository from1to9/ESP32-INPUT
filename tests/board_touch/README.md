# Touch observer boundary tests

Run `python3 tests/board_touch/run.py` for production observer tests with explicit
LCD-driver and LVGL-adapter substitutes. Each scan reads once, copies both real
contact IDs before the primary pointer event, and fails closed on driver error,
observed overflow, rejected coordinator admission or exhausted sequence.

The pinned Mosaico BSP uses a 1:1 logical coordinate scale. The pinned CST9220
provider exposes at most two contacts. The first-party IO observer preserves raw
header/count validity before the upstream decoder can normalize it. A second
composition test compiles the unchanged pinned CST9220 source with the real
observer and portable multi-touch recognizer: malformed headers and raw counts
above two cancel the pair/held drag, issue no click/scroll, preserve the actual
ACK and perform exactly one read. Generic IO/GPIO/RTOS boundaries are modeled.
This suite does not prove controller report timing, physical multi-touch quality
or arbitrary-controller portability.
