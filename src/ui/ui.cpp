#include "ui.h"

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>

#include <algorithm>
#include <vector>

#include "hw/battery.h"
#include "hw/haptics.h"
#include "hw/knob.h"
#include "ui/knob_step.h"
#include "voltra/voltra_client.h"

// Arduino.h defines a global `class Client` (the network stream base), so the
// Voltra client gets an alias rather than a plain using-declaration.
using VClient = voltra::Client;
using voltra::ConnState;
using voltra::DeviceState;
using voltra::FoundDevice;

namespace {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
#define C_BG       lv_color_hex(0x000000)
#define C_CARD     lv_color_hex(0x111827)
#define C_TRACK    lv_color_hex(0x1f2937)
#define C_TEXT     lv_color_hex(0xf8fafc)
#define C_MUTED    lv_color_hex(0x94a3b8)
#define C_UNLOADED lv_color_hex(0x3b82f6)
#define C_LOADED   lv_color_hex(0xbefa3c)
// Unloaded, as on the Voltra: the weight in a muted green and the ring in grey.
#define C_IDLE_NUM  lv_color_hex(0x283308)
#define C_IDLE_RING lv_color_hex(0x6b7280)
#define C_CHAINS   lv_color_hex(0xa78bfa)
#define C_ECC      lv_color_hex(0xf472b6)
#define C_WARN     lv_color_hex(0xf59e0b)
#define C_DANGER   lv_color_hex(0xef4444)
// Same colours as text for label recolour markup ("#rrggbb text#").
#define C_BT_HEX    "3b82f6"
#define C_WHITE_HEX "f8fafc"

// Accessory icons in font_icons_26 (drawn by tools/gen_icons.py after the Voltra's own).
#define ICON_ECCENTRIC "\xEE\x80\x80"   // U+E000
#define ICON_CHAINS    "\xEE\x80\x81"   // U+E001
#define ICON_INVERSE   "\xEE\x80\x82"   // U+E002
#define ICON_MOUNTAIN  "\xEE\x80\x83"   // U+E003
#define ICON_SETTINGS  "\xEE\x80\x84"   // U+E004

// Knob stepping (fine/coarse selection and snapping) lives in ui/knob_step.h.
using knobstep::apply_step;
constexpr int STEP_COARSE_LB = knobstep::STEP_COARSE_LB;

constexpr uint32_t SEND_DEBOUNCE_MS = 150;
constexpr uint32_t EDIT_HOLD_MS = 900;
// Long enough to cover the client re-issuing the command while the Voltra steps
// through its intermediate state, so the screen says "LOADING..." rather than
// flicking back to unloaded mid-sequence.
constexpr uint32_t TOGGLE_PENDING_MS = 5000;
constexpr uint32_t AUTO_LOAD_PENDING_MS = 4000;

enum class Screen { Main, Settings, AdjustChains, AdjustEcc, Connect };

/**
 * Chains, inverse chains and mountain are one accessory in three styles: the Voltra
 * keeps a single amount (the chains percentage) and two flags pick the style, so only
 * one of them can be on at a time.
 */
enum class ChainStyle { Chains, Inverse, Mountain };

/** Rows of the settings menu, stored in each row's user data. */
enum SettingsRow : intptr_t {
    ROW_CHAINS = 1,
    ROW_INVERSE,
    ROW_ECCENTRIC,
    ROW_MOUNTAIN,
    ROW_CLOSE,
};

struct Ui {
    // main
    lv_obj_t *scr_main = nullptr;
    lv_obj_t *arc_weight = nullptr;
    lv_obj_t *btn_top = nullptr;
    lv_obj_t *lbl_top = nullptr;
    lv_obj_t *lbl_reps = nullptr;
    // big set / rep counters, shown instead of the bubbles while a set is under way
    lv_obj_t *lbl_set_cap = nullptr, *lbl_set_num = nullptr;
    lv_obj_t *lbl_rep_cap = nullptr, *lbl_rep_num = nullptr;
    // before the first rep: three dots pulsing left to right, as on the Voltra
    lv_obj_t *rep_dots = nullptr;
    lv_obj_t *rep_dot[3] = {};
    bool rep_dots_running = false;
    lv_obj_t *btn_center = nullptr;
    lv_obj_t *lbl_weight = nullptr;
    lv_obj_t *lbl_unit = nullptr;
    lv_obj_t *lbl_state = nullptr;
    lv_obj_t *btn_gear = nullptr;
    lv_obj_t *lbl_gear = nullptr;
    // active accessories either side of the settings button: eccentric left, chain style right
    struct Chip {
        lv_obj_t *btn = nullptr;
        lv_obj_t *icon = nullptr;
        lv_obj_t *primary = nullptr;     // in the unit the Voltra is set to show first
        lv_obj_t *secondary = nullptr;   // the other unit, muted
    };
    Chip chip_ecc, chip_chain;

    // Two weight presets either side of the weight, kept on the knob only (NVS).
    struct Preset {
        lv_obj_t *btn = nullptr;
        lv_obj_t *value = nullptr;
        lv_obj_t *unit = nullptr;
        int lb = 0;   // 0 = empty
    };
    Preset presets[2];
    lv_obj_t *lbl_kbat = nullptr;

    // settings menu: one bubble per accessory
    lv_obj_t *scr_settings = nullptr;
    struct Bubble {
        lv_obj_t *btn = nullptr;
        lv_obj_t *icon = nullptr;
        lv_obj_t *title = nullptr;
        lv_obj_t *value = nullptr;
    };
    Bubble bub_ecc, bub_chains, bub_mountain, bub_inverse;
    lv_obj_t *btn_settings_done = nullptr;

    // adjust
    lv_obj_t *scr_adjust = nullptr;
    lv_obj_t *arc_adj = nullptr;
    lv_obj_t *lbl_adj_title = nullptr;
    lv_obj_t *btn_adj_center = nullptr;
    lv_obj_t *lbl_adj_val = nullptr;
    lv_obj_t *lbl_adj_unit = nullptr;
    lv_obj_t *lbl_adj_range = nullptr;
    lv_obj_t *btn_done = nullptr;

    // connect
    lv_obj_t *scr_connect = nullptr;
    lv_obj_t *lbl_conn_status = nullptr;
    lv_obj_t *spinner = nullptr;
    lv_obj_t *list = nullptr;
    lv_obj_t *btn_close = nullptr;

    Screen screen = Screen::Main;
    Screen adj_return = Screen::Settings;   // where the dial screen's Done goes back to

