# FAMILIAR - the badge that thinks, builds and remembers

> Custom firmware for the Hack the North 2026 hacker badge that turns it into a
> pocket AI companion powered by the Claude API. Flagship GitHub project.
> North star: this is NOT a boring chatbot demo. Every screen, animation and
> interaction must feel polished, alive and genuinely useful - a real product
> that shocks people because it runs on a ~$12 badge.

This file is the durable spec. Keep it updated as decisions are made across sessions.

---

## Hardware (verify everything against the firmware repo; do not assume)
- **MCU:** ESP32-C3-MINI-1 - single-core RISC-V, ~400 KB SRAM, **no PSRAM**, 4 MB flash, Wi-Fi + BLE
- **Display:** small color TFT (resolution TBD - confirm from firmware)
- **Buttons:** D-pad (Up/Down/Left/Right), A, B, Home, Start
- **Other:** RGB LEDs, accelerometer, NFC, power switch, USB-C
- **Stock firmware:** ESP-IDF v5.5.3, app registry + launcher, sandboxed Lua runtime
  loading user apps from `/littlefs/apps`
- Stock firmware does **not** use Wi-Fi and has **no HTTP stack** - we add that.
- Read before writing any code: `AGENTS.md`, `README.md`, `docs/LUA-API.md`,
  `tools/badgectl/web/README.md`. Follow existing conventions and the app interface exactly.

## What Familiar does

### 1. Chat
- Real chatbot: streaming replies word-by-word, proper word-wrap, smooth scrolling
- Every reply ends with 3 short follow-up options mapped to buttons (converse without typing)
- On-screen D-pad keyboard with word suggestions
- Multiple saved conversations on flash; reopen any

### 2. Hands (Claude controls hardware via tool use)
Client-side tools executed by the badge in a tool-use loop:
- `set_leds` (colors/animations matching mood)
- `draw_pixels` (Claude draws pixel art on request)
- `read_tilt` (accelerometer)
- `save_note` / `read_notes` (on-flash notes)
- `set_timer` (countdowns with LED + screen alerts)
- Animated creature on idle screen reacting to state (thinking, happy, error, sleeping).
  Tilt or press a button to wake it.

### 3. Forge (the badge programs itself)
- Describe an app in plain words → Claude writes a Lua app (official Lua guide as context)
- Badge compiles it (`luaL_loadbuffer`) **before** installing, saves to `/littlefs/apps`,
  appears in launcher
- Auto-repair: compile error or crash → error back to Claude → fix and reinstall
- Edit existing apps ("make it faster", "add a high score") and delete them

### 4. Senses
- **Scan:** tap any NFC tag/card; Familiar reads it and Claude explains/acts on it
- **Web search:** via Claude API server-side web search tool for live answers
- **Phone keyboard:** on Wi-Fi, badge serves a tiny page at `familiar.local` (mDNS) to
  type long prompts from phone straight into the badge

### 5. Memory
- Remembers facts about owner across sessions (on flash, periodically summarized by Claude
  to stay small) and uses them in replies
- Owner can view and delete memories on the badge

### 6. Bump-share (stretch)
- Send a forged app to a nearby badge over BLE, building on the firmware's badge-bump
  channel (needs fragmentation; frames are 232 bytes)

## Hard technical rules
1. Fork on top of official firmware. All original apps keep working; official firmware
   stays restorable.
2. Wi-Fi and TLS only run while a Familiar screen is open. Shut them down on exit, matching
   how the firmware handles NFC and radio.
3. HTTPS to `api.anthropic.com` via `esp_http_client` + ESP-IDF certificate bundle. Parse
   SSE streams incrementally; **never** buffer whole responses.
4. **Memory is the #1 risk.** Log free heap and largest free block at every stage and report
   the numbers. No large static buffers.
5. **Never** put an API key or Wi-Fi password in source or git. First boot → SoftAP captive
   portal where owner enters Wi-Fi creds + Anthropic API key, stored in NVS. Add "reset setup".
