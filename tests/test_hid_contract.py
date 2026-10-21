#!/usr/bin/env python3
"""Independently decode the production HID descriptor and check its wire contract.

Only a C11 compiler and Python's standard library are required. The probe reads
the linked descriptor bytes; expected IDs, usages and offsets do not come from
production macros. This does not exercise GATT registration or a host HID stack.
"""
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Field:
    kind: str
    report_id: int
    offset: int
    size: int
    count: int
    flags: int
    logical_min: int
    logical_max: int
    usages: tuple
    collections: tuple


def parse_descriptor(data):
    """Decode HID 1.11 short items, with independent report bit offsets.

    Unsupported/reserved items fail explicitly rather than disappearing from
    the contract check. Local state is cleared after every Main item; Global
    state survives collections and supports Push/Pop.
    """
    state = {"page": 0, "min": 0, "max": 0, "size": 0, "count": 0, "id": 0}
    stack, collections, fields, applications = [], [], [], []
    offsets = defaultdict(int)
    local = []
    usage_min = usage_max = None
    cursor = 0

    def usage(value, width):
        return (value >> 16, value & 0xffff) if width == 4 else (state["page"], value)

    while cursor < len(data):
        prefix = data[cursor]
        cursor += 1
        if prefix == 0xfe:
            raise ValueError("Long items are outside this descriptor contract")
        width = (0, 1, 2, 4)[prefix & 3]
        if cursor + width > len(data):
            raise ValueError("Truncated HID item")
        payload = data[cursor:cursor + width]
        cursor += width
        value = int.from_bytes(payload, "little")
        signed = int.from_bytes(payload, "little", signed=True)
        item_type, tag = (prefix >> 2) & 3, prefix >> 4
        if item_type == 1:  # Global
            if tag == 0:
                state["page"] = value
            elif tag == 1:
                state["min"] = signed
            elif tag == 2:
                state["max"] = signed if state["min"] < 0 else value
            elif tag in (3, 4, 5, 6):
                raise ValueError("Physical/unit items require an explicit contract update")
            elif tag == 7:
                state["size"] = value
            elif tag == 8:
                if not 1 <= value <= 255:
                    raise ValueError("Invalid explicit Report ID")
                state["id"] = value
            elif tag == 9:
                state["count"] = value
            elif tag == 10:
                stack.append(state.copy())
            elif tag == 11:
                if not stack:
                    raise ValueError("Global Pop without Push")
                state = stack.pop()
            else:
                raise ValueError(f"Reserved Global tag {tag}")
            continue
        if item_type == 2:  # Local
            if tag == 0:
                local.append(usage(value, width))
            elif tag == 1:
                usage_min = usage(value, width)
            elif tag == 2:
                usage_max = usage(value, width)
            else:
                raise ValueError(f"Unsupported Local tag {tag}")
            continue
        if item_type != 0:
            raise ValueError("Reserved HID item type")

        usages = list(local)
        if usage_min is not None or usage_max is not None:
            if (usage_min is None or usage_max is None or
                    usage_min[0] != usage_max[0] or
                    not 0 <= usage_max[1] - usage_min[1] <= 255):
                raise ValueError("Invalid Usage range")
            usages.extend((usage_min[0], number)
                          for number in range(usage_min[1], usage_max[1] + 1))
        if tag == 10:  # Collection
            if len(usages) != 1:
                raise ValueError("Collection requires one Usage")
            collection = (value, *usages[0])
            if not collections:
                applications.append(collection)
            collections.append(collection)
        elif tag == 12:
            if not collections:
                raise ValueError("End Collection without Collection")
            collections.pop()
        elif tag in (8, 9, 11):
            if not collections or not state["id"]:
                raise ValueError("Report field requires a collection and explicit ID")
            if not state["size"] or not state["count"]:
                raise ValueError("Report field requires a nonzero Size and Count")
            kind = {8: "input", 9: "output", 11: "feature"}[tag]
            key = (kind, state["id"])
            fields.append(Field(kind, state["id"], offsets[key], state["size"],
                                state["count"], value, state["min"], state["max"],
                                tuple(usages), tuple(collections)))
            offsets[key] += state["size"] * state["count"]
        else:
            raise ValueError(f"Reserved Main tag {tag}")
        local = []
        usage_min = usage_max = None
    if collections or stack or local or usage_min is not None or usage_max is not None:
        raise ValueError("Unbalanced or unfinished HID descriptor")
    return fields, dict(offsets), applications