    // local (dial-edited) values
    int weight = 45;
    // Accessory amounts in the unit the Voltra is set to (see lb_mode()): percent of the
    // base weight, or pounds.
    int chains = 0;            // shared by chains, inverse chains and mountain
    int ecc = 0;
    int last_display = -2;     // Voltra lb/% setting the values above were taken in
    ChainStyle style = ChainStyle::Chains;       // what the amount currently drives
    ChainStyle adj_style = ChainStyle::Chains;   // which style the dial screen edits
    bool w_dirty = false, c_dirty = false, e_dirty = false;
    bool style_dirty = false;  // send the style flags along with the next amount
    uint32_t w_changed = 0, c_changed = 0, e_changed = 0;
    uint32_t editing_until = 0;
    uint32_t toggle_pending_until = 0;
    bool toggle_target_loaded = false;
    // Auto load was just requested: show it until the Voltra reports its own progress.
    uint32_t auto_load_pending_until = 0;
    // A set is under way: loaded and the cable has moved since the last rest.
    bool set_active = false;

    knobstep::RateTracker knob_rate;

    uint32_t last_state_version = 0xFFFFFFFF;
    uint32_t last_dev_version = 0xFFFFFFFF;
    uint32_t last_kbat_ms = 0;
    DeviceState st;
    std::vector<FoundDevice> devs;
} ui;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }


/** Effective ceiling: the device reports a higher one when overdrive is configured. */
int weight_max()
{
    return ui.st.max_weight > voltra::MIN_TARGET_LB ? ui.st.max_weight : voltra::MAX_TARGET_LB;
}

/**
 * The Voltra's own lb/% setting decides the unit accessories are dialled and stored in:
 * it keeps the chosen unit authoritative and derives the other, so the knob works in the
 * same unit and shows the other alongside.
 */
bool lb_mode() { return ui.st.accessory_lb(); }
const char *accessory_unit() { return ui.st.accessory_unit(); }

/** Pound equivalent of a percentage of the current base weight, for display only. */
int pct_to_lb(int pct) { return (ui.weight * pct) / 100; }

/** Accessory amount (in the Voltra's unit) as percent and as pounds. */
int amount_pct(int v)
{
    if (!lb_mode()) return v;
    if (ui.weight <= 0) return 0;
    return (v * 100 + (v >= 0 ? ui.weight / 2 : -ui.weight / 2)) / ui.weight;
}
int amount_lb(int v) { return lb_mode() ? v : pct_to_lb(v); }

// Limits are percentages on the device; in pound mode they apply to the current base weight.
int chains_max() { return lb_mode() ? pct_to_lb(ui.st.max_chains_pct) : ui.st.max_chains_pct; }
int ecc_max() { return lb_mode() ? pct_to_lb(ui.st.max_ecc_pct) : ui.st.max_ecc_pct; }

/** An amount in the Voltra's unit, e.g. "25%", "+6 lb". */
void format_amount(char *buf, size_t n, int v, bool signed_value)
{
    if (lb_mode()) snprintf(buf, n, signed_value ? "%+d lb" : "%d lb", v);
    else snprintf(buf, n, signed_value ? "%+d%%" : "%d%%", v);
}

/** Fill a chip's two value lines: the Voltra's unit on top, the other below. */
void set_chip_values(Ui::Chip &c, int v, bool signed_value)
{
    char p[12], lb[12];
    snprintf(p, sizeof(p), signed_value ? "%+d%%" : "%d%%", amount_pct(v));
    snprintf(lb, sizeof(lb), signed_value ? "%+d lb" : "%d lb", amount_lb(v));
    lv_label_set_text(c.primary, lb_mode() ? lb : p);
    lv_label_set_text(c.secondary, lb_mode() ? p : lb);
}

/** Amount a given chain style is set to: the shared amount if it is the live style, else 0. */
int style_amount(ChainStyle s) { return ui.style == s ? ui.chains : 0; }

const char *style_name(ChainStyle s)
{
    switch (s) {
        case ChainStyle::Inverse: return "Inverse Chains";
        case ChainStyle::Mountain: return "Mountain";
        default: return "Chains";
    }
}

lv_color_t style_colour(ChainStyle s) { return s == ChainStyle::Mountain ? C_WARN : C_CHAINS; }

/** Style the device flags describe. Both flags set is not a state the Voltra's UI makes. */
ChainStyle device_style(const DeviceState &st)
{
    if (st.mountain == 1) return ChainStyle::Mountain;
    if (st.inverse_chains == 1) return ChainStyle::Inverse;
    return ChainStyle::Chains;
}

void show(Screen s)
{
    ui.screen = s;
    lv_obj_t *scr = ui.scr_main;
    if (s == Screen::Settings) scr = ui.scr_settings;
    if (s == Screen::AdjustChains || s == Screen::AdjustEcc) scr = ui.scr_adjust;
    if (s == Screen::Connect) scr = ui.scr_connect;
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, false);
}

// ---------------------------------------------------------------------------
// Widget helpers
// ---------------------------------------------------------------------------
lv_obj_t *make_screen()
{
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *make_ring(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, 344, 344);
    lv_obj_center(arc);
    lv_arc_set_bg_angles(arc, 135, 405);
    lv_arc_set_rotation(arc, 0);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    return arc;
}

lv_obj_t *make_flat_button(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

lv_obj_t *make_pill_button(lv_obj_t *parent, int w, int h, lv_color_t border, const char *text, lv_obj_t **out_label)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, border, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_bg_color(b, border, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = make_label(b, &font_poppins_14, C_TEXT, text);
    lv_obj_center(l);
    if (out_label) *out_label = l;
    return b;
}

/**
 * The Done / Close button of a sub-screen, in the gap at the bottom of the ring. Sized
 * for a thumb: the whole pill is the touch target, plus a margin around it.
 */
lv_obj_t *make_action_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = nullptr;
    lv_obj_t *b = make_pill_button(parent, 140, 46, C_MUTED, text, &label);
    lv_obj_set_style_text_font(label, &font_poppins_18, 0);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 134);
    lv_obj_set_ext_click_area(b, 10);
    return b;
}

/** Big number with a small unit tucked against its baseline, as one auto-sized row. */
lv_obj_t *make_value_row(lv_obj_t *parent, const lv_font_t *big, lv_obj_t **num, lv_obj_t **unit)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    *num = make_label(row, big, C_TEXT, "--");
    *unit = make_label(row, &font_poppins_22, C_MUTED, "lb");
    // Lift the unit so it sits on the number's baseline: the row aligns label bottoms,
    // and the big number's box extends base_line px below its baseline.
    lv_obj_set_style_pad_bottom(*unit, big->base_line - 1, 0);
    return row;
}

// ---------------------------------------------------------------------------
// Sending edits to the device
// ---------------------------------------------------------------------------
void mark_weight_changed()
{
    uint32_t now = millis();
    ui.w_dirty = true;
    ui.w_changed = now;
    ui.editing_until = now + EDIT_HOLD_MS;
}

void mark_chains_changed();
void mark_ecc_changed();

/**
 * Set the target weight from the knob side (dial or preset). It is sent after the usual
 * debounce, and chains / eccentric are pulled inside the limits the new weight allows.
 * @return false if the weight did not change.
 */
