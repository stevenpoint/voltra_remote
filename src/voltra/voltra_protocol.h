#pragma once
/*
 * Beyond Power Voltra BLE protocol (community reverse engineering).
 *
 * Frame layout (all multi-byte fields little-endian):
 *   [0]     0x55 magic
 *   [1]     frame length (low byte; type 0x05/0x09 frames add 0x100)
 *   [2]     frame type (0x04 = app write)
 *   [3]     CRC-8 over bytes 0..2
 *   [4]     sender   (0xAA = app)
 *   [5]     receiver (0x10 = Voltra)
 *   [6..7]  sequence number
 *   [8..9]  protocol id (0x0020)
 *   [10]    command id
 *   [11..]  payload
 *   [-2..]  CRC-16 over everything before it
 *
 * Sources: dylanmaniatakes/Beyond-Power-HomeAssistant and
 * dylanmaniatakes/Beyond-Power-Voltra-Android (iOS PacketLogger captures),
 * jpamorgan/voltra-sdk (captured command tables).
 *
 * This file has no Arduino/ESP dependencies so it can be unit-tested on the host.
 */
#include <stddef.h>
#include <stdint.h>

namespace voltra {

// ---------------------------------------------------------------------------
// BLE identifiers
// ---------------------------------------------------------------------------
constexpr const char *SERVICE_UUID        = "e4dada34-0867-8783-9f70-2ca29216c7e4";
constexpr const char *CHAR_COMMAND_UUID   = "55ca1e52-7354-25de-6afc-b7df1e8816ac";  // notify: responses / telemetry
constexpr const char *CHAR_NOTIFY_UUID    = "ca94658c-0525-5046-e78b-5391b65f47ad";  // notify
constexpr const char *CHAR_TRANSPORT_UUID = "a010891d-f50f-44f0-901f-9a2421a9e050";  // write (+notify)
constexpr const char *DEVICE_NAME_PREFIX  = "VTR-";

// ---------------------------------------------------------------------------
// Frame constants
// ---------------------------------------------------------------------------
constexpr uint8_t  FRAME_MAGIC              = 0x55;
constexpr uint8_t  FRAME_TYPE_APP_WRITE     = 0x04;
constexpr uint8_t  FRAME_TYPE_EXT_APP_WRITE = 0x05;
constexpr uint8_t  APP_SENDER               = 0xAA;
constexpr uint8_t  DEVICE_RECEIVER          = 0x10;
constexpr uint16_t PROTO                    = 0x0020;
constexpr size_t   FRAME_OVERHEAD           = 13;   // header (11) + crc16 (2)
constexpr size_t   MIN_FRAME                = 13;
constexpr size_t   MAX_FRAME                = 512;

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
constexpr uint8_t CMD_PARAM_READ      = 0x0F;
constexpr uint8_t CMD_ASYNC_STATE     = 0x10;
constexpr uint8_t CMD_PARAM_WRITE     = 0x11;
constexpr uint8_t CMD_SERIAL_INFO     = 0x19;
constexpr uint8_t CMD_HANDSHAKE_CHECK = 0x27;
constexpr uint8_t CMD_DEVICE_NAME     = 0x4F;
constexpr uint8_t CMD_COMMON_STATE    = 0x74;
constexpr uint8_t CMD_FIRMWARE_INFO   = 0x77;
constexpr uint8_t CMD_TELEMETRY       = 0xAA;
constexpr uint8_t CMD_VENDOR          = 0xAA;   // app -> device: vendor sub-commands (first payload byte)

// Vendor sub-commands (payload of a CMD_VENDOR frame), from the Beyond-Power-Voltra-Android port.
constexpr uint8_t VENDOR_DIRECT_LOAD    = 0x12;   // start the Voltra's auto load ("direct load")
constexpr uint8_t VENDOR_STATE_REFRESH  = 0x13;   // + 0x01: refresh the vendor state stream
constexpr uint8_t CMD_ACTIVATION      = 0xAB;

// Twin mode, captured from Beyond+ (docs/PROTOCOL.md, "Twin mode").
constexpr uint8_t CMD_TWIN_STATUS     = 0xA7;   // empty request; see parse_twin_status()
constexpr uint8_t CMD_TWIN_LINK       = 0xA8;   // TWIN_LINK_* + the other unit's address
constexpr uint8_t TWIN_LINK_JOIN      = 0x01;   // to the follower: join this host
constexpr uint8_t TWIN_LINK_LEAVE     = 0x02;   // to the host: drop this follower
constexpr int TWIN_STATE_ALONE        = 0x00;
constexpr int TWIN_STATE_JOINING      = 0x11;
constexpr int TWIN_STATE_TWINNED      = 0x12;
// While hosting a twin the host's fitness mode carries a flag in the high byte:
// 0x0100 idle (also what a refused load settles at), 0x0101 loaded. Load and unload are
// written the same way. Its auto load reads 0x2003 rather than 0x23 and finishes at
// DIRECT_LOAD_ST 15 rather than 14 (captured, docs/PROTOCOL.md).
constexpr uint16_t FITNESS_MODE_TWIN_LOAD      = 0x0101;
constexpr uint16_t FITNESS_MODE_TWIN_UNLOAD    = 0x0100;
constexpr uint16_t FITNESS_MODE_TWIN_AUTO_LOAD = 0x2003;

// ---------------------------------------------------------------------------
// Parameters (subset we use; the registry in the .cpp knows the sizes of more)
// ---------------------------------------------------------------------------
constexpr uint16_t PARAM_BMS_RSOC_LEGACY            = 0x1B5D;  // u8  battery %
constexpr uint16_t PARAM_BP_RUNTIME_POSITION_CM     = 0x3E82;  // i16 cable position
constexpr uint16_t PARAM_BP_RUNTIME_WIRE_WEIGHT_LBS = 0x3E83;  // i16 live force on cable
constexpr uint16_t PARAM_BP_BASE_WEIGHT             = 0x3E86;  // u16 target weight (lb)
constexpr uint16_t PARAM_BP_CHAINS_WEIGHT           = 0x3E87;  // u16 chains (lb)
constexpr uint16_t PARAM_BP_ECCENTRIC_WEIGHT        = 0x3E88;  // i16 eccentric +/- (lb)
constexpr uint16_t PARAM_BP_SET_FITNESS_MODE        = 0x3E89;  // u16 0 unloaded, 1 loaded idle, 5 set active
constexpr uint16_t PARAM_BMS_RSOC                   = 0x4E2D;  // u8  battery %
constexpr uint16_t PARAM_FITNESS_WORKOUT_STATE      = 0x4FB0;  // u8  0 = inactive, 1 = weight training
constexpr uint16_t PARAM_MC_DEFAULT_OFFLEN_CM       = 0x506A;  // u16 cable offset
constexpr uint16_t PARAM_FITNESS_ASSIST_MODE        = 0x5106;  // u8
constexpr uint16_t PARAM_FITNESS_INVERSE_CHAIN      = 0x53B0;  // u8  1 = inverse chains

/**
 * The vendor dictionary describes this as how chains/eccentric adapt when the base weight
 * changes. It is NOT the Voltra's lb/% setting (that is PARAM_ACCESSORY_DISPLAY_PCT): it
 * stayed 0 in both modes. Unused.
 */
constexpr uint16_t PARAM_WEIGHT_TRAINING_EXTRA_MODE = 0x53C6;  // u8

// Accessory amounts as percent x 100 (2500 = 25.00%). The dictionary also lists 16-bit
// variants (0x5189 / 0x518A); they stay 0 on current firmware and are not used.
constexpr uint16_t PARAM_CHAINS_PCT_X100_S32    = 0x54DA;  // i32
constexpr uint16_t PARAM_ECCENTRIC_PCT_X100_S32 = 0x53D0;  // i32
constexpr uint16_t PARAM_MAX_CHAINS_PCT         = 0x54D4;  // u8  100..128
constexpr uint16_t PARAM_MAX_ECCENTRIC_PCT      = 0x5319;  // u8  60..100

/**
 * Mountain accessory. Undocumented: newer than the vendor dictionary, found by sweeping
 * the unlisted ids in 0x5300-0x57FF (env:remote_diag) while switching modes on the Voltra.
 *
 * 1 selects Mountain, 0 selects plain chains; polarity confirmed by writing it and
 * watching the Voltra's own screen. It shapes the chains resistance, so it acts on the
 * chains amount, and the device keeps the selection even while chains are off. It is
 * never pushed asynchronously, so it has to be polled.
 */
constexpr uint16_t PARAM_MOUNTAIN_CURVE = 0x556F;   // u8  1 = mountain, 0 = chains

/**
 * Auto load ("direct load"): after the trigger, the Voltra waits for the cable to be
 * pulled out and held, counts down, then loads. These report its progress.
 */
constexpr uint16_t PARAM_DIRECT_LOAD_SAFETY_CHECK     = 0x538D;  // u8  1 = safety check enabled
constexpr uint16_t PARAM_DIRECT_LOAD_SAFETY_ST        = 0x53C7;  // u8  progress, see DIRECT_LOAD_ST_*
constexpr uint16_t PARAM_DIRECT_LOAD_COUNTDOWN_MS     = 0x53C8;  // u16 countdown to load, 3000..0
constexpr uint16_t PARAM_DIRECT_LOAD_SAFETY_CTRL      = 0x53C9;  // u8  cancel / bypass

/**
 * The Voltra's own lb/% setting for how accessories are shown. Undocumented, found the
 * same way as Mountain: the one register that followed the setting in a sweep. Display
 * only: the accessory amounts are always stored as percentages. Not pushed; polled.
 */
constexpr uint16_t PARAM_ACCESSORY_DISPLAY_PCT = 0x5569;   // u8  1 = percent, 0 = pounds

// Overdrive raises the target-weight ceiling above the classic 200 lb.
constexpr uint16_t PARAM_OVERDRIVE_FORCE_MAX    = 0x541E;  // u16 lb, 100..250
constexpr uint16_t PARAM_OVERDRIVE_AVAILABLE    = 0x5421;  // u8
constexpr uint16_t PARAM_OVERDRIVE_ACTIVE       = 0x541D;  // u8

/**
 * Fitness mode as the Voltra reports it: 0 = unloaded, 1 = loaded but idle (just loaded,
 * or resting between sets), 5 = loaded with a set active. The device drops from 5 to 1 on
 * its own when a set ends, and after a weight or accessory change. Writing 5 loads (from
 * 0 it steps through 1 first) and writing 4 unloads.
 */
constexpr uint16_t FITNESS_MODE_UNLOADED        = 0x0000;
constexpr uint16_t FITNESS_MODE_STRENGTH_IDLE   = 0x0001;
constexpr uint16_t FITNESS_MODE_STRENGTH_READY  = 0x0004;
constexpr uint16_t FITNESS_MODE_STRENGTH_LOADED = 0x0005;
/** True for both loaded states: a set active, or idle/resting with the weight on. */
constexpr bool fitness_mode_loaded(int mode)
{
    return mode >= 0 && ((mode & 0xFF) == FITNESS_MODE_STRENGTH_LOADED ||
                         (mode & 0xFF) == FITNESS_MODE_STRENGTH_IDLE);
}

/**
 * Auto load. Captured on this firmware: the Voltra enters 0x23 when auto load starts and
 * stays there once it has loaded, so PARAM_DIRECT_LOAD_SAFETY_ST tells the phases apart.
 * The Android port lists 0x26 / 0x27 for its firmware; they are treated the same way.
 */
constexpr uint16_t FITNESS_MODE_AUTO_LOAD          = 0x0023;
constexpr uint16_t FITNESS_MODE_DIRECT_LOAD_READY  = 0x0026;
constexpr uint16_t FITNESS_MODE_DIRECT_LOAD_ACTIVE = 0x0027;

// PARAM_DIRECT_LOAD_SAFETY_ST during auto load (captured)
constexpr int DIRECT_LOAD_ST_WAITING   = 11;   // waiting for the cable to be pulled out
constexpr int DIRECT_LOAD_ST_COUNTDOWN = 12;   // held: counting down (restarts if it moves)
constexpr int DIRECT_LOAD_ST_ENGAGING  = 13;   // countdown done, weight coming on
constexpr int DIRECT_LOAD_ST_LOADED    = 14;   // loaded
constexpr int DIRECT_LOAD_ST_TWIN_LOADED = 15; // loaded, twinned

constexpr bool fitness_mode_is_auto_load(int mode)
{
    return mode >= 0 && ((mode & 0xFF) == FITNESS_MODE_AUTO_LOAD ||
                         (mode & 0xFF) == FITNESS_MODE_DIRECT_LOAD_READY ||
                         (mode & 0xFF) == FITNESS_MODE_DIRECT_LOAD_ACTIVE ||
                         mode == FITNESS_MODE_TWIN_AUTO_LOAD);
}

/** Weight on: a set active, idle/resting, or an auto load that has finished its countdown. */
constexpr bool voltra_loaded(int mode, int direct_load_st)
{
    return fitness_mode_loaded(mode) ||
           (fitness_mode_is_auto_load(mode) && direct_load_st >= DIRECT_LOAD_ST_ENGAGING);
}

/** Auto load started but not yet loaded: waiting for the pull, or counting down. */
constexpr bool voltra_auto_loading(int mode, int direct_load_st)
{
    return fitness_mode_is_auto_load(mode) && direct_load_st < DIRECT_LOAD_ST_ENGAGING;
}


constexpr uint8_t WORKOUT_STATE_INACTIVE = 0x00;
constexpr uint8_t WORKOUT_STATE_ACTIVE   = 0x01;   // weight training

constexpr int MIN_TARGET_LB    = 5;
/** Classic ceiling. Overdrive firmware reports a higher one via PARAM_OVERDRIVE_FORCE_MAX. */
constexpr int MAX_TARGET_LB          = 230;
constexpr int MAX_TARGET_LB_CLASSIC  = 200;
constexpr int MAX_TARGET_LB_OVERDRIVE = 250;
constexpr int MIN_CHAINS_LB    = 0;
constexpr int MAX_CHAINS_LB    = 200;
constexpr int MIN_ECCENTRIC_LB = -200;
constexpr int MAX_ECCENTRIC_LB = 200;

// ---------------------------------------------------------------------------
// CRC
// ---------------------------------------------------------------------------
uint8_t  crc8(const uint8_t *data, size_t len);
uint16_t crc16(const uint8_t *data, size_t len);

// ---------------------------------------------------------------------------
// Frame builders. All return the number of bytes written to `out` (0 on error).
// ---------------------------------------------------------------------------
size_t build_frame(uint8_t *out, size_t cap, uint8_t cmd, const uint8_t *payload, size_t payload_len,
                   uint16_t seq, uint8_t sender = APP_SENDER, uint8_t receiver = DEVICE_RECEIVER);

size_t build_param_write(uint8_t *out, size_t cap, uint16_t param, const uint8_t *value, size_t value_len, uint16_t seq);
size_t build_param_write_u8(uint8_t *out, size_t cap, uint16_t param, uint8_t value, uint16_t seq);
size_t build_param_write_u16(uint8_t *out, size_t cap, uint16_t param, uint16_t value, uint16_t seq);
size_t build_param_write_i16(uint8_t *out, size_t cap, uint16_t param, int16_t value, uint16_t seq);
size_t build_param_read(uint8_t *out, size_t cap, const uint16_t *ids, size_t count, uint16_t seq);

/** Vendor frame (CMD_VENDOR) carrying a sub-command and its argument bytes. */
size_t build_vendor(uint8_t *out, size_t cap, const uint8_t *payload, size_t payload_len, uint16_t seq);

/** "App hello" handshake frame (cmd 0x4F from sender 0x01). Must be the first write after connecting. */
size_t build_app_hello(uint8_t *out, size_t cap, const char *app_name);

// Captured bootstrap frames sent (in order) after the app hello.
struct BootFrame {
    const char *label;
    const uint8_t *data;
    size_t len;
};
extern const BootFrame BOOTSTRAP_FRAMES[];
extern const size_t BOOTSTRAP_FRAME_COUNT;

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
struct Packet {
    uint8_t type;
    uint8_t sender;
    uint8_t receiver;
    uint16_t seq;
    uint16_t proto;
    uint8_t cmd;
    const uint8_t *payload;
    size_t payload_len;
    bool crc_ok;
};

/**
 * Total frame length announced by a buffer starting with a header.
 * @return >0 length, 0 if more bytes are needed, -1 if the buffer is not a frame.
 */
int expected_frame_length(const uint8_t *buf, size_t len);

/** Parse a complete frame. Returns false if it is malformed (CRC mismatches only set crc_ok). */
bool parse_packet(const uint8_t *frame, size_t len, Packet &out);

struct ParamValue {
    uint16_t id;
    int32_t value;
};

/** Decode the (id, value) list of a PARAM_READ response or ASYNC_STATE push. */
size_t decode_params(const Packet &pkt, ParamValue *out, size_t max);

struct RepTelemetry {
    uint8_t set_count;
    uint16_t rep_count;
    uint8_t phase;   // 0 idle, 1 pull, 2 transition, 3 return
};
bool parse_rep_telemetry(const Packet &pkt, RepTelemetry &out);

/**
 * Workout status, pushed by the Voltra (cmd 0xAA, "80 25 01 00 SS ...") the moment it
 * changes. Captured: 0 unloaded, 1 loading, 2 set in progress / ready for one, 3 resting
 * (about 3.5 s after the last rep), 4 unloading. Unlike the fitness mode, it follows
 * every set, however the Voltra was loaded.
 */
constexpr int WORKOUT_STATUS_UNLOADED  = 0;
constexpr int WORKOUT_STATUS_LOADING   = 1;
constexpr int WORKOUT_STATUS_ACTIVE    = 2;
constexpr int WORKOUT_STATUS_RESTING   = 3;
constexpr int WORKOUT_STATUS_UNLOADING = 4;

/** @return the status byte, or -1 if this is not a workout-status packet */
int parse_workout_status(const Packet &pkt);

/** @return 1 activated, 0 not activated, -1 not an activation packet */
int parse_activation(const Packet &pkt);

/** A unit's twin status (CMD_TWIN_STATUS reply or push). */
struct TwinStatus {
    int state = -1;          // TWIN_STATE_*
    uint8_t own[6] = {0};    // this unit's address, in printed order
    uint8_t peer[6] = {0};   // the other unit's, all zero when alone
    bool has_addrs = false;  // own/peer present (short replies carry only the state)
};

/**
 * Decode `39 01 SS <own addr> <peer addr> ...`, optionally after a leading status byte.
 * Returns false for any other packet.
 */
bool parse_twin_status(const Packet &pkt, TwinStatus &out);

/** Reassembles frames that arrive split across (or packed into) BLE notifications. */
class FrameAssembler {
public:
    typedef void (*Sink)(void *ctx, const uint8_t *frame, size_t len);
    void accept(const uint8_t *data, size_t len, Sink sink, void *ctx);
    void clear() { len_ = 0; }

private:
    uint8_t buf_[MAX_FRAME];
    size_t len_ = 0;
};

}  // namespace voltra
