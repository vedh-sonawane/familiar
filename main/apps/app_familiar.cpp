// FAMILIAR - the badge that thinks, builds and remembers.
//
// Phase 1, step 1: the Creature. A living pixel companion that breathes,
// blinks, tracks your tilt, naps when ignored and wakes when you touch it.
// This is the idle screen of Familiar and the first thing you flash: it proves
// the fork builds on stock firmware and needs no network. Networking (SoftAP
// setup, the Anthropic streaming client) layers on in later steps; everything
// here uses only display / LED / accelerometer HALs that already exist.
//
// Controls:  A pet (happy)   B think   D-pad glance   SHAKE/TAP boop
//            tilt to make the eyes look around   HOME exits.

#include "app.h"
#include "app_registry.h"
#include "hal_lights.h"
#include "hal_gesture.h"
#include "ui_theme.h"
#include "familiar_config.h"
#include "familiar_setup.h"
#include "familiar_net.h"
#include "hal_fs.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>

#include "esp_random.h"
#include "esp_system.h"   // esp_restart
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace {

constexpr int kCx = 160;   // screen centre x (320 wide)
constexpr int kCy = 116;   // creature centre y (240 tall)

constexpr int kSleepAfterTicks = 600;   // ~12 s at 50 Hz before a nap

// Claude's look: warm off-white background, near-black warm text, coral accent.
namespace Cl {
    constexpr uint32_t BG      = 0xF5F4EE;   // Claude cream / off-white
    constexpr uint32_t TEXT    = 0x262624;   // warm near-black
    constexpr uint32_t MUTED   = 0x83817A;   // muted warm gray
    constexpr uint32_t CORAL   = 0xC15F3C;   // Claude coral (deeper, for light bg)
    constexpr uint32_t USERBUB = 0xE7E5DC;   // user message pill
    constexpr uint32_t SELTEXT = 0xFFFFFF;   // text on a coral-highlighted row
}

inline int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }


enum class Mood : uint8_t { Idle, Happy, Thinking, Surprised, Sleeping };

// Antenna-tip / glow signature colour for each mood (0xRRGGBB).
uint32_t mood_hue(Mood m)
{
    switch (m) {
        case Mood::Happy:     return 0xf4c95d; // warm gold
        case Mood::Thinking:  return 0x8a7bd8; // indigo
        case Mood::Surprised: return 0xffd166; // amber pop
        case Mood::Sleeping:  return 0x4f5f9a; // dim blue
        case Mood::Idle:      default: return 0xb087b1; // theme violet
    }
}

const char *mood_word(Mood m)
{
    switch (m) {
        case Mood::Happy:     return "happy";
        case Mood::Thinking:  return "hmm...";
        case Mood::Surprised: return "!!";
        case Mood::Sleeping:  return "zzz";
        case Mood::Idle:      default: return "curious";
    }
}

class FamiliarApp : public App {
public:
    const char *name() const override { return "Claude"; }
    const char *icon() const override { return "*"; }
    const char *slug() const override { return "familiar"; }
    AppAccess   access() const override { return AppAccess::Default; }

