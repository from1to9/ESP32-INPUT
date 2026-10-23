#!/usr/bin/env python3
"""Check the real forced-header ordering and target/mode profile boundary."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ConfigTests(unittest.TestCase):
    def test_forced_include_matrix(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        cases = [
            ("s31_dma", "#define CONFIG_IDF_TARGET_ESP32S31 1\n#define CONFIG_TINYUSB_MODE_DMA 1\n", True, 1),
            ("s31_slave", "#define CONFIG_IDF_TARGET_ESP32S31 1\n#define CONFIG_TINYUSB_MODE_SLAVE 1\n", True, 0),
            ("other_dma", "#define CONFIG_IDF_TARGET_ESP32P4 1\n#define CONFIG_TINYUSB_MODE_DMA 1\n", True, 0),
            ("zero_target", "#define CONFIG_IDF_TARGET_ESP32S31 0\n#define CONFIG_TINYUSB_MODE_DMA 1\n", True, 0),
            ("zero_dma", "#define CONFIG_IDF_TARGET_ESP32S31 1\n#define CONFIG_TINYUSB_MODE_DMA 0\n", True, 0),
            ("host", "", False, 0),
        ]
        for name, sdk, platform, expected in cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory(prefix="usb-profile-") as directory:
                base = Path(directory); (base / "sdkconfig.h").write_text(sdk)
                # No CONFIG macros are supplied on the command line. They must
                # arrive from sdkconfig while the production header is forced first.
                flags = [compiler, "-E", "-x", "c", "-I", str(base), "-I", str(ROOT / "tests/usb_hid/stubs"),
                         "-isystem", str(ROOT / "firmware/managed_components/espressif__tinyusb/src"),
                         "-include", str(ROOT / "firmware/main/usb_audio_config.h")]
                if platform:
                    flags += ["-DESP_PLATFORM=1"]
                source = '#include "tusb_option.h"\n' + (
                    f"#if INPUT_USB_AUDIO_PTI_REQUIRED != {expected} || CFG_TUD_DWC2_PTI_ENABLE != {expected}\n"
                    '#error "Wrong effective USB profile"\n#endif\n')
                result = subprocess.run(flags + ["-"], input=source, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
