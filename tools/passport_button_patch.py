#!/usr/bin/env python3
"""Repair repeated holds and exhausted callbacks in Passport's pinned driver."""
from pathlib import Path
import sys


def patch_source(source: str) -> str:
    anchor = "    /** State machine */\n    switch (btn->state) {"
    if source.count(anchor) != 1 or "case 3:" not in source:
        raise ValueError("Pinned button state machine changed; review the repeat-hold repair")
    source = source.replace(anchor, """    // Passport: a repeated press uses the same long-press transition as the
    // first press. Run it after debounce, in the producer's event order.
    if (btn->state == PRESS_REPEAT_UP_CHECK && btn->button_level == BUTTON_ACTIVE &&
            btn->ticks >= btn->long_press_ticks) {
        btn->state = PRESS_UP_CHECK;
    }

""" + anchor)
    # LONG_START increments count after its last callback. The next held poll
    # must check the remaining count before reading that event's callback table.
    scan = """if (btn->cb_info[BUTTON_LONG_PRESS_START]) {
                button_cb_info_t *cb_info = btn->cb_info[BUTTON_LONG_PRESS_START];"""
    if source.count(scan) != 1:
        raise ValueError("Pinned long-press callback scan changed; review its bounds repair")
    return source.replace(scan, """if (btn->cb_info[BUTTON_LONG_PRESS_START] &&
                    btn->count[0] < btn->size[BUTTON_LONG_PRESS_START]) {
                button_cb_info_t *cb_info = btn->cb_info[BUTTON_LONG_PRESS_START];""")


if __name__ == "__main__":
    source, output = map(Path, sys.argv[1:])
    patched = patch_source(source.read_text())
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != patched:
        output.write_text(patched)
