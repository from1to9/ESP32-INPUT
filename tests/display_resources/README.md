# Display SPI resource boundary

Run `python3 tests/display_resources/run.py` after installing the pinned SDK.
The existing board adapter runner also invokes this suite.

The production board factory wrapper caps only the Mosaico display's exact SPI
host and chip select. Tests cover both supported ceilings, borrowed argument
copying, unchanged callbacks and transport flags, unrelated devices, smaller or
invalid requested depths, and lower-layer errors.

The transport fixture compiles the pinned SDK's actual LCD SPI implementation
and public IO definitions, renaming only its factory symbol for native linkage.
Its SPI boundary substitutes bounded internal DMA bounce storage and completion
events. It transmits and compares all 460,800 bytes of a 480 x 480 RGB565 frame,
checks descriptor recycling, and requires the final callback to follow the last
byte. The original ten-deep queue fails with the same constrained storage;
partial failures do not produce a successful completion. Already-completed
descriptors retain their bounce storage until SDK result recycling.

These sanitizer tests prove policy and SDK transport lifecycle behavior. They
do not measure DMA hardware, TE timing, throughput, optical output, or complete
firmware link wrapping. The target build must verify `--wrap` linkage, and the
device must qualify full-frame output and memory headroom with all concurrent
features. The production display-transfer observer remains unchanged.