def production_descriptor():
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("A C11 compiler is required")
    with tempfile.TemporaryDirectory(prefix="esp32-hid-contract-") as temporary:
        directory = Path(temporary)
        probe = directory / "descriptor_probe.c"
        binary = directory / "descriptor_probe"
        probe.write_text(
            '#include "hid_engine.h"\n#include <stdio.h>\n'
            'int main(void) {\n'
            '    return fwrite(hid_engine_report_descriptor, 1,\n'
            '                  hid_engine_report_descriptor_length, stdout)\n'
            '           == hid_engine_report_descriptor_length ? 0 : 1;\n}\n',
            encoding="utf-8")
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(ROOT / "components/input_core/include"),
            "-I", str(ROOT / "components/hid_engine/include"),
            str(ROOT / "components/input_core/input_core.c"),
            str(ROOT / "components/hid_engine/hid_engine.c"), str(probe),
            "-lm", "-o", str(binary)
        ], check=True)
        return subprocess.check_output([str(binary)])


class DescriptorContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fields, cls.bits, cls.applications = parse_descriptor(production_descriptor())

    def report(self, kind, report_id):
        return [field for field in self.fields
                if (field.kind, field.report_id) == (kind, report_id)]

    def data_field(self, field, offset, size, count, flags, page, usages, minimum, maximum):
        self.assertEqual((field.offset, field.size, field.count, field.flags),
                         (offset, size, count, flags))
        self.assertEqual(field.usages, tuple((page, number) for number in usages))
        self.assertEqual((field.logical_min, field.logical_max), (minimum, maximum))

    def padding(self, field, offset, bits):
        self.assertEqual(field.offset, offset)
        self.assertEqual(field.size * field.count, bits)
        self.assertTrue(field.flags & 1, "Padding must be Constant")

    def test_composite_ids_sizes_and_applications(self):
        # Payload sizes exclude the Report ID, which selects the GATT report.
        self.assertEqual(self.bits, {("input", 1): 64, ("output", 1): 8,
                                     ("input", 2): 32})
        self.assertCountEqual(self.applications, [(1, 1, 6), (1, 1, 2)])
        for field in self.fields:
            expected_usage = 6 if field.report_id == 1 else 2
            self.assertEqual(field.collections[0], (1, 1, expected_usage))

    def test_keyboard_modifiers_reserved_and_six_key_array(self):
        modifiers, reserved, keys = self.report("input", 1)
        self.data_field(modifiers, 0, 1, 8, 2, 7, range(0xe0, 0xe8), 0, 1)
        self.padding(reserved, 8, 8)
        self.data_field(keys, 16, 8, 6, 0, 7, range(0, 0x66), 0, 0x65)

    def test_keyboard_led_output_and_padding(self):
        leds, reserved = self.report("output", 1)
        self.data_field(leds, 0, 1, 5, 2, 8, range(1, 6), 0, 1)
        self.padding(reserved, 5, 3)

    def test_mouse_buttons_and_signed_relative_axes(self):
        buttons, reserved, axes = self.report("input", 2)
        self.data_field(buttons, 0, 1, 3, 2, 9, range(1, 4), 0, 1)
        self.padding(reserved, 3, 5)
        self.data_field(axes, 8, 8, 3, 6, 1, (0x30, 0x31, 0x38), -127, 127)
        self.assertEqual(axes.collections[-1], (0, 1, 1))  # Physical Pointer


class ParserFailureBoundaries(unittest.TestCase):
    def test_rejects_truncation_and_invalid_scopes(self):
        for data in (b"\x75", b"\xc0", b"\xb4", b"\x85\x00", b"\xfe"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                parse_descriptor(data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