6. Models configurable. Defaults: `claude-haiku-5-5` for chat (fast),
   `claude-sonnet-5-5` for Forge (better code). Use prompt caching for big system prompts
   (e.g. Lua guide).  *(Verify exact model IDs against the Claude API before first call.)*
7. Every error → friendly on-screen message. Never a silent hang or reboot.

## Phases (do ONE at a time; stop after each, show what to test on the real badge, wait for go-ahead)

- **PHASE 1 - Foundation:** read repo docs; build + flash stock firmware to confirm toolchain.
  Report screen resolution, display/button/LED/accelerometer APIs, app interface, baseline
  free heap at launcher. Add a Familiar app to the launcher with its own icon. SoftAP setup
  portal (Wi-Fi + API key → NVS). First real Claude API call over HTTPS; show streamed reply.
  Report heap before/during/after.
  **Milestone:** open Familiar on the badge and Claude says hello on screen.
- **PHASE 2 - Chat:** streaming, options, keyboard, saved conversations
- **PHASE 3 - Hands:** tools, LEDs, pixel art, timers, notes, idle creature
- **PHASE 4 - Forge:** build, auto-repair, edit and delete Lua apps
- **PHASE 5 - Senses:** NFC scan, web search, `familiar.local` phone keyboard
- **PHASE 6 - Memory**
- **PHASE 7 - Bump-share (stretch)**
- **PHASE 8 - Ship it:** README with GIFs, architecture diagram, install guide (web flasher),
  release build, onboarding flow for other Hack the North attendees

---

## Expanded feature vision (added 2026-11-08, beyond the core 6)

Familiar is a platform, not a chatbot. On top of Chat/Hands/Forge/Senses/Memory/Bump:

- **The Hive** - badges discover each other over BLE (fragmented 232-byte frames); Claude
  invents a multiplayer party game on the spot, pushes it to every badge in range, runs it
  live as game master. Familiars chat with each other; bump to share Forged apps.
- **Shapeshift** - badge becomes a BLE HID keyboard / mouse / gamepad for a laptop or phone;
  tilt-to-mouse; presentation clicker with Claude-written speaker notes; Forge custom layouts.
- **The Creature** - animated pixel companion with real moods (curious/sleepy/excited/bored/
  proud) driven by usage; remembers its owner; **evolves** at milestones into new Claude-drawn
  pixel forms so no two Familiars match; reacts to shakes, tilts, NFC taps, Hive neighbors.
- **Spells** - air gestures via accelerometer template matching ($1-recognizer adapted to
  motion): circle summons, shake rerolls, flick sends to the Hive; owner teaches new spells.
- **World Link** - live web-search answers; remote MCP connectors (calendar) as stretch;
  proactive attention-getting via LEDs + creature when something matters.
- **Grow** - asked to do something it can't, Familiar uses Forge to write itself a new Lua
  skill, tests it, installs it, and uses it thereafter; skill list grows and is viewable.
- **Duels** - two Familiars battle over BLE; stats from evolution history + memories; spells
  are the attacks, cast with real gestures; Claude narrates live on both screens with synced
  LED effects; winner earns evolution progress.
- **Wander** - send your creature to a friend's badge; it hangs out, talks to their Familiar,
  comes home with a story and a Claude-drawn pixel souvenir.
- **Ghost Notes** - leave a note/drawing/app "in the air"; it hops badge-to-badge through the
  event like gossip; Claude generates chain games and spreading riddles.
- **Mosaic** - line up several badges; they detect arrangement + tilt over BLE and merge into
  one big display for Claude-generated art/animation/games spanning every screen.
