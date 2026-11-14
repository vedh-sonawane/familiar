# FAMILIAR

**The badge that thinks, builds, and remembers.**

FAMILIAR is custom firmware that turns the $12 Hack the North 2026 hacker badge into a pocket AI companion powered by the Claude API. It runs entirely on an ESP32-C3 (single core RISC-V, ~400KB of SRAM, no PSRAM) and talks to Claude live over Wi-Fi with streaming TLS, all on a chip smaller than a coin.

It is not a chatbot demo. It is a living little creature that chats, draws, writes its own apps, and remembers you.

> Status: Phase 1 is live. Claude streams a reply onto the badge screen over home Wi-Fi. The rest of the roadmap below is in active development.

## Why this is hard

The ESP32-C3 has no PSRAM and only a few tens of kilobytes of heap free once the display stack is running. Getting a full TLS 1.2 handshake to api.anthropic.com, plus an incremental SSE parser, to fit in that budget took real memory engineering:

- Wi-Fi and TLS are brought up only while a FAMILIAR screen is open, then torn down on exit.
- The TLS handshake runs with dynamic mbedTLS buffers and a trimmed LVGL pool so the certificate actually fits in RAM.
- The server sent event stream is parsed incrementally and never fully buffered.
- Setup credentials live only in NVS. No API key or Wi-Fi password ever touches the source or git.

## Hardware

- ESP32-C3-MINI-1: single core RISC-V, ~400KB SRAM, no PSRAM, 4MB flash, Wi-Fi and BLE
- 320x240 color TFT (LVGL)
- D-pad, A, B, Home, Start buttons
- RGB LEDs, accelerometer, NFC, USB-C

## Features

### Live now
- **Creature**: an animated pixel companion on the idle screen with five moods (idle, happy, thinking, surprised, sleeping), breathing and blinking, tilt tracked gaze, and LED mood glow. Tilt or press a button to wake it.
- **Setup portal**: first boot starts a SoftAP captive portal where you enter your Wi-Fi credentials and Anthropic API key. Stored in NVS, with a reset option.
- **Chat**: press A and Claude replies, streamed live onto the screen over HTTPS.

### Roadmap
- **Chat**: multi turn conversations, word wrapped streaming with smooth scroll, three follow up options mapped to buttons, a D-pad keyboard with word suggestions, and saved conversations on flash.
- **Hands**: Claude controls the hardware through tool use. Set LEDs by mood, draw pixel art, read tilt, save notes, set timers.
- **Forge**: describe an app in plain words and the badge writes, compiles, and installs a Lua app into the launcher, with auto repair on failure.
- **Senses**: NFC tag scanning, live web search, and a phone keyboard served at familiar.local over mDNS.
- **Memory**: FAMILIAR remembers facts about its owner across sessions, periodically summarized to stay small.
- **The Hive**: badges find each other over BLE, share Forged apps, and play Claude run party games.

See [SPEC.md](SPEC.md) for the full vision and engineering log.

## Build and flash

FAMILIAR builds with ESP-IDF v5.5.3.

```
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash
```

The firmware is a fork of the official open source badge firmware, so all of the original apps keep working and the stock firmware stays restorable.

## First time setup

1. Flash the firmware and open FAMILIAR from the launcher.
2. Hold Start to enter setup. The badge starts a Wi-Fi access point named FAMILIAR-xxxx.
3. Join that network from your phone or laptop. A setup page opens automatically.
4. Enter your home Wi-Fi name and password and your Anthropic API key, then save.
5. Open FAMILIAR and press A. Claude says hello.

Your credentials are stored in the badge NVS partition and are never written to flash as a file or committed to git.

## Architecture

```
app_familiar.cpp     Creature, setup, and chat screens (LVGL)
familiar_config.*    NVS storage for Wi-Fi creds, API key, model ids
familiar_setup.*     SoftAP captive portal (esp_http_server)
familiar_net.*       Wi-Fi STA connect, TLS, and the Claude streaming client
```

The Claude client is a raw HTTPS POST to api.anthropic.com/v1/messages using esp_http_client with the ESP-IDF certificate bundle. Replies arrive as a server sent event stream and are parsed one delta at a time.

## Credits

Built on top of the open source [Hack the North badge firmware](https://github.com/hackthenorth/badge-firmware). Licensed under GPL-3.0, same as the base firmware.
