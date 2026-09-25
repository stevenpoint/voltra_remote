/*
 * Host-side tests for the Voltra protocol layer.
 * Expected byte strings are frames captured from the official Beyond+ app
 * (jpamorgan/voltra-sdk, dylanmaniatakes/Beyond-Power-HomeAssistant).
 *
 *   pio test -e native
 */
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "ui/knob_step.h"
#include "voltra/voltra_protocol.h"

using namespace voltra;

void setUp() {}
void tearDown() {}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hex2bin(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; hex[0] && hex[1] && n < cap; hex += 2) {
        int hi = nibble(hex[0]);
        int lo = nibble(hex[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static void expect_frame(const char *expected_hex, const uint8_t *got, size_t got_len)
{
    uint8_t exp[128];
    size_t n = hex2bin(expected_hex, exp, sizeof(exp));
    TEST_ASSERT_EQUAL_UINT32(n, got_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, got, n);
}

void test_weight_frames_match_captures()
{
    uint8_t buf[64];
    size_t n = build_param_write_u16(buf, sizeof(buf), PARAM_BP_BASE_WEIGHT, 5, 0x0015);
    expect_frame("55130403aa1015002000110100863e0500cb3f", buf, n);
    n = build_param_write_u16(buf, sizeof(buf), PARAM_BP_BASE_WEIGHT, 100, 0x0020);
    expect_frame("55130403aa1020002000110100863e64004ddb", buf, n);
    n = build_param_write_u16(buf, sizeof(buf), PARAM_BP_BASE_WEIGHT, 200, 0x0048);
    expect_frame("55130403aa1048002000110100863ec800374e", buf, n);
}

/** The auto-load trigger is a well-formed vendor frame (cmd 0xAA, sub-command 0x12). */
void test_direct_load_trigger_frame()
{
    uint8_t buf[32];
    const uint8_t trigger[] = {VENDOR_DIRECT_LOAD};
    const size_t n = build_vendor(buf, sizeof(buf), trigger, sizeof(trigger), 0x0042);
    TEST_ASSERT_EQUAL(FRAME_OVERHEAD + 1, n);
    Packet pkt;
    TEST_ASSERT_TRUE(parse_packet(buf, n, pkt));
    TEST_ASSERT_TRUE(pkt.crc_ok);
    TEST_ASSERT_EQUAL_HEX8(0xAA, pkt.cmd);
    TEST_ASSERT_EQUAL(1, pkt.payload_len);
    TEST_ASSERT_EQUAL_HEX8(0x12, pkt.payload[0]);
}

/** Workout status pushes captured across a load, a set, rest and an unload. */
void test_workout_status_frames()
{
    static const struct { const char *hex; int status; } cases[] = {
        {"8025010002640000001e0001000000a80000000000000005", WORKOUT_STATUS_ACTIVE},
        {"8025010003640000001e0001000000a80000000000000005", WORKOUT_STATUS_RESTING},
        {"8025010004640000001e0001000000a80000000000000005", WORKOUT_STATUS_UNLOADING},
        {"812b030c0001000000b20269006f029ffd1d0000", -1},   // rep telemetry, not status
    };
    for (const auto &c : cases) {
        uint8_t payload[32];
        size_t n = strlen(c.hex) / 2;
        for (size_t i = 0; i < n; i++) {
            unsigned b;
            sscanf(c.hex + 2 * i, "%2x", &b);
            payload[i] = (uint8_t)b;
        }
        Packet pkt = {};
        pkt.cmd = CMD_TELEMETRY;
        pkt.payload = payload;
        pkt.payload_len = n;
        TEST_ASSERT_EQUAL(c.status, parse_workout_status(pkt));
    }
}

/** Auto load sits in mode 0x23 both while waiting and once loaded (captured). */
void test_auto_load_phases()
{
    TEST_ASSERT_TRUE(voltra_auto_loading(0x23, 11));    // waiting for the pull
    TEST_ASSERT_TRUE(voltra_auto_loading(0x23, 12));    // counting down
    TEST_ASSERT_FALSE(voltra_loaded(0x23, 12));
    TEST_ASSERT_TRUE(voltra_loaded(0x23, 13));          // countdown done
    TEST_ASSERT_TRUE(voltra_loaded(0x23, 14));          // loaded
    TEST_ASSERT_FALSE(voltra_auto_loading(0x23, 14));
    TEST_ASSERT_TRUE(voltra_auto_loading(0x23, -1));    // status not read yet
    TEST_ASSERT_TRUE(voltra_loaded(1, -1));             // plain load is unaffected
    TEST_ASSERT_FALSE(voltra_loaded(0, 14));
    TEST_ASSERT_FALSE(voltra_auto_loading(5, 11));
}

/** Resting between sets reports 1, which still has the weight on (captured). */
void test_fitness_mode_loaded_includes_idle()
{
    TEST_ASSERT_FALSE(fitness_mode_loaded(-1));   // unknown
    TEST_ASSERT_FALSE(fitness_mode_loaded(0));    // unloaded
    TEST_ASSERT_TRUE(fitness_mode_loaded(1));     // idle / resting
    TEST_ASSERT_FALSE(fitness_mode_loaded(4));    // the unload command value
    TEST_ASSERT_TRUE(fitness_mode_loaded(5));     // set active
}

void test_chains_frame_matches_capture()
{
    uint8_t buf[64];
    size_t n = build_param_write_u16(buf, sizeof(buf), PARAM_BP_CHAINS_WEIGHT, 0, 0x0ad3);
    expect_frame("55130403aa10d30a2000110100873e0000c6c6", buf, n);
    n = build_param_write_u16(buf, sizeof(buf), PARAM_BP_CHAINS_WEIGHT, 1, 0x0a6e);
    expect_frame("55130403aa106e0a2000110100873e0100a469", buf, n);
}

void test_load_unload_frames_match_captures()
{
    uint8_t buf[64];
    size_t n = build_param_write_u16(buf, sizeof(buf), PARAM_BP_SET_FITNESS_MODE, FITNESS_MODE_STRENGTH_LOADED, 0x0016);
    expect_frame("55130403aa1016002000110100893e05008173", buf, n);
    n = build_param_write_u16(buf, sizeof(buf), PARAM_BP_SET_FITNESS_MODE, FITNESS_MODE_STRENGTH_READY, 0x001b);
    expect_frame("55130403aa101b002000110100893e040037dd", buf, n);
    n = build_param_write_u8(buf, sizeof(buf), PARAM_FITNESS_WORKOUT_STATE, WORKOUT_STATE_ACTIVE, 0x0014);
    expect_frame("551204c7aa1014002000110100b04f01db10", buf, n);
}

void test_param_read_frame_matches_capture()
{
    uint8_t buf[64];
    const uint16_t ids[] = {PARAM_MC_DEFAULT_OFFLEN_CM, PARAM_BP_RUNTIME_POSITION_CM};
    size_t n = build_param_read(buf, sizeof(buf), ids, 2, 0x0015);
    expect_frame("55130403aa10150020000f02006a50823e8f2f", buf, n);
}

void test_app_hello_matches_capture()
{
    uint8_t buf[64];
    size_t n = build_app_hello(buf, sizeof(buf), "iPad");
    expect_frame("552904c90110000020004f69506164000000000000000000000000000000000084ab1a5f292001ea4f", buf, n);
    n = build_app_hello(buf, sizeof(buf), "iPhone");
    expect_frame("552904c90110000020004f6950686f6e6500000000000000000000000000000084ab1a5f292001" "72d8", buf, n);
}

void test_bootstrap_constants_have_valid_crcs()
{
    for (size_t i = 0; i < BOOTSTRAP_FRAME_COUNT; i++) {
        const BootFrame &f = BOOTSTRAP_FRAMES[i];
        TEST_ASSERT_EQUAL_UINT8(f.data[1], f.len);
        TEST_ASSERT_EQUAL_HEX8(f.data[3], crc8(f.data, 3));
        uint16_t crc = crc16(f.data, f.len - 2);
        TEST_ASSERT_EQUAL_HEX8(crc & 0xFF, f.data[f.len - 2]);
        TEST_ASSERT_EQUAL_HEX8(crc >> 8, f.data[f.len - 1]);
    }
}

void test_eccentric_frame_round_trips()
{
    uint8_t buf[64];
    size_t n = build_param_write_i16(buf, sizeof(buf), PARAM_BP_ECCENTRIC_WEIGHT, -25, 0x0102);
    TEST_ASSERT_EQUAL_UINT32(19, n);
    Packet p;
    TEST_ASSERT_TRUE(parse_packet(buf, n, p));
    TEST_ASSERT_TRUE(p.crc_ok);
    TEST_ASSERT_EQUAL_HEX8(CMD_PARAM_WRITE, p.cmd);
    TEST_ASSERT_EQUAL_HEX16(0x0102, p.seq);
    TEST_ASSERT_EQUAL_HEX8(0xe7, p.payload[4]);   // -25 = 0xffe7 LE
    TEST_ASSERT_EQUAL_HEX8(0xff, p.payload[5]);
}

void test_parse_async_state_push()
{
    // Device -> app push of chains = 0 (captured as "step 2" in the SDK tables)
    uint8_t buf[64];
    size_t n = hex2bin("5513040310aa13152000100100873e00009cee", buf, sizeof(buf));
    Packet p;
    TEST_ASSERT_TRUE(parse_packet(buf, n, p));
    TEST_ASSERT_TRUE(p.crc_ok);
    TEST_ASSERT_EQUAL_HEX8(CMD_ASYNC_STATE, p.cmd);
    TEST_ASSERT_EQUAL_HEX8(0x10, p.sender);
    ParamValue vals[4];
    size_t cnt = decode_params(p, vals, 4);
    TEST_ASSERT_EQUAL_UINT32(1, cnt);
    TEST_ASSERT_EQUAL_HEX16(PARAM_BP_CHAINS_WEIGHT, vals[0].id);
    TEST_ASSERT_EQUAL_INT32(0, vals[0].value);
}

void test_decode_param_read_response_with_status_byte()
{
    // Synthesise a device response: status 0x00, 3 params (weight 45, mode 5, eccentric -10)
    const uint8_t payload[] = {0x00, 0x03, 0x00, 0x86, 0x3e, 45, 0x00, 0x89, 0x3e, 0x05, 0x00, 0x88, 0x3e, 0xf6, 0xff};
    uint8_t buf[64];
    size_t n = build_frame(buf, sizeof(buf), CMD_PARAM_READ, payload, sizeof(payload), 9, 0x10, 0xAA);
    Packet p;
    TEST_ASSERT_TRUE(parse_packet(buf, n, p));
    ParamValue vals[4];
    size_t cnt = decode_params(p, vals, 4);
    TEST_ASSERT_EQUAL_UINT32(3, cnt);
    TEST_ASSERT_EQUAL_INT32(45, vals[0].value);
    TEST_ASSERT_EQUAL_INT32(5, vals[1].value);
    TEST_ASSERT_EQUAL_INT32(-10, vals[2].value);
}

static int s_frames = 0;
static size_t s_last_len = 0;
static void count_sink(void *, const uint8_t *, size_t len)
{
    s_frames++;
    s_last_len = len;
}

void test_assembler_handles_fragments_and_packing()
{
    uint8_t f1[64], f2[64];
    size_t n1 = hex2bin("5513040310aa13152000100100873e00009cee", f1, sizeof(f1));   // 19 bytes
    size_t n2 = hex2bin("551f044eaa10000020002781105eab9ef41c864ff5877a9c8c1d5f0d603e86", f2, sizeof(f2));  // 31 bytes

    FrameAssembler a;
    s_frames = 0;
    a.accept(f2, 20, count_sink, nullptr);          // first fragment
    TEST_ASSERT_EQUAL_INT(0, s_frames);
    a.accept(f2 + 20, n2 - 20, count_sink, nullptr); // rest
    TEST_ASSERT_EQUAL_INT(1, s_frames);
    TEST_ASSERT_EQUAL_UINT32(31, s_last_len);

    uint8_t packed[128];
    memcpy(packed, f1, n1);
    memcpy(packed + n1, f2, n2);
    s_frames = 0;
    a.accept(packed, n1 + n2, count_sink, nullptr);
    TEST_ASSERT_EQUAL_INT(2, s_frames);

    // Garbage before a frame is skipped
    uint8_t junk[64] = {0x01, 0x02, 0x03};
    memcpy(junk + 3, f1, n1);
    s_frames = 0;
    a.accept(junk, 3 + n1, count_sink, nullptr);
    TEST_ASSERT_EQUAL_INT(1, s_frames);
}

void test_rep_telemetry()
{
    const uint8_t payload[] = {0x81, 0x2B, 0x01, 0x02, 0x00, 0x07, 0, 0, 0, 0};
    uint8_t buf[64];
    size_t n = build_frame(buf, sizeof(buf), CMD_TELEMETRY, payload, sizeof(payload), 3, 0x10, 0xAA);
    Packet p;
    TEST_ASSERT_TRUE(parse_packet(buf, n, p));
    RepTelemetry rep;
    TEST_ASSERT_TRUE(parse_rep_telemetry(p, rep));
    TEST_ASSERT_EQUAL_UINT8(2, rep.set_count);
    TEST_ASSERT_EQUAL_UINT16(7, rep.rep_count);
    TEST_ASSERT_EQUAL_UINT8(1, rep.phase);
}

void test_activation()
{
    const uint8_t payload[] = {0x00, 0x01};
    uint8_t buf[64];
    size_t n = build_frame(buf, sizeof(buf), CMD_ACTIVATION, payload, sizeof(payload), 3, 0x10, 0xAA);
    Packet p;
    TEST_ASSERT_TRUE(parse_packet(buf, n, p));
    TEST_ASSERT_EQUAL_INT(1, parse_activation(p));
}

// ---------------------------------------------------------------------------
// Knob stepping
// ---------------------------------------------------------------------------

void test_fine_steps_are_one_pound()
{
    using namespace knobstep;
    TEST_ASSERT_EQUAL_INT(46, apply_step(45, 1, STEP_FINE_LB));
    TEST_ASSERT_EQUAL_INT(44, apply_step(45, -1, STEP_FINE_LB));
    TEST_ASSERT_EQUAL_INT(48, apply_step(45, 3, STEP_FINE_LB));
    // Fine mode must be able to reach values that are not multiples of 5.
    TEST_ASSERT_EQUAL_INT(47, apply_step(46, 1, STEP_FINE_LB));
}

void test_coarse_steps_snap_to_multiples_of_five()
{
    using namespace knobstep;
    // Already on a multiple: plain 5 lb moves.
    TEST_ASSERT_EQUAL_INT(50, apply_step(45, 1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(40, apply_step(45, -1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(60, apply_step(45, 3, STEP_COARSE_LB));
    // Off a multiple: the first coarse detent snaps in the direction of travel
    // rather than overshooting to 52 / 42.
    TEST_ASSERT_EQUAL_INT(50, apply_step(47, 1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(45, apply_step(47, -1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(55, apply_step(47, 2, STEP_COARSE_LB));
}

void test_coarse_steps_snap_correctly_below_zero()
{
    using namespace knobstep;
    // Eccentric goes negative, so the snapping must round the right way there too.
    TEST_ASSERT_EQUAL_INT(-5, apply_step(-7, 1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(-10, apply_step(-7, -1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(-5, apply_step(-10, 1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(-15, apply_step(-10, -1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(0, apply_step(-3, 1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(5, apply_step(0, 1, STEP_COARSE_LB));
    TEST_ASSERT_EQUAL_INT(-5, apply_step(0, -1, STEP_COARSE_LB));
}

// Twin status replies captured from Beyond+ (docs/PROTOCOL.md, "Twin mode").
static bool twin_from_payload(const char *payload_hex, TwinStatus &ts)
{
    uint8_t payload[64];
    const size_t n = hex2bin(payload_hex, payload, sizeof(payload));
    uint8_t frame[96];
    const size_t len = build_frame(frame, sizeof(frame), CMD_TWIN_STATUS, payload, n, 1, 0x10, 0xAA);
    Packet pkt;
    TEST_ASSERT_TRUE(parse_packet(frame, len, pkt));
    return parse_twin_status(pkt, ts);
}

void test_twin_status_alone()
{
    TwinStatus ts;
    TEST_ASSERT_TRUE(twin_from_payload("39010080b54e0702a600000000000000010000001600", ts));
    TEST_ASSERT_EQUAL_INT(TWIN_STATE_ALONE, ts.state);
    TEST_ASSERT_TRUE(ts.has_addrs);
    TEST_ASSERT_EQUAL_HEX8(0xa6, ts.own[5]);
    TEST_ASSERT_EQUAL_HEX8(0x00, ts.peer[0]);
}

void test_twin_status_twinned()
{
    TwinStatus ts;
    // with the leading status byte
    TEST_ASSERT_TRUE(twin_from_payload("0039011280b54e0702a680b54e0742a239010000001618", ts));
    TEST_ASSERT_EQUAL_INT(TWIN_STATE_TWINNED, ts.state);
    const uint8_t peer[6] = {0x80, 0xb5, 0x4e, 0x07, 0x42, 0xa2};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(peer, ts.peer, 6);
}

void test_twin_status_short()
{
    TwinStatus ts;
    TEST_ASSERT_TRUE(twin_from_payload("39010080b54e07", ts));
    TEST_ASSERT_EQUAL_INT(TWIN_STATE_ALONE, ts.state);
    TEST_ASSERT_FALSE(ts.has_addrs);
}

// Twinned fitness-mode values (captured).
void test_twin_modes()
{
    TEST_ASSERT_TRUE(voltra_loaded(FITNESS_MODE_TWIN_LOAD, -1));
    TEST_ASSERT_FALSE(voltra_loaded(FITNESS_MODE_TWIN_UNLOAD, -1));
    TEST_ASSERT_TRUE(voltra_auto_loading(FITNESS_MODE_TWIN_AUTO_LOAD, DIRECT_LOAD_ST_WAITING));
    TEST_ASSERT_TRUE(voltra_auto_loading(FITNESS_MODE_TWIN_AUTO_LOAD, DIRECT_LOAD_ST_COUNTDOWN));
    TEST_ASSERT_TRUE(voltra_loaded(FITNESS_MODE_TWIN_AUTO_LOAD, DIRECT_LOAD_ST_TWIN_LOADED));
    TEST_ASSERT_FALSE(voltra_auto_loading(FITNESS_MODE_TWIN_AUTO_LOAD, DIRECT_LOAD_ST_TWIN_LOADED));
}

void test_slow_turn_stays_fine()
{
    knobstep::RateTracker r;
    uint32_t t = 1000;
    // One detent every 250 ms: comfortably slower than the coarse threshold.
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_INT(knobstep::STEP_FINE_LB, r.step(1, t));
        t += 250;
    }
    TEST_ASSERT_FALSE(r.coarse());
}

void test_fast_turn_becomes_coarse()
{
    knobstep::RateTracker r;
    uint32_t t = 1000;
    // First detent after idle is always fine.
    TEST_ASSERT_EQUAL_INT(knobstep::STEP_FINE_LB, r.step(1, t));
    // Then a fast spin: a detent every 25 ms.
    t += 25;
    TEST_ASSERT_EQUAL_INT(knobstep::STEP_COARSE_LB, r.step(1, t));
    for (int i = 0; i < 5; i++) {
        t += 25;
        TEST_ASSERT_EQUAL_INT(knobstep::STEP_COARSE_LB, r.step(1, t));
    }
    TEST_ASSERT_TRUE(r.coarse());
}

void test_several_detents_in_one_poll_are_coarse()
{
    knobstep::RateTracker r;
    uint32_t t = 1000;
    r.step(1, t);
    // The UI polls every 25 ms; 4 detents in one poll is a fast spin.
    t += 25;
    TEST_ASSERT_EQUAL_INT(knobstep::STEP_COARSE_LB, r.step(4, t));
}

void test_pause_returns_to_fine()
{
    knobstep::RateTracker r;
    uint32_t t = 1000;
    r.step(1, t);
    t += 25;
    TEST_ASSERT_EQUAL_INT(knobstep::STEP_COARSE_LB, r.step(1, t));
    // Let go of the knob, then nudge it again: back to 1 lb.
    t += knobstep::IDLE_RESET_MS + 10;
    TEST_ASSERT_EQUAL_INT(knobstep::STEP_FINE_LB, r.step(1, t));
    TEST_ASSERT_FALSE(r.coarse());
}

void test_hysteresis_holds_mode_in_the_dead_band()
{
    knobstep::RateTracker r;
    uint32_t t = 1000;
    r.step(1, t);
    t += 30;
    TEST_ASSERT_EQUAL_INT(knobstep::STEP_COARSE_LB, r.step(1, t));   // now coarse
    // 100 ms sits between the exit and enter thresholds: stay coarse.
    t += 100;
    TEST_ASSERT_EQUAL_INT(knobstep::STEP_COARSE_LB, r.step(1, t));
    // Slower than the exit threshold: drop back to fine.
    t += 200;
    TEST_ASSERT_EQUAL_INT(knobstep::STEP_FINE_LB, r.step(1, t));
    // And 100 ms while fine keeps it fine.
    t += 100;
    TEST_ASSERT_EQUAL_INT(knobstep::STEP_FINE_LB, r.step(1, t));
}

void test_direction_reversal_keeps_stepping_sane()
{
    using namespace knobstep;
    // Spin up to 100, reverse: should come back down on multiples of 5.
    int v = 100;
    v = apply_step(v, -1, STEP_COARSE_LB);
    TEST_ASSERT_EQUAL_INT(95, v);
    v = apply_step(v, 1, STEP_COARSE_LB);
    TEST_ASSERT_EQUAL_INT(100, v);
    // Then fine-tune down to an exact odd value.
    v = apply_step(v, -1, STEP_FINE_LB);
    TEST_ASSERT_EQUAL_INT(99, v);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_weight_frames_match_captures);
    RUN_TEST(test_fitness_mode_loaded_includes_idle);
    RUN_TEST(test_direct_load_trigger_frame);
    RUN_TEST(test_auto_load_phases);
    RUN_TEST(test_workout_status_frames);
    RUN_TEST(test_chains_frame_matches_capture);
    RUN_TEST(test_load_unload_frames_match_captures);
    RUN_TEST(test_param_read_frame_matches_capture);
    RUN_TEST(test_app_hello_matches_capture);
    RUN_TEST(test_bootstrap_constants_have_valid_crcs);
    RUN_TEST(test_eccentric_frame_round_trips);
    RUN_TEST(test_parse_async_state_push);
    RUN_TEST(test_decode_param_read_response_with_status_byte);
    RUN_TEST(test_assembler_handles_fragments_and_packing);
    RUN_TEST(test_rep_telemetry);
    RUN_TEST(test_activation);
    RUN_TEST(test_fine_steps_are_one_pound);
    RUN_TEST(test_coarse_steps_snap_to_multiples_of_five);
    RUN_TEST(test_coarse_steps_snap_correctly_below_zero);
    RUN_TEST(test_twin_status_alone);
    RUN_TEST(test_twin_status_twinned);
    RUN_TEST(test_twin_status_short);
    RUN_TEST(test_twin_modes);
    RUN_TEST(test_slow_turn_stays_fine);
    RUN_TEST(test_fast_turn_becomes_coarse);
    RUN_TEST(test_several_detents_in_one_poll_are_coarse);
    RUN_TEST(test_pause_returns_to_fine);
    RUN_TEST(test_hysteresis_holds_mode_in_the_dead_band);
    RUN_TEST(test_direction_reversal_keeps_stepping_sane);
    return UNITY_END();
}
