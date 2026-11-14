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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

inline int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A bare filled rectangle/circle: no theme chrome, no scrollbar, no padding.
lv_obj_t *clean_box(lv_obj_t *parent, uint32_t color, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

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
    const char *name() const override { return "Familiar"; }
    const char *icon() const override { return ":3"; }
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

        mode_ = Mode::Creature;
        mood_ = Mood::Idle;
        mood_ticks_ = 0;
        idle_ticks_ = 0;
        frame_ = 0;
        look_x_ = look_y_ = look_tx_ = look_ty_ = 0.f;
        blink_ = 0;
        next_blink_ = 120 + (int)(esp_random() % 120);
        led_phase_ = 0;

        title_ = lv_label_create(root);
        lv_label_set_text(title_, "FAMILIAR");
        lv_obj_set_style_text_color(title_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_14, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 8);

        mood_label_ = lv_label_create(root);
        lv_obj_set_style_text_color(mood_label_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(mood_label_, &lv_font_montserrat_14, 0);
        lv_obj_align(mood_label_, LV_ALIGN_TOP_MID, 0, 26);

        // Soft ground shadow for grounding; squashes as the creature bounces.
        shadow_ = clean_box(root, 0x120a13, 120, 20);
        lv_obj_set_style_radius(shadow_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(shadow_, LV_OPA_60, 0);

        // Antenna (stalk + glowing tip), drawn behind the body.
        antenna_ = clean_box(root, BadgeTheme::BORDER, 3, 24);
        tip_ = clean_box(root, mood_hue(Mood::Idle), 12, 12);
        lv_obj_set_style_radius(tip_, LV_RADIUS_CIRCLE, 0);

        // Body.
        body_ = clean_box(root, BadgeTheme::ACCENT, kBodyW, kBodyH);
        lv_obj_set_style_radius(body_, 28, 0);
        lv_obj_set_style_border_width(body_, 2, 0);
        lv_obj_set_style_border_color(body_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);

        // Cheeks (shown when happy).
        cheek_l_ = clean_box(root, 0xe08a9a, 12, 7);
        cheek_r_ = clean_box(root, 0xe08a9a, 12, 7);
        lv_obj_set_style_radius(cheek_l_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_radius(cheek_r_, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(cheek_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cheek_r_, LV_OBJ_FLAG_HIDDEN);

        // Eyes + pupils.
        eye_l_ = clean_box(root, BadgeTheme::TEXT, kEye, kEye);
        eye_r_ = clean_box(root, BadgeTheme::TEXT, kEye, kEye);
        lv_obj_set_style_radius(eye_l_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_radius(eye_r_, LV_RADIUS_CIRCLE, 0);
        pupil_l_ = clean_box(root, 0x120a13, kPupil, kPupil);
        pupil_r_ = clean_box(root, 0x120a13, kPupil, kPupil);
        lv_obj_set_style_radius(pupil_l_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_radius(pupil_r_, LV_RADIUS_CIRCLE, 0);

        // Mouth.
        mouth_ = clean_box(root, 0x2a0f1a, 20, 6);
        lv_obj_set_style_radius(mouth_, LV_RADIUS_CIRCLE, 0);

        zzz_ = lv_label_create(root);
        lv_label_set_text(zzz_, "z Z z");
        lv_obj_set_style_text_color(zzz_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_style_text_font(zzz_, &lv_font_montserrat_18, 0);
        lv_obj_add_flag(zzz_, LV_OBJ_FLAG_HIDDEN);

        hint_ = lv_label_create(root);
        lv_label_set_text(hint_, "A talk to Claude   B think   HOME exit");
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_14, 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -8);

        sync_mood_label();
        render();
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
        // Chat is running on a worker task - ignore all buttons so A can't spawn
        // a second, colliding network task. HOME still exits (registry → on_exit).
        if (mode_ == Mode::Chat) return;

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
            case Button::A:     set_mood(Mood::Happy, 40); enter_chat(root_); return;
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

        // Hold Start ~2 s to wipe Wi-Fi + key and drop back into setup.
        if (start_hold_ > 0) {
            start_hold_++;
            if (start_hold_ == 30 && mood_label_) lv_label_set_text(mood_label_, "hold to reset...");
            if (start_hold_ >= 110) { familiar_cfg_reset(); esp_restart(); }
        }

        if (gesture_poll_shake() || gesture_poll_tap()) {
            wake();
            set_mood(Mood::Surprised, 55);
        }

        // Tilt gives the eyes a resting gaze direction when nothing else is set.
        AccelSample s = gesture_last_sample();
        // Gaze follows the tilt so the eyes look "downhill" toward the screen's
        // low edge (signs verified on hardware - the creature now looks the way
        // you tip it, not away from it).
        float tilt_x = clampf(-s.x_mg * 0.012f, -5.f, 5.f);
        float tilt_y = clampf(s.y_mg * 0.012f, -4.f, 4.f);

        // Idle / sleep bookkeeping.
        idle_ticks_++;
        if (mood_ == Mood::Idle && idle_ticks_ > kSleepAfterTicks) {
            mood_ = Mood::Sleeping;
            sync_mood_label();
        }
        if (mood_ticks_ > 0 && --mood_ticks_ == 0 && is_transient(mood_)) {
            mood_ = Mood::Idle;
            sync_mood_label();
        }

        // Ease the gaze toward its target, then let the target relax to tilt.
        if (mood_ == Mood::Thinking) { look_tx_ = -2.f; look_ty_ = -5.f; }
        look_x_ += (look_tx_ - look_x_) * 0.18f;
        look_y_ += (look_ty_ - look_y_) * 0.18f;
        look_tx_ += (tilt_x - look_tx_) * 0.05f;
        look_ty_ += (tilt_y - look_ty_) * 0.05f;

        // Blinking (never while napping - the eyes are already shut).
        if (mood_ != Mood::Sleeping) {
            if (blink_ > 0) {
                blink_--;
            } else if (--next_blink_ <= 0) {
                blink_ = 7;
                next_blink_ = 90 + (int)(esp_random() % 160);
            }
        }

        render();
        if ((frame_ & 1) == 0) render_leds();
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
    static void chat_task(void *arg)
    {
        auto *self = static_cast<FamiliarApp *>(arg);
        char err[64] = {0};
        chat_status_cb("connecting to Wi-Fi...", self);
        if (!familiar_net_sta_connect(60000, err, sizeof(err))) {   // WPA3 SAE: ~4 patient, spaced attempts
            chat_status_cb(err[0] ? err : "Wi-Fi failed", self);
            self->chat_done_ = true;
            self->chat_task_ = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        chat_status_cb("Claude is thinking...", self);
        familiar_net_claude_stream(
            "You are Familiar, a tiny AI companion living on a 12 dollar hacker "
            "badge made at Hack the North. Greet your owner for the very first "
            "time in 1 to 2 short, warm, playful sentences.",
            &FamiliarApp::chat_delta_cb, &FamiliarApp::chat_status_cb, self);
        self->chat_done_ = true;
        self->chat_task_ = nullptr;
        vTaskDelete(nullptr);
    }

    void enter_chat(lv_obj_t *root)
    {
        // Leave the creature behind: clear its widgets and null the handles so
        // the (now-disabled) creature render path can't touch freed objects.
        lv_obj_clean(root);
        title_ = mood_label_ = hint_ = zzz_ = nullptr;
        shadow_ = antenna_ = tip_ = body_ = nullptr;
        eye_l_ = eye_r_ = pupil_l_ = pupil_r_ = mouth_ = cheek_l_ = cheek_r_ = nullptr;
        hal_lights_clear(); hal_lights_show();

        mode_ = Mode::Chat;
        chat_reply_len_ = 0;
        chat_reply_buf_[0] = '\0';
        strlcpy(chat_status_buf_, "waking up...", sizeof(chat_status_buf_));
        chat_dirty_ = true;
        chat_done_ = false;
        if (!chat_mu_) chat_mu_ = xSemaphoreCreateMutex();

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "FAMILIAR");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        chat_status_ = lv_label_create(root);
        lv_obj_set_style_text_color(chat_status_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(chat_status_, &lv_font_montserrat_14, 0);
        lv_obj_align(chat_status_, LV_ALIGN_TOP_MID, 0, 28);

        chat_reply_ = lv_label_create(root);
        lv_obj_set_style_text_color(chat_reply_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(chat_reply_, &lv_font_montserrat_18, 0);
        lv_label_set_long_mode(chat_reply_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(chat_reply_, 296);
        lv_obj_align(chat_reply_, LV_ALIGN_TOP_LEFT, 12, 56);
        lv_label_set_text(chat_reply_, "");

        lv_obj_t *hint = lv_label_create(root);
        lv_label_set_text(hint, "HOME to go back");
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

        // Worker owns the blocking network work; TLS needs a big stack.
        xTaskCreate(&FamiliarApp::chat_task, "fam_chat", 12288, this, 5, &chat_task_);
    }

    void tick_chat()
    {
        if (!chat_dirty_ || !chat_status_) return;
        xSemaphoreTake(chat_mu_, portMAX_DELAY);
        lv_label_set_text(chat_status_, chat_status_buf_);
        if (chat_reply_) lv_label_set_text(chat_reply_, chat_reply_buf_);
        chat_dirty_ = false;
        xSemaphoreGive(chat_mu_);
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

    enum class Mode : uint8_t { Creature, Setup, Chat };
    Mode mode_ = Mode::Creature;
    lv_obj_t *root_ = nullptr;
    int start_hold_ = 0;   // frames Start has been held (creature reset gesture)
    bool setup_ap_done_ = false;
    lv_obj_t *setup_ssid_ = nullptr, *setup_status_ = nullptr, *setup_steps_ = nullptr, *setup_diag_ = nullptr;

    // Chat: a worker task connects Wi-Fi and streams Claude's reply into shared
    // buffers (guarded by chat_mu_); tick_chat() renders them on the UI task.
    lv_obj_t *chat_status_ = nullptr, *chat_reply_ = nullptr;
    SemaphoreHandle_t chat_mu_ = nullptr;
    TaskHandle_t chat_task_ = nullptr;
    char chat_reply_buf_[1024] = {0};
    int  chat_reply_len_ = 0;
    char chat_status_buf_[80] = {0};
    volatile bool chat_dirty_ = false, chat_done_ = false;

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