bool set_weight(int lb)
{
    const int v = clampi(lb, voltra::MIN_TARGET_LB, weight_max());
    if (v == ui.weight) return false;
    ui.weight = v;
    mark_weight_changed();
    int cm = chains_max();
    if (ui.chains > cm) { ui.chains = cm; mark_chains_changed(); }
    int em = ecc_max();
    if (ui.ecc > em) { ui.ecc = em; mark_ecc_changed(); }
    if (ui.ecc < -em) { ui.ecc = -em; mark_ecc_changed(); }
    return true;
}

void mark_chains_changed()
{
    uint32_t now = millis();
    ui.c_dirty = true;
    ui.c_changed = now;
    ui.editing_until = now + EDIT_HOLD_MS;
}

void mark_ecc_changed()
{
    uint32_t now = millis();
    ui.e_dirty = true;
    ui.e_changed = now;
    ui.editing_until = now + EDIT_HOLD_MS;
}

void flush_pending(uint32_t now)
{
    VClient &c = VClient::instance();
    if (ui.w_dirty && now - ui.w_changed >= SEND_DEBOUNCE_MS) {
        ui.w_dirty = false;
        if (ui.st.connected()) c.setWeight(ui.weight);
    }
    if (ui.c_dirty && now - ui.c_changed >= SEND_DEBOUNCE_MS) {
        ui.c_dirty = false;
        if (ui.st.connected()) {
            // the client writes the flags before the amount
            if (ui.style_dirty) {
                c.setInverseChains(ui.style == ChainStyle::Inverse);
                c.setMountain(ui.style == ChainStyle::Mountain);
            }
            c.setChains(ui.chains, lb_mode());
        }
        ui.style_dirty = false;
    }
    if (ui.e_dirty && now - ui.e_changed >= SEND_DEBOUNCE_MS) {
        ui.e_dirty = false;
        if (ui.st.connected()) c.setEccentric(ui.ecc, lb_mode());
    }
}

// ---------------------------------------------------------------------------
// Main screen
// ---------------------------------------------------------------------------
void set_obj_hidden(lv_obj_t *o, bool hidden)
{
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void rep_dot_opa_cb(void *dot, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)dot, (lv_opa_t)v, 0); }

/**
 * Show or hide the waiting-for-first-rep dots. Each dot fades up and back down; the
 * starts are staggered so the pulse travels left to right. Animations only run while
 * the dots are on screen.
 */
void set_rep_dots(bool show)
{
    if (show == ui.rep_dots_running) return;
    ui.rep_dots_running = show;
    set_obj_hidden(ui.rep_dots, !show);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *d = ui.rep_dot[i];
        lv_anim_del(d, rep_dot_opa_cb);
        lv_obj_set_style_bg_opa(d, LV_OPA_30, 0);
        if (!show) continue;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, d);
        lv_anim_set_exec_cb(&a, rep_dot_opa_cb);
        lv_anim_set_values(&a, LV_OPA_30, LV_OPA_COVER);
        lv_anim_set_time(&a, 300);
        lv_anim_set_playback_time(&a, 300);
        lv_anim_set_repeat_delay(&a, 300);   // one cycle = 900 ms, a third per dot
        lv_anim_set_delay(&a, i * 300);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

// ---------------------------------------------------------------------------
// Weight presets
// ---------------------------------------------------------------------------
void refresh_main();

constexpr const char *PRESET_NS = "knob";
const char *const PRESET_KEYS[2] = {"preset0", "preset1"};

void load_presets()
{
    Preferences p;
    if (!p.begin(PRESET_NS, true)) return;   // namespace not created yet: all empty
    for (int i = 0; i < 2; i++) ui.presets[i].lb = p.getUShort(PRESET_KEYS[i], 0);
    p.end();
}

void save_preset(int i)
{
    Preferences p;
    if (!p.begin(PRESET_NS, false)) return;
    p.putUShort(PRESET_KEYS[i], (uint16_t)ui.presets[i].lb);
    p.end();
}

