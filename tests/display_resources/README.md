# Display SPI resource boundary

Run `python3 tests/display_resources/run.py` after installing the pinned SDK.
The existing board adapter runner also invokes this suite.

The board wrappers bound controller descriptor storage, the dedicated Mosaico
SPI2 bus's reported maximum, and the panel queue depth. The bus-init match
requires the display host, all four data pins, either supported clock pin and
automatic DMA. Its 4092-byte request alone does not bound color allocations:
the SDK rounds it to 4095 or 8160 bytes with the tested alignment conditions.
The max-length query used by the real LCD IO factory is therefore capped at
2048 bytes after that rounding. Smaller native limits and errors propagate;
other buses are unchanged. The IO queue match still uses the host/chip select.
No framebuffer, reserved DMA arena, allocation retry or new runtime task is added.

The transport fixture compiles the pinned SDK LCD SPI implementation and public
IO definitions. Its max-length call resolves to the actual production wrapper,
as the firmware linker does. Only the SPI bus/DMA/completion boundary is modeled.
It separates total DMA bytes from the largest contiguous allocation and rounds
bounce allocations to 1-, 16- and 64-byte alignment. The original production
chunker fails at a 3072-byte allocation limit despite a bounded queue. The fixed
path transmits all 460800 RGB565 bytes using 225 chunks of at most 2048 bytes.
Both queue ceilings run full-frame, nonaligned-source, partial-tail, one-byte-tail
and exact-boundary cases. Chip select stays active until the last chunk; exactly
one final completion follows the last byte. Smaller allocation/total limits,
factory/query errors and a failure after several chunks remain explicit errors,
with no successful final callback. Deletion recycles outstanding bounce storage.
The original unbounded queue still fails under the same constrained total budget.

These strict sanitizer tests establish software chunking, data continuity,
callback and resource-lifecycle behavior. Descriptor geometry and allocation
alignment are models, not execution of the SPI DMA allocator or hardware.
The target build must confirm that the LCD IO factory calls the wrapper and
that the wrapper calls the native query. Physical TE timing, optical output,
throughput and concurrent HFP/A2DP/USB memory headroom require device evidence.
The display-transfer observer, touch layout and audio policy are unchanged.