    void on_enter(lv_obj_t *root) override
    {
        familiar_cfg_init();
        root_ = root;
        start_hold_ = 0;
        // First run (no Wi-Fi + key yet): become a setup access point instead of
        // the creature. Everything else below is the creature screen.
        if (!familiar_cfg_is_configured()) { enter_setup(root); return; }

        // HOME: the Claude logo on a warm charcoal screen. Tilt stretches the rays.
        mode_ = Mode::Creature;
        frame_ = 0;
        look_x_ = look_y_ = look_tx_ = look_ty_ = 0.f;

        lv_obj_set_style_bg_color(root, lv_color_hex(Cl::BG), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

        logo_line_ = lv_line_create(root);
        lv_obj_set_pos(logo_line_, 0, 0);
        lv_obj_set_size(logo_line_, 320, 190);
        lv_obj_set_style_line_color(logo_line_, lv_color_hex(Cl::CORAL), 0);
        lv_obj_set_style_line_width(logo_line_, 5, 0);
        lv_obj_set_style_line_rounded(logo_line_, true, 0);

        home_name_ = lv_label_create(root);
        lv_label_set_text(home_name_, "Claude");
        lv_obj_set_style_text_color(home_name_, lv_color_hex(Cl::TEXT), 0);
        lv_obj_set_style_text_font(home_name_, &lv_font_montserrat_24, 0);
        lv_obj_align(home_name_, LV_ALIGN_CENTER, 0, 52);

        home_hint_ = lv_label_create(root);
        lv_label_set_text(home_hint_, "press A to chat");
        lv_obj_set_style_text_color(home_hint_, lv_color_hex(Cl::MUTED), 0);
        lv_obj_set_style_text_font(home_hint_, &lv_font_montserrat_14, 0);
        lv_obj_align(home_hint_, LV_ALIGN_BOTTOM_MID, 0, -8);

        render();
    }

    // Draw the Claude sunburst as one polyline that zig-zags out to each ray tip
    // and back through the centre. Rays breathe, slowly rotate, and stretch toward
    // however the badge is tilted (look_x_/look_y_ track the accelerometer).
    void render_home()
    {
        if (!logo_line_) return;
        const int cx = 160, cy = 84;
        const int kRays = 12;
        const float t = frame_ * 0.05f;
        for (int i = 0; i < kRays; i++) {
            float ang = (float)i / kRays * 6.2832f + t * 0.12f;
            float dx = cosf(ang), dy = sinf(ang);
            float tilt  = dx * look_x_ * 2.4f + dy * look_y_ * 2.4f;
            float pulse = 5.f * sinf(t * 1.4f + i * 0.5f);
            float R = 42.f + pulse + tilt;
            if (R < 16.f) R = 16.f;
            logo_pts_[2 * i].x     = (lv_coord_t)(cx + lroundf(dx * R));
            logo_pts_[2 * i].y     = (lv_coord_t)(cy + lroundf(dy * R));
            logo_pts_[2 * i + 1].x = cx;
            logo_pts_[2 * i + 1].y = cy;
        }
        logo_pts_[2 * kRays].x = (lv_coord_t)(cx + 42);
        logo_pts_[2 * kRays].y = cy;
        lv_line_set_points(logo_line_, logo_pts_, 2 * kRays + 1);
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        if (mode_ == Mode::Setup) {
            familiar_setup_stop();   // tear down the AP + HTTP server on the way out
            // We released the BLE controller's RAM for Wi-Fi; a reboot is the only
            // way to get BLE back, so leaving setup always boots fresh. (Harmless
            // for the one-time setup flow, and keeps every stock BLE app working.)
            esp_restart();
        }
        if (mode_ == Mode::Chat) {
            save_chat();   // persist the conversation before we reboot
            // Chat also released BLE for Wi-Fi; reboot to restore it (and to kill
            // the network worker cleanly if it's mid-request).
            esp_restart();
        }
        title_ = mood_label_ = hint_ = zzz_ = nullptr;
        shadow_ = antenna_ = tip_ = body_ = nullptr;
        eye_l_ = eye_r_ = pupil_l_ = pupil_r_ = mouth_ = nullptr;
        cheek_l_ = cheek_r_ = nullptr;
        setup_ssid_ = setup_status_ = setup_steps_ = setup_diag_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (mode_ == Mode::Setup) {
            if (ev.kind == ButtonEventKind::Pressed && ev.button == Button::A
                && familiar_setup_saved()) {
                esp_restart();   // reboot into the configured creature + connect
            }
            return;
        }
        // Chat list: pick New chat or a saved one with Up/Down, open with A.
        if (mode_ == Mode::ChatList) {
            if (ev.kind != ButtonEventKind::Pressed) return;
            int n = cl_count_ + 1;   // row 0 is "New chat"
            if (ev.button == Button::Up || ev.button == Button::Down) {
                cl_highlight(cl_sel_, false);
                cl_sel_ = (ev.button == Button::Up) ? (cl_sel_ - 1 + n) % n : (cl_sel_ + 1) % n;
                cl_highlight(cl_sel_, true);
                return;
            }
            if (ev.button == Button::A) {
                if (cl_sel_ == 0) enter_chat(root_, -1);                 // new
                else              enter_chat(root_, cl_ids_[cl_sel_ - 1]); // reopen
                return;
            }
            if (ev.button == Button::B && cl_sel_ > 0) {   // delete the highlighted chat
                delete_chat(cl_ids_[cl_sel_ - 1]);
                enter_chat_list(root_);                    // rebuild the list
                return;
            }
            return;   // HOME exits the app via the registry
        }

        // Chat. Scrolling is always allowed; option/new-chat buttons only fire when
        // the turn has fully finished (no worker task running), so A can never spawn
        // a second colliding network task. HOME exits via the registry -> on_exit.
        if (mode_ == Mode::Chat) {
            if (ev.kind != ButtonEventKind::Pressed) return;
            // The keyboard is modal: while open it owns every button.
            if (kb_open_) {
                switch (ev.button) {
                    case Button::Up:    kb_move(-1, 0); break;
                    case Button::Down:  kb_move( 1, 0); break;
                    case Button::Left:  kb_move( 0, -1); break;
                    case Button::Right: kb_move( 0,  1); break;
                    case Button::A:     kb_type(); break;
                    case Button::B:     kb_backspace(); break;
                    case Button::Start: close_keyboard(true); break;   // empty = cancel
                    default: break;
                }
                return;
            }
            if (ev.button == Button::Up)   { if (chat_scroll_) lv_obj_scroll_by(chat_scroll_, 0,  44, LV_ANIM_ON); return; }
            if (ev.button == Button::Down) { if (chat_scroll_) lv_obj_scroll_by(chat_scroll_, 0, -44, LV_ANIM_ON); return; }
            bool busy = !chat_done_ || !chat_processed_;   // a turn is in flight
            if (busy) return;
            if (ev.button == Button::Right) { open_keyboard(); return; }   // type your own
            if (ev.button == Button::Start) { new_conversation(); return; }
            int pick = (ev.button == Button::A)    ? 0
                     : (ev.button == Button::B)    ? 1
                     : (ev.button == Button::Left) ? 2 : -1;
            if (pick >= 0 && pick < opt_n_) pick_option(pick);
            return;
        }

        // Creature. Start is a deliberate hold-to-reset (see on_tick); a tap does
        // nothing so it can't be mistaken for a reset. Track press/release here.
        if (ev.button == Button::Start) {
            start_hold_ = (ev.kind == ButtonEventKind::Pressed) ? 1 : 0;
            return;
        }
        if (ev.kind != ButtonEventKind::Pressed) return;
        const bool was_asleep = (mood_ == Mood::Sleeping);
        wake();

        switch (ev.button) {
            case Button::A:     set_mood(Mood::Happy, 40); enter_chat_list(root_); return;
            case Button::B:     set_mood(Mood::Thinking, 110); break;
            case Button::Left:  glance(-7.f,  1.f);            break;
            case Button::Right: glance( 7.f,  1.f);            break;
            case Button::Up:    glance( 0.f, -6.f);            break;
            case Button::Down:  glance( 0.f,  6.f);            break;
            default: break;
        }
        // Waking is its own little event; don't also read it as a pet.
        if (was_asleep && ev.button != Button::A && ev.button != Button::B) {
            set_mood(Mood::Surprised, 40);
        }
    }

    void on_tick() override
    {
        frame_++;

        if (mode_ == Mode::Setup) { tick_setup(); return; }
        if (mode_ == Mode::Chat)  { tick_chat();  return; }
        if (mode_ == Mode::ChatList) return;   // static list, nothing to animate

        // HOME. Hold Start ~2 s to wipe Wi-Fi + key and drop back into setup.
        if (start_hold_ > 0) {
            start_hold_++;
            if (start_hold_ == 30 && home_hint_) lv_label_set_text(home_hint_, "hold to reset...");
            if (start_hold_ >= 110) { familiar_cfg_reset(); esp_restart(); }
        }

        // Tilt stretches the logo rays (signs tuned so it leans the way you tip it).
        AccelSample s = gesture_last_sample();
        float tilt_x = clampf(-s.x_mg * 0.010f, -6.f, 6.f);
        float tilt_y = clampf( s.y_mg * 0.010f, -6.f, 6.f);
        look_x_ += (tilt_x - look_x_) * 0.12f;
        look_y_ += (tilt_y - look_y_) * 0.12f;

        render_home();

        // Coral breathing on the LEDs.
        if ((frame_ & 1) == 0) {
            float k = 0.25f + 0.25f * (0.5f + 0.5f * sinf(frame_ * 0.06f));
            set_all(0xD9, 0x77, 0x57, k);
        }
    }

private:
    // ---- first-run setup screen -------------------------------------------
    void enter_setup(lv_obj_t *root)
    {
        mode_ = Mode::Setup;
        setup_ap_done_ = false;
        familiar_setup_begin();   // AP comes up a step per tick (see tick_setup)

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "FAMILIAR");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

        lv_obj_t *sub = lv_label_create(root);
        lv_label_set_text(sub, "first-time setup");
        lv_obj_set_style_text_color(sub, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 36);

        setup_steps_ = lv_label_create(root);
        lv_obj_set_style_text_color(setup_steps_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(setup_steps_, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(setup_steps_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(setup_steps_, 300);
        lv_obj_align(setup_steps_, LV_ALIGN_TOP_LEFT, 12, 66);
        lv_label_set_text(setup_steps_,
            "1  On your phone, join this Wi-Fi:");

        setup_ssid_ = lv_label_create(root);
        lv_obj_set_style_text_color(setup_ssid_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(setup_ssid_, &lv_font_montserrat_20, 0);
        lv_obj_align(setup_ssid_, LV_ALIGN_TOP_MID, 0, 92);
        lv_label_set_text_fmt(setup_ssid_, "%s", familiar_setup_ap_ssid());

        lv_obj_t *step2 = lv_label_create(root);
        lv_obj_set_style_text_color(step2, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(step2, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(step2, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(step2, 300);
        lv_obj_align(step2, LV_ALIGN_TOP_LEFT, 12, 122);
        lv_label_set_text(step2,
            "2  Open  http://192.168.4.1\n3  Enter Wi-Fi + Anthropic key");

        setup_status_ = lv_label_create(root);
        lv_obj_set_style_text_color(setup_status_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(setup_status_, &lv_font_montserrat_14, 0);
        lv_obj_align(setup_status_, LV_ALIGN_BOTTOM_MID, 0, -26);
        lv_label_set_text(setup_status_, "waiting for your phone...");

        // Diagnostics line (AP bring-up result + live heap) so setup problems
        // are visible on the badge without a serial cable.
        setup_diag_ = lv_label_create(root);
        lv_obj_set_style_text_color(setup_diag_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_style_text_font(setup_diag_, &lv_font_montserrat_14, 0);
        lv_obj_align(setup_diag_, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    void tick_setup()
    {
        if (!setup_status_) return;

        // Bring the AP up one step per tick, but only after the screen has had a
        // few frames to render first - so if a step crashes, the badge is frozen
        // showing the step name + heap from this diag line.
        if (!setup_ap_done_ && frame_ > 12) {
            int r = familiar_setup_step();
            if (r != 0) setup_ap_done_ = true;
        }
        if (setup_diag_ && (frame_ % 3) == 0) {
            unsigned f = (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
            unsigned l = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
            lv_label_set_text_fmt(setup_diag_, "%s  heap %u/%u", familiar_setup_status_str(), f, l);
        }
        if (familiar_setup_saved()) {
            lv_label_set_text(setup_status_, "Saved! Press A to wake Familiar");
            // Steady green: configured.
            float k = 0.5f + 0.5f * sinf(frame_ * 0.08f);
            set_all(40, 220, 90, k);
            return;
        }
        int clients = familiar_setup_client_count();
        if (clients > 0) {
            lv_label_set_text(setup_status_, "phone connected - open 192.168.4.1");
        } else {
            lv_label_set_text(setup_status_, "waiting for your phone...");
        }
        // Breathing violet while waiting, warmer once a phone joins.
        float k = 0.25f + 0.3f * (0.5f + 0.5f * sinf(frame_ * 0.05f));
        if (clients > 0) set_all(180, 140, 70, k);
        else             set_all(120, 80, 150, k);
    }

    // ---- chat: connect Wi-Fi + stream a Claude greeting -------------------
    static void chat_delta_cb(const char *text, int len, void *ctx)
    {
        auto *self = static_cast<FamiliarApp *>(ctx);
        xSemaphoreTake(self->chat_mu_, portMAX_DELAY);
        for (int i = 0; i < len && self->chat_reply_len_ < (int)sizeof(self->chat_reply_buf_) - 1; i++)
            self->chat_reply_buf_[self->chat_reply_len_++] = text[i];
        self->chat_reply_buf_[self->chat_reply_len_] = '\0';
        self->chat_dirty_ = true;
        xSemaphoreGive(self->chat_mu_);
    }
    static void chat_status_cb(const char *status, void *ctx)
    {
        auto *self = static_cast<FamiliarApp *>(ctx);
        xSemaphoreTake(self->chat_mu_, portMAX_DELAY);
        strlcpy(self->chat_status_buf_, status, sizeof(self->chat_status_buf_));
        self->chat_dirty_ = true;
        xSemaphoreGive(self->chat_mu_);
    }
    // ONE persistent worker for the whole chat session, created in enter_chat while
    // the heap still has a big contiguous block (before Wi-Fi fragments it). It
    // sleeps on a task notification and runs one turn per notify. This replaces the
    // old spawn-a-task-per-turn design, whose 12KB stack could not be allocated
    // once Wi-Fi was up and the heap fragmented - the exact off-computer bug.
    static void chat_worker_fn(void *arg)
    {
        auto *self = static_cast<FamiliarApp *>(arg);
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // wait for the next turn
            self->run_one_turn();
        }
    }

    void run_one_turn()
    {
        char err[64] = {0};
        int r = -1;
        familiar_net_log("turn: entered");
        // Two tries: if the request fails because the Wi-Fi link went stale between
        // turns, tear it down and reconnect from scratch, then retry once.
        for (int tryc = 0; tryc < 2; tryc++) {
            familiar_net_log("turn: try %d has_ip=%d", tryc, (int)familiar_net_sta_has_ip());
            if (!familiar_net_sta_has_ip()) {
                chat_status_cb(tryc == 0 ? "connecting to Wi-Fi..." : "reconnecting...", this);
                if (!familiar_net_sta_connect(60000, err, sizeof(err))) {
                    familiar_net_log("turn: sta_connect FAILED (%s)", err);
                    chat_status_cb(err[0] ? err : "Wi-Fi failed", this);
                    chat_done_ = true;
                    chat_processed_ = true;   // nothing to parse
                    return;
                }
            }
            chat_status_cb("Claude is thinking...", this);
            char msgs[2048];
            build_messages(msgs, sizeof(msgs));
            r = familiar_net_claude_stream_msgs(kSysPrompt, msgs,
                &FamiliarApp::chat_delta_cb, &FamiliarApp::chat_status_cb, this);
            familiar_net_log("turn: stream r=%d", r);
            if (r == 0 || r == -5) break;   // success, or a real HTTP error (shown): done
            if (tryc == 0) { familiar_net_log("turn: dropping link to reconnect"); familiar_net_sta_disconnect(); }
        }
        familiar_net_log("turn: exiting r=%d", r);
        chat_done_ = true;
    }

    // ---- multi-turn helpers ----------------------------------------------
    static int json_escape(const char *in, char *out, int cap)
    {
        int o = 0;
        for (const char *p = in; *p && o < cap - 3; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
            else if (c == '\n')        { out[o++] = '\\'; out[o++] = 'n'; }
            else if (c == '\t')        { out[o++] = ' '; }
            else if (c < 0x20)         { /* drop other control chars */ }
            else                       { out[o++] = (char)c; }
        }
        out[o] = '\0';
        return o;
    }

    void build_messages(char *out, int cap)
    {
        int o = 0;
        o += snprintf(out + o, cap - o, "[");
        for (int i = 0; i < hist_n_ && o < cap - 16; i++) {
            char esc[kMsgMax * 2 + 4];
            json_escape(hist_[i].text, esc, sizeof(esc));
            o += snprintf(out + o, cap - o, "%s{\"role\":\"%s\",\"content\":\"%s\"}",
                          i ? "," : "", hist_[i].user ? "user" : "assistant", esc);
        }
        snprintf(out + o, cap - o, "]");
    }

    void hist_add(bool user, const char *text)
    {
        if (hist_n_ >= kMaxTurns) {   // drop the oldest pair to keep the window small
            int drop = 2;
            for (int i = drop; i < hist_n_; i++) hist_[i - drop] = hist_[i];
            hist_n_ -= drop;
        }
        hist_[hist_n_].user = user;
        strlcpy(hist_[hist_n_].text, text, kMsgMax);
        hist_n_++;
    }

    // Reset the streaming buffers and spawn a worker for the next turn.
    void send_turn()
    {
        if (!chat_mu_) chat_mu_ = xSemaphoreCreateMutex();
        xSemaphoreTake(chat_mu_, portMAX_DELAY);
        chat_reply_len_ = 0;
        chat_reply_buf_[0] = '\0';
        strlcpy(chat_status_buf_, "Claude is thinking...", sizeof(chat_status_buf_));
        chat_dirty_ = true;
        xSemaphoreGive(chat_mu_);
        chat_done_ = false;
        chat_processed_ = false;
        opt_n_ = 0;
        for (int i = 0; i < 3; i++) if (opt_lbl_[i]) lv_label_set_text(opt_lbl_[i], "");
        if (chat_reply_) lv_label_set_text(chat_reply_, "");
        // Wake the single persistent worker (created once in enter_chat). No
        // per-turn task allocation, so a fragmented heap can never block a turn.
        familiar_net_log("send_turn: notify");
        if (chat_worker_) {
            xTaskNotifyGive(chat_worker_);
        } else {
            chat_status_cb("worker missing", this);
            chat_done_ = true;
            chat_processed_ = true;
        }
    }

    void pick_option(int i)
    {
        submit_user(opt_[i]);
    }

    void new_conversation()
    {
        save_chat();                 // keep the one we were in
        cur_chat_id_ = next_chat_id();
        hist_n_ = 0;
        if (chat_scroll_) lv_obj_clean(chat_scroll_);   // wipe the transcript
        msg_count_ = 0;
        streaming_bubble_ = nullptr;
        opt_n_ = 0;
        for (int i = 0; i < 3; i++) if (opt_lbl_[i]) lv_label_set_text(opt_lbl_[i], "");
        if (chat_status_) lv_label_set_text(chat_status_, "");
        show_greeting();
        chat_done_ = true;
        chat_processed_ = true;
    }

    // ---- saved conversations on flash ------------------------------------
    void chat_path(int id, char *out, int cap) { snprintf(out, cap, "%s/%d.txt", kChatsDir, id); }

    int next_chat_id()
    {
        int mx = 0;
        DIR *d = opendir(kChatsDir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != nullptr) { int id = atoi(e->d_name); if (id > mx) mx = id; }
            closedir(d);
        }
        return mx + 1;
    }

    // Persist the current bounded history (last few messages) to this chat's file.
    void save_chat()
    {
        if (cur_chat_id_ < 0 || hist_n_ < 2) return;   // nothing worth saving
        hal_fs_mkdir_p(kChatsDir);
        char path[64]; chat_path(cur_chat_id_, path, sizeof(path));
        FILE *f = fopen(path, "w");
        if (!f) return;
        for (int i = 0; i < hist_n_; i++) {
            fputc(hist_[i].user ? 'u' : 'a', f);
            for (const char *p = hist_[i].text; *p; p++)
                fputc((*p == '\n' || *p == '\r') ? ' ' : *p, f);   // one line per message
            fputc('\n', f);
        }
        fclose(f);
    }

    void load_chat(int id)
    {
        hist_n_ = 0;
        cur_chat_id_ = id;
        char path[64]; chat_path(id, path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (!f) return;
        char line[kMsgMax + 8];
        while (fgets(line, sizeof(line), f)) {
            int L = (int)strlen(line);
            while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
            if (L < 1) continue;
            hist_add(line[0] == 'u', line + 1);
        }
        fclose(f);
    }

    // The user's first question makes the most recognizable list title.
    void chat_title(int id, char *out, int cap)
    {
        out[0] = '\0';
        char path[64]; chat_path(id, path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (f) {
            char line[kMsgMax + 8];
            while (fgets(line, sizeof(line), f)) {
                if (line[0] == 'u') {
                    strlcpy(out, line + 1, cap);
                    int L = (int)strlen(out);
                    while (L > 0 && (out[L - 1] == '\n' || out[L - 1] == '\r')) out[--L] = '\0';
                    break;
                }
            }
            fclose(f);
        }
        if (!out[0]) snprintf(out, cap, "chat %d", id);
    }

    void delete_chat(int id)
    {
        char path[64]; chat_path(id, path, sizeof(path));
        remove(path);
    }

    void scan_chats()
    {
        cl_count_ = 0;
        DIR *d = opendir(kChatsDir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != nullptr && cl_count_ < kMaxSaved) {
                int id = atoi(e->d_name);
                if (id > 0) cl_ids_[cl_count_++] = id;
            }
            closedir(d);
        }
        for (int i = 1; i < cl_count_; i++) {   // newest (highest id) first
            int v = cl_ids_[i], j = i - 1;
            while (j >= 0 && cl_ids_[j] < v) { cl_ids_[j + 1] = cl_ids_[j]; j--; }
            cl_ids_[j + 1] = v;
        }
    }

    // ---- the chat-list screen --------------------------------------------
    lv_obj_t *make_cl_row(lv_obj_t *root, const char *txt, int y)
    {
        lv_obj_t *r = lv_label_create(root);
        lv_label_set_text(r, txt);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(r, lv_color_hex(Cl::TEXT), 0);
        lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r, 300);
        lv_obj_set_style_bg_color(r, lv_color_hex(Cl::USERBUB), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(r, 4, 0);
        lv_obj_set_style_pad_left(r, 6, 0);
        lv_obj_set_style_pad_ver(r, 2, 0);
        lv_obj_align(r, LV_ALIGN_TOP_LEFT, 8, y);
        return r;
    }

    void cl_highlight(int idx, bool on)
    {
        if (idx < 0 || idx > kMaxSaved || !cl_rows_[idx]) return;
        lv_obj_set_style_bg_opa(cl_rows_[idx], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(cl_rows_[idx], lv_color_hex(on ? Cl::CORAL : Cl::USERBUB), 0);
        lv_obj_set_style_text_color(cl_rows_[idx], lv_color_hex(on ? Cl::SELTEXT : Cl::TEXT), 0);
    }

    void enter_chat_list(lv_obj_t *root)
    {
        lv_obj_clean(root);
        title_ = mood_label_ = hint_ = zzz_ = nullptr;
        shadow_ = antenna_ = tip_ = body_ = nullptr;
        eye_l_ = eye_r_ = pupil_l_ = pupil_r_ = mouth_ = cheek_l_ = cheek_r_ = nullptr;
        for (int i = 0; i <= kMaxSaved; i++) cl_rows_[i] = nullptr;
        hal_lights_clear(); hal_lights_show();
        mode_ = Mode::ChatList;
        lv_obj_set_style_bg_color(root, lv_color_hex(Cl::BG), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

        scan_chats();
        cl_sel_ = 0;

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Your chats");
        lv_obj_set_style_text_color(title, lv_color_hex(Cl::CORAL), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        const int y0 = 36;
        cl_rows_[0] = make_cl_row(root, "+  New chat", y0);
        for (int i = 0; i < cl_count_; i++) {
            char t[48]; chat_title(cl_ids_[i], t, sizeof(t));
            cl_rows_[i + 1] = make_cl_row(root, t, y0 + (i + 1) * 20);
        }

        lv_obj_t *hint = lv_label_create(root);
        lv_label_set_text(hint, "up/dn   A open   B delete   HOME exit");
        lv_obj_set_style_text_color(hint, lv_color_hex(Cl::MUTED), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

        cl_highlight(0, true);
    }

    // Parse the finished reply: split off the OPTS line into the three follow-up
    // buttons and commit the clean reply text to the conversation history.
    void process_reply()
    {
        const char *opts = strstr(chat_reply_buf_, "OPTS:");
        int full_n = opts ? (int)(opts - chat_reply_buf_) : (int)strlen(chat_reply_buf_);
        while (full_n > 0 && (chat_reply_buf_[full_n - 1] == '\n' || chat_reply_buf_[full_n - 1] == ' '
                              || chat_reply_buf_[full_n - 1] == '\r')) full_n--;

        // The bubble shows the WHOLE reply (never truncated). Only the copy kept as
        // conversation CONTEXT is capped at kMsgMax to keep the request body small.
        if (streaming_bubble_) {
            static char full[1536];
            int fn = full_n; if (fn > (int)sizeof(full) - 1) fn = sizeof(full) - 1;
            memcpy(full, chat_reply_buf_, fn); full[fn] = '\0';
            lv_label_set_text(streaming_bubble_, full[0] ? full : "(no reply)");
            streaming_bubble_ = nullptr;
        }
        char clean[kMsgMax];
        int cn = full_n; if (cn > kMsgMax - 1) cn = kMsgMax - 1;
        memcpy(clean, chat_reply_buf_, cn); clean[cn] = '\0';
        if (clean[0]) hist_add(false, clean);

        opt_n_ = 0;
        if (opts) {
            char buf[160];
            strlcpy(buf, opts + 5, sizeof(buf));   // text after "OPTS:"
            char *tok = buf;
            while (opt_n_ < 3 && tok && *tok) {
                char *bar = strchr(tok, '|');
                if (bar) *bar = '\0';
                while (*tok == ' ' || *tok == '\n' || *tok == '\r') tok++;
                int L = (int)strlen(tok);
                while (L > 0 && (tok[L - 1] == ' ' || tok[L - 1] == '\n' || tok[L - 1] == '\r'))
                    tok[--L] = '\0';
                if (L > 0) { strlcpy(opt_[opt_n_], tok, sizeof(opt_[opt_n_])); opt_n_++; }
                if (!bar) break;
                tok = bar + 1;
            }
        }

        const char *keycap[3] = {"A", "B", "LEFT"};
        for (int i = 0; i < 3; i++) {
            if (!opt_lbl_[i]) continue;
            if (i < opt_n_) lv_label_set_text_fmt(opt_lbl_[i], "%s  %s", keycap[i], opt_[i]);
            else            lv_label_set_text(opt_lbl_[i], "");
        }
        if (opt_n_ == 0 && opt_lbl_[0]) lv_label_set_text(opt_lbl_[0], "START  new chat");
        save_chat();   // persist after every reply, not just on exit
    }

    // ---- on-screen keyboard ----------------------------------------------
    lv_obj_t *make_key(const char *txt, int x, int y, int w, int h)
    {
        lv_obj_t *k = lv_label_create(root_);
        lv_label_set_text(k, txt);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(Cl::TEXT), 0);
        lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(k, lv_color_hex(Cl::USERBUB), 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(k, 4, 0);
        lv_obj_set_style_pad_top(k, 3, 0);
        lv_obj_set_size(k, w, h);
        lv_obj_set_pos(k, x, y);
        return k;
    }

    void kb_highlight(int r, int c, bool on)
    {
        lv_obj_t *k = kb_keys_[r][c];
        if (!k) return;
        lv_obj_set_style_bg_color(k, lv_color_hex(on ? Cl::CORAL : Cl::USERBUB), 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(on ? Cl::SELTEXT : Cl::TEXT), 0);
    }

    void kb_update_line()
    {
        if (kb_textline_) lv_label_set_text_fmt(kb_textline_, "%s_", kb_text_);
    }

    void open_keyboard()
    {
        if (kb_open_) return;
        kb_open_ = true;
        kb_len_ = 0; kb_text_[0] = '\0';
        kb_row_ = 1; kb_col_ = 0;

        if (chat_scroll_) lv_obj_add_flag(chat_scroll_, LV_OBJ_FLAG_HIDDEN);
        if (chat_status_) lv_obj_add_flag(chat_status_, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) if (opt_lbl_[i]) lv_obj_add_flag(opt_lbl_[i], LV_OBJ_FLAG_HIDDEN);

        kb_textline_ = lv_label_create(root_);
        lv_obj_set_style_text_color(kb_textline_, lv_color_hex(Cl::TEXT), 0);
        lv_obj_set_style_text_font(kb_textline_, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(kb_textline_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(kb_textline_, 304);
        lv_obj_align(kb_textline_, LV_ALIGN_TOP_LEFT, 8, 30);
        lv_label_set_text(kb_textline_, "_");

        const int y0 = 74;
        for (int r = 0; r < 5; r++) {
            int len = (int)strlen(kKbRows[r]);
            int rowy = y0 + r * 27;
            if (r == 4) {
                kb_keys_[4][0] = make_key("space", 20, rowy, 168, 24);
                kb_keys_[4][1] = make_key("del",   196, rowy, 92, 24);
            } else {
                int x0 = 6 + ((10 - len) * 31) / 2;
                for (int c = 0; c < len; c++) {
                    char s[2] = { kKbRows[r][c], 0 };
                    kb_keys_[r][c] = make_key(s, x0 + c * 31, rowy, 28, 24);
                }
            }
        }

        kb_hint_ = lv_label_create(root_);
        lv_label_set_text(kb_hint_, "move   A type   B back   START send");
        lv_obj_set_style_text_color(kb_hint_, lv_color_hex(Cl::MUTED), 0);
        lv_obj_set_style_text_font(kb_hint_, &lv_font_montserrat_14, 0);
        lv_obj_align(kb_hint_, LV_ALIGN_BOTTOM_MID, 0, -4);

        kb_highlight(kb_row_, kb_col_, true);
    }

    void close_keyboard(bool send)
    {
        if (!kb_open_) return;
        kb_open_ = false;
        if (kb_textline_) { lv_obj_del(kb_textline_); kb_textline_ = nullptr; }
        if (kb_hint_)     { lv_obj_del(kb_hint_);     kb_hint_ = nullptr; }
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 10; c++)
                if (kb_keys_[r][c]) { lv_obj_del(kb_keys_[r][c]); kb_keys_[r][c] = nullptr; }

        if (chat_status_) lv_obj_clear_flag(chat_status_, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) if (opt_lbl_[i]) lv_obj_clear_flag(opt_lbl_[i], LV_OBJ_FLAG_HIDDEN);
        if (chat_scroll_) lv_obj_clear_flag(chat_scroll_, LV_OBJ_FLAG_HIDDEN);

        if (send && kb_len_ > 0) {   // empty + Start = cancel
            submit_user(kb_text_);
        }
    }

    int kb_row_len(int r) { return (r == 4) ? 2 : (int)strlen(kKbRows[r]); }  // row 4 = space + del

    void kb_move(int dr, int dc)
    {
        kb_highlight(kb_row_, kb_col_, false);
        if (dr) {
            kb_row_ = clampi(kb_row_ + dr, 0, 4);
            kb_col_ = clampi(kb_col_, 0, kb_row_len(kb_row_) - 1);
        }
        if (dc) {
            kb_col_ = clampi(kb_col_ + dc, 0, kb_row_len(kb_row_) - 1);
        }
        kb_highlight(kb_row_, kb_col_, true);
    }

    void kb_type()
    {
        if (kb_row_ == 4) {                      // bottom row: space or del
            if (kb_col_ == 1) { kb_backspace(); return; }
            if (kb_len_ < (int)sizeof(kb_text_) - 1) { kb_text_[kb_len_++] = ' '; kb_text_[kb_len_] = '\0'; kb_update_line(); }
            return;
        }
        char ch = kKbRows[kb_row_][kb_col_];
        if (kb_len_ < (int)sizeof(kb_text_) - 1) {
            kb_text_[kb_len_++] = ch;
            kb_text_[kb_len_] = '\0';
            kb_update_line();
        }
    }

    void kb_backspace()
    {
        if (kb_len_ > 0) { kb_text_[--kb_len_] = '\0'; kb_update_line(); }
    }

    void enter_chat(lv_obj_t *root, int load_id = -1)
    {
        // Leave the creature behind: clear its widgets and null the handles so
        // the (now-disabled) creature render path can't touch freed objects.
        lv_obj_clean(root);
        title_ = mood_label_ = hint_ = zzz_ = nullptr;
        shadow_ = antenna_ = tip_ = body_ = nullptr;
        eye_l_ = eye_r_ = pupil_l_ = pupil_r_ = mouth_ = cheek_l_ = cheek_r_ = nullptr;
        greeting_ = nullptr;   // was deleted by lv_obj_clean
        hal_lights_clear(); hal_lights_show();

        mode_ = Mode::Chat;
        if (!chat_mu_) chat_mu_ = xSemaphoreCreateMutex();

        lv_obj_set_style_bg_color(root, lv_color_hex(Cl::BG), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

        lv_obj_t *brand = lv_label_create(root);
        lv_label_set_text(brand, "Claude");
        lv_obj_set_style_text_color(brand, lv_color_hex(Cl::CORAL), 0);
        lv_obj_set_style_text_font(brand, &lv_font_montserrat_16, 0);
        lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 10, 6);

        chat_status_ = lv_label_create(root);
        lv_obj_set_style_text_color(chat_status_, lv_color_hex(Cl::MUTED), 0);
        lv_obj_set_style_text_font(chat_status_, &lv_font_montserrat_14, 0);
        lv_obj_align(chat_status_, LV_ALIGN_TOP_RIGHT, -8, 8);
        lv_label_set_text(chat_status_, "");

        // The transcript: a vertical flex-column scroll of message bubbles. Every
        // message gets its own bubble, so scrolling shows the whole conversation.
        chat_scroll_ = lv_obj_create(root);
        lv_obj_set_size(chat_scroll_, 316, 146);
        lv_obj_align(chat_scroll_, LV_ALIGN_TOP_MID, 0, 28);
        lv_obj_set_style_bg_opa(chat_scroll_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chat_scroll_, 0, 0);
        lv_obj_set_style_pad_all(chat_scroll_, 2, 0);
        lv_obj_set_style_pad_row(chat_scroll_, 6, 0);
        lv_obj_set_flex_flow(chat_scroll_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(chat_scroll_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(chat_scroll_, LV_SCROLLBAR_MODE_AUTO);
        chat_reply_ = nullptr;
        streaming_bubble_ = nullptr;
        msg_count_ = 0;

        // Follow-up chips, mapped to A / B / LEFT.
        for (int i = 0; i < 3; i++) {
            opt_lbl_[i] = lv_label_create(root);
            lv_obj_set_style_text_color(opt_lbl_[i], lv_color_hex(Cl::CORAL), 0);
            lv_obj_set_style_text_font(opt_lbl_[i], &lv_font_montserrat_14, 0);
            lv_label_set_long_mode(opt_lbl_[i], LV_LABEL_LONG_DOT);
            lv_obj_set_width(opt_lbl_[i], 300);
            lv_obj_align(opt_lbl_[i], LV_ALIGN_BOTTOM_LEFT, 10, -60 + i * 18);
            lv_label_set_text(opt_lbl_[i], "");
        }

        lv_obj_t *hint = lv_label_create(root);
        lv_label_set_text(hint, "A/B/LEFT reply   RIGHT type   HOME exit");
        lv_obj_set_style_text_color(hint, lv_color_hex(Cl::MUTED), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

        familiar_net_log_reset();
        familiar_net_log("enter_chat load=%d", load_id);
        // Create the persistent network worker NOW, while the heap still has a big
        // contiguous block (Wi-Fi has not fragmented it yet). One 12KB stack for the
        // whole session, reused every turn.
        if (!chat_worker_)
            xTaskCreate(&FamiliarApp::chat_worker_fn, "fam_chat", 12288, this, 5, &chat_worker_);

        if (load_id >= 0) {
            // Reopen a saved chat: rebuild the transcript. Idle (no auto-turn) -
            // the owner continues by typing (Right).
            load_chat(load_id);
            for (int i = 0; i < hist_n_; i++) add_bubble(hist_[i].user, hist_[i].text);
            lv_label_set_text(chat_status_, "RIGHT to type");
            if (chat_scroll_) lv_obj_scroll_to_y(chat_scroll_, LV_COORD_MAX, LV_ANIM_OFF);
        } else {
            // Brand new empty chat: a short randomized greeting, the user asks.
            cur_chat_id_ = next_chat_id();
            hist_n_ = 0;
            lv_label_set_text(chat_status_, "");
            show_greeting();
        }
        chat_done_ = true;
        chat_processed_ = true;
        opt_n_ = 0;
    }

    // Add one message bubble to the transcript. User bubbles sit right in a filled
    // pill; Claude replies are plain cream text on the left, like the Claude app.
    lv_obj_t *add_bubble(bool user, const char *text)
    {
        if (!chat_scroll_) return nullptr;
        lv_obj_t *b = lv_label_create(chat_scroll_);
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(b, user ? 224 : 300);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(b, lv_color_hex(Cl::TEXT), 0);
        if (user) {
            lv_obj_set_style_bg_color(b, lv_color_hex(Cl::USERBUB), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(b, 12, 0);
            lv_obj_set_style_pad_all(b, 7, 0);
            lv_obj_set_style_margin_left(b, 64, 0);   // push the pill to the right
        } else {
            lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_all(b, 2, 0);
        }
        lv_label_set_text(b, text);
        if (++msg_count_ > 16) {   // bound memory: drop the oldest bubble
            lv_obj_t *first = lv_obj_get_child(chat_scroll_, 0);
            if (first) { lv_obj_del(first); msg_count_--; }
        }
        return b;
    }

    static const char *rand_greeting()
    {
        static const char *g[] = {
            "Hey there.", "What's up?", "Hi!", "Ask me anything.",
            "Ready when you are.", "What's on your mind?", "Let's chat.",
            "How can I help?", "Hello.", "Go ahead, ask away.",
        };
        return g[esp_random() % (sizeof(g) / sizeof(g[0]))];
    }

    // A short, friendly greeting centred on an otherwise-empty new chat.
    void show_greeting()
    {
        if (greeting_) { lv_obj_del(greeting_); greeting_ = nullptr; }
        greeting_ = lv_label_create(root_);
        lv_label_set_text(greeting_, rand_greeting());
        lv_obj_set_style_text_color(greeting_, lv_color_hex(Cl::TEXT), 0);
        lv_obj_set_style_text_font(greeting_, &lv_font_montserrat_24, 0);
        lv_obj_align(greeting_, LV_ALIGN_CENTER, 0, -14);
    }

    // Send a user message: show its bubble, record it, open an empty Claude bubble
    // to stream into, and wake the worker.
    void submit_user(const char *text)
    {
        if (!text || !text[0]) return;
        if (greeting_) { lv_obj_del(greeting_); greeting_ = nullptr; }   // first message clears it
        add_bubble(true, text);
        hist_add(true, text);
        streaming_bubble_ = add_bubble(false, "...");
        send_turn();
    }

    void tick_chat()
    {
        if (chat_dirty_) {
            xSemaphoreTake(chat_mu_, portMAX_DELAY);
            if (chat_status_) lv_label_set_text(chat_status_, chat_status_buf_);
            if (streaming_bubble_) {
                // Show the reply so far minus the trailing OPTS line (which becomes
                // the follow-up chips once the turn finishes).
                const char *opts = strstr(chat_reply_buf_, "OPTS:");
                static char disp[1536];
                int n = opts ? (int)(opts - chat_reply_buf_) : (int)strlen(chat_reply_buf_);
                if (n > (int)sizeof(disp) - 1) n = sizeof(disp) - 1;
                memcpy(disp, chat_reply_buf_, n);
                while (n > 0 && (disp[n - 1] == '\n' || disp[n - 1] == ' ' || disp[n - 1] == '\r')) n--;
                disp[n] = '\0';
                lv_label_set_text(streaming_bubble_, disp[0] ? disp : "...");
            }
            chat_dirty_ = false;
            xSemaphoreGive(chat_mu_);
            if (chat_scroll_) lv_obj_scroll_to_y(chat_scroll_, LV_COORD_MAX, LV_ANIM_OFF);
        }
        // Turn finished: parse the OPTS line into chips + commit to history once.
        if (chat_done_ && !chat_processed_) {
            process_reply();
            chat_processed_ = true;
        }
    }

    static bool is_transient(Mood m) { return m != Mood::Idle && m != Mood::Sleeping; }

    void wake()
    {
        idle_ticks_ = 0;
        if (mood_ == Mood::Sleeping) {
            mood_ = Mood::Idle;
            sync_mood_label();
        }
    }

    void set_mood(Mood m, int ticks)
    {
        mood_ = m;
        mood_ticks_ = ticks;
        idle_ticks_ = 0;
        sync_mood_label();
    }

    void glance(float tx, float ty)
    {
        look_tx_ = tx;
        look_ty_ = ty;
    }

    void sync_mood_label()
    {
        if (mood_label_) lv_label_set_text(mood_label_, mood_word(mood_));
    }

    // Centre an object of known size on an absolute screen point. We pass the
    // size explicitly rather than calling lv_obj_get_width(): a size set earlier
    // this frame is not reflected in the object's coords until LVGL's next
    // layout pass, so querying it here would centre using a stale value.
    static void place(lv_obj_t *o, int x, int y, int w, int h)
    {
        lv_obj_set_pos(o, x - w / 2, y - h / 2);
    }

    void render()
    {
        if (!body_) return;
        const float t = frame_ * 0.1f;
        const bool sleeping = (mood_ == Mood::Sleeping);
        const bool happy = (mood_ == Mood::Happy);
        const bool surprised = (mood_ == Mood::Surprised);

        // Breathing + mood bounce.
        const float breathe = sinf(t * (sleeping ? 0.5f : 1.1f));
        float bob = 0.f;
        if (happy)          bob = -fabsf(sinf(t * 2.4f)) * 9.f;
        else if (surprised) bob = -9.f + (float)mood_ticks_ * 0.15f;
        bob = clampf(bob, -14.f, 0.f);

        const int cy = kCy + (int)lroundf(bob);
        const int bodyH = kBodyH + (int)lroundf(breathe * 3.f);
        const int bodyW = kBodyW - (int)lroundf(breathe * 2.f);
        lv_obj_set_size(body_, bodyW, bodyH);
        place(body_, kCx, cy, bodyW, bodyH);

        // Ground shadow: wider/darker when grounded, tighter when airborne.
        const int shadowW = clampi(118 + (int)lroundf(bob), 70, 124);
        lv_obj_set_size(shadow_, shadowW, 20);
        place(shadow_, kCx, 196, shadowW, 20);

        // Antenna sways gently; tip glows the mood hue.
        const int sway = (int)lroundf(sinf(t * 0.9f) * 3.f);
        const int headTop = cy - bodyH / 2;
        lv_obj_set_pos(antenna_, kCx - 1 + sway / 2, headTop - 22);
        place(tip_, kCx + sway, headTop - 24, 12, 12);
        lv_obj_set_style_bg_color(tip_, lv_color_hex(mood_hue(mood_)), 0);

        // Eyes.
        const int eyeY = cy - 10;
        const int eyeDX = 26;
        const int lx = kCx - eyeDX, rx = kCx + eyeDX;

        int ew = kEye, eh = kEye;
        if (sleeping || blink_ > 0) {
            // Closed: thin little arcs, pupils hidden.
            ew = kEye; eh = 3;
            lv_obj_add_flag(pupil_l_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(pupil_r_, LV_OBJ_FLAG_HIDDEN);
        } else {
            eh = happy ? (kEye - 6) : (surprised ? kEye + 4 : kEye);
            ew = surprised ? kEye + 4 : kEye;
            lv_obj_clear_flag(pupil_l_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(pupil_r_, LV_OBJ_FLAG_HIDDEN);
            const int ps = surprised ? (kPupil - 3) : kPupil;
            lv_obj_set_size(pupil_l_, ps, ps);
            lv_obj_set_size(pupil_r_, ps, ps);
            const int px = (int)lroundf(look_x_);
            const int py = (int)lroundf(look_y_);
            place(pupil_l_, lx + px, eyeY + py, ps, ps);
            place(pupil_r_, rx + px, eyeY + py, ps, ps);
        }
        lv_obj_set_size(eye_l_, ew, eh);
        lv_obj_set_size(eye_r_, ew, eh);
        place(eye_l_, lx, eyeY, ew, eh);
        place(eye_r_, rx, eyeY, ew, eh);

        // Cheeks only when happy.
        if (happy) {
            lv_obj_clear_flag(cheek_l_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cheek_r_, LV_OBJ_FLAG_HIDDEN);
            place(cheek_l_, kCx - 34, eyeY + 14, 12, 7);
            place(cheek_r_, kCx + 34, eyeY + 14, 12, 7);
        } else {
            lv_obj_add_flag(cheek_l_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(cheek_r_, LV_OBJ_FLAG_HIDDEN);
        }

        // Mouth shape by mood.
        int mw = 18, mh = 5;
        if (happy)          { mw = 34; mh = 14; }
        else if (surprised) { mw = 14; mh = 14; }
        else if (sleeping)  { mw = 14; mh = 3;  }
        else if (mood_ == Mood::Thinking) { mw = 12; mh = 4; }
        lv_obj_set_size(mouth_, mw, mh);
        place(mouth_, kCx, cy + 24, mw, mh);

        // Zzz drifts up and fades on a loop while asleep.
        if (sleeping) {
            lv_obj_clear_flag(zzz_, LV_OBJ_FLAG_HIDDEN);
            const int rise = (frame_ % 120);
            lv_obj_align(zzz_, LV_ALIGN_TOP_MID, 70, 40 - rise / 3);
            lv_obj_set_style_text_opa(zzz_, (lv_opa_t)clampi(220 - rise * 2, 0, 220), 0);
        } else {
            lv_obj_add_flag(zzz_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Build one LED frame for the current mood and latch it.
    void render_leds()
    {
        led_phase_++;
        uint32_t hue = mood_hue(mood_);
        uint8_t br = (uint8_t)r(hue), bg = (uint8_t)g(hue), bb = (uint8_t)b(hue);

        switch (mood_) {
            case Mood::Idle: {
                float k = 0.35f + 0.35f * (0.5f + 0.5f * sinf(frame_ * 0.06f));
                set_all(br, bg, bb, k);
                break;
            }
            case Mood::Sleeping: {
                float k = 0.08f + 0.10f * (0.5f + 0.5f * sinf(frame_ * 0.03f));
                set_all(br, bg, bb, k);
                break;
            }
            case Mood::Thinking: {
                // Indigo comet chasing around the ring.
                hal_lights_clear();
                int head = (led_phase_ / 2) % (int)LED_COUNT;
                for (int i = 0; i < (int)LED_COUNT; i++) {
                    int idx = BadgeLed::ClockwiseFromUpperRight[i];
                    float tail = (i == head) ? 1.f : (i == (head + 5) % 6 ? 0.4f : 0.f);
                    hal_lights_set(idx, (uint8_t)(br * tail), (uint8_t)(bg * tail), (uint8_t)(bb * tail));
                }
                hal_lights_show();
                break;
            }
            case Mood::Happy: {
                // Warm bounce: both sides pulse brightly.
                float k = 0.5f + 0.5f * fabsf(sinf(frame_ * 0.18f));
                set_all(br, bg, bb, k);
                break;
            }
            case Mood::Surprised: {
                // Bright pop that decays with the mood timer.
                float k = clampf(mood_ticks_ / 55.f, 0.f, 1.f);
                set_all(255, 240, 210, 0.3f + 0.7f * k);
                break;
            }
        }
    }

    static int r(uint32_t c) { return (c >> 16) & 0xff; }
    static int g(uint32_t c) { return (c >> 8) & 0xff; }
    static int b(uint32_t c) { return c & 0xff; }

    static void set_all(uint8_t r, uint8_t g, uint8_t b, float k)
    {
        k = clampf(k, 0.f, 1.f);
        uint8_t rr = (uint8_t)(r * k), gg = (uint8_t)(g * k), bb = (uint8_t)(b * k);
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, rr, gg, bb);
        hal_lights_show();
    }

    static constexpr int kBodyW = 104;
    static constexpr int kBodyH = 92;
    static constexpr int kEye = 28;
    static constexpr int kPupil = 12;

    enum class Mode : uint8_t { Creature, Setup, Chat, ChatList };
    Mode mode_ = Mode::Creature;
    lv_obj_t *root_ = nullptr;
    int start_hold_ = 0;   // frames Start has been held (creature reset gesture)
    bool setup_ap_done_ = false;
    lv_obj_t *setup_ssid_ = nullptr, *setup_status_ = nullptr, *setup_steps_ = nullptr, *setup_diag_ = nullptr;

    // Chat: a worker task connects Wi-Fi and streams Claude's reply into shared
    // buffers (guarded by chat_mu_); tick_chat() renders them on the UI task.
    lv_obj_t *chat_status_ = nullptr, *chat_reply_ = nullptr, *chat_scroll_ = nullptr;
    lv_obj_t *opt_lbl_[3] = {nullptr, nullptr, nullptr};
    // Claude-style transcript: one label per message in a flex-column scroll.
    lv_obj_t *streaming_bubble_ = nullptr;   // the Claude bubble being streamed into
    lv_obj_t *greeting_ = nullptr;           // centered greeting on an empty new chat
    int msg_count_ = 0;
    // Home screen: the Claude sunburst logo drawn as one polyline through centre.
    lv_obj_t *logo_line_ = nullptr, *home_name_ = nullptr, *home_hint_ = nullptr;
    lv_point_precise_t logo_pts_[26];
    SemaphoreHandle_t chat_mu_ = nullptr;
    TaskHandle_t chat_worker_ = nullptr;   // one persistent turn-runner per session
    char chat_reply_buf_[1536] = {0};
    int  chat_reply_len_ = 0;
    char chat_status_buf_[80] = {0};
    volatile bool chat_dirty_ = false, chat_done_ = false;
    volatile bool chat_processed_ = true;   // parsed the finished reply + options yet

    // Multi-turn conversation. Bounded so the request body stays small (memory is
    // the #1 constraint): keep the last kMaxTurns messages, each <= kMsgMax chars.
    static constexpr int kMaxTurns = 4;
    static constexpr int kMsgMax   = 150;
    struct ChatMsg { bool user; char text[kMsgMax]; };
    ChatMsg hist_[kMaxTurns];
    int  hist_n_ = 0;
    char opt_[3][40] = {{0}};
    int  opt_n_ = 0;

    // On-screen D-pad keyboard (type your own prompt). Modal over the chat screen:
    // Right opens it, the D-pad moves a highlighted key, A types, B backspaces,
    // Start sends (or cancels if empty). Built from plain labels so the highlight
    // is fully under our control; there is no network up while typing.
    static constexpr const char *kKbRows[5] = {
        "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm.?", " "
    };
    bool kb_open_ = false;
    char kb_text_[160] = {0};
    int  kb_len_ = 0, kb_row_ = 1, kb_col_ = 0;
    lv_obj_t *kb_keys_[5][10] = {{nullptr}};
    lv_obj_t *kb_textline_ = nullptr, *kb_hint_ = nullptr;

    // Saved conversations on flash: /littlefs/fchats/<id>.txt, one line per message
    // prefixed 'u' or 'a'. cur_chat_id_ is the file the current chat writes on exit;
    // the ChatList screen lists them newest-first for reopening.
    static constexpr const char *kChatsDir = "/littlefs/fchats";
    static constexpr int kMaxSaved = 10;
    int cur_chat_id_ = -1;
    int cl_ids_[kMaxSaved] = {0};
    int cl_count_ = 0;
    int cl_sel_ = 0;
    lv_obj_t *cl_rows_[kMaxSaved + 1] = {nullptr};

    // System prompt: keep it free of double quotes and backslashes so it embeds in
    // the JSON body with no escaping. Claude ends each reply with an OPTS line we
    // parse into the three button-mapped follow-ups.
    // Normal Claude. No persona, no identity, no mention of a badge - just a
    // concise assistant. The only added instruction is the OPTS line that powers
    // the tap-to-reply buttons; it is a format requirement, not a personality.
    static constexpr const char *kSysPrompt =
        "Answer normally and concisely. Keep replies reasonably short. If a question "
        "needs current facts, use web search before answering rather than guessing. "
        "After your reply, add one final line in exactly this format: "
        "OPTS: first | second | third  where each is a short follow-up the user might "
        "tap to say next (max 5 words each). Do not use quotation marks or backslashes.";

    Mood mood_ = Mood::Idle;
    int mood_ticks_ = 0;
    int idle_ticks_ = 0;
    uint32_t frame_ = 0;
    uint32_t led_phase_ = 0;
    float look_x_ = 0, look_y_ = 0, look_tx_ = 0, look_ty_ = 0;
    int blink_ = 0, next_blink_ = 0;

    lv_obj_t *title_ = nullptr, *mood_label_ = nullptr, *hint_ = nullptr, *zzz_ = nullptr;
    lv_obj_t *shadow_ = nullptr, *antenna_ = nullptr, *tip_ = nullptr, *body_ = nullptr;
    lv_obj_t *eye_l_ = nullptr, *eye_r_ = nullptr, *pupil_l_ = nullptr, *pupil_r_ = nullptr;
    lv_obj_t *mouth_ = nullptr, *cheek_l_ = nullptr, *cheek_r_ = nullptr;
};

FamiliarApp s_instance;

} // namespace

App *app_familiar_instance() { return &s_instance; }
