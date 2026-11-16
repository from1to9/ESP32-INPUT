# Publishing Passport Input to the FoloToy community

The [FoloToy community guide](https://github.com/FoloToy/ai-passport/blob/main/docs/development/release/publish-to-community.zh_CN.md)
calls for a complete, usable firmware release. A source commit or an ESP-IDF
application binary alone is not a community release.

## Required release material

- A public HTTPS Git source page, such as this repository.
- Chinese and English titles and summaries that describe the actual shipped
  capabilities and limitations.
- A JPEG, PNG, or WebP cover image no larger than 10 MiB.
- One merged ESP32-C3 8 MiB-flash image named
  `build/FoloToy-AI-Passport-full.bin`, validated against the firmware layout.
  `passport/build/passport_c3.bin` is the application image only and must not
  be submitted as the full image.

The official workflow installs its
[publisher package](https://ai-passport.folotoy.cn/skills/folotoy-ai-passport-publisher.zip),
checks the project and listing, and uses the creator's authorization on
`https://ai-passport.folotoy.cn`. It shows every proposed field to the creator
before uploading. It does not require storing site credentials in this repo.

## Firmware layout gate

The [upstream layout guide](https://github.com/FoloToy/ai-passport/blob/main/docs/development/engineering/firmware-layout.md)
requires the merged image, not just the app, to match the configured
`flash_args`: the bootloader, partition table and app must occupy their
declared offsets within 8 MiB. The partition table needs a valid MD5 marker,
nonoverlapping partitions and an app partition large enough for the image.
The upstream `./tools/validate.sh --firmware` builds and verifies the
**upstream project** in an isolated directory. This repository has a
separate `passport/` ESP-IDF project and therefore needs its own equivalent
release gate before claiming that the community package is ready.

An image merged at flash offset `0x0` can overwrite stored NVS settings.
Provide it only for intended full provisioning or refresh, with a clear data
warning. Ordinary development flashing should use the documented segmented
images and a compatible partition layout.

Before submission, qualify the final candidate on a real Passport: three
buttons and both display themes, BLE pairing and HID input on intended hosts,
LAN and fallback AP, desktop/iPhone/iPad browser control, Wi-Fi/BLE
coexistence, runtime memory, and optional speaker cues. Record unresolved
device or host limits in the bilingual listing. Do not upload solely on the
strength of source review, host tests or a target build.
