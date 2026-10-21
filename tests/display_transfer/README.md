# Display transfer receipt observer

Run `python3 tests/display_transfer/run.py`. The harness compiles the actual
first-party observer with strict C11 diagnostics, AddressSanitizer and
UndefinedBehaviorSanitizer. Driver interface and interrupt scheduling doubles
test software ordering; they do not claim an illuminated or readable panel.

The board creates the BSP panel, installs the observer and only then starts
the LVGL adapter. Public panel/IO interfaces are decorated; managed components
remain untouched. The observer forwards the original panel pointer, rectangle,
pixels, IO command/bytes and return values. It captures the adapter's actual
callback and context at registration, then forwards the same event and returns
that callback's exact yield result. Registration failure and unregister do not
manufacture observer readiness. A callback replacement during pending IO is
forwarded natively and invalidates observation, preserving renderer behavior.
The pinned adapter's unregister convention (same callback, NULL impl context)
is forwarded as a detach, and successful registration replacement changes the
display generation so no older receipt survives it.

The pinned CO5300 driver submits one color transfer per draw. The pinned SDK's
SPI panel IO marks only the final color chunk for `on_color_trans_done`; the
observer also decorates `tx_color` so a draw returning success without a color
submission is insufficient. The color pointer and exact RGB565 byte count must
match the rectangle; short payloads cannot confirm full coverage. The TE
adapter's native callback supplies its task
notification. No public notification helper substitutes for it, and no extra
flush wait or audio callback is introduced.

The UI owner binds a nonzero display/capture/indicator/refresh token and required
half-open 480x480 rectangle before a forced refresh. At matching refresh end it
seals the observation. Each receipt requires actual successful draw return,
successful color submission, and the matching final-color ISR. The ISR can
precede draw return. No-transfer, unrelated transfer or one intersecting
rectangle cannot acknowledge the warning. Exact rectangle-union coverage uses
at most 32 transfer records and bounded task-context interval sweeps; there is
no pixel map or ISR coverage loop. A copied snapshot is calculated without
LVGL APIs, hardware IO, allocation, display lock or mutation.

Cancel/replacement invalidates the old token but retains its in-flight transfer
association until completion. A new refresh cannot reuse that completion.
Unbound overlapping drawing invalidates an old warning receipt; unrelated
drawing preserves it. Unexpected concurrent drawing, extra/unknown color
completion, record exhaustion and nonzero panel gaps fail association closed.
Panel reinitialization, rotation, on/off, sleep and deletion invalidate prior
receipts and change display generation. A new token must use that generation.
The implementation is enabled only for the currently pinned TE configuration.
`invalidation_revision` advances on identity/region replacement, explicit
invalidation, overlapping unbound drawing, errors, observer association loss,
callback changes and panel changes. A same-identity empty or unrelated refresh
does not advance it, allowing the presenter to retain existing completed pixel
evidence without inventing a new transfer. Saturation permanently fails closed.

Tests cover early and delayed ISR, missing ISR, draw rejection before color,
failure after color submission, color failure, split and overlapping regions
with a real gap, empty/unrelated refresh, late old completion, stale end/begin,
same text with a new capture identity, external drawing, registration failure,
unregister, callback replacement during IO, native context/yield preservation,
resource exhaustion, display changes and late completion after deletion.
An independent 8x8 pixel-union oracle checks 1000 deterministic rectangle sets.
The board adapter suite additionally verifies observer installation precedes
adapter startup and an observer failure leaves display functionality intact.

Source contract: IDF `esp_lcd_panel_io_spi.c` final-color chunk/post-transaction
callback; CO5300 `panel_co5300_draw_bitmap`; adapter
`display_bridge_v9_on_io_color_trans_done` and the TE flush branch. The reviewed
pins are retained in the local integration evidence and tracked dependency
lock. Render return, `LV_EVENT_FLUSH_FINISH`, refresh completion and a generic
task wake alone cannot establish this receipt.

`indicator_transfer_confirmed` is bus-completion evidence. Optical readability,
OLED scanout, physical display orientation, panel/cable integrity and a person
seeing the warning remain unqualified. Positive acknowledged brightness, UI
owner liveness, current indicator/token ownership and independent bounded audio
stop remain the presentation/privacy owners' responsibility.
