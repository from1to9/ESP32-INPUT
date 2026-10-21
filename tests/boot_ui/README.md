# Native boot UI checks

This isolated harness compiles the production
[boot module](../../firmware/main/app_boot_ui.c) with the pinned managed LVGL
source. It shares the existing host LVGL configuration and allocator observer
from `tests/ui/` without modifying that suite. Resolve firmware dependencies
first as described in [firmware setup](../../firmware/README.md).

```sh
python3 tests/boot_ui/run.py
```

The runner discovers the project-local toolchain when needed, requires a fresh
evidence directory, retains build/test logs, converts the five frames to PNG,
and records hashes before and after validation. Pass `--output-dir <new-path>`
to choose the evidence location. A changed source invalidates the result.

For manual compiler diagnosis:

```sh
cmake -S tests/boot_ui -B .build/boot-ui -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/boot-ui
mkdir -p .agent/local/boot-ui
ASAN_OPTIONS=detect_leaks=0 .build/boot-ui/boot_ui .agent/local/boot-ui
```

Address/undefined sanitizers default on; `detect_leaks=0` supports platforms
without LeakSanitizer. The separate LVGL allocator observer must still reach
zero live bytes/blocks after teardown. It measures requested CLIB bytes, not
allocator overhead or target SRAM/DMA. Sanitizers can be disabled explicitly via
`-DBOOT_UI_SANITIZERS=OFF` on unsupported compilers; record that limitation.

The harness writes five 480 x 480 PPM previews (starting, real step progress,
fault, ready and maximum text) plus a JSON result on stdout. Inspect the actual
images as well as the geometry assertions. It checks:

- Bounded label contents and fitting geometry, including maximum-width strings.
- Unknown progress, observed progress, ready/fault states and atomic rejection
  of invalid snapshots without replacing the preceding view.
- One thousand repeated updates per representative state without allocations,
  reallocations or display flushes once settled.
- One thousand create/destroy cycles with stable live allocation counts/bytes,
  no added timers and at most sixteen boot objects.
- Duplicate/null/wrong-size creation rejection, unrelated screen object
  preservation, idempotent destroy, external screen/display deletion and reuse.
- Zero remaining LVGL allocation blocks/bytes at final teardown.

The host's partial draw buffer and RGB screenshot buffer belong to the harness;
the production module has neither. No hardware state is simulated as a verified
device result. Main-loop integration, boot timing, target memory and display
behavior require separate validation.