- **Concert Mode** - Claude composes a light show from a prompt, synced across every badge in
  range (builds on the firmware's existing light-show feature).
- **Questline** - persistent text RPG with Claude-drawn pixel scenes saved on flash; nearby
  badges join as party via the Hive; gestures and NFC taps are in-game actions.
- **Dreams** - idle overnight on USB power, Familiar "dreams": Claude turns the day's
  conversations/duels/wanderings into a short dream story + pixel art; influences mood/evolution.
- **Sidekick** - on your laptop's network, Familiar is a companion display for Claude Code;
  hook scripts push task progress; permission requests show on screen (approve A / deny B);
  the creature reacts to builds passing and failing.

## Verified Phase-1 facts (from the official firmware, 2026-11-08)
- **Base:** forked `github.com/hackthenorth/badge-firmware` (GPL-3.0) in place with full git
  history. ESP-IDF v5.5.3, LVGL via `esp_lvgl_port`, LittleFS, Lua 5.5, 149 built-in apps.
- **Screen:** 320×240 color TFT (LVGL). **LEDs:** 6 addressable RGB (`hal_lights`, logical
  0-255 with gamma; `BadgeLed::` physical positions). **Accel:** `hal_accel` + `hal_gesture`
  (shake/tap/orientation, cached 50 Hz). **Buttons:** `Button` enum A/B/Home/Down/Left/Right/
  Up/Aux1/Start via `ButtonEvent`.
- **App interface:** subclass `App` (`main/apps/app.h`) - `name/icon/slug/on_enter(root)` +
  optional `on_exit/on_tick/on_button`; all hooks under the LVGL lock. `AppAccess`
  Default/Public/Internal. Register in `main/main.cpp`, forward-declare in `app_launcher.h`;
  new `.cpp` needs `idf.py reconfigure` (sources globbed at configure time).
- **Config/NVS:** `hal_config_get/set_string|int(slug, key, ...)` → our Wi-Fi creds + API key
  store (never in source). NVS partition is only 0x4000 (16 KB) - keep keys small.
- **Partitions:** factory app `0x2A0000` (2.75 MB, generous for added TLS/HTTP), storage
  LittleFS `0x140000`. **No Wi-Fi/HTTP in stock deps - we add esp-tls/esp_http_client.**
- **Dev loop:** `tools/badge/badge.py` drives the badge over USB (press/shot/tree/expect) with
  no human watching - the vibecoding loop. WASM emulator in `tools/emulator/` (no heap/timing
  fidelity - confirm memory-sensitive changes on hardware).
- **Build:** `source ~/.espressif/tools/activate_idf_v5.5.3.sh` then
  `python "$IDF_PATH/tools/idf.py" build`. Flash/monitor touch the device - only on request.

## Decisions & status log
- **2026-11-09 - Setup portal DONE + hard-won hardware lessons (verified on badge):**
  - **Dual-power-source crash (the big one):** the badge resets the instant Wi-Fi transmits if BOTH
    the battery switch is ON and USB is plugged - the two supplies fight and the rail collapses. Use
    ONE source: **batteries only (switch on, USB out)** or **a wall charger / PC-USB with the battery
    switch OFF**. USB-only is required for flashing. This was the root cause behind most of the
    "setup won't appear" pain, not code.
  - **BLE-reclaim is the memory unlock:** `esp_bt_controller_mem_release(ESP_BT_MODE_BLE)` at the
    start of any Familiar Wi-Fi flow frees **~130 KB** (heap jumps from ~60 KB to ~196 KB free /
    117 KB largest). This is what makes Wi-Fi AP + HTTP server + TLS all fit. BLE is gone until the
    next reboot, so Familiar **reboots on exit** from any Wi-Fi screen to restore it (stock BLE apps
    keep working). With this, the earlier TLS-memory fear is moot - mbedTLS fits easily.
  - Other gotchas now in `main/familiar_setup.cpp`: brownout detector must be **disabled** (Wi-Fi TX
    spikes trip even the least-sensitive level on this BLE-tuned board); cap Wi-Fi TX power
    (`esp_wifi_set_max_tx_power`); `LWIP_MAX_SOCKETS` must be ≥ httpd's `max_open_sockets`+3 (httpd
    failed silently at 4); the POST handler's form buffer must be `static` (stack overflow crashed
    submit); iOS needs "Auto-Join OFF" on the home network or it bounces off the badge AP, and the
    portal page must be opened manually at `http://192.168.4.1/` (no DNS captive hijack yet).
  - **Models:** the spec's `claude-haiku-5-5` / `claude-sonnet-5-5` don't exist. Using the verified
    IDs - **`claude-haiku-4-5`** for chat (fastest/cheapest), `claude-sonnet-4-6` for Forge. Config
    defaults in `familiar_config.h`; overridable via NVS.
  - **Claude client (built, awaiting live test):** `familiar_net_claude_stream()` does a raw-HTTP
    `POST https://api.anthropic.com/v1/messages` (`x-api-key` + `anthropic-version: 2023-06-01`,
    `stream:true`) over esp_http_client + the ESP-IDF cert bundle, parsing the SSE
    `content_block_delta` → `delta.text` incrementally (never buffering the whole response) and
    streaming decoded text to the UI. Runs on a 12 KB worker task; the creature's **A** button opens
    a Chat screen that connects Wi-Fi and streams Claude's first-boot greeting. Heap logged at each
    stage (sta before/after, tls before/connected/after).
