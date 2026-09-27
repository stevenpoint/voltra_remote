#include "ui.h"

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>

#include "esp_heap_caps.h"

#include <algorithm>
#include <vector>

#include "hw/battery.h"
#include "hw/display.h"
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
#define ICON_ATTACH    "\xEE\x80\x85"   // U+E005
#define C_ATTACH   lv_color_hex(0x38bdf8)
#define C_ATTACH_HEX "38bdf8"

// Knob stepping (fine/coarse selection and snapping) lives in ui/knob_step.h.
using knobstep::apply_step;
constexpr int STEP_COARSE_LB = knobstep::STEP_COARSE_LB;

constexpr uint32_t SEND_DEBOUNCE_MS = 150;
constexpr uint32_t EDIT_HOLD_MS = 900;
// Long enough to cover the client re-issuing the command while the Voltra steps
// through its intermediate state, so the screen says "LOADING..." rather than
// flicking back to unloaded mid-sequence.
constexpr uint32_t TOGGLE_PENDING_MS = 5000;
constexpr uint32_t LOAD_REFUSED_SHOW_MS = 10000;   // how long a tap means "override"
constexpr uint32_t AUTO_LOAD_PENDING_MS = 4000;
// Switch off after this long with no touch or knob input, unless the Voltra is loaded.
constexpr uint32_t POWER_OFF_IDLE_MS = 10 * 60 * 1000;
// Attachment (bar, handle...) weight added to the displayed weight, never sent to the Voltra.
constexpr int MAX_ATTACH_LB = 50;
// A stand-in Voltra (serial "demo") left on by mistake would hide the real one, so it
// lapses this long after the last serial command.
constexpr uint32_t DEMO_TIMEOUT_MS = 2 * 60 * 1000;
// After asking two connected Voltras to pair, how long to wait for the host to report it.
constexpr uint32_t PAIR_WINDOW_MS = 30000;

enum class Screen { Main, Settings, AdjustChains, AdjustEcc, AdjustAttach, Connect };

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
    ROW_ATTACH,
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
        lv_obj_t *arc = nullptr;         // watch: a small dial showing the amount
    };
    Chip chip_ecc, chip_chain, chip_attach;
    lv_obj_t *dock = nullptr;   // watch: the row of chips and the settings button

    // Two weight presets either side of the weight, kept on the knob only (NVS).
    struct Preset {
        lv_obj_t *btn = nullptr;
        lv_obj_t *value = nullptr;
        lv_obj_t *unit = nullptr;
        int lb = 0;   // 0 = empty
    };
    Preset presets[2];
    lv_obj_t *lbl_kbat = nullptr;
    // Covers the whole main screen while loaded, so a tap anywhere unloads.
    lv_obj_t *unload_catcher = nullptr;

    // settings menu: one bubble per accessory
    lv_obj_t *scr_settings = nullptr;
    struct Bubble {
        lv_obj_t *btn = nullptr;
        lv_obj_t *icon = nullptr;
        lv_obj_t *title = nullptr;
        lv_obj_t *value = nullptr;
    };
    Bubble bub_ecc, bub_chains, bub_mountain, bub_inverse, bub_attach;
    lv_obj_t *btn_settings_done = nullptr;

    // eccentric / chains / attachment (watch): the amount, a slider and quick picks
    lv_obj_t *scr_value = nullptr;
    lv_obj_t *lbl_val_title = nullptr;
    lv_obj_t *lbl_val_num = nullptr, *lbl_val_unit = nullptr, *lbl_val_info = nullptr;
    lv_obj_t *dial_val = nullptr;
    lv_obj_t *val_pick[4] = {};

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
    int attach = 0;            // attachment lb, kept on this device only (NVS)
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
    // The Voltra refused a tap-to-load (cable out). Until then, a tap sends the
    // override (Client::loadOverride()) instead of a plain load.
    uint32_t load_refused_until = 0;
    uint32_t last_load_refused = 0;
    // A set is under way: loaded and the cable has moved since the last rest.
    bool set_active = false;
    // The Voltra adds eccentric only once it knows the rep's length, which it learns from
    // the first full rep after loading. Set once a return has finished while loaded.
    bool ecc_learned = false;
    uint8_t last_rep_phase = 0;
    uint8_t shown_phase = 0;   // rep phase on screen: 1 pull, 3 return, held through the others

    knobstep::RateTracker knob_rate;

    uint32_t last_state_version = 0xFFFFFFFF;
    uint32_t last_dev_version = 0xFFFFFFFF;
    uint32_t last_kbat_ms = 0;
    uint32_t last_input_ms = 0;   // last knob turn or swipe (touches are LVGL's own count)
    // Stand-in Voltra state for screenshots (ui_command "demo"), used in place of the real one.
    bool demo = false;
    uint32_t last_cmd_ms = 0;     // last serial command: the stand-in lapses without them
    DeviceState demo_st;
    DeviceState st;
    std::vector<FoundDevice> devs;

    // Two Voltras (voltra::Client::COUNT): the one on screen, and the other one.
    int active = 0;
    DeviceState st_other;
    uint32_t last_other_version = 0xFFFFFFFF;
    bool dual = false;            // both in use: the top bar shows both, tap to switch
    int twin_host = -1;           // slot hosting a twin made or seen here, -1 none
    uint32_t pair_until = 0;      // a pair was just asked for: let it settle
    bool pair_pending = false;    // waiting for the other to be let go before twinning
    uint32_t last_pair_poll = 0;
} ui;

/** The Voltra on screen, and the other one. */
VClient &vc() { return VClient::instance(ui.active); }
VClient &vc_other() { return VClient::instance(1 - ui.active); }
/** Slot 0 runs the scan behind the connect screen. */
VClient &scanner() { return VClient::instance(0); }
const DeviceState &slot_state(int slot) { return slot == ui.active ? ui.st : ui.st_other; }
bool slot_busy(const DeviceState &s)
{
    return s.connected() || s.conn == ConnState::Connecting || s.conn == ConnState::Reconnecting;
}

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
#ifdef WATCH206
    // One line: the amount in the Voltra's unit, pounds without the unit.
    if (lb_mode()) lv_label_set_text_fmt(c.primary, signed_value ? "%+d" : "%d", v);
    else lv_label_set_text_fmt(c.primary, signed_value ? "%+d%%" : "%d%%", v);
    return;
#endif
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

lv_obj_t *screen_obj(Screen s)
{
    switch (s) {
        case Screen::Settings: return ui.scr_settings;
#ifdef WATCH206
        case Screen::AdjustChains:
        case Screen::AdjustEcc:
        case Screen::AdjustAttach: return ui.scr_value;
#else
        case Screen::AdjustChains:
        case Screen::AdjustEcc:
        case Screen::AdjustAttach: return ui.scr_adjust;
#endif
        case Screen::Connect: return ui.scr_connect;
        default: return ui.scr_main;
    }
}

void show(Screen s)
{
    ui.screen = s;
    lv_scr_load_anim(screen_obj(s), LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, false);
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

/** A 270-degree dial, open at the bottom, that only shows a value (not touchable). */
lv_obj_t *make_dial(lv_obj_t *parent, int size, int width, lv_color_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_bg_angles(arc, 135, 405);
    lv_arc_set_rotation(arc, 0);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    return arc;
}

[[maybe_unused]] lv_obj_t *make_ring(lv_obj_t *parent, lv_color_t color) { return make_dial(parent, 344, 14, color); }

#ifdef WATCH206
// ---------------------------------------------------------------------------
// Rail (watch): in place of the round ring, a thick line follows the screen's rounded
// edge from low on the left side, over the top, to low on the right side. It fills up
// to the weight, and a short white bar across it marks the weight itself.
// ---------------------------------------------------------------------------
constexpr float RAIL_INSET = 12;         // centre line from the screen edge
constexpr float RAIL_RADIUS = 94;        // centre-line radius at the top corners
constexpr float RAIL_BOTTOM_GAP = 110;   // where the ends stop, clear of the rounded bottom corners
constexpr int RAIL_WIDTH = 12;
constexpr int RAIL_MARK_LEN = 22, RAIL_MARK_WIDTH = 6;
#define C_RAIL_TRACK lv_color_hex(0x5f6b80)

struct RailState {
    int lo = voltra::MIN_TARGET_LB;
    int hi = voltra::MAX_TARGET_LB;
    int value = voltra::MIN_TARGET_LB;
    lv_color_t color = C_RAIL_TRACK;
} s_rail;

/** Left side (upwards), top-left corner, top, top-right corner, right side (downwards). */
struct RailGeom {
    float x0, x1, y_top, y_bot, r;
    float side, quarter, top, total;
};

RailGeom rail_geom(float w, float h)
{
    RailGeom g;
    g.r = RAIL_RADIUS;
    g.x0 = RAIL_INSET;
    g.x1 = w - RAIL_INSET;
    g.y_top = RAIL_INSET;
    g.y_bot = h - RAIL_BOTTOM_GAP;
    g.side = g.y_bot - (g.y_top + g.r);
    g.quarter = (float)M_PI * g.r / 2;
    g.top = (g.x1 - g.x0) - 2 * g.r;
    g.total = 2 * g.side + 2 * g.quarter + g.top;
    return g;
}

/** Point at distance s along the rail, with the unit normal across it. */
void rail_point(const RailGeom &g, float s, float &x, float &y, float &nx, float &ny)
{
    if (s < g.side) { x = g.x0; y = g.y_bot - s; nx = 1; ny = 0; return; }
    s -= g.side;
    if (s < g.quarter) {
        const float a = (float)M_PI + s / g.r;
        nx = cosf(a); ny = sinf(a);
        x = g.x0 + g.r + g.r * nx; y = g.y_top + g.r + g.r * ny;
        return;
    }
    s -= g.quarter;
    if (s < g.top) { x = g.x0 + g.r + s; y = g.y_top; nx = 0; ny = 1; return; }
    s -= g.top;
    if (s < g.quarter) {
        const float a = 1.5f * (float)M_PI + s / g.r;
        nx = cosf(a); ny = sinf(a);
        x = g.x1 - g.r + g.r * nx; y = g.y_top + g.r + g.r * ny;
        return;
    }
    s -= g.quarter;
    x = g.x1; y = g.y_top + g.r + std::min(s, g.side); nx = -1; ny = 0;
}

/** Draw the stretch [a, b] of the rail, with square ends. */
void rail_stroke(lv_draw_ctx_t *ctx, const lv_area_t &area, const RailGeom &g, float a, float b,
                 lv_color_t color)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = RAIL_WIDTH;
    line.color = color;
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.width = RAIL_WIDTH;
    arc.color = color;
    arc.rounded = 0;
    const lv_coord_t ox = area.x1, oy = area.y1;
    auto straight = [&](float x1, float y1, float x2, float y2) {
        lv_point_t p1 = {(lv_coord_t)(ox + lroundf(x1)), (lv_coord_t)(oy + lroundf(y1))};
        lv_point_t p2 = {(lv_coord_t)(ox + lroundf(x2)), (lv_coord_t)(oy + lroundf(y2))};
        lv_draw_line(ctx, &line, &p1, &p2);
    };
    auto corner = [&](float cx, float cy, float deg0, float deg1) {
        lv_point_t c = {(lv_coord_t)(ox + lroundf(cx)), (lv_coord_t)(oy + lroundf(cy))};
        lv_draw_arc(ctx, &arc, &c, (uint16_t)lroundf(g.r + RAIL_WIDTH / 2.0f), (uint16_t)lroundf(deg0),
                    (uint16_t)lroundf(deg1));
    };
    float s0 = 0;
    // Each piece gets the part of [a, b] that falls on it, as offsets p..q into the piece.
    auto piece = [&](float len, int kind) {
        const float p = std::max(a, s0) - s0, q = std::min(b, s0 + len) - s0;
        s0 += len;
        if (q <= p) return;
        switch (kind) {
            case 0: straight(g.x0, g.y_bot - p, g.x0, g.y_bot - q); break;
            case 1: corner(g.x0 + g.r, g.y_top + g.r, 180 + 90 * p / len, 180 + 90 * q / len); break;
            case 2: straight(g.x0 + g.r + p, g.y_top, g.x0 + g.r + q, g.y_top); break;
            case 3: corner(g.x1 - g.r, g.y_top + g.r, 270 + 90 * p / len, 270 + 90 * q / len); break;
            default: straight(g.x1, g.y_top + g.r + p, g.x1, g.y_top + g.r + q); break;
        }
    };
    piece(g.side, 0);
    piece(g.quarter, 1);
    piece(g.top, 2);
    piece(g.quarter, 3);
    piece(g.side, 4);
}

