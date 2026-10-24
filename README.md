<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Muse Waveshare Charm

A fork of Meta's [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk)
tuned for one device: a pocket "charm" built on the
[Waveshare ESP32-S3-Touch-AMOLED-1.75C](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C)
(round 466 px AMOLED, dual mics, speaker, battery). It's a push-to-talk
front end to a Muse agent: press the side button, talk, and the reply is
shown and spoken on the charm.

Our work lives on the `charm` branch. `main` mirrors upstream untouched.
Everything under [Upstream README](#upstream-readme-muse-gadgets) below is
Meta's, kept as it was.

## What's different from upstream

**Face and screen**
- **Custom avatar:** a crystal (a dodecahedron–icosahedron compound) rendered in real 3D, with motion for each state ([`components/muse/avatar/muse_pixel.c`](esp32/components/muse/avatar/muse_pixel.c)).
- **Readable captions:** slowed to 10 characters a second, with the last lines held for 4 s.
- **24-hour clock** on its own line at the top of the face, set by SNTP (US Central) ([`charm_clock.c`](esp32/components/muse/charm_clock.c)).

**Notifications from Muse**
- **`notify.show` / `notify.clear` commands.** The agent can push a reminder with a chime, short text, ✓/✗ buttons and an icon it draws itself: 16×16 pixel art that the charm extrudes into a rocking 3D token. The answer goes back to Muse as a chat message, `[charm notify] id=… answer=confirmed|declined` ([`charm_notify.c`](esp32/components/muse/charm_notify.c), [`charm_token.c`](esp32/components/muse/charm_token.c)).
- **Inbox:** a page to the left of the face holds recent notifications. Swipe one away to answer it later.

**Voice**
- **Spoken replies, on-device:** [PicoTTS](esp32/components/picotts) (US English) speaks Muse's text replies, with the captions following the speech. It runs at 90% pitch and below the UI's priority, so it only uses spare CPU. Its 1.1 MB work area and task stack are in PSRAM. With the speaker off, replies show as captions only ([`charm_tts.c`](esp32/components/muse/charm_tts.c)).
- **Tagged voice notes:** each note goes to Muse with the message `[charm]` and the file name `charm_note.wav`, so the agent knows it came from the charm and can answer briefly.
- **No saved notes:** a press only records when Muse is reachable. Otherwise the screen says why ("NO WI-FI", "CAN'T REACH MUSE"). A note that fails partway is dropped and the reason shown.

**Power and reliability**
- **Wi-Fi never naps on a timer**, so notifications still arrive on battery.
- **Long talk presses no longer power-cycle the board.** The talk button is the PWR key, which the AXP2101 power chip also watches: holding it 10 s turned the board off, and 16 s reset the chip. Both are now turned off at boot ([`muse_pmu.c`](esp32/components/muse/muse_pmu.c)).
- **Boot diagnostics:** the boot log shows the last six reset reasons and why the power chip last turned on and off.

**Build and release**
- `esp32/version.txt` is `1.0.0`. [CI](.github/workflows/charm-firmware.yml) builds every push to `charm` as `1.0.<run number>` and publishes it as a release (see below).

## Build and flash over USB

Needs ESP-IDF v6.0.1 and Python 3.10+, as in [`esp32/README.md`](esp32/README.md).

```bash
cd esp32
IDF_EXPORT=~/esp/esp-idf-v6/export.sh tools/muse/board.sh build s3
IDF_EXPORT=~/esp/esp-idf-v6/export.sh tools/muse/board.sh flash s3
```

Pair it with the Muse app as upstream describes, and give it an SDK token over
serial (`>sdk_token=…`).

## OTA updates

- Every push to `charm` runs [`charm-firmware.yml`](.github/workflows/charm-firmware.yml). It makes the release `fw-1.0.<run>` with the signed OTA image `charm-1.0.<run>.bin`, a USB image set, and `SHA256SUMS`.
- To update, Muse calls the charm's `device.ota` command with the image's URL, for example `https://github.com/beardy-dev/muse-waveshare-charm/releases/download/fw-1.0.1/charm-1.0.1.bin`. The charm checks that the version is newer (unless `force` is set) and checks the signature, installs it to the other app slot, and reboots. It rolls back if the new image can't reach Muse.
- **Signing:** images are signed with `esp32/dev_signing_key.pem`, upstream's *public* development key. Anyone could sign an image this firmware accepts, although installing one still takes a Muse session or an encrypted BLE link. Switch to a private key before trusting this beyond a hobby device.

## Bench console

Over the USB serial console: `>say=<text>` speaks text, `>tts.pitch=<50-200>` sets the voice pitch, and `>power` prints battery and power stats. Bench keys `d` / `u` press and release the talk button. Note that opening the port with pyserial resets the board. To watch the log without a reset, read the port raw, the way `cat` does.

## Known limits

- Voice notes stop at 15 s (Muse's limit, according to the SDK).
- Memory is tight: about 31 KB of internal RAM free, about 830 KB of PSRAM with the speech engine up, and the app fills 3.8 MB of its 4 MB slot.
- PicoTTS only has female voices; the lowered pitch makes it deeper.

---

# Upstream README: Muse Gadgets

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/images/muse-gadgets-dark.png">
    <img src=".github/images/muse-gadgets-light.png" width="900" alt="Muse gadgets: a Waveshare round AMOLED, an M5Stack StickS3, Muse Home Link, a Raspberry Pi and a Seeed reTerminal e-ink display">
  </picture>
</p>

Muse gadgets are open source devices you build yourself. Program an
off-the-shelf ESP32 board or set up a Raspberry Pi with our device SDKs, then
connect Muse to your displays, buttons, sensors, actuators, and whatever else
you've got lying on your workbench.

We open sourced the SDKs and firmware here. It's built by hackers, for hackers,
just for fun. Side effects of tinkering may include bricked boards, voided
warranties, brownouts, or bankruptcies. Proceed at your own risk!

| | |
|---|---|
| [**ESP32 Device SDK**](esp32) | Connect your ESP32 board to Muse through our open source SDK. Throw in a screen to show images, add audio in and out, or wire up other sensors. |
| [**Linux Device SDK**](linux) | Turn that spare Raspberry Pi or Linux box into a Muse gadget. Hack in your own commands to let Muse handle sysadmin chores or your Home Assistant setup. |
| [**Muse Gadgets Test**](android/musegadgetstest) ([iPhone](ios/musegadgetstest)) | Stream your phone's camera, or a clip, to an ESP32 gadget's screen over Wi-Fi: H.264 or VP8 decoded on the board, 30 fps on the round 466 px boards. [`protocols/VIDEO.md`](protocols/VIDEO.md) explains how. |

Before you flash or pair a gadget, get an
[SDK token](https://gadgets.muse.ai/settings/sdk-tokens) and review the
[Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms). Every gadget needs a
token to pair.

ESP32 and Linux gadgets pair with the Muse app on iOS and Android, via
Settings > Devices. Turn on Developer mode there first, then look for devices
prefixed with "MuseGadget".
Each directory has a `README.md` to get started and an `AGENTS.md` for coding
agents like [Muse Code](https://developer.meta.com/ai/lp/muse-code/).

For battery-input power profiling, see the [porting recipe](esp32/docs/power/README.md).
The opt-in diagnostic core is board-neutral; boards supply its hooks and state
matrices, and [power_sweep.py](esp32/tools/power/power_sweep.py) drives the
USB-free sweep. The isolated diagnostic does not pair or need an SDK token;
fixture safety and explicit hardware authorization still come first, as the
[PPK2 rules](esp32/docs/power/ppk2.md) describe.

## Community

Meet other hackers who are building and customizing Muse gadgets in our
community [Discord](https://discord.gg/3bhjCkZdd6). Get inspired, support each
other, and share what you make.

## License

Muse Gadgets is licensed under the Apache License, Version 2.0, found in
[`LICENSE`](LICENSE), except for these third-party files, which keep their
upstream licenses:

| Path | Upstream | License |
|---|---|---|
| [`esp32/components/minimp3/include/minimp3.h`](esp32/components/minimp3) | [lieff/minimp3](https://github.com/lieff/minimp3) | CC0-1.0, see [`LICENSE`](esp32/components/minimp3/LICENSE) |
| [`esp32/components/muse/boards/ostb_echoear_display_init.h`](esp32/components/muse/boards/ostb_echoear_display_init.h) | [78/xiaozhi-esp32 ESP-VoCat panel sequence](https://github.com/78/xiaozhi-esp32/blob/0d576d3d4c049c6f55eaf879725dc23e516511b4/main/boards/espressif/esp-vocat/esp_vocat.cc) | MIT, retained in the file header |
| [`esp32/main/pixel_font.c`](esp32/main/pixel_font.c) | Adafruit GFX `glcdfont.c` | BSD-2-Clause, in the file header |
| [`esp32/main/epd_ssd1677.c`](esp32/main/epd_ssd1677.c) | Seeed-reTerminal-E10xx-Firmware's SSD1677 refresh sequence | MIT, retained in the file header |
| [`esp32/components/tinyh264/src`](esp32/components/tinyh264) | the Android Open Source Project's H.264 decoder, via [udevbe/tinyh264](https://github.com/udevbe/tinyh264) | Apache-2.0, AOSP's copyright, see [`LICENSE`](esp32/components/tinyh264/LICENSE) |
| [`esp32/components/libvpx_vp8/src`](esp32/components/libvpx_vp8) | [libvpx](https://chromium.googlesource.com/webm/libvpx) v1.16.0's VP8 decoder, trimmed | BSD-3-Clause, the WebM Project authors' copyright, see [`LICENSE`](esp32/components/libvpx_vp8/LICENSE), with Google's patent grant in [`PATENTS`](esp32/components/libvpx_vp8/PATENTS) |
| [`esp32/components/picotts`](esp32/components/picotts) (charm) | SVOX Pico TTS from the Android Open Source Project, via [jmattsson/picotts](https://components.espressif.com/components/jmattsson/picotts) 1.1.3 (DiUS ESP-IDF glue), with charm changes marked `charm:` | Apache-2.0, SVOX AG's copyright, see [`NOTICE`](esp32/components/picotts/pico/lib/NOTICE) |

Dependencies fetched at build time are under their own licenses: ESP-IDF
components (into `esp32/managed_components/`), and the simulator's LVGL and
SDL (listed in [`esp32/simulator/THIRD_PARTY.md`](esp32/simulator/THIRD_PARTY.md)).

The Apache License does not cover the [Jollybot avatar](esp32/avatar).

Muse Gadgets Test's sample clips aren't in this repository: the app fetches a few
seconds of the Blender Foundation's open movies (CC BY 3.0) when one is
played, and shows their credits.

Muse Gadgets Test can use x264 (GPL-2.0) on the phone; this repository carries none
of its code. An APK or iPhone app you build with it is under the GPL
([`android/musegadgetstest/README.md`](android/musegadgetstest/README.md),
[`ios/musegadgetstest/README.md`](ios/musegadgetstest/README.md)).
