# Display SPI resource boundary

Run `python3 tests/display_resources/run.py` after installing the pinned SDK.
The existing board adapter runner also invokes this suite.

The production board wrappers cap the Mosaico display bus's transfer request
and the panel's queue depth. The bus match requires the display host, all four
data pins, either supported board clock pin, and automatic DMA. The request
ceiling is 4092 bytes: one DMA descriptor, reported as 4095 payload bytes by the
pinned S31 driver. The IO match still requires the exact host and chip select.
Tests cover both board clock pins and supported queue ceilings, borrowed argument
copying, unchanged callbacks and transport flags, unrelated buses/devices,
smaller or invalid requests, and lower-layer errors.

The transport fixture compiles the pinned SDK's actual LCD SPI implementation
and public IO definitions, renaming only its factory symbol for native linkage.
Its SPI boundary substitutes bounded internal DMA bounce storage and completion
events. Total DMA storage and the largest individual allocation are separate
limits. A 7 KiB allocation limit rejects the old 8190-byte chunk even at queue
depth one, while the bounded bus request sends the same complete frame. It
transmits and compares all 460,800 bytes of a 480 x 480 RGB565 frame,
checks descriptor recycling, and requires the final callback to follow the last
byte. The original ten-deep queue fails with the same constrained storage;
chip select remains asserted across nonfinal chunks, and smaller allocation
limits or a failure after several chunks still propagate errors without final
completion. Already-completed
descriptors retain their bounce storage until SDK result recycling.

These sanitizer tests prove policy and SDK transport lifecycle behavior. They
model the tested 4095-byte S31 descriptor payload at the SPI bus boundary; they do not execute
the SDK allocator or measure DMA hardware, TE timing, throughput, optical output, or complete
firmware link wrapping. The target build must verify `--wrap` linkage, and the
device must qualify full-frame output and memory headroom with all concurrent
features. The production display-transfer observer remains unchanged.