- **2026-11-09 - Wi-Fi RAM diet WORKS (verified on hardware):** With the `sdkconfig.defaults` diet
  (STATIC_RX=2, DYN_RX/TX=6, AMPDU off, RX_MGMT=2, `ESP_WIFI_IRAM_OPT`/`RX_IRAM_OPT` off),
  `esp_wifi_init()` + `esp_wifi_start()` now **succeed** (were `ESP_ERR_NO_MEM`). Probe from a ~59 KB
  state: netif ~10 KB, wifi driver ~22 KB; **full Wi-Fi up leaves free=25.7 KB / largest=17.4 KB**,
  cleanly reclaimed on teardown. Remaining gap: a TLS handshake needs a ~16 KB contiguous record
  buffer + working space, so 17 KB largest is short. Endgame: (a) run networking from the leanest
  screen (Familiar app with launcher torn down ⇒ ~82 KB start, more headroom + larger blocks);
  (b) enable `CONFIG_MBEDTLS_DYNAMIC_BUFFER` + `MBEDTLS_DYNAMIC_FREE_PEER_CERT` and trim
  `MBEDTLS_SSL_OUT_CONTENT_LEN` (requests are tiny; IN must stay ~16 KB since Cloudflare won't honor
  RFC-6066 max_fragment_length); (c) consider shrinking `CONFIG_LV_MEM_SIZE_KILOBYTES` only if needed
  (risks stock apps). Also: the C3 USB-JTAG port wedges on Windows after serial console reads - a
  physical USB replug is needed before each flash (known quirk, not our bug).