/** Stored weight, or "+" when empty; outlined in white when it matches the current weight. */
void refresh_presets()
{
    for (auto &pr : ui.presets) {
        if (pr.lb > 0) {
            lv_label_set_text_fmt(pr.value, "%d", pr.lb);
            lv_obj_set_style_text_color(pr.value, C_TEXT, 0);
            lv_obj_clear_flag(pr.unit, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(pr.value, LV_ALIGN_CENTER, 0, -6);
        } else {
            lv_label_set_text(pr.value, "+");
            lv_obj_set_style_text_color(pr.value, C_MUTED, 0);
            lv_obj_add_flag(pr.unit, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(pr.value, LV_ALIGN_CENTER, 0, 0);
        }
        const bool match = pr.lb > 0 && pr.lb == ui.weight;
        lv_obj_set_style_border_color(pr.btn, match ? C_TEXT : C_TRACK, 0);
    }
}

/** Tap: send the stored weight to the Voltra. */
void on_preset_tap(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    const int lb = ui.presets[i].lb;
    if (lb <= 0) {
        haptics_buzz();   // empty: hold to store one first
        return;
    }
    haptics_click();
    set_weight(lb);
    refresh_main();
}

/** Press and hold: store the current weight. */
void on_preset_hold(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    ui.presets[i].lb = ui.weight;
    save_preset(i);
    haptics_double();
    refresh_presets();
}

void make_preset(int i, int x, int y)
{
    auto &pr = ui.presets[i];
    pr.btn = make_pill_button(ui.scr_main, 56, 56, C_TRACK, "", nullptr);
    lv_obj_align(pr.btn, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_bg_color(pr.btn, C_MUTED, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(pr.btn, 6);
    // Short-clicked, not clicked: LVGL still sends CLICKED on release after a long
    // press, which would apply the preset that was just stored.
    lv_obj_add_event_cb(pr.btn, on_preset_tap, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(pr.btn, on_preset_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    pr.value = make_label(pr.btn, &font_poppins_20, C_TEXT, "+");
    lv_obj_align(pr.value, LV_ALIGN_CENTER, 0, -6);
    pr.unit = make_label(pr.btn, &font_poppins_14, C_MUTED, "lb");
    lv_obj_align(pr.unit, LV_ALIGN_CENTER, 0, 13);
}

void refresh_main()
{
    const DeviceState &st = ui.st;
    bool connected = st.connected();
    bool ready = st.conn == ConnState::Ready;
    uint32_t now = millis();

    // ring + weight
    lv_arc_set_range(ui.arc_weight, voltra::MIN_TARGET_LB, weight_max());
    lv_arc_set_value(ui.arc_weight, ui.weight);
    if (connected) {
        lv_label_set_text_fmt(ui.lbl_weight, "%d", ui.weight);
    } else {
        lv_label_set_text_fmt(ui.lbl_weight, "%d", ui.weight);
    }

    bool loaded = st.loaded();
    bool pending = now < ui.toggle_pending_until && loaded != ui.toggle_target_loaded;
    lv_color_t ring = loaded ? C_LOADED : C_IDLE_RING;
    if (!connected) ring = C_TRACK;
    lv_obj_set_style_arc_color(ui.arc_weight, ring, LV_PART_INDICATOR);

    const bool auto_loading = connected && (st.auto_loading() || now < ui.auto_load_pending_until);
    if (!connected) {
        lv_label_set_text(ui.lbl_state, "NOT CONNECTED");
        lv_obj_set_style_text_color(ui.lbl_state, C_MUTED, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, C_MUTED, 0);
    } else if (auto_loading && !loaded) {
        // The Voltra's auto load: waiting for the cable to be pulled out and held, then
        // a 3 s countdown before it loads.
        const int ms = st.direct_load_countdown_ms;
        if (ms > 0 && ms <= 3000) lv_label_set_text_fmt(ui.lbl_state, "LOADING IN %d", (ms + 999) / 1000);
        else lv_label_set_text(ui.lbl_state, "PULL & HOLD CABLE");
        lv_obj_set_style_text_color(ui.lbl_state, C_WARN, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, C_IDLE_NUM, 0);
    } else if (pending) {
        lv_label_set_text(ui.lbl_state, ui.toggle_target_loaded ? "LOADING..." : "UNLOADING...");
        lv_obj_set_style_text_color(ui.lbl_state, C_WARN, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, loaded ? C_LOADED : C_IDLE_NUM, 0);
    } else if (loaded) {
        lv_label_set_text(ui.lbl_state, "Tap to Unload");
        lv_obj_set_style_text_color(ui.lbl_state, C_TEXT, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, C_LOADED, 0);
    } else if (!ready) {
        lv_label_set_text(ui.lbl_state, "CONNECTING...");
        lv_obj_set_style_text_color(ui.lbl_state, C_MUTED, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, C_MUTED, 0);
    } else if (st.activation == 0) {
        lv_label_set_text(ui.lbl_state, "VOLTRA NOT ACTIVATED");
        lv_obj_set_style_text_color(ui.lbl_state, C_DANGER, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, C_IDLE_NUM, 0);
    } else {
        lv_label_set_text(ui.lbl_state, "Tap to Load");
        lv_obj_set_style_text_color(ui.lbl_state, C_TEXT, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, C_IDLE_NUM, 0);
    }

    // top bar: Bluetooth icon (blue when connected, white when not) and, once connected,
    // the Voltra's battery level. Recolour markup colours just the icon.
    char top[64];
    const char *bt_hex = connected ? C_BT_HEX : C_WHITE_HEX;
    if (connected && st.battery >= 0) {
        const char *sym = st.battery > 80 ? LV_SYMBOL_BATTERY_FULL
                        : st.battery > 60 ? LV_SYMBOL_BATTERY_3
                        : st.battery > 40 ? LV_SYMBOL_BATTERY_2
                        : st.battery > 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(top, sizeof(top), "#%s " LV_SYMBOL_BLUETOOTH "#   %s %d%%", bt_hex, sym, st.battery);
    } else {
        snprintf(top, sizeof(top), "#%s " LV_SYMBOL_BLUETOOTH "#", bt_hex);
    }
    lv_label_set_text(ui.lbl_top, top);
    lv_obj_set_style_text_color(ui.lbl_top, C_TEXT, 0);

    // During a set the bubbles give way to big set / rep counters.
    const bool set_mode = ready && loaded && ui.set_active;
    lv_obj_t *const set_widgets[] = {ui.lbl_set_cap, ui.lbl_set_num, ui.lbl_rep_cap};
    for (lv_obj_t *o : set_widgets) {
        if (set_mode) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    // No rep counted yet: pulsing dots in place of a 0.
    const bool waiting_for_rep = set_mode && st.reps == 0;
    set_obj_hidden(ui.lbl_rep_num, !set_mode || waiting_for_rep);
    set_rep_dots(waiting_for_rep);
    if (set_mode) {
        lv_label_set_text_fmt(ui.lbl_set_num, "%u", (unsigned)st.sets);
        lv_label_set_text_fmt(ui.lbl_rep_num, "%u", (unsigned)st.reps);
    }

    // reps / live force (small line, outside a set)
    if (set_mode) {
        lv_label_set_text(ui.lbl_reps, "");
    } else if (ready && loaded) {
        if (st.reps > 0 || st.sets > 0) {
            lv_label_set_text_fmt(ui.lbl_reps, "SET %u   REP %u", (unsigned)st.sets, (unsigned)st.reps);
        } else if (st.force_known) {
            lv_label_set_text_fmt(ui.lbl_reps, "%d lb on cable", st.force_lb);
        } else {
            lv_label_set_text(ui.lbl_reps, "");
        }
    } else {
        lv_label_set_text(ui.lbl_reps, "");
    }

    // settings button: white with nothing on, green once any accessory is on
    const bool any_accessory = ui.chains > 0 || ui.ecc != 0;
    lv_obj_set_style_text_color(ui.lbl_gear, any_accessory ? C_LOADED : C_TEXT, 0);

    // eccentric chip (left)
    if (ui.ecc != 0) {
        set_chip_values(ui.chip_ecc, ui.ecc, true);
        lv_obj_clear_flag(ui.chip_ecc.btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.chip_ecc.btn, LV_OBJ_FLAG_HIDDEN);
    }

    // chains / inverse chains / mountain chip (right), whichever style is live
    if (ui.chains > 0) {
        const char *icon = ui.style == ChainStyle::Inverse ? ICON_INVERSE
                         : ui.style == ChainStyle::Mountain ? ICON_MOUNTAIN : ICON_CHAINS;
        const lv_color_t col = style_colour(ui.style);
        lv_label_set_text(ui.chip_chain.icon, icon);
        lv_obj_set_style_text_color(ui.chip_chain.icon, col, 0);
        lv_obj_set_style_border_color(ui.chip_chain.btn, col, 0);
        lv_obj_set_style_bg_color(ui.chip_chain.btn, col, LV_STATE_PRESSED);
        set_chip_values(ui.chip_chain, ui.chains, false);
        lv_obj_clear_flag(ui.chip_chain.btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.chip_chain.btn, LV_OBJ_FLAG_HIDDEN);
    }

    refresh_presets();

    if (set_mode) {
        lv_obj_add_flag(ui.btn_gear, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.chip_ecc.btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.chip_chain.btn, LV_OBJ_FLAG_HIDDEN);
        for (auto &pr : ui.presets) lv_obj_add_flag(pr.btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(ui.btn_gear, LV_OBJ_FLAG_HIDDEN);
        for (auto &pr : ui.presets) lv_obj_clear_flag(pr.btn, LV_OBJ_FLAG_HIDDEN);
        // the chips were shown or hidden above according to the accessories
    }
}

void on_center_clicked(lv_event_t *)
{
    VClient &c = VClient::instance();
    if (!ui.st.connected()) {
        haptics_click();
        show(Screen::Connect);
        c.scanStart();
        return;
    }
    if (ui.st.conn != ConnState::Ready) {
        haptics_buzz();
        return;
    }
    // Push any pending dial edits first so the device loads the weight on screen.
    flush_pending(millis() + SEND_DEBOUNCE_MS);
    if (!ui.st.loaded() && (ui.st.auto_loading() || millis() < ui.auto_load_pending_until)) {
        // Tapping during auto load cancels it.
        ui.auto_load_pending_until = 0;
        c.unload();
        haptics_click();
        refresh_main();
        return;
    }
    bool loaded = ui.st.loaded();
    ui.toggle_target_loaded = !loaded;
    ui.toggle_pending_until = millis() + TOGGLE_PENDING_MS;
    if (loaded) c.unload(); else c.load();
    haptics_double();
    refresh_main();
}

/** Press and hold: start the Voltra's auto load (loads once the cable is pulled and held). */
void on_center_hold(lv_event_t *)
{
    if (ui.st.conn != ConnState::Ready || ui.st.loaded() || ui.st.auto_loading()) {
        haptics_buzz();
        return;
    }
    flush_pending(millis() + SEND_DEBOUNCE_MS);
    ui.toggle_pending_until = 0;
    ui.auto_load_pending_until = millis() + AUTO_LOAD_PENDING_MS;
    VClient::instance().autoLoad();
    haptics_double();
    refresh_main();
}

void on_top_clicked(lv_event_t *)
{
    haptics_click();
    show(Screen::Connect);
    VClient::instance().scanStart();
}

void on_gear_clicked(lv_event_t *);
void refresh_settings();
void refresh_adjust();

/** Tapping a chip goes straight to that accessory's dial, and Done comes back here. */
void on_chip_clicked(lv_event_t *e)
{
    haptics_click();
    ui.adj_return = Screen::Main;
    if (lv_event_get_target(e) == ui.chip_ecc.btn) {
        show(Screen::AdjustEcc);
    } else {
        ui.adj_style = ui.style;
        show(Screen::AdjustChains);
    }
    refresh_adjust();
}

/** A small round button: an active accessory's icon over its amount in both units. */
void make_chip(Ui::Chip &c, const char *icon, lv_color_t colour, int x, int y)
{
    c.btn = make_pill_button(ui.scr_main, 70, 70, colour, "", nullptr);
    lv_obj_align(c.btn, LV_ALIGN_CENTER, x, y);
    lv_obj_add_event_cb(c.btn, on_chip_clicked, LV_EVENT_CLICKED, nullptr);
    c.icon = make_label(c.btn, &font_icons_18, colour, icon);
    lv_obj_align(c.icon, LV_ALIGN_CENTER, 0, -17);
    c.primary = make_label(c.btn, &font_poppins_14, C_TEXT, "");
    lv_obj_align(c.primary, LV_ALIGN_CENTER, 0, 1);
    c.secondary = make_label(c.btn, &font_poppins_14, C_MUTED, "");
    lv_obj_align(c.secondary, LV_ALIGN_CENTER, 0, 17);
    lv_obj_add_flag(c.btn, LV_OBJ_FLAG_HIDDEN);
}

void build_main()
{
    ui.scr_main = make_screen();
    ui.arc_weight = make_ring(ui.scr_main, C_IDLE_RING);

    // top bar (tap -> connect menu)
    ui.btn_top = make_flat_button(ui.scr_main, 220, 44);
    lv_obj_align(ui.btn_top, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_add_event_cb(ui.btn_top, on_top_clicked, LV_EVENT_CLICKED, nullptr);
    ui.lbl_top = make_label(ui.btn_top, &font_poppins_16, C_TEXT, "");
    lv_label_set_recolor(ui.lbl_top, true);
    lv_label_set_text(ui.lbl_top, "#" C_WHITE_HEX " " LV_SYMBOL_BLUETOOTH "#");
    lv_obj_center(ui.lbl_top);

    // sets / reps sit under the weight
    ui.lbl_reps = make_label(ui.scr_main, &font_poppins_14, C_MUTED, "");
    lv_obj_align(ui.lbl_reps, LV_ALIGN_CENTER, 0, 50);

    // During a set: two big counters filling the space the bubbles use at rest.
    ui.lbl_set_cap = make_label(ui.scr_main, &font_poppins_22, C_MUTED, "Set");
    ui.lbl_rep_cap = make_label(ui.scr_main, &font_poppins_22, C_MUTED, "Reps");
    ui.lbl_set_num = make_label(ui.scr_main, &font_poppins_bold_64, C_TEXT, "0");
    ui.lbl_rep_num = make_label(ui.scr_main, &font_poppins_bold_64, C_TEXT, "0");
    lv_obj_t *const caps[] = {ui.lbl_set_cap, ui.lbl_rep_cap};
    lv_obj_align(ui.lbl_set_cap, LV_ALIGN_CENTER, -64, 38);
    lv_obj_align(ui.lbl_rep_cap, LV_ALIGN_CENTER, 64, 38);
    lv_obj_t *const nums[] = {ui.lbl_set_num, ui.lbl_rep_num};
    for (lv_obj_t *o : nums) {
        // centre the digits on their column however many there are
        lv_obj_set_width(o, 120);
        lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    }
    // digits run from ~62 to ~108 px below centre, inside the ring
    lv_obj_align(ui.lbl_set_num, LV_ALIGN_CENTER, -64, 86);
    lv_obj_align(ui.lbl_rep_num, LV_ALIGN_CENTER, 64, 86);
    for (lv_obj_t *o : caps) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t *o : nums) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);

    // Waiting-for-first-rep dots, sitting on the digits' baseline in the reps column.
    ui.rep_dots = lv_obj_create(ui.scr_main);
    lv_obj_remove_style_all(ui.rep_dots);
    lv_obj_set_size(ui.rep_dots, 72, 14);
    lv_obj_clear_flag(ui.rep_dots, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(ui.rep_dots, LV_ALIGN_CENTER, 64, 100);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *d = lv_obj_create(ui.rep_dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 14, 14);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, C_LOADED, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_30, 0);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, i * 29, 0);
        ui.rep_dot[i] = d;
    }
    lv_obj_add_flag(ui.rep_dots, LV_OBJ_FLAG_HIDDEN);

    // centre: weight, tap to load/unload
    // The weight's touch target also covers the tap-to-load line at its top.
    ui.btn_center = make_flat_button(ui.scr_main, 236, 150);
    lv_obj_align(ui.btn_center, LV_ALIGN_CENTER, 0, -34);
    // Short-clicked, not clicked, so releasing a hold (auto load) is not also a tap.
    lv_obj_add_event_cb(ui.btn_center, on_center_clicked, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui.btn_center, on_center_hold, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_t *row = make_value_row(ui.btn_center, &font_poppins_96, &ui.lbl_weight, &ui.lbl_unit);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);   // 8 px higher on screen than before
    ui.lbl_state = make_label(ui.btn_center, &font_poppins_14, C_MUTED, "NOT CONNECTED");
    lv_obj_align(ui.lbl_state, LV_ALIGN_TOP_MID, 0, 4);

    ui.btn_gear = make_pill_button(ui.scr_main, 64, 64, C_TRACK, "", nullptr);
    lv_obj_align(ui.btn_gear, LV_ALIGN_CENTER, 0, 114);
    lv_obj_add_event_cb(ui.btn_gear, on_gear_clicked, LV_EVENT_CLICKED, nullptr);
    ui.lbl_gear = make_label(ui.btn_gear, &font_icons_26, C_TEXT, ICON_SETTINGS);
    lv_obj_center(ui.lbl_gear);

    // Chips for the active accessories, shown only while on. Kept inside the ring: the
    // outer edge is ~156 px from centre against the ring's 158 px.
    make_chip(ui.chip_ecc, ICON_ECCENTRIC, C_ECC, -74, 96);
    make_chip(ui.chip_chain, ICON_CHAINS, C_CHAINS, 74, 96);

    // Weight presets at the sides, just below the weight's baseline. Outer edge ~151 px
    // from centre against the ring's 158 px.
    load_presets();
    make_preset(0, -122, 30);
    make_preset(1, 122, 30);
    refresh_presets();

    // knob battery (bottom, inside the gap at the bottom of the ring)
    ui.lbl_kbat = make_label(ui.scr_main, &font_poppins_14, C_MUTED, "");
    lv_obj_align(ui.lbl_kbat, LV_ALIGN_BOTTOM_MID, 0, -6);
}

// ---------------------------------------------------------------------------
// Adjust screen (chains / eccentric)
// ---------------------------------------------------------------------------
void refresh_adjust()
{
    bool chains = ui.screen == Screen::AdjustChains;
    lv_label_set_text(ui.lbl_adj_unit, accessory_unit());
    if (chains) {
        int mx = chains_max();
        const int v = style_amount(ui.adj_style);
        const lv_color_t col = style_colour(ui.adj_style);
        lv_label_set_text(ui.lbl_adj_title, style_name(ui.adj_style));
        lv_obj_set_style_text_color(ui.lbl_adj_title, col, 0);
        lv_obj_set_style_arc_color(ui.arc_adj, col, LV_PART_INDICATOR);
        lv_arc_set_mode(ui.arc_adj, LV_ARC_MODE_NORMAL);
        lv_arc_set_range(ui.arc_adj, 0, std::max(mx, STEP_COARSE_LB));
        lv_arc_set_value(ui.arc_adj, v);
        lv_label_set_text_fmt(ui.lbl_adj_val, "%d", v);
        if (lb_mode()) {
            lv_label_set_text_fmt(ui.lbl_adj_range, "0 - %d lb   =  %d%% of %d lb",
                                  mx, amount_pct(v), ui.weight);
        } else {
            lv_label_set_text_fmt(ui.lbl_adj_range, "0 - %d%%   =  %d lb of %d lb",
                                  mx, pct_to_lb(v), ui.weight);
        }
    } else {
        int mx = ecc_max();
        lv_label_set_text(ui.lbl_adj_title, "Eccentric");
        lv_obj_set_style_text_color(ui.lbl_adj_title, C_ECC, 0);
        lv_obj_set_style_arc_color(ui.arc_adj, C_ECC, LV_PART_INDICATOR);
        lv_arc_set_mode(ui.arc_adj, LV_ARC_MODE_SYMMETRICAL);
        lv_arc_set_range(ui.arc_adj, -std::max(mx, STEP_COARSE_LB), std::max(mx, STEP_COARSE_LB));
        lv_arc_set_value(ui.arc_adj, ui.ecc);
        lv_label_set_text_fmt(ui.lbl_adj_val, "%+d", ui.ecc);
        if (lb_mode()) {
            lv_label_set_text_fmt(ui.lbl_adj_range, "-%d - +%d lb   =  %+d%% of %d lb",
                                  mx, mx, amount_pct(ui.ecc), ui.weight);
        } else {
            lv_label_set_text_fmt(ui.lbl_adj_range, "-%d - +%d%%   =  %+d lb of %d lb",
                                  mx, mx, pct_to_lb(ui.ecc), ui.weight);
        }
    }
}

void refresh_settings();

void on_adjust_done(lv_event_t *)
{
    haptics_click();
    flush_pending(millis() + SEND_DEBOUNCE_MS);
    show(ui.adj_return);
    if (ui.adj_return == Screen::Main) refresh_main();
    else refresh_settings();
}

void on_gear_clicked(lv_event_t *)
{
    haptics_click();
    show(Screen::Settings);
    refresh_settings();
}

void build_adjust()
{
    ui.scr_adjust = make_screen();
    ui.arc_adj = make_ring(ui.scr_adjust, C_CHAINS);

    ui.lbl_adj_title = make_label(ui.scr_adjust, &font_poppins_18, C_CHAINS, "Chains");
    lv_obj_align(ui.lbl_adj_title, LV_ALIGN_TOP_MID, 0, 40);

    ui.btn_adj_center = make_flat_button(ui.scr_adjust, 230, 130);
    lv_obj_align(ui.btn_adj_center, LV_ALIGN_CENTER, 0, -26);
    lv_obj_add_event_cb(ui.btn_adj_center, on_adjust_done, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *row = make_value_row(ui.btn_adj_center, &font_poppins_96, &ui.lbl_adj_val, &ui.lbl_adj_unit);
    lv_obj_center(row);

    ui.lbl_adj_range = make_label(ui.scr_adjust, &font_poppins_14, C_MUTED, "");
    lv_obj_align(ui.lbl_adj_range, LV_ALIGN_CENTER, 0, 50);

    ui.btn_done = make_action_button(ui.scr_adjust, "DONE");
    lv_obj_add_event_cb(ui.btn_done, on_adjust_done, LV_EVENT_CLICKED, nullptr);
}


// ---------------------------------------------------------------------------
// Settings menu (chains, inverse chains, eccentric, mountain)
// ---------------------------------------------------------------------------
void on_settings_row(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const intptr_t row = (intptr_t)lv_obj_get_user_data(btn);
    haptics_click();
    switch (row) {
        case ROW_CHAINS:
        case ROW_INVERSE:
        case ROW_MOUNTAIN:
            ui.adj_style = row == ROW_INVERSE ? ChainStyle::Inverse
                         : row == ROW_MOUNTAIN ? ChainStyle::Mountain : ChainStyle::Chains;
            ui.adj_return = Screen::Settings;
            show(Screen::AdjustChains);
            refresh_adjust();
            break;
        case ROW_ECCENTRIC:
            ui.adj_return = Screen::Settings;
            show(Screen::AdjustEcc);
            refresh_adjust();
            break;
        case ROW_CLOSE:
        default:
            show(Screen::Main);
            refresh_main();
            break;
    }
}

/** One round accessory button: the accessory's icon, a small title, and its current value. */
void make_bubble(Ui::Bubble &b, const char *icon, const char *title, intptr_t row, int x, int y)
{
    b.btn = make_pill_button(ui.scr_settings, 96, 96, C_TRACK, "", nullptr);
    lv_obj_align(b.btn, LV_ALIGN_CENTER, x, y);
    lv_obj_set_user_data(b.btn, (void *)row);
    lv_obj_add_event_cb(b.btn, on_settings_row, LV_EVENT_CLICKED, nullptr);
    b.icon = make_label(b.btn, &font_icons_26, C_TEXT, icon);
    lv_obj_align(b.icon, LV_ALIGN_CENTER, 0, -22);
    b.title = make_label(b.btn, &font_poppins_14, C_MUTED, title);
    lv_obj_align(b.title, LV_ALIGN_CENTER, 0, 1);
    b.value = make_label(b.btn, &font_poppins_22, C_TEXT, "--");
    lv_obj_align(b.value, LV_ALIGN_CENTER, 0, 22);
}

void set_bubble(Ui::Bubble &b, const char *value, bool active, lv_color_t colour)
{
    lv_label_set_text(b.value, value);
    lv_obj_set_style_border_color(b.btn, active ? colour : C_TRACK, 0);
    lv_obj_set_style_bg_color(b.btn, active ? colour : C_CARD, 0);
    lv_obj_set_style_bg_opa(b.btn, active ? LV_OPA_20 : LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(b.icon, active ? colour : C_TEXT, 0);
    lv_obj_set_style_text_color(b.title, active ? colour : C_MUTED, 0);
}

void refresh_settings()
{
    char buf[16];

    if (ui.ecc != 0) format_amount(buf, sizeof(buf), ui.ecc, true);
    else snprintf(buf, sizeof(buf), "OFF");
    set_bubble(ui.bub_ecc, buf, ui.ecc != 0, C_ECC);

    // one amount, three styles: only the live style's bubble shows it
    const struct { Ui::Bubble &b; ChainStyle s; } chain_bubbles[] = {
        {ui.bub_chains, ChainStyle::Chains},
        {ui.bub_inverse, ChainStyle::Inverse},
        {ui.bub_mountain, ChainStyle::Mountain},
    };
    for (const auto &cb : chain_bubbles) {
        const int v = style_amount(cb.s);
        if (v > 0) format_amount(buf, sizeof(buf), v, false);
        else snprintf(buf, sizeof(buf), "OFF");
        set_bubble(cb.b, buf, v > 0, style_colour(cb.s));
    }
}

void build_settings()
{
    ui.scr_settings = make_screen();
    lv_obj_t *ring = make_ring(ui.scr_settings, C_TRACK);
    lv_arc_set_value(ring, 0);

    lv_obj_t *title = make_label(ui.scr_settings, &font_poppins_18, C_TEXT, "SETTINGS");
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    // 2x2 grid, in the order eccentric, chains / mountain, inverse chains. Kept inside the
    // ring: the farthest bubble edge is ~126 px from centre against the ring's 158 px.
    make_bubble(ui.bub_ecc,      ICON_ECCENTRIC, "Eccentric", ROW_ECCENTRIC, -52, -46);
    make_bubble(ui.bub_chains,   ICON_CHAINS,    "Chains",    ROW_CHAINS,     52, -46);
    make_bubble(ui.bub_mountain, ICON_MOUNTAIN,  "Mountain",  ROW_MOUNTAIN,  -52,  58);
    make_bubble(ui.bub_inverse,  ICON_INVERSE,   "Inverse",   ROW_INVERSE,    52,  58);

    // Done sits in the gap at the bottom of the ring.
    ui.btn_settings_done = make_action_button(ui.scr_settings, "DONE");
    lv_obj_set_user_data(ui.btn_settings_done, (void *)(intptr_t)ROW_CLOSE);
    lv_obj_add_event_cb(ui.btn_settings_done, on_settings_row, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// Connect screen
// ---------------------------------------------------------------------------
void on_device_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    intptr_t idx = (intptr_t)lv_obj_get_user_data(btn);
    haptics_click();
    VClient &c = VClient::instance();
    if (idx < 0) {
        c.disconnect();
        c.scanStart();
        return;
    }
    if ((size_t)idx < ui.devs.size()) {
        c.scanStop();
        c.connectTo(ui.devs[idx]);
        show(Screen::Main);
        refresh_main();
    }
}

void on_close_clicked(lv_event_t *)
{
    haptics_click();
    VClient::instance().scanStop();
    show(Screen::Main);
    refresh_main();
}

void rebuild_device_list()
{
    lv_obj_clean(ui.list);
    const DeviceState &st = ui.st;
    if (st.connected() || st.conn == ConnState::Connecting || st.conn == ConnState::Reconnecting) {
        const char *name = st.device_name[0] ? st.device_name : "Voltra";
        char buf[48];
        snprintf(buf, sizeof(buf), "Disconnect %s", name);
        lv_obj_t *b = lv_list_add_btn(ui.list, LV_SYMBOL_CLOSE, buf);
        lv_obj_set_user_data(b, (void *)(intptr_t)-1);
        lv_obj_set_style_text_color(b, C_DANGER, 0);
        lv_obj_add_event_cb(b, on_device_clicked, LV_EVENT_CLICKED, nullptr);
    }
    for (size_t i = 0; i < ui.devs.size(); i++) {
        const FoundDevice &d = ui.devs[i];
        if (st.connected() && strcmp(d.address.c_str(), st.address) == 0) continue;
        char buf[48];
        snprintf(buf, sizeof(buf), "%s  (%d dBm)", d.name.c_str(), d.rssi);
        lv_obj_t *b = lv_list_add_btn(ui.list, LV_SYMBOL_BLUETOOTH, buf);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_set_style_text_color(b, C_TEXT, 0);
        lv_obj_add_event_cb(b, on_device_clicked, LV_EVENT_CLICKED, nullptr);
    }
    if (ui.devs.empty() && !st.connected()) {
        lv_obj_t *t = lv_list_add_text(ui.list, "No Voltra found yet.\nSwitch the Voltra on.");
        lv_obj_set_style_text_color(t, C_MUTED, 0);
    }
}

void refresh_connect()
{
    const DeviceState &st = ui.st;
    if (st.conn == ConnState::Scanning) {
        lv_label_set_text(ui.lbl_conn_status, "Scanning for VTR- devices");
    } else {
        lv_label_set_text(ui.lbl_conn_status, st.status);
    }
    // The scan runs in repeating windows while this screen is open.
    lv_obj_clear_flag(ui.spinner, LV_OBJ_FLAG_HIDDEN);
}

void build_connect()
{
    ui.scr_connect = make_screen();
    lv_obj_t *ring = make_ring(ui.scr_connect, C_TRACK);
    lv_obj_set_style_arc_color(ring, C_TRACK, LV_PART_INDICATOR);
    lv_arc_set_value(ring, 0);

    lv_obj_t *title = make_label(ui.scr_connect, &font_poppins_18, C_TEXT, "CONNECT");
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    ui.lbl_conn_status = make_label(ui.scr_connect, &font_poppins_14, C_MUTED, "");
    lv_obj_align(ui.lbl_conn_status, LV_ALIGN_TOP_MID, 0, 62);

    ui.spinner = lv_spinner_create(ui.scr_connect, 1200, 60);
    lv_obj_set_size(ui.spinner, 26, 26);
    lv_obj_set_style_arc_width(ui.spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.spinner, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.spinner, C_UNLOADED, LV_PART_INDICATOR);
    lv_obj_align(ui.spinner, LV_ALIGN_TOP_MID, 0, 88);

    ui.list = lv_list_create(ui.scr_connect);
    lv_obj_set_size(ui.list, 236, 150);
    lv_obj_align(ui.list, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_bg_color(ui.list, C_CARD, 0);
    lv_obj_set_style_border_width(ui.list, 0, 0);
    lv_obj_set_style_radius(ui.list, 18, 0);
    lv_obj_set_style_pad_all(ui.list, 6, 0);
    lv_obj_set_style_text_font(ui.list, &font_poppins_16, 0);
    lv_obj_set_scrollbar_mode(ui.list, LV_SCROLLBAR_MODE_OFF);

    ui.btn_close = make_action_button(ui.scr_connect, "CLOSE");
    lv_obj_add_event_cb(ui.btn_close, on_close_clicked, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// Periodic poll: knob, device state, debounced sends
// ---------------------------------------------------------------------------
void handle_knob(int delta)
{
    const int step = ui.knob_rate.step(delta, millis());

    switch (ui.screen) {
        case Screen::Main: {
            if (set_weight(apply_step(ui.weight, delta, step))) {
                haptics_tick();
                refresh_main();
            }
            break;
        }
        case Screen::AdjustChains: {
            const int cur = style_amount(ui.adj_style);
            int v = clampi(apply_step(cur, delta, step), 0, chains_max());
            if (v != cur) {
                // Dialling an amount into another style switches the Voltra over to it,
                // which turns the previous style off.
                if (v > 0 && ui.style != ui.adj_style) {
                    ui.style = ui.adj_style;
                    ui.style_dirty = true;
                }
                ui.chains = v;
                mark_chains_changed();
                haptics_tick();
                refresh_adjust();
            }
            break;
        }
        case Screen::AdjustEcc: {
            int m = ecc_max();
            int v = clampi(apply_step(ui.ecc, delta, step), -m, m);
            if (v != ui.ecc) {
                ui.ecc = v;
                mark_ecc_changed();
                haptics_tick();
                refresh_adjust();
            }
            break;
        }
        case Screen::Settings:
            break;   // all four bubbles are visible at once; selection is by touch
        case Screen::Connect:
            lv_obj_scroll_by(ui.list, 0, -delta * 44, LV_ANIM_ON);
            break;
    }
}

void poll_cb(lv_timer_t *)
{
    VClient &c = VClient::instance();
    uint32_t now = millis();

    int delta = knob_take_delta();
    if (delta) handle_knob(delta);

    flush_pending(now);

    bool state_changed = c.version() != ui.last_state_version;
    bool devs_changed = c.devicesVersion() != ui.last_dev_version;

    if (state_changed) {
        ui.last_state_version = c.version();
        ui.st = c.state();
        if (ui.st.accessory_display != ui.last_display) {
            ui.last_display = ui.st.accessory_display;
            ui.c_dirty = ui.e_dirty = ui.style_dirty = false;
            ui.editing_until = 0;
        }
        // Adopt device values unless the user is mid-edit on the dial.
        if (now >= ui.editing_until) {
            if (ui.st.weight >= voltra::MIN_TARGET_LB && !ui.w_dirty) ui.weight = ui.st.weight;
            if (lb_mode()) {
                if (!ui.c_dirty) ui.chains = ui.st.chains_lb;
                if (!ui.e_dirty) ui.ecc = ui.st.eccentric_lb;
            } else {
                if (ui.st.chains >= 0 && !ui.c_dirty) ui.chains = ui.st.chains;
                if (ui.st.eccentric_known && !ui.e_dirty) ui.ecc = ui.st.eccentric;
            }
            if (!ui.c_dirty) ui.style = device_style(ui.st);
        }
        if (now < ui.toggle_pending_until && ui.st.loaded() == ui.toggle_target_loaded) {
            ui.toggle_pending_until = 0;   // confirmed
        }
    }
    if (state_changed) {
        // A set is under way while the Voltra's workout status says so and the cable
        // has actually moved (the status turns "active" on loading, before any rep).
        // It ends the moment the status goes to resting or unloading. The fitness mode
        // cannot do this: it stays at 1 through every set after the first.
        const bool moving = ui.st.rep_phase >= 1 && ui.st.rep_phase <= 3;
        const int ws = ui.st.workout_status;
        if (!ui.st.loaded()) {
            ui.set_active = false;
        } else if (ws >= 0) {
            if (ws != voltra::WORKOUT_STATUS_ACTIVE) ui.set_active = false;
            else if (moving) ui.set_active = true;
        } else {
            // no status seen yet: fall back to the mode, which covers the first set
            const int mode = ui.st.fitness_mode >= 0 ? (ui.st.fitness_mode & 0xFF) : -1;
            if (mode == voltra::FITNESS_MODE_STRENGTH_IDLE) ui.set_active = false;
            else if (moving) ui.set_active = true;
        }
    }
    if (ui.auto_load_pending_until && (ui.st.auto_loading() || ui.st.loaded())) {
        ui.auto_load_pending_until = 0;   // the Voltra has taken over
    }
    if (ui.auto_load_pending_until && now >= ui.auto_load_pending_until) {
        ui.auto_load_pending_until = 0;
        if (ui.screen == Screen::Main) refresh_main();
    }
    bool pending_expired = ui.toggle_pending_until && now >= ui.toggle_pending_until;
    if (pending_expired) ui.toggle_pending_until = 0;

    if (devs_changed) {
        ui.last_dev_version = c.devicesVersion();
        ui.devs = c.devices();
    }

    switch (ui.screen) {
        case Screen::Main:
            if (state_changed || pending_expired) refresh_main();
            break;
        case Screen::Settings:
            if (state_changed) refresh_settings();
            break;
        case Screen::AdjustChains:
        case Screen::AdjustEcc:
            if (state_changed) refresh_adjust();
            break;
        case Screen::Connect:
            if (state_changed || devs_changed) {
                rebuild_device_list();
                refresh_connect();
            }
            break;
    }


    // knob battery, every 10 s
    if (now - ui.last_kbat_ms > 10000) {
        ui.last_kbat_ms = now;
        int pct = battery_percent();
        if (pct >= 0) {
            const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 60 ? LV_SYMBOL_BATTERY_3
                            : pct > 40 ? LV_SYMBOL_BATTERY_2 : pct > 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
            lv_label_set_text_fmt(ui.lbl_kbat, "%s %d%%", sym, pct);
        } else {
            lv_label_set_text(ui.lbl_kbat, LV_SYMBOL_USB);
        }
    }
}

}  // namespace

void ui_init()
{
    build_main();
    build_settings();
    build_adjust();
    build_connect();
    lv_scr_load(ui.scr_main);
    ui.st = VClient::instance().state();
    refresh_main();
    lv_timer_create(poll_cb, 25, nullptr);
}