void rail_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const RailGeom g = rail_geom(lv_area_get_width(&a), lv_area_get_height(&a));
    const int span = s_rail.hi > s_rail.lo ? s_rail.hi - s_rail.lo : 1;
    const float s = g.total * (s_rail.value - s_rail.lo) / span;

    rail_stroke(ctx, a, g, s, g.total, C_RAIL_TRACK);
    rail_stroke(ctx, a, g, 0, s, s_rail.color);

    float x, y, nx, ny;
    rail_point(g, s, x, y, nx, ny);
    lv_draw_line_dsc_t mark;
    lv_draw_line_dsc_init(&mark);
    mark.width = RAIL_MARK_WIDTH;
    mark.color = C_TEXT;
    const float h = RAIL_MARK_LEN / 2.0f;
    lv_point_t p1 = {(lv_coord_t)(a.x1 + lroundf(x - nx * h)), (lv_coord_t)(a.y1 + lroundf(y - ny * h))};
    lv_point_t p2 = {(lv_coord_t)(a.x1 + lroundf(x + nx * h)), (lv_coord_t)(a.y1 + lroundf(y + ny * h))};
    lv_draw_line(ctx, &mark, &p1, &p2);
}

lv_obj_t *make_rail(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, rail_draw, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}
#endif

/** Weight gauge: the rail on the watch, the ring on the knob. */
void set_weight_gauge(lv_obj_t *gauge, int lo, int hi, int value, lv_color_t color)
{
#ifdef WATCH206
    value = value < lo ? lo : (value > hi ? hi : value);
    if (s_rail.lo == lo && s_rail.hi == hi && s_rail.value == value &&
        lv_color_to32(s_rail.color) == lv_color_to32(color)) {
        return;
    }
    s_rail.lo = lo;
    s_rail.hi = hi;
    s_rail.value = value;
    s_rail.color = color;
    lv_obj_invalidate(gauge);
#else
    lv_arc_set_range(gauge, lo, hi);
    lv_arc_set_value(gauge, value);
    lv_obj_set_style_arc_color(gauge, color, LV_PART_INDICATOR);
#endif
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
[[maybe_unused]] lv_obj_t *make_action_button(lv_obj_t *parent, const char *text)
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
    VClient &c = vc();
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

/** The amount the adjust screen on show edits: eccentric, the chain style's, or attachment. */
int adjust_value()
{
    switch (ui.screen) {
        case Screen::AdjustChains: return style_amount(ui.adj_style);
        case Screen::AdjustEcc: return ui.ecc;
        case Screen::AdjustAttach: return ui.attach;
        default: return 0;
    }
}

/** Set that amount, within its limits. @return false if it did not change. */
bool set_adjust_value(int v)
{
    switch (ui.screen) {
        case Screen::AdjustChains: {
            v = clampi(v, 0, chains_max());
            if (v == style_amount(ui.adj_style)) return false;
            // Dialling an amount into another style switches the Voltra over to it,
            // which turns the previous style off.
            if (v > 0 && ui.style != ui.adj_style) {
                ui.style = ui.adj_style;
                ui.style_dirty = true;
            }
            ui.chains = v;
            mark_chains_changed();
            return true;
        }
        case Screen::AdjustEcc: {
            const int m = ecc_max();
            v = clampi(v, -m, m);
            if (v == ui.ecc) return false;
            ui.ecc = v;
            mark_ecc_changed();
            return true;
        }
        case Screen::AdjustAttach:
            v = clampi(v, 0, MAX_ATTACH_LB);
            if (v == ui.attach) return false;
            ui.attach = v;
            return true;
        default:
            return false;
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

[[maybe_unused]] void load_presets()
{
    Preferences p;
    if (!p.begin(PRESET_NS, true)) return;   // namespace not created yet: all empty
    for (int i = 0; i < 2; i++) ui.presets[i].lb = p.getUShort(PRESET_KEYS[i], 0);
    p.end();
}

/** The attachment is kept per Voltra: each may have its own bar or handle on. */
const char *attach_key() { return ui.active == 0 ? "attach" : "attach1"; }

void load_attach()
{
    Preferences p;
    if (!p.begin(PRESET_NS, true)) return;
    ui.attach = clampi(p.getUChar(attach_key(), 0), 0, MAX_ATTACH_LB);
    p.end();
}

void save_attach()
{
    Preferences p;
    if (!p.begin(PRESET_NS, false)) return;
    if (p.getUChar(attach_key(), 0) != ui.attach) p.putUChar(attach_key(), (uint8_t)ui.attach);
    p.end();
}

/** Put the other Voltra on screen. Edits not yet sent go to the one being left. */
void switch_to(int slot)
{
    if (slot == ui.active) return;
    flush_pending(millis() + SEND_DEBOUNCE_MS);
    save_attach();
    ui.active = slot;
    ui.w_dirty = ui.c_dirty = ui.e_dirty = ui.style_dirty = false;
    ui.editing_until = 0;
    ui.toggle_pending_until = ui.auto_load_pending_until = ui.load_refused_until = 0;
    ui.set_active = ui.ecc_learned = false;
    ui.last_rep_phase = ui.shown_phase = 0;
    ui.last_display = -2;
    ui.last_state_version = ui.last_other_version = 0xFFFFFFFF;
    std::swap(ui.st, ui.st_other);
    ui.last_load_refused = ui.st.load_refused;
    load_attach();
    haptics_double();
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
        if (!pr.btn) continue;   // not built (watch)
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

[[maybe_unused]] void make_preset(int i, int x, int y)
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

// Main screen layout, as offsets from the screen centre.
#ifdef WATCH206
// 410x502, inside the rail: the tap-to-load line at the top of the weight's touch target,
// the weight, a line under it (what makes up the weight, or the rep phase), and the dock
// of chips at the bottom, where the set and rep counts go during a set.
#define L_WEIGHT_FONT font_poppins_160
#define L_STATE_FONT font_poppins_20
#define L_REPS_FONT font_poppins_16
#define L_IDLE_NUM lv_color_hex(0xe2e8f0)   // unloaded: light enough to read
#define L_IDLE_GAUGE C_TEXT
#define L_OFF_GAUGE C_RAIL_TRACK
constexpr const char *L_WEIGHT_UNIT = "lb";
constexpr const char *L_TXT_LOAD = "TAP TO LOAD";
constexpr const char *L_TXT_UNLOAD = "TAP TO UNLOAD";
// The weight's touch target starts 12 px below the top bar's, so a tap meant for the
// batteries (switching Voltras) does not load.
constexpr int L_CENTER_W = 330, L_CENTER_H = 230, L_CENTER_Y = -42;
constexpr int L_ROW_Y = 10;       // weight row, from the centre of its touch target
constexpr int L_REPS_Y = 52;
constexpr int L_DOCK_Y = 151;
constexpr int L_COUNT_X = 77, L_COUNT_CAP_Y = 104, L_COUNT_NUM_Y = 152;
#else
#define L_WEIGHT_FONT font_poppins_96
#define L_STATE_FONT font_poppins_14
#define L_REPS_FONT font_poppins_14
#define L_IDLE_NUM C_IDLE_NUM
#define L_IDLE_GAUGE C_IDLE_RING
#define L_OFF_GAUGE C_TRACK
constexpr const char *L_WEIGHT_UNIT = "lb";
constexpr const char *L_TXT_LOAD = "Tap to Load";
constexpr const char *L_TXT_UNLOAD = "Tap to Unload";
constexpr int L_CENTER_W = 236, L_CENTER_H = 150, L_CENTER_Y = -34;
constexpr int L_ROW_Y = 0;
constexpr int L_REPS_Y = 50;
constexpr int L_GEAR_Y = 114;
constexpr int L_CHIP_X = 74, L_CHIP_Y = 96;
#endif

void refresh_main()
{
    const DeviceState &st = ui.st;
    bool connected = st.connected();
    bool ready = st.conn == ConnState::Ready;
    uint32_t now = millis();

    bool loaded = st.loaded();
    bool pending = now < ui.toggle_pending_until && loaded != ui.toggle_target_loaded;
    // During a set the bubbles give way to big set / rep counters.
    const bool set_mode = ready && loaded && ui.set_active;
    // With eccentric on, the Voltra adds it on the way back (the return phase of each
    // rep), so the weight follows the rep: base on the pull, base + eccentric on return.
    // Not on the first rep after loading, which the Voltra uses to learn the rep length.
    const bool ecc_phase = set_mode && ui.ecc != 0 && ui.ecc_learned && st.rep_phase == 3;

    // weight
    // The Voltra's weight is per unit; twinned, show the pair's total like its screen does.
    const int per_unit_x = st.twinned() ? 2 : 1;
    const int device_lb = (ui.weight + (ecc_phase ? amount_lb(ui.ecc) : 0)) * per_unit_x;
    // The attachment's weight is added for display only.
    lv_label_set_text_fmt(ui.lbl_weight, "%d", device_lb + ui.attach);

    lv_color_t ring = loaded ? C_LOADED : L_IDLE_GAUGE;
    if (!connected) ring = L_OFF_GAUGE;
    set_weight_gauge(ui.arc_weight, voltra::MIN_TARGET_LB * per_unit_x, weight_max() * per_unit_x,
                     device_lb, ring);

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
        lv_obj_set_style_text_color(ui.lbl_weight, L_IDLE_NUM, 0);
    } else if (!loaded && now < ui.load_refused_until) {
        // Tap-to-load refused because the cable is out. A tap now overrides and loads
        // at once; hold still starts the Voltra's auto load.
        lv_label_set_text(ui.lbl_state, "CABLE OUT: TAP TO OVERRIDE");
        lv_obj_set_style_text_color(ui.lbl_state, C_WARN, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, L_IDLE_NUM, 0);
    } else if (pending) {
        lv_label_set_text(ui.lbl_state, ui.toggle_target_loaded ? "LOADING..." : "UNLOADING...");
        lv_obj_set_style_text_color(ui.lbl_state, C_WARN, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, loaded ? C_LOADED : L_IDLE_NUM, 0);
    } else if (loaded) {
        lv_label_set_text(ui.lbl_state, L_TXT_UNLOAD);
        lv_obj_set_style_text_color(ui.lbl_state, C_TEXT, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, C_LOADED, 0);
    } else if (!ready) {
        lv_label_set_text(ui.lbl_state, "CONNECTING...");
        lv_obj_set_style_text_color(ui.lbl_state, C_MUTED, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, C_MUTED, 0);
    } else if (st.activation == 0) {
        lv_label_set_text(ui.lbl_state, "VOLTRA NOT ACTIVATED");
        lv_obj_set_style_text_color(ui.lbl_state, C_DANGER, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, L_IDLE_NUM, 0);
    } else {
        lv_label_set_text(ui.lbl_state, L_TXT_LOAD);
        lv_obj_set_style_text_color(ui.lbl_state, C_TEXT, 0);
        lv_obj_set_style_text_color(ui.lbl_weight, L_IDLE_NUM, 0);
    }

    // top bar: Bluetooth icon (blue when connected, white when not) and, once connected,
    // the Voltra's battery level. Recolour markup colours just the icon.
    char top[128];
    const char *bt_hex = connected ? C_BT_HEX : C_WHITE_HEX;
    auto battery_sym = [](int pct) {
        return pct > 80 ? LV_SYMBOL_BATTERY_FULL : pct > 60 ? LV_SYMBOL_BATTERY_3
             : pct > 40 ? LV_SYMBOL_BATTERY_2 : pct > 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    };
    // Two Voltras in use: both, left and right by slot, the one on screen in white.
    ui.dual = !ui.demo && !vc_other().paused() && slot_busy(ui.st_other) && !st.twinned();
    if (ui.dual) {
        size_t n = 0;
        for (int i = 0; i < VClient::COUNT; i++) {
            const DeviceState &s = slot_state(i);
            const char *name = s.device_name[0] ? s.device_name : "Voltra";
            const size_t len = strlen(name);
            const char *tail = len > 4 ? name + len - 4 : name;   // "VTR-002166" -> "2166"
            const bool on = i == ui.active;
            char bat[24] = "";
            if (s.connected() && s.battery >= 0) snprintf(bat, sizeof(bat), "  %s %d%%", battery_sym(s.battery), s.battery);
            if (on) {
                n += snprintf(top + n, sizeof(top) - n, "#%s " LV_SYMBOL_BLUETOOTH "# %s%s",
                              s.connected() ? C_BT_HEX : C_WHITE_HEX, tail, bat);
            } else {
                n += snprintf(top + n, sizeof(top) - n, "#64748b " LV_SYMBOL_BLUETOOTH " %s%s#", tail, bat);
            }
            if (i == 0) n += snprintf(top + n, sizeof(top) - n, "     ");
        }
    } else if (connected && st.battery >= 0) {
        snprintf(top, sizeof(top), "#%s " LV_SYMBOL_BLUETOOTH "#   %s %d%%", bt_hex, battery_sym(st.battery),
                 st.battery);
        if (st.twinned() && st.twin_peer_battery >= 0) {
            // Twinned: the follower's battery after the host's.
            const size_t n = strlen(top);
            snprintf(top + n, sizeof(top) - n, "  %s %d%%", battery_sym(st.twin_peer_battery),
                     st.twin_peer_battery);
        }
    } else {
        snprintf(top, sizeof(top), "#%s " LV_SYMBOL_BLUETOOTH "#", bt_hex);
    }
    if (connected && (st.twinned() || st.twin_state == voltra::TWIN_STATE_JOINING)) {
        // Twin mode: this Voltra hosts, and every command drives the pair.
        const size_t n = strlen(top);
        snprintf(top + n, sizeof(top) - n, st.twinned() ? "   #%s TWIN#" : "   #%s TWINNING#", C_WHITE_HEX);
    }
    lv_label_set_text(ui.lbl_top, top);
    lv_obj_set_style_text_color(ui.lbl_top, C_TEXT, 0);

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

    // The line under the weight: during a set (watch) the rep phase, else reps or the live
    // force between sets, else what the weight is made of.
    if (!set_mode) ui.shown_phase = 0;
    else if (st.rep_phase == 1 || st.rep_phase == 3) ui.shown_phase = st.rep_phase;   // else keep
    if (set_mode) {
#ifdef WATCH206
        if (ui.shown_phase == 3 && ecc_phase) {
            lv_label_set_text_fmt(ui.lbl_reps, LV_SYMBOL_DOWN "  Return    %+d eccentric",
                                  amount_lb(ui.ecc) * per_unit_x);
        } else if (ui.shown_phase == 3) {
            lv_label_set_text(ui.lbl_reps, LV_SYMBOL_DOWN "  Return");
        } else if (ui.shown_phase == 1) {
            lv_label_set_text(ui.lbl_reps, LV_SYMBOL_UP "  Pull");
        } else {
            lv_label_set_text(ui.lbl_reps, "");
        }
#else
        lv_label_set_text(ui.lbl_reps, "");
#endif
    } else if (ready && loaded && (st.reps > 0 || st.sets > 0)) {
        lv_label_set_text_fmt(ui.lbl_reps, "SET %u   REP %u", (unsigned)st.sets, (unsigned)st.reps);
    } else if (ready && loaded && st.force_known) {
        lv_label_set_text_fmt(ui.lbl_reps, "%d lb on cable", st.force_lb);
    } else if (ui.attach > 0) {
#ifdef WATCH206
        lv_label_set_text_fmt(ui.lbl_reps, "%d Voltra  +  #" C_ATTACH_HEX " %d attachment#", device_lb, ui.attach);
#else
        lv_label_set_text_fmt(ui.lbl_reps, "Voltra %d lb  +  attachment %d lb", device_lb, ui.attach);
#endif
    } else {
        lv_label_set_text(ui.lbl_reps, "");
    }

    // settings button: white with nothing on, green once any accessory is on
    const bool any_accessory = ui.chains > 0 || ui.ecc != 0;
    lv_obj_set_style_text_color(ui.lbl_gear, any_accessory ? C_LOADED : C_TEXT, 0);

#ifdef WATCH206
    {
        const int em = std::max(ecc_max(), 1), cm = std::max(chains_max(), 1);
        lv_arc_set_mode(ui.chip_ecc.arc, LV_ARC_MODE_SYMMETRICAL);
        lv_arc_set_range(ui.chip_ecc.arc, -em, em);
        lv_arc_set_value(ui.chip_ecc.arc, ui.ecc);
        lv_arc_set_range(ui.chip_chain.arc, 0, cm);
        lv_arc_set_value(ui.chip_chain.arc, ui.chains);
        lv_obj_set_style_arc_color(ui.chip_chain.arc, style_colour(ui.style), LV_PART_INDICATOR);
        lv_arc_set_range(ui.chip_attach.arc, 0, MAX_ATTACH_LB);
        lv_arc_set_value(ui.chip_attach.arc, ui.attach);
    }
#endif
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

#ifdef WATCH206
    // attachment chip
    if (ui.attach > 0) {
        lv_label_set_text_fmt(ui.chip_attach.primary, "%d", ui.attach);
        lv_obj_clear_flag(ui.chip_attach.btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.chip_attach.btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (set_mode) lv_obj_add_flag(ui.chip_attach.btn, LV_OBJ_FLAG_HIDDEN);
#endif

    refresh_presets();

    if (set_mode) {
        lv_obj_add_flag(ui.btn_gear, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.chip_ecc.btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.chip_chain.btn, LV_OBJ_FLAG_HIDDEN);
        for (auto &pr : ui.presets) if (pr.btn) lv_obj_add_flag(pr.btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(ui.btn_gear, LV_OBJ_FLAG_HIDDEN);
        for (auto &pr : ui.presets) if (pr.btn) lv_obj_clear_flag(pr.btn, LV_OBJ_FLAG_HIDDEN);
        // the chips were shown or hidden above according to the accessories
    }

    set_obj_hidden(ui.unload_catcher, !(ready && loaded));
}

void on_center_clicked(lv_event_t *)
{
    VClient &c = vc();
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
    // A tap straight after a refused load (cable out) is the override.
    const bool override = !ui.st.loaded() && millis() < ui.load_refused_until;
    ui.load_refused_until = 0;
    // Push any pending dial edits first so the device loads the weight on screen.
    flush_pending(millis() + SEND_DEBOUNCE_MS);
    if (override) {
        // Runs through the Voltra's auto load, so show it as one.
        c.loadOverride();
        ui.toggle_pending_until = 0;
        ui.auto_load_pending_until = millis() + AUTO_LOAD_PENDING_MS;
        haptics_double();
        refresh_main();
        return;
    }
    if (!ui.st.loaded() && (ui.st.auto_loading() || millis() < ui.auto_load_pending_until)) {
        // Tapping during auto load cancels it.
        ui.auto_load_pending_until = 0;
        c.unload();
        haptics_click();
        refresh_main();
        return;
    }
    bool loaded = ui.st.loaded();
    log_i("ui: %s tapped on slot %d (%s), conn %d, mode %d", loaded ? "unload" : "load", ui.active,
          ui.st.device_name, (int)ui.st.conn, ui.st.fitness_mode);
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
    ui.load_refused_until = 0;
    ui.auto_load_pending_until = millis() + AUTO_LOAD_PENDING_MS;
    vc().autoLoad();
    haptics_double();
    refresh_main();
}

/**
 * Top bar: with two Voltras in use, tapping the other one's half puts it on screen;
 * otherwise (or on the one already on screen) it opens the connect screen.
 */
void on_top_clicked(lv_event_t *)
{
    if (ui.dual) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_get_act(), &p);
        const int slot = p.x < lv_obj_get_width(lv_scr_act()) / 2 ? 0 : 1;
        if (slot != ui.active) {
            switch_to(slot);
            refresh_main();
            return;
        }
    }
    haptics_click();
    show(Screen::Connect);
    scanner().scanStart();
}

void on_gear_clicked(lv_event_t *);
void refresh_settings();
void refresh_adjust();

/** Tapping a chip goes straight to that accessory's dial, and Done comes back here. */
void on_chip_clicked(lv_event_t *e)
{
    haptics_click();
    ui.adj_return = Screen::Main;
    lv_obj_t *target = lv_event_get_target(e);
    if (target == ui.chip_ecc.btn) {
        show(Screen::AdjustEcc);
    } else if (target == ui.chip_attach.btn) {
        show(Screen::AdjustAttach);
    } else {
        ui.adj_style = ui.style;
        show(Screen::AdjustChains);
    }
    refresh_adjust();
}

/**
 * A small round button for an active accessory: its icon over its amount, in both units
 * on the knob and in the Voltra's unit on the watch. On the watch it sits in the dock
 * (a row laid out for it), on the knob at (x, y) from the centre.
 */
void make_chip(Ui::Chip &c, const char *icon, lv_color_t colour, lv_obj_t *parent, int x, int y)
{
#ifdef WATCH206
    // A small version of the adjust screen's dial, the amount inside.
    constexpr int size = 84;
    c.btn = make_flat_button(parent, size, size);
    c.arc = make_dial(c.btn, size, 6, colour);
    c.icon = make_label(c.btn, &font_icons_26, colour, icon);
    lv_obj_align(c.icon, LV_ALIGN_CENTER, 0, -12);
    c.primary = make_label(c.btn, &font_poppins_20, C_TEXT, "");
    lv_obj_align(c.primary, LV_ALIGN_CENTER, 0, 16);
    c.secondary = make_label(c.btn, &font_poppins_14, C_MUTED, "");
    lv_obj_add_flag(c.secondary, LV_OBJ_FLAG_HIDDEN);
    (void)x;
    (void)y;
#else
    constexpr int size = 70, row = 17;
    c.btn = make_pill_button(parent, size, size, colour, "", nullptr);
    lv_obj_align(c.btn, LV_ALIGN_CENTER, x, y);
    c.icon = make_label(c.btn, &font_icons_18, colour, icon);
    lv_obj_align(c.icon, LV_ALIGN_CENTER, 0, -row);
    c.primary = make_label(c.btn, &font_poppins_14, C_TEXT, "");
    lv_obj_align(c.primary, LV_ALIGN_CENTER, 0, 1);
    c.secondary = make_label(c.btn, &font_poppins_14, C_MUTED, "");
    lv_obj_align(c.secondary, LV_ALIGN_CENTER, 0, row);
#endif
    lv_obj_add_event_cb(c.btn, on_chip_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(c.btn, LV_OBJ_FLAG_HIDDEN);
}

void build_main()
{
    ui.scr_main = make_screen();
#ifdef WATCH206
    ui.arc_weight = make_rail(ui.scr_main);
    constexpr int TOP_BAR_Y = 26;   // inside the rail
#else
    ui.arc_weight = make_ring(ui.scr_main, C_IDLE_RING);
    constexpr int TOP_BAR_Y = 20;
#endif

    // top bar (tap -> connect menu)
#ifdef WATCH206
    ui.btn_top = make_flat_button(ui.scr_main, 330, 56);   // two Voltras: one half each
#else
    ui.btn_top = make_flat_button(ui.scr_main, 220, 44);
#endif
    lv_obj_align(ui.btn_top, LV_ALIGN_TOP_MID, 0, TOP_BAR_Y);
    lv_obj_add_event_cb(ui.btn_top, on_top_clicked, LV_EVENT_CLICKED, nullptr);
    ui.lbl_top = make_label(ui.btn_top, &font_poppins_16, C_TEXT, "");
    lv_label_set_recolor(ui.lbl_top, true);
    lv_label_set_text(ui.lbl_top, "#" C_WHITE_HEX " " LV_SYMBOL_BLUETOOTH "#");
    lv_obj_center(ui.lbl_top);

    // sets / reps sit under the weight
    ui.lbl_reps = make_label(ui.scr_main, &L_REPS_FONT, C_MUTED, "");
    lv_label_set_recolor(ui.lbl_reps, true);
    lv_obj_align(ui.lbl_reps, LV_ALIGN_CENTER, 0, L_REPS_Y);

    // During a set: two big counters filling the space the bubbles use at rest.
#ifdef WATCH206
    // In the dock's place, under the weight, which stays where it is.
    ui.lbl_set_cap = make_label(ui.scr_main, &font_poppins_16, C_MUTED, "SET");
    ui.lbl_rep_cap = make_label(ui.scr_main, &font_poppins_16, C_MUTED, "REPS");
    constexpr int cx = L_COUNT_X, cap_y = L_COUNT_CAP_Y, num_y = L_COUNT_NUM_Y;
#else
    ui.lbl_set_cap = make_label(ui.scr_main, &font_poppins_22, C_MUTED, "Set");
    ui.lbl_rep_cap = make_label(ui.scr_main, &font_poppins_22, C_MUTED, "Reps");
    // digits run from ~62 to ~108 px below centre, inside the ring
    constexpr int cx = 64, cap_y = 38, num_y = 86;
#endif
    ui.lbl_set_num = make_label(ui.scr_main, &font_poppins_bold_64, C_TEXT, "0");
    ui.lbl_rep_num = make_label(ui.scr_main, &font_poppins_bold_64, C_TEXT, "0");
    lv_obj_t *const caps[] = {ui.lbl_set_cap, ui.lbl_rep_cap};
    for (lv_obj_t *o : caps) lv_obj_set_style_text_letter_space(o, 2, 0);
    lv_obj_align(ui.lbl_set_cap, LV_ALIGN_CENTER, -cx, cap_y);
    lv_obj_align(ui.lbl_rep_cap, LV_ALIGN_CENTER, cx, cap_y);
    lv_obj_t *const nums[] = {ui.lbl_set_num, ui.lbl_rep_num};
    for (lv_obj_t *o : nums) {
        // centre the digits on their column however many there are
        lv_obj_set_width(o, 120);
        lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_align(ui.lbl_set_num, LV_ALIGN_CENTER, -cx, num_y);
    lv_obj_align(ui.lbl_rep_num, LV_ALIGN_CENTER, cx, num_y);
    for (lv_obj_t *o : caps) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    for (lv_obj_t *o : nums) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);

    // Waiting-for-first-rep dots, sitting on the digits' baseline in the reps column.
    ui.rep_dots = lv_obj_create(ui.scr_main);
    lv_obj_remove_style_all(ui.rep_dots);
    lv_obj_set_size(ui.rep_dots, 72, 14);
    lv_obj_clear_flag(ui.rep_dots, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(ui.rep_dots, LV_ALIGN_CENTER, cx, num_y + 14);
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
    ui.btn_center = make_flat_button(ui.scr_main, L_CENTER_W, L_CENTER_H);
    lv_obj_align(ui.btn_center, LV_ALIGN_CENTER, 0, L_CENTER_Y);
    // Short-clicked, not clicked, so releasing a hold (auto load) is not also a tap.
    lv_obj_add_event_cb(ui.btn_center, on_center_clicked, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui.btn_center, on_center_hold, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_t *row = make_value_row(ui.btn_center, &L_WEIGHT_FONT, &ui.lbl_weight, &ui.lbl_unit);
    lv_label_set_text(ui.lbl_unit, L_WEIGHT_UNIT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, L_ROW_Y);
    ui.lbl_state = make_label(ui.btn_center, &L_STATE_FONT, C_MUTED, "NOT CONNECTED");
#ifdef WATCH206
    lv_obj_set_style_text_letter_space(ui.lbl_state, 2, 0);
    lv_obj_align(ui.lbl_state, LV_ALIGN_TOP_MID, 0, 6);

    // The dock: whichever chips are on, with the settings button, as one centred row.
    // Settings sits between eccentric / chains and the attachment.
    ui.dock = lv_obj_create(ui.scr_main);
    lv_obj_remove_style_all(ui.dock);
    lv_obj_set_size(ui.dock, 380, 92);
    lv_obj_clear_flag(ui.dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ui.dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui.dock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui.dock, 10, 0);
    lv_obj_align(ui.dock, LV_ALIGN_CENTER, 0, L_DOCK_Y);
    make_chip(ui.chip_ecc, ICON_ECCENTRIC, C_ECC, ui.dock, 0, 0);
    make_chip(ui.chip_chain, ICON_CHAINS, C_CHAINS, ui.dock, 0, 0);
    ui.btn_gear = make_pill_button(ui.dock, 72, 72, C_TRACK, "", nullptr);
    make_chip(ui.chip_attach, ICON_ATTACH, C_ATTACH, ui.dock, 0, 0);
#else
    lv_obj_align(ui.lbl_state, LV_ALIGN_TOP_MID, 0, 4);
    ui.btn_gear = make_pill_button(ui.scr_main, 64, 64, C_TRACK, "", nullptr);
    lv_obj_align(ui.btn_gear, LV_ALIGN_CENTER, 0, L_GEAR_Y);
    // Chips for the active accessories, shown only while on. Kept inside the ring: the
    // outer edge is ~156 px from centre against the ring's 158 px.
    make_chip(ui.chip_ecc, ICON_ECCENTRIC, C_ECC, ui.scr_main, -L_CHIP_X, L_CHIP_Y);
    make_chip(ui.chip_chain, ICON_CHAINS, C_CHAINS, ui.scr_main, L_CHIP_X, L_CHIP_Y);
#endif
    lv_obj_add_event_cb(ui.btn_gear, on_gear_clicked, LV_EVENT_CLICKED, nullptr);
    ui.lbl_gear = make_label(ui.btn_gear, &font_icons_26, C_TEXT, ICON_SETTINGS);
    lv_obj_center(ui.lbl_gear);

#ifndef WATCH206
    // Weight presets at the sides, just below the weight's baseline. Outer edge ~151 px
    // from centre against the ring's 158 px. Not on the watch.
    load_presets();
    make_preset(0, -122, 30);
    make_preset(1, 122, 30);
    refresh_presets();
#endif

    // knob battery (bottom, inside the gap at the bottom of the ring)
    ui.lbl_kbat = make_label(ui.scr_main, &font_poppins_14, C_MUTED, "");
    lv_obj_align(ui.lbl_kbat, LV_ALIGN_BOTTOM_MID, 0, -6);

    // Created last so it sits above every other main-screen widget. Invisible; shown
    // only while loaded, when a tap anywhere unloads.
    ui.unload_catcher = lv_obj_create(ui.scr_main);
    lv_obj_remove_style_all(ui.unload_catcher);
    lv_obj_set_size(ui.unload_catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(ui.unload_catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui.unload_catcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui.unload_catcher, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ui.unload_catcher, on_center_clicked, LV_EVENT_CLICKED, nullptr);
}

// ---------------------------------------------------------------------------
// Adjust screen (chains / eccentric)
// ---------------------------------------------------------------------------
#ifdef WATCH206
void refresh_value();
#endif

void refresh_adjust()
{
#ifdef WATCH206
    refresh_value();
    return;
#endif
    bool chains = ui.screen == Screen::AdjustChains;
    lv_label_set_text(ui.lbl_adj_unit, accessory_unit());
    if (ui.screen == Screen::AdjustAttach) {
        const int base = ui.weight * (ui.st.twinned() ? 2 : 1);
        lv_label_set_text(ui.lbl_adj_unit, "lb");
        lv_label_set_text(ui.lbl_adj_title, "Attachment");
        lv_obj_set_style_text_color(ui.lbl_adj_title, C_ATTACH, 0);
        lv_obj_set_style_arc_color(ui.arc_adj, C_ATTACH, LV_PART_INDICATOR);
        lv_arc_set_mode(ui.arc_adj, LV_ARC_MODE_NORMAL);
        lv_arc_set_range(ui.arc_adj, 0, MAX_ATTACH_LB);
        lv_arc_set_value(ui.arc_adj, ui.attach);
        lv_label_set_text_fmt(ui.lbl_adj_val, "%d", ui.attach);
        lv_label_set_text_fmt(ui.lbl_adj_range, "%d lb Voltra  =  %d lb total", base, base + ui.attach);
    } else if (chains) {
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
    if (ui.screen == Screen::AdjustAttach) save_attach();
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

#ifndef WATCH206
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
#endif


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
        case ROW_ATTACH:
            ui.adj_return = Screen::Settings;
            show(Screen::AdjustAttach);
            refresh_adjust();
            break;
        case ROW_CLOSE:
        default:
            show(Screen::Main);
            refresh_main();
            break;
    }
}

#ifdef WATCH206
#define C_OFF lv_color_hex(0x64748b)

/** Header of a watch sub-screen: a round back button, then the title beside it. */
lv_obj_t *make_header(lv_obj_t *scr, const char *title, lv_color_t colour, lv_event_cb_t back_cb, intptr_t back_data)
{
    lv_obj_t *lbl = nullptr;
    lv_obj_t *back = make_pill_button(scr, 44, 44, C_CARD, LV_SYMBOL_LEFT, &lbl);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_bg_color(back, C_TRACK, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_font(lbl, &font_poppins_20, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 28, 30);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_set_user_data(back, (void *)back_data);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *t = make_label(scr, &font_poppins_22, colour, title);
    lv_obj_align_to(t, back, LV_ALIGN_OUT_RIGHT_MID, 14, 1);
    return t;
}

/** One row of the settings list: the accessory's icon and name, its value on the right. */
void make_bubble(Ui::Bubble &b, lv_obj_t *list, const char *icon, lv_color_t colour, const char *title, intptr_t row)
{
    b.btn = lv_btn_create(list);
    lv_obj_remove_style_all(b.btn);
    lv_obj_set_size(b.btn, LV_PCT(100), 64);
    lv_obj_set_style_radius(b.btn, 20, 0);
    lv_obj_set_style_bg_color(b.btn, C_CARD, 0);
    lv_obj_set_style_bg_opa(b.btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b.btn, C_TRACK, LV_STATE_PRESSED);
    lv_obj_add_flag(b.btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b.btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(b.btn, (void *)row);
    lv_obj_add_event_cb(b.btn, on_settings_row, LV_EVENT_CLICKED, nullptr);
    b.icon = make_label(b.btn, &font_icons_26, colour, icon);
    lv_obj_align(b.icon, LV_ALIGN_LEFT_MID, 18, 0);
    b.title = make_label(b.btn, &font_poppins_18, C_TEXT, title);
    lv_obj_align(b.title, LV_ALIGN_LEFT_MID, 60, 1);
    b.value = make_label(b.btn, &font_poppins_18, C_OFF, "Off");
    lv_obj_align(b.value, LV_ALIGN_RIGHT_MID, -18, 1);
}

/** On: tinted with the accessory's colour and the value in it. Off: plain, "Off" muted. */
void set_bubble(Ui::Bubble &b, const char *value, bool active, lv_color_t colour)
{
    lv_label_set_text(b.value, value);
    lv_obj_set_style_text_color(b.value, active ? colour : C_OFF, 0);
    lv_obj_set_style_bg_color(b.btn, active ? colour : C_CARD, 0);
    lv_obj_set_style_bg_opa(b.btn, active ? LV_OPA_20 : LV_OPA_COVER, 0);
}
constexpr const char *SETTING_OFF = "Off";
#else
constexpr const char *SETTING_OFF = "OFF";

/** One round accessory button: the accessory's icon, a small title, and its current value. */
constexpr int BUBBLE_SIZE = 88;

void make_bubble(Ui::Bubble &b, const char *icon, const char *title, intptr_t row, int x, int y)
{
    b.btn = make_pill_button(ui.scr_settings, BUBBLE_SIZE, BUBBLE_SIZE, C_TRACK, "", nullptr);
    lv_obj_align(b.btn, LV_ALIGN_CENTER, x, y);
    lv_obj_set_user_data(b.btn, (void *)row);
    lv_obj_add_event_cb(b.btn, on_settings_row, LV_EVENT_CLICKED, nullptr);
    b.icon = make_label(b.btn, &font_icons_26, C_TEXT, icon);
    lv_obj_align(b.icon, LV_ALIGN_CENTER, 0, -21);
    b.title = make_label(b.btn, &font_poppins_14, C_MUTED, title);
    lv_obj_align(b.title, LV_ALIGN_CENTER, 0, 1);
    b.value = make_label(b.btn, &font_poppins_20, C_TEXT, "--");
    lv_obj_align(b.value, LV_ALIGN_CENTER, 0, 21);
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
#endif

void refresh_settings()
{
    char buf[16];

    if (ui.ecc != 0) format_amount(buf, sizeof(buf), ui.ecc, true);
    else snprintf(buf, sizeof(buf), "%s", SETTING_OFF);
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
        else snprintf(buf, sizeof(buf), "%s", SETTING_OFF);
        set_bubble(cb.b, buf, v > 0, style_colour(cb.s));
    }

    if (ui.attach > 0) snprintf(buf, sizeof(buf), "%d lb", ui.attach);
    else snprintf(buf, sizeof(buf), "%s", SETTING_OFF);
    set_bubble(ui.bub_attach, buf, ui.attach > 0, C_ATTACH);
}

void build_settings()
{
    ui.scr_settings = make_screen();
#ifdef WATCH206
    make_header(ui.scr_settings, "Settings", C_TEXT, on_settings_row, ROW_CLOSE);
    lv_obj_t *list = lv_obj_create(ui.scr_settings);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 366, LV_SIZE_CONTENT);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 86);
    make_bubble(ui.bub_attach,   list, ICON_ATTACH,    C_ATTACH, "Attachment",     ROW_ATTACH);
    make_bubble(ui.bub_ecc,      list, ICON_ECCENTRIC, C_ECC,    "Eccentric",      ROW_ECCENTRIC);
    make_bubble(ui.bub_chains,   list, ICON_CHAINS,    C_CHAINS, "Chains",         ROW_CHAINS);
    make_bubble(ui.bub_inverse,  list, ICON_INVERSE,   C_CHAINS, "Inverse chains", ROW_INVERSE);
    make_bubble(ui.bub_mountain, list, ICON_MOUNTAIN,  C_WARN,   "Mountain",       ROW_MOUNTAIN);
#else
    lv_obj_t *ring = make_ring(ui.scr_settings, C_TRACK);
    lv_arc_set_value(ring, 0);

    lv_obj_t *title = make_label(ui.scr_settings, &font_poppins_18, C_TEXT, "SETTINGS");
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    // Three over two: eccentric, chains, inverse chains / mountain, attachment. Kept inside
    // the ring: the farthest bubble edge is ~151 px from centre against the ring's 158 px.
    make_bubble(ui.bub_ecc,      ICON_ECCENTRIC, "Eccentric",  ROW_ECCENTRIC, -96, -42);
    make_bubble(ui.bub_chains,   ICON_CHAINS,    "Chains",     ROW_CHAINS,      0, -42);
    make_bubble(ui.bub_inverse,  ICON_INVERSE,   "Inverse",    ROW_INVERSE,    96, -42);
    make_bubble(ui.bub_mountain, ICON_MOUNTAIN,  "Mountain",   ROW_MOUNTAIN,  -48,  54);
    make_bubble(ui.bub_attach,   ICON_ATTACH,    "Attachment", ROW_ATTACH,     48,  54);

    // Done sits in the gap at the bottom of the ring.
    ui.btn_settings_done = make_action_button(ui.scr_settings, "DONE");
    lv_obj_set_user_data(ui.btn_settings_done, (void *)(intptr_t)ROW_CLOSE);
    lv_obj_add_event_cb(ui.btn_settings_done, on_settings_row, LV_EVENT_CLICKED, nullptr);
#endif
}

#ifdef WATCH206
// ---------------------------------------------------------------------------
// Adjust screen (watch): eccentric, chains (any style) or attachment. The amount inside
// a dial showing it (swipe to change, as everywhere), and quick picks. Eccentric and
// chains are in the Voltra's unit, pounds or percent.
// ---------------------------------------------------------------------------

void value_picks(int out[4])
{
    static const int attach[4] = {0, 10, 25, 50};
    static const int ecc_lb[4] = {0, 5, 10, 20}, ecc_pct[4] = {0, 10, 20, 40};
    static const int chains_lb[4] = {0, 10, 20, 30}, chains_pct[4] = {0, 25, 50, 100};
    const int *p = ui.screen == Screen::AdjustAttach ? attach
                 : ui.screen == Screen::AdjustEcc ? (lb_mode() ? ecc_lb : ecc_pct)
                 : (lb_mode() ? chains_lb : chains_pct);
    for (int i = 0; i < 4; i++) out[i] = p[i];
}

void refresh_value()
{
    const int v = adjust_value();
    int lo = 0, hi = MAX_ATTACH_LB;
    lv_color_t col = C_ATTACH;
    const char *title = "Attachment";
    const bool ecc = ui.screen == Screen::AdjustEcc;
    char unit[4] = "lb";
    if (ui.screen == Screen::AdjustAttach) {
        const int base = ui.weight * (ui.st.twinned() ? 2 : 1);
        lv_label_set_text_fmt(ui.lbl_val_info, "Total #" C_WHITE_HEX " %d lb#\nwith the Voltra's %d",
                              base + v, base);
    } else {
        snprintf(unit, sizeof(unit), "%s", accessory_unit());
        const int mx = ecc ? ecc_max() : chains_max();
        hi = std::max(mx, STEP_COARSE_LB);
        lo = ecc ? -hi : 0;
        col = ecc ? C_ECC : style_colour(ui.adj_style);
        title = ecc ? "Eccentric" : style_name(ui.adj_style);
        // the other unit, and the limit
        const char *fmt_lb = ecc ? "#" C_WHITE_HEX " %+d%%# of %d lb\nup to %d lb either way"
                                 : "#" C_WHITE_HEX " %d%%# of %d lb\nup to %d lb";
        const char *fmt_pct = ecc ? "#" C_WHITE_HEX " %+d lb# of %d lb\nup to %d%% either way"
                                  : "#" C_WHITE_HEX " %d lb# of %d lb\nup to %d%%";
        if (v == 0) {
            lv_label_set_text_fmt(ui.lbl_val_info, ecc ? "Off\nup to %d%s either way" : "Off\nup to %d%s", mx,
                                  lb_mode() ? " lb" : "%");
        } else if (lb_mode()) {
            lv_label_set_text_fmt(ui.lbl_val_info, fmt_lb, amount_pct(v), ui.weight, mx);
        } else {
            lv_label_set_text_fmt(ui.lbl_val_info, fmt_pct, pct_to_lb(v), ui.weight, mx);
        }
    }
    lv_label_set_text(ui.lbl_val_title, title);
    lv_obj_set_style_text_color(ui.lbl_val_title, col, 0);
    lv_label_set_text_fmt(ui.lbl_val_num, ecc && v != 0 ? "%+d" : "%d", v);
    lv_label_set_text(ui.lbl_val_unit, unit);

    lv_arc_set_mode(ui.dial_val, ecc ? LV_ARC_MODE_SYMMETRICAL : LV_ARC_MODE_NORMAL);
    lv_arc_set_range(ui.dial_val, lo, hi);
    lv_arc_set_value(ui.dial_val, clampi(v, lo, hi));
    lv_obj_set_style_arc_color(ui.dial_val, col, LV_PART_INDICATOR);

    int picks[4];
    value_picks(picks);
    const bool pct = ui.screen != Screen::AdjustAttach && !lb_mode();
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui.val_pick[i];
        lv_obj_t *lbl = lv_obj_get_child(b, 0);
        if (picks[i] == 0) lv_label_set_text(lbl, "Off");
        else lv_label_set_text_fmt(lbl, ecc ? (pct ? "+%d%%" : "+%d") : (pct ? "%d%%" : "%d"), picks[i]);
        lv_obj_set_user_data(b, (void *)(intptr_t)picks[i]);
        const bool on = picks[i] == v;
        // beyond what the weight allows: shown, but dimmed
        const bool reachable = picks[i] >= lo && picks[i] <= hi &&
                               (ui.screen == Screen::AdjustAttach || picks[i] <= (ecc ? ecc_max() : chains_max()));
        lv_obj_set_style_border_color(b, on ? col : C_TRACK, 0);
        lv_obj_set_style_bg_color(b, on ? col : C_CARD, 0);
        lv_obj_set_style_bg_color(b, col, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, on ? LV_OPA_20 : LV_OPA_COVER, 0);
        lv_obj_set_style_opa(b, reachable ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

void on_value_pick(lv_event_t *e)
{
    const int v = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    const int limit = ui.screen == Screen::AdjustAttach ? MAX_ATTACH_LB
                    : ui.screen == Screen::AdjustEcc ? ecc_max() : chains_max();
    if (v > limit) {
        haptics_buzz();   // more than this weight allows
        return;
    }
    set_adjust_value(v);
    haptics_click();
    refresh_value();
}

void build_value()
{
    ui.scr_value = make_screen();
    ui.lbl_val_title = make_header(ui.scr_value, "Attachment", C_ATTACH, on_adjust_done, 0);

    // The dial: 140 px to the middle of its 14 px line, with a white dot at the amount.
    ui.dial_val = make_dial(ui.scr_value, 294, 14, C_ATTACH);
    lv_obj_align(ui.dial_val, LV_ALIGN_CENTER, 0, -1);
    lv_obj_set_style_bg_color(ui.dial_val, C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ui.dial_val, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(ui.dial_val, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui.dial_val, 7, LV_PART_KNOB);

    lv_obj_t *row = make_value_row(ui.scr_value, &font_poppins_96, &ui.lbl_val_num, &ui.lbl_val_unit);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -26);
    ui.lbl_val_info = make_label(ui.scr_value, &font_poppins_16, C_MUTED, "");
    lv_label_set_recolor(ui.lbl_val_info, true);
    lv_obj_set_width(ui.lbl_val_info, 230);
    lv_obj_set_style_text_align(ui.lbl_val_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(ui.lbl_val_info, 4, 0);
    lv_obj_align(ui.lbl_val_info, LV_ALIGN_CENTER, 0, 62);

    lv_obj_t *picks = lv_obj_create(ui.scr_value);
    lv_obj_remove_style_all(picks);
    // Narrower than the screen and lifted: the panel's bottom corners are well rounded.
    lv_obj_set_size(picks, 320, 48);
    lv_obj_clear_flag(picks, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(picks, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(picks, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(picks, LV_ALIGN_BOTTOM_MID, 0, -36);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *lbl = nullptr;
        ui.val_pick[i] = make_pill_button(picks, 74, 48, C_TRACK, "", &lbl);
        lv_obj_set_style_text_font(lbl, &font_poppins_16, 0);
        lv_obj_add_event_cb(ui.val_pick[i], on_value_pick, LV_EVENT_CLICKED, nullptr);
    }
}
#endif

// ---------------------------------------------------------------------------
// Connect screen
// ---------------------------------------------------------------------------
// Connect-list rows: a device index, or one of these.
constexpr intptr_t ROW_UNTWIN = -2;
constexpr intptr_t ROW_PAIR = -3;
constexpr intptr_t ROW_DISCONNECT_BASE = -10;   // - slot: disconnect that one
constexpr intptr_t ROW_TWIN_BASE = 1000;   // + device index: twin with that device

/** The slot a newly picked Voltra goes into: the one on screen if free, else the other. */
int free_slot()
{
    if (!slot_busy(ui.st)) return ui.active;
    if (!slot_busy(ui.st_other) && !vc_other().paused()) return 1 - ui.active;
    return -1;
}

void on_device_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    intptr_t idx = (intptr_t)lv_obj_get_user_data(btn);
    haptics_click();
    if (idx == ROW_UNTWIN) {
        vc().untwin();
        return;
    }
    if (idx == ROW_PAIR) {
        // The Voltra on screen hosts. A Voltra will not join a twin while we are connected
        // to both, so let go of the other first; manage_twin() then twins them the way
        // "Twin with" does, over a short connection of its own. Stay here: the status
        // follows the pairing.
        vc_other().pause();
        ui.twin_host = ui.active;
        ui.pair_pending = true;
        ui.pair_until = millis() + PAIR_WINDOW_MS;
        return;
    }
    if (idx <= ROW_DISCONNECT_BASE) {
        const int slot = (int)(ROW_DISCONNECT_BASE - idx);
        VClient &c = VClient::instance(slot);
        c.disconnect();
        if (slot != 0) c.forgetSavedDevice();   // a second Voltra is only kept while wanted
        if (slot == ui.active && slot_busy(ui.st_other)) switch_to(1 - slot);
        scanner().scanStart();
        return;
    }
    if (idx >= ROW_TWIN_BASE) {
        // Stay on this screen: its status line follows the twin being set up.
        const size_t i = (size_t)(idx - ROW_TWIN_BASE);
        if (i < ui.devs.size()) {
            vc().twinWith(ui.devs[i]);
            ui.twin_host = ui.active;   // the one on screen hosts
            ui.pair_until = millis() + PAIR_WINDOW_MS;
        }
        return;
    }
    int slot = free_slot();
    if (slot >= 0 && idx >= 0 && (size_t)idx < ui.devs.size()) {
        // A Voltra the other slot already knows goes back there, never into both.
        const int other = 1 - slot;
        if (!slot_busy(slot_state(other)) &&
            strcasecmp(VClient::instance(other).savedAddress().c_str(), ui.devs[idx].address.c_str()) == 0) {
            slot = other;
        }
        scanner().scanStop();
        VClient::instance(slot).connectTo(ui.devs[idx]);
        show(Screen::Main);
        refresh_main();
    }
}

void on_close_clicked(lv_event_t *)
{
    haptics_click();
    scanner().scanStop();
    show(Screen::Main);
    refresh_main();
}

/**
 * One row of the connect list. On the watch a full-width card like the settings rows,
 * with larger text; the label takes recolour markup (the signal strength, muted).
 */
lv_obj_t *add_connect_row(const char *icon, const char *text, lv_color_t colour, intptr_t data)
{
    lv_obj_t *b = lv_list_add_btn(ui.list, icon, text);
#ifdef WATCH206
    lv_obj_set_height(b, 60);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C_TRACK, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_pad_column(b, 14, 0);
    lv_obj_set_style_text_font(b, &font_poppins_18, 0);
    lv_obj_t *lbl = lv_obj_get_child(b, lv_obj_get_child_cnt(b) - 1);
    lv_label_set_recolor(lbl, true);
    lv_label_set_text(lbl, text);   // again, now that markup is on
#endif
    lv_obj_set_user_data(b, (void *)data);
    lv_obj_set_style_text_color(b, colour, 0);
    lv_obj_add_event_cb(b, on_device_clicked, LV_EVENT_CLICKED, nullptr);
    return b;
}

void rebuild_device_list()
{
    lv_obj_clean(ui.list);
    const DeviceState &st = ui.st;
    // A disconnect row for each Voltra in use, in slot order.
    for (int slot = 0; slot < VClient::COUNT; slot++) {
        const DeviceState &s = slot_state(slot);
        if (!slot_busy(s)) continue;
        char buf[48];
        snprintf(buf, sizeof(buf), "Disconnect %s", s.device_name[0] ? s.device_name : "Voltra");
        add_connect_row(LV_SYMBOL_CLOSE, buf, C_DANGER, ROW_DISCONNECT_BASE - slot);
    }
    // Twin mode: offered only on a ready connection, which becomes the host.
    const bool ready = st.conn == ConnState::Ready;
    const bool other_ready = ui.st_other.conn == ConnState::Ready;
    if (ready && st.twin_state > voltra::TWIN_STATE_ALONE) {
        add_connect_row(LV_SYMBOL_LOOP, "Un-twin", C_WARN, ROW_UNTWIN);
    }
    // Both connected and neither twinned: pair them, the one on screen hosting.
    if (ready && other_ready && st.twin_state <= voltra::TWIN_STATE_ALONE &&
        ui.st_other.twin_state <= voltra::TWIN_STATE_ALONE) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Pair %s + %s", st.device_name, ui.st_other.device_name);
        add_connect_row(LV_SYMBOL_LOOP, buf, C_LOADED, ROW_PAIR);
    }
    // Voltras in range, while there is a slot for one.
    const bool room = free_slot() >= 0;
    for (size_t i = 0; room && i < ui.devs.size(); i++) {
        const FoundDevice &d = ui.devs[i];
        if (slot_busy(st) && strcasecmp(d.address.c_str(), st.address) == 0) continue;
        if (slot_busy(ui.st_other) && strcasecmp(d.address.c_str(), ui.st_other.address) == 0) continue;
        char buf[64];
#ifdef WATCH206
        snprintf(buf, sizeof(buf), "%s   #64748b %d dBm#", d.name.c_str(), d.rssi);
#else
        snprintf(buf, sizeof(buf), "%s  (%d dBm)", d.name.c_str(), d.rssi);
#endif
        add_connect_row(LV_SYMBOL_BLUETOOTH, buf, C_TEXT, (intptr_t)i);
        // Unknown (no twin status yet) counts as alone: the join works without it.
        if (ready && st.twin_state <= voltra::TWIN_STATE_ALONE) {
            snprintf(buf, sizeof(buf), "Twin with %s", d.name.c_str());
            add_connect_row(LV_SYMBOL_LOOP, buf, C_LOADED, ROW_TWIN_BASE + (intptr_t)i);
        }
    }
    if (ui.devs.empty() && !st.connected()) {
        lv_obj_t *t = lv_list_add_text(ui.list, "No Voltra found yet.\nSwitch the Voltra on.");
        lv_obj_set_style_text_color(t, C_MUTED, 0);
#ifdef WATCH206
        lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_font(t, &font_poppins_18, 0);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(t, LV_PCT(100));
        lv_obj_set_style_pad_top(t, 40, 0);
#endif
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
#ifdef WATCH206
    // Header, a status line with the scan spinner, then the rows filling the screen.
    make_header(ui.scr_connect, "Connect", C_TEXT, on_close_clicked, 0);
    ui.spinner = lv_spinner_create(ui.scr_connect, 1200, 60);
    lv_obj_set_size(ui.spinner, 22, 22);
    lv_obj_set_style_arc_width(ui.spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.spinner, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.spinner, C_UNLOADED, LV_PART_INDICATOR);
    lv_obj_align(ui.spinner, LV_ALIGN_TOP_LEFT, 36, 90);
    ui.lbl_conn_status = make_label(ui.scr_connect, &font_poppins_16, C_MUTED, "");
    lv_label_set_long_mode(ui.lbl_conn_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui.lbl_conn_status, 300);
    lv_obj_align(ui.lbl_conn_status, LV_ALIGN_TOP_LEFT, 70, 90);

    ui.list = lv_list_create(ui.scr_connect);
    lv_obj_set_size(ui.list, 366, 306);   // ends clear of the rounded bottom corners; scrolls
    lv_obj_align(ui.list, LV_ALIGN_TOP_MID, 0, 126);
    lv_obj_set_style_bg_opa(ui.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.list, 0, 0);
    lv_obj_set_style_radius(ui.list, 0, 0);
    lv_obj_set_style_pad_all(ui.list, 0, 0);
    lv_obj_set_style_pad_row(ui.list, 8, 0);
    lv_obj_set_scrollbar_mode(ui.list, LV_SCROLLBAR_MODE_OFF);
#else
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
#endif
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
        case Screen::AdjustChains:
        case Screen::AdjustEcc:
        case Screen::AdjustAttach:
            if (set_adjust_value(apply_step(adjust_value(), delta, step))) {
                haptics_tick();
                refresh_adjust();
            }
            break;
        case Screen::Settings:
            break;   // all the bubbles are visible at once; selection is by touch
        case Screen::Connect:
            lv_obj_scroll_by(ui.list, 0, -delta * 44, LV_ANIM_ON);
            break;
    }
}

void demo_off();

/**
 * Whether slot i hosts a twin. Both units of a twin report being twinned (or joining), so
 * the host is the one we set the twin up from; failing that, the one whose fitness mode
 * has taken the twinned host's shape (0x01xx / 0x20xx, docs/PROTOCOL.md).
 */
bool hosting(int i)
{
    const DeviceState &s = slot_state(i);
    if (!s.twinned() && s.twin_state != voltra::TWIN_STATE_JOINING) return false;
    if (ui.twin_host >= 0) return ui.twin_host == i;
    return s.fitness_mode >= 0 && (s.fitness_mode & 0xFF00) != 0;
}

/**
 * Two Voltras and twin mode. While one hosts a twin, the other follows it and takes its
 * commands through the host, so let go of our own connection to it (it is not forgotten)
 * and keep the host on screen. Once the host is on its own again, reconnect to the other.
 */
void manage_twin(uint32_t now)
{
    const bool pairing = (int32_t)(now - ui.pair_until) < 0;
    for (int i = 0; i < VClient::COUNT; i++) {
        const DeviceState &hs = slot_state(i);
        VClient &follower = VClient::instance(1 - i);
        if (hosting(i)) {
            if (pairing && hs.twinned() && ui.screen == Screen::Connect) {
                // Paired: off the connect screen, so the scan stops taking radio time.
                ui.pair_until = 0;
                scanner().scanStop();
                show(Screen::Main);
                refresh_main();
            }
            ui.twin_host = i;
            const std::string addr = follower.savedAddress();
            const bool is_peer = !hs.twin_peer[0] || addr.empty() || strcasecmp(addr.c_str(), hs.twin_peer) == 0;
            if (is_peer && !follower.paused() && slot_busy(slot_state(1 - i))) follower.pause();
            if (ui.active != i) {
                switch_to(i);
                if (ui.screen == Screen::Main) refresh_main();
            }
        } else if (ui.twin_host == i && !pairing && hs.conn == ConnState::Ready &&
                   hs.twin_state == voltra::TWIN_STATE_ALONE) {
            // un-twinned (or the pairing did not take): back to two Voltras
            ui.twin_host = -1;
            if (follower.paused()) follower.resume();
        }
    }
    // Pairing: once the other is let go, the host asks it to join.
    if (ui.pair_pending && ui.twin_host >= 0 && !slot_busy(slot_state(1 - ui.twin_host))) {
        ui.pair_pending = false;
        const FoundDevice dev = VClient::instance(1 - ui.twin_host).savedDevice();
        if (!dev.address.empty()) VClient::instance(ui.twin_host).twinWith(dev);
    }
    for (int i = 0; i < VClient::COUNT; i++) VClient::instance(i).setTwinHost(ui.twin_host == i);
    // While pairing, ask the host how it is going rather than wait for its next poll.
    if (pairing && now - ui.last_pair_poll > 3000) {
        VClient::instance(ui.twin_host >= 0 ? ui.twin_host : ui.active).pollTwin();
        ui.last_pair_poll = now;
    }
}

void poll_cb(lv_timer_t *)
{
    VClient &c = vc();
    uint32_t now = millis();

    if (ui.demo && now - ui.last_cmd_ms > DEMO_TIMEOUT_MS) {
        demo_off();
        show(Screen::Main);
    }

    int delta = knob_take_delta();
    if (delta) {
        ui.last_input_ms = now;
        handle_knob(delta);
    }

    flush_pending(now);

    bool state_changed = c.version() != ui.last_state_version;
    bool devs_changed = scanner().devicesVersion() != ui.last_dev_version;
    VClient &o = vc_other();
    const bool other_changed = o.version() != ui.last_other_version;
    if (other_changed) {
        ui.last_other_version = o.version();
        ui.st_other = o.state();
    }

    if (state_changed) {
        ui.last_state_version = c.version();
        ui.st = ui.demo ? ui.demo_st : c.state();
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
        if (!ui.st.loaded()) ui.ecc_learned = false;
        else if (ui.last_rep_phase == 3 && ui.st.rep_phase != 3) ui.ecc_learned = true;
        ui.last_rep_phase = ui.st.rep_phase;
        if (ui.demo && ui.st.reps > 0) ui.ecc_learned = true;
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
    if (ui.st.load_refused != ui.last_load_refused) {
        ui.last_load_refused = ui.st.load_refused;
        ui.toggle_pending_until = 0;
        ui.load_refused_until = now + LOAD_REFUSED_SHOW_MS;
        haptics_buzz();
        if (ui.screen == Screen::Main) refresh_main();
    }
    if (ui.load_refused_until && now >= ui.load_refused_until) {
        ui.load_refused_until = 0;
        if (ui.screen == Screen::Main) refresh_main();
    }
    bool pending_expired = ui.toggle_pending_until && now >= ui.toggle_pending_until;
    if (pending_expired) ui.toggle_pending_until = 0;

    if (devs_changed) {
        ui.last_dev_version = scanner().devicesVersion();
        ui.devs = scanner().devices();
    }
    if (!ui.demo) manage_twin(now);

    switch (ui.screen) {
        case Screen::Main:
            if (state_changed || other_changed || pending_expired) refresh_main();
            break;
        case Screen::Settings:
            if (state_changed) refresh_settings();
            break;
        case Screen::AdjustChains:
        case Screen::AdjustEcc:
        case Screen::AdjustAttach:
            if (state_changed) refresh_adjust();
            break;
        case Screen::Connect:
            if (state_changed || other_changed || devs_changed) {
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

    // Power off when left alone, but never with the weight on, nor while plugged in.
    if (ui.st.loaded() || ui.st.auto_loading() || ui.st_other.loaded()) ui.last_input_ms = now;
    const uint32_t idle = std::min<uint32_t>(now - ui.last_input_ms, lv_disp_get_inactive_time(nullptr));
    if (idle >= POWER_OFF_IDLE_MS && on_external_power()) {
        ui.last_input_ms = now;   // look again in another 10 minutes
        lv_disp_trig_activity(nullptr);
    } else if (idle >= POWER_OFF_IDLE_MS) {
        if (ui.screen == Screen::AdjustAttach) save_attach();
        haptics_buzz();
        delay(300);   // let the buzz play
        power_off();
    }
}

// ---------------------------------------------------------------------------
// Serial commands: screenshots and stand-in states (tools/screenshot.py)
// ---------------------------------------------------------------------------
void send_screenshot()
{
    lv_obj_t *scr = lv_scr_act();
    const uint32_t size = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
    void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_img_dsc_t dsc;
    if (!buf || lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &dsc, buf, size) != LV_RES_OK) {
        Serial.print("\nSHOT ERR\n");
        if (buf) heap_caps_free(buf);
        return;
    }
    Serial.printf("\nSHOT %u %u %d %u\n", (unsigned)dsc.header.w, (unsigned)dsc.header.h,
                  LV_COLOR_16_SWAP, (unsigned)dsc.data_size);
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    for (uint32_t off = 0; off < dsc.data_size; off += 4096) {
        Serial.write(p + off, std::min<uint32_t>(4096, dsc.data_size - off));
    }
    Serial.print("\nEND\n");
    Serial.flush();
    heap_caps_free(buf);
}

/** A connected, activated Voltra at 45 lb in pound mode, nothing on. */
DeviceState demo_base()
{
    DeviceState s;
    s.conn = ConnState::Ready;
    strncpy(s.device_name, "VTR-002166", sizeof(s.device_name) - 1);
    s.protocol_ok = true;
    s.battery = 82;
    s.weight = 45;
    s.chains = 0;
    s.eccentric_known = true;
    s.inverse_chains = 0;
    s.mountain = 0;
    s.accessory_display = 0;
    s.fitness_mode = voltra::FITNESS_MODE_STRENGTH_READY;
    s.workout_status = voltra::WORKOUT_STATUS_UNLOADED;
    s.activation = 1;
    s.twin_state = voltra::TWIN_STATE_ALONE;
    strncpy(s.status, "Connected", sizeof(s.status) - 1);
    return s;
}

void demo_loaded(DeviceState &s, int workout_status, uint8_t phase, uint16_t reps)
{
    s.fitness_mode = voltra::FITNESS_MODE_STRENGTH_LOADED;
    s.workout_status = workout_status;
    s.rep_phase = phase;
    s.reps = reps;
    s.sets = 2;
    s.force_known = true;
    s.force_lb = 45;
}

/** Back to the real Voltra, its scan results and the saved attachment weight. */
void demo_off()
{
    ui.demo = false;
    ui.last_dev_version = 0xFFFFFFFF;
    ui.last_state_version = 0xFFFFFFFF;
    load_attach();   // undo any "set attach"
}

bool run_command(const char *cmd)
{
    char verb[12] = {0}, arg[12] = {0};
    int n = 0;
    const int got = sscanf(cmd, "%11s %11s %d", verb, arg, &n);
    if (got < 1) return false;

    if (!strcmp(verb, "ping")) return true;   // answers once the UI is up
    if (!strcmp(verb, "shot")) {
        lv_refr_now(nullptr);
        send_screenshot();
        return true;
    }
    if (!strcmp(verb, "show") && got >= 2) {
        const struct { const char *name; Screen s; } screens[] = {
            {"main", Screen::Main}, {"settings", Screen::Settings}, {"ecc", Screen::AdjustEcc},
            {"chains", Screen::AdjustChains}, {"attach", Screen::AdjustAttach}, {"connect", Screen::Connect},
        };
        for (const auto &e : screens) {
            if (strcmp(arg, e.name)) continue;
            ui.adj_style = ui.style;
            ui.adj_return = Screen::Main;
            // no fade, so a screenshot straight after shows the new screen
            ui.screen = e.s;
            lv_scr_load(screen_obj(e.s));
            if (e.s == Screen::Main) refresh_main();
            else if (e.s == Screen::Settings) refresh_settings();
            else if (e.s == Screen::Connect) { rebuild_device_list(); refresh_connect(); }
            else refresh_adjust();
            return true;
        }
        return false;
    }
    if (!strcmp(verb, "demo") && got >= 2 && !strcmp(arg, "devs")) {
        // Two Voltras in range, for the connect screen (until the next scan result).
        ui.devs.clear();
        const char *names[2] = {"VTR-066162", "VTR-104377"};
        const int rssi[2] = {-58, -74};
        for (int i = 0; i < 2; i++) {
            FoundDevice d;
            d.name = names[i];
            d.address = i ? "d4:3c:11:20:9a:05" : "80:b5:4e:07:02:a7";
            d.rssi = rssi[i];
            ui.devs.push_back(d);
        }
        if (ui.screen == Screen::Connect) rebuild_device_list();
        return true;
    }
    if (!strcmp(verb, "demo") && got >= 2) {
        DeviceState s = demo_base();
        if (!strcmp(arg, "off")) {
            demo_off();
        } else if (!strcmp(arg, "idle")) {
            ui.demo = true;
        } else if (!strcmp(arg, "loaded")) {
            demo_loaded(s, voltra::WORKOUT_STATUS_RESTING, 0, 0);
            s.sets = 0;
            ui.demo = true;
        } else if (!strcmp(arg, "set")) {
            demo_loaded(s, voltra::WORKOUT_STATUS_ACTIVE, 1, 7);
            ui.demo = true;
        } else if (!strcmp(arg, "ecc")) {
            demo_loaded(s, voltra::WORKOUT_STATUS_ACTIVE, 3, 7);
            s.eccentric_lb = 15;
            ui.demo = true;
        } else if (!strcmp(arg, "twin")) {
            s.twin_state = voltra::TWIN_STATE_TWINNED;
            s.twin_peer_battery = 64;
            ui.demo = true;
        } else {
            return false;
        }
        // keep settings changed with "set" across demo states
        if (ui.demo_st.conn == ConnState::Ready) {
            if (strcmp(arg, "ecc")) s.eccentric_lb = ui.demo_st.eccentric_lb;
            s.chains_lb = ui.demo_st.chains_lb;
            s.weight = ui.demo_st.weight;
        }
        ui.demo_st = s;
        ui.w_dirty = ui.c_dirty = ui.e_dirty = false;
        ui.editing_until = 0;
        ui.last_state_version = 0xFFFFFFFF;   // picked up on the next poll
        return true;
    }
    if (!strcmp(verb, "set") && got == 3) {
        DeviceState &s = ui.demo_st;
        if (s.conn != ConnState::Ready) s = demo_base();
        if (!strcmp(arg, "attach")) ui.attach = clampi(n, 0, MAX_ATTACH_LB);
        else if (!strcmp(arg, "ecc")) s.eccentric_lb = n;
        else if (!strcmp(arg, "chains")) s.chains_lb = n;
        else if (!strcmp(arg, "weight")) s.weight = n;
        else if (!strcmp(arg, "reps")) s.reps = (uint16_t)n;
        else return false;
        ui.last_state_version = 0xFFFFFFFF;
        return true;
    }
    return false;
}

}  // namespace

bool ui_command(const char *cmd)
{
    LvLock lock;
    ui.last_input_ms = millis();   // someone is at the other end: stay on
    ui.last_cmd_ms = millis();
    const bool ok = run_command(cmd);
    if (!ok) Serial.printf("\nERR %s\n", cmd);
    else if (strncmp(cmd, "shot", 4)) Serial.print("\nOK\n");
    return ok;
}

void ui_init()
{
    build_main();
    build_settings();
#ifdef WATCH206
    build_value();
#else
    build_adjust();
#endif
    build_connect();
    lv_scr_load(ui.scr_main);
    load_attach();
    ui.last_input_ms = millis();
    ui.st = vc().state();
    ui.last_load_refused = ui.st.load_refused;
    refresh_main();
    lv_timer_create(poll_cb, 25, nullptr);
}