- **2026-11-08 - Repartition + Wi-Fi RAM probe (verified on hardware):** Grew `factory` to
  0x350000; binary with `esp_wifi`/`esp_netif` linked is 0x305550 (3.02 MB, +510 KB vs creature-
  only) - **9% free on the new partition; would have massively overflowed the old 2.75 MB one.**
  Backed up the badge FS, flashed, LittleFS reformatted, **restored `identity.json` + `solana.json`
  over the console `put` protocol (verified; badge provisioned again).** Backups + restore script in
  `C:\esp\familiar-badge-backup\` (contains a Solana secret key - never commit).
  - 🔬 **`wifiprobe` result - THE constraint:** `esp_wifi_init()` with **default** config fails
    `ESP_ERR_NO_MEM` ("malloc buffer fail", wants 32 RX + 32 TX dyn buffers). Free heap is also
    **screen-dependent: ~82 KB on the My Badge page vs only ~40 KB free / 27 KB largest on the
    Launcher grid** (the launcher holds ~40 KB; LVGL pool only ~12 KB used, so it's system-heap, not
    the LVGL pool). **Plan to make Wi-Fi fit:** (1) slash Wi-Fi buffers in `sdkconfig.defaults`
    (`ESP_WIFI_STATIC_RX_BUFFER_NUM`, `DYNAMIC_RX/TX_BUFFER_NUM` to minimums, `TX_BUFFER_TYPE`, AMPDU
    off); (2) bring Wi-Fi up from a lean state - tear down the launcher/other UI and possibly shrink
    the 64 KB LVGL pool during the Claude call; (3) then tune mbedTLS (reduce
    `MBEDTLS_SSL_IN/OUT_CONTENT_LEN`, dynamic buffers). Re-probe after each. Added
    `main/familiar_net.{h,cpp}` + `wifiprobe` console cmd for this measurement loop.

- **2026-11-08:** Spec captured. Found + forked the official firmware (above). Read all four
  required docs (AGENTS.md, README.md, docs/LUA-API.md, tools/badgectl/web/README.md) and the
  core architecture (app.h, main.cpp, HAL headers, launcher_slugs).
- **2026-11-08 - Toolchain + first flash DONE (verified on hardware):** Installed ESP-IDF v5.5.3
  at `C:\esp\esp-idf` (bootstrapped with Python 3.12 at
  `C:\Users\sonaw\AppData\Local\Programs\Python\Python312`; system Python 3.14 left untouched).
  Build clean (exit 0, `app_familiar.cpp` compiled no warnings). Flashed to the badge on **COM3**
  (ESP32-C3 rev v0.4, MAC e8:3d:c1:27:c7:80), hash verified. **Baseline heap at launcher (read via
  the `heap` console cmd): sys_free=78,540 B, sys_largest=65,536 B (64 KB), sys_min=69,508 B;
  LVGL pool 63,428 total / 54,352 free.**
  - ⚠️ **App partition 97% full** (`hello_world.bin` 0x288bf0 in 0x2a0000 factory, ~95 KB free)
    AND only a **64 KB largest free block** at runtime. The Anthropic TLS client is therefore the
    hardest part of the project: naive `esp_http_client` + default mbedTLS (16 KB in + 16 KB out
    record buffers + cert parse peak) will **not** fit the 64 KB block, and the extra code won't fit
    the partition. Plan for the network step: (1) repartition - shrink `storage` LittleFS to grow
    `factory` (user OK'd wiping the badge); (2) tune mbedTLS down (reduce
    `MBEDTLS_SSL_IN/OUT_CONTENT_LEN`, enable dynamic buffers / cert bundle trimming); (3) tear down
    the chat UI's LVGL widgets before the handshake; (4) measure heap at every step.
- **2026-11-08 - Phase 1, step 1 DONE (awaiting on-badge test):** Added `main/apps/app_familiar.cpp`
  - the **Creature** idle screen (5 moods: idle/happy/thinking/surprised/sleeping; breathing,
  blinking, tilt-tracked gaze, LED mood-glow, sleep/wake). Registered first in `main.cpp`,
  forward-declared in `app_launcher.h`. Text icon `:3` (real image icon TBD via image-convert
  pipeline). Pure LVGL + existing HALs, no networking. **Next:** user builds/flashes, confirms
  the creature is alive, and reports **baseline free heap + largest free block at the launcher**
  (needed before choosing the TLS strategy - memory is the #1 risk). Then: config/NVS schema →
  SoftAP captive portal → Anthropic HTTPS SSE client → "Claude says hello."
- **2026-11-14 - PHASE 1 COMPLETE ✅ - Claude streamed a hello onto the badge over home Wi-Fi.**
  Full chain working: SoftAP setup portal (Wi-Fi creds + API key -> NVS) → STA connect → HTTPS/TLS
  to api.anthropic.com → streamed SSE reply on screen. The two real blockers (found only after
  capturing serial logs via `tools/wifi_capture.py`, not from the one-line on-screen error):
  (1) **Association** - the AP is WPA2 on ch6, and the all-channel in-connect scan kept landing on
  ch11 (reason 2/205). FIX: explicit SSID scan + pin strongest BSS (bssid+channel) + patient spaced
  connect loop. (2) **TLS** - memory starvation (only ~11KB free with the 64KB LVGL pool; Wi-Fi
  driver logged `wifi:mem fail` dropping the cert packets -> le0x8006 timeout). FIX:
  `CONFIG_LV_MEM_SIZE_KILOBYTES 64->48` frees 16KB -> ~27KB during TLS -> cert validated -> reply.
  NOT WPA3/MTU/router/creds (all red herrings). Heap at tls-connected: 11K free / 5.6K largest.
  See [[badge-firmware-base]] for the full heap map and the supporting fixes (ps-none, ipv6 off,
  mss 1200, dynamic mbedTLS, SNTP, brownout off). **TODO before shipping:** verify stock apps
  (esp. Mines9) still run at 48KB LVGL; strip the diagnostic code (cf/an tcp probes, verbose scan
  logging, the double TLS connect from the leftover probe). **Next: Phase 2 - Chat** (multi-turn,
  streaming word-wrap + scroll, 3 button-mapped follow-ups, D-pad keyboard, saved conversations).
- **2026-11-14 - PHASE 2 (part 1) DONE: multi-turn chat with button-mapped follow-ups.** Press A on
  the creature and Familiar holds a real conversation. Each reply streams in word-wrapped to a
  scrollable view (Up/Down scroll), and ends with three short follow-up suggestions parsed from an
  OPTS line and mapped to A / B / LEFT, so a whole conversation runs on buttons alone. Start begins a
  fresh chat; Home exits. New networking entry point familiar_net_claude_stream_msgs sends the full
  bounded history (last kMaxTurns messages) plus a system prompt, and reuses the Wi-Fi link between
  turns (no reconnect). Memory note: the multi-turn buffers cost ~8 KB and re-broke the TLS cert
  verification (Dynamic alloc 4437 failed, mbedtls -0x3000). Reclaimed it by moving the message
  builder to the task stack, shrinking buffers (chat_reply 1024, body 2560, history 4x150), capping
  max_tokens at 200, and LVGL 48 -> 44 KB. Verified on badge: 4 turns, cert validated every turn,
  heap steady at ~27 KB free after each turn, no leak. **Next in Phase 2: D-pad keyboard** to type
  your own prompt (not just the suggestions), then saved conversations on flash.
- **2026-11-14 - PHASE 2 (part 2) DONE: D-pad keyboard + runs fully STANDALONE (off a computer).**
  Added an on-screen keyboard (Right opens it; D-pad moves the key, A types, B deletes, Start sends)
  so you can type any prompt. The hard part was making chat work on battery / a wall charger, not
  just tethered to a PC. Root causes found with an on-flash black-box recorder (familiar_net_log ->
  /littlefs/famlog.txt, read back with `cat`), since serial is unavailable off-computer:
    1. esp_http_client returns ESP_ERR_HTTP_CONNECT (0x7002) off-computer though raw esp-tls to the
       same host works. FIX: dropped esp_http_client; send the HTTP/1.1 request by hand over esp-tls
       (connection: close; feed the body straight to the SSE parser, which ignores chunk-size lines).
    2. USB-Serial-JTAG console write blocks forever with no host draining it, hanging the net task.
       FIX: esp_log_set_vprintf hook that drops log lines when usb_serial_jtag_is_connected() is
       false. (An earlier O_NONBLOCK-on-stdout hack ALSO broke stdin -> REPL spun printing "badge> "
       millions of times, starving tasks - reverted.)
    3. THE follow-up bug: a fresh 12 KB worker task was spawned PER TURN; once Wi-Fi fragments the
       heap the 12 KB contiguous stack could not be allocated (worked tethered by luck). FIX: ONE
       persistent worker created in enter_chat (before Wi-Fi), woken per turn via task notification.
  Supporting: SNTP now retries 4x8s (single "clock sync failed" was a lost UDP query); TX power 72
  -> 52 (lower current for weak batteries); APB_FREQ_MAX pm-lock held for the session; self-heal
  reconnect if a turn's link is stale. Verified: full multi-turn conversation on a wall charger with
  no computer attached. **Next in Phase 2: saved conversations on flash** (reopen past chats). Then
  Phase 3 (Hands: tools, LEDs, pixel art, timers, notes, idle creature).
