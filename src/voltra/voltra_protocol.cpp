#include "voltra_protocol.h"

#include <string.h>

namespace voltra {

namespace {

uint8_t reflect8(uint8_t v)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        r = (uint8_t)((r << 1) | (v & 1));
        v >>= 1;
    }
    return r;
}

uint16_t reflect16(uint16_t v)
{
    uint16_t r = 0;
    for (int i = 0; i < 16; i++) {
        r = (uint16_t)((r << 1) | (v & 1));
        v >>= 1;
    }
    return r;
}

inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

inline uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

struct ParamDef {
    uint16_t id;
    uint8_t len;
    bool is_signed;
};

// Parameter registry (sizes) taken from the Home Assistant integration.
const ParamDef PARAM_REGISTRY[] = {
    {0x1B5D, 1, false},  // BMS_RSOC_LEGACY
    {0x3E82, 2, true},   // BP_RUNTIME_POSITION_CM
    {0x3E83, 2, true},   // BP_RUNTIME_WIRE_WEIGHT_LBS
    {0x3E86, 2, false},  // BP_BASE_WEIGHT
    {0x3E87, 2, false},  // BP_CHAINS_WEIGHT
    {0x3E88, 2, true},   // BP_ECCENTRIC_WEIGHT
    {0x3E89, 2, false},  // BP_SET_FITNESS_MODE
    {0x4E2D, 1, false},  // BMS_RSOC
    {0x4FB0, 1, false},  // FITNESS_WORKOUT_STATE
    {0x5011, 1, false},  // APP_CUR_SCR_ID
    {0x506A, 2, false},  // MC_DEFAULT_OFFLEN_CM
    {0x5103, 1, false},  // FITNESS_DAMPER_RATIO_IDX
    {0x5106, 1, false},  // FITNESS_ASSIST_MODE
    {0x51F8, 1, false},  // EP_LOGO_APPLY_ACTION
    {0x52CA, 1, false},  // RESISTANCE_EXPERIENCE
    {0x52E3, 1, false},  // EP_RESISTANCE_BAND_INVERSE
    {0x5314, 2, false},  // EP_MAX_ALLOWED_FORCE
    {0x5350, 4, false},  // EP_ISOKINETIC_TARGET_SPEED_MMS
    {0x535A, 4, false},  // EP_ISOMETRIC_TESTING_BODY_WEIGHT_N
    {0x535B, 2, false},  // EP_ISOMETRIC_TESTING_BODY_WEIGHT_100G
    {0x535C, 2, false},  // EP_ISOMETRIC_TESTING_BODY_WEIGHT_LBS
    {0x5361, 1, false},  // RESISTANCE_BAND_ALGORITHM
    {0x5362, 2, false},  // RESISTANCE_BAND_MAX_FORCE
    {0x53A6, 1, false},  // POWER_OFF_LOGO_EN
    {0x53A7, 1, false},  // FITNESS_ROWING_DAMPER_RATIO_IDX
    {0x53AE, 1, false},  // EP_ROW_CHAIN_GEAR
    {0x53B0, 1, false},  // FITNESS_INVERSE_CHAIN
    {0x53B6, 1, false},  // RESISTANCE_BAND_LEN_BY_ROM
    {0x53B7, 2, false},  // RESISTANCE_BAND_LEN
    {0x53C6, 1, false},  // WEIGHT_TRAINING_EXTRA_MODE (0 = lb, 1 = percent)
    {0x5189, 2, false},  // EP_WORKOUT_CHAINS_PCT_X100
    {0x518A, 2, false},  // EP_WORKOUT_ECCENTRIC_PCT_X100
    {0x54DA, 4, true},   // EP_WORKOUT_CHAINS_PCT_X100_S32
    {0x53D0, 4, true},   // EP_WORKOUT_ECCENTRIC_PCT_X100_S32
    {0x54D4, 1, false},  // EP_MAX_CHAINS_PCT
    {0x5319, 1, false},  // EP_MAX_ECCENTRIC_PCT
    {0x541E, 2, false},  // OVERDRIVE_USER_CFG_FORCE_MAX (lb)
    {0x5421, 1, false},  // OVERDRIVE_AVAILABLE
    {0x541D, 1, false},  // OVERDRIVE_ACTIVE_STATUS
    {0x53D1, 1, false},  // ISOMETRIC_METRICS_TYPE
    {0x53D2, 2, false},  // ISOMETRIC_MAX_DURATION
    {0x5410, 1, false},  // ISOKINETIC_ECC_MODE
    {0x538D, 1, false},  // EP_DIRECT_LOAD_SAFETY_CHECK
    {0x53C7, 1, false},  // DIRECT_LOAD_SAFETY_CHECK_ST
    {0x53C8, 2, false},  // DIRECT_LOAD_SAFETY_CHECK_COUNTDOWN (ms)
    {0x53C9, 1, false},  // DIRECT_LOAD_SAFETY_CHECK_CTRL
    {0x5411, 2, false},  // ISOKINETIC_ECC_SPEED_LIMIT
    {0x5412, 2, false},  // ISOKINETIC_ECC_CONST_WEIGHT
    {0x5413, 2, false},  // ISOKINETIC_ECC_OVERLOAD_WEIGHT
    {0x5431, 2, false},  // ISOMETRIC_MAX_FORCE
    {0x5448, 2, false},  // CUSTOM_LOGO_X
    {0x5449, 2, false},  // CUSTOM_LOGO_Y
    {0x544A, 4, false},  // CUSTOM_LOGO_BG_COLOR
    {0x5467, 2, false},  // FITNESS_ONGOING_UI
    {0x54BC, 1, false},  // QUICK_CABLE_ADJUSTMENT
    {0x556F, 1, false},  // mountain curve (undocumented; 1 = mountain, 0 = chains)
    {0x5569, 1, false},  // accessory display unit (undocumented; 1 = percent, 0 = pounds)
};

const ParamDef *find_param(uint16_t id)
{
    for (const ParamDef &d : PARAM_REGISTRY) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

// Captured frames (Beyond+ iOS app), CRCs verified against the frame builder.
const uint8_t BOOT_CONNECT_REQUEST[] = {0x55, 0x0f, 0x08, 0x01, 0xaa, 0xd2, 0x00, 0x00, 0x20, 0x00, 0xff, 0x00, 0xaa, 0x04, 0x19};
const uint8_t BOOT_HANDSHAKE_CHECK[] = {0x55, 0x1f, 0x04, 0x4e, 0xaa, 0x10, 0x00, 0x00, 0x20, 0x00, 0x27, 0x81, 0x10, 0x5e, 0xab,
                                        0x9e, 0xf4, 0x1c, 0x86, 0x4f, 0xf5, 0x87, 0x7a, 0x9c, 0x8c, 0x1d, 0x5f, 0x0d, 0x60, 0x3e, 0x86};
const uint8_t BOOT_READ_COMMON_STATE[] = {0x55, 0x0d, 0x04, 0x33, 0xaa, 0x10, 0x00, 0x00, 0x20, 0x00, 0x74, 0x03, 0xbc};
const uint8_t BOOT_READ_FW0[] = {0x55, 0x0e, 0x04, 0x66, 0xaa, 0x10, 0x01, 0x00, 0x20, 0x00, 0x77, 0x00, 0x38, 0x89};
const uint8_t BOOT_READ_FW1[] = {0x55, 0x0e, 0x04, 0x66, 0xaa, 0x10, 0x02, 0x00, 0x20, 0x00, 0x77, 0x01, 0xcc, 0x94};
const uint8_t BOOT_READ_SERIAL[] = {0x55, 0x0e, 0x04, 0x66, 0xaa, 0x10, 0x03, 0x00, 0x20, 0x00, 0x19, 0x00, 0x2b, 0x7e};
const uint8_t BOOT_READ_ACTIVATION[] = {0x55, 0x0e, 0x04, 0x66, 0xaa, 0x10, 0x04, 0x00, 0x20, 0x00, 0xab, 0x01, 0xad, 0x7a};

}  // namespace

const BootFrame BOOTSTRAP_FRAMES[] = {
    {"connect request", BOOT_CONNECT_REQUEST, sizeof(BOOT_CONNECT_REQUEST)},
    {"handshake check", BOOT_HANDSHAKE_CHECK, sizeof(BOOT_HANDSHAKE_CHECK)},
    {"read common state", BOOT_READ_COMMON_STATE, sizeof(BOOT_READ_COMMON_STATE)},
    {"read firmware 0", BOOT_READ_FW0, sizeof(BOOT_READ_FW0)},
    {"read firmware 1", BOOT_READ_FW1, sizeof(BOOT_READ_FW1)},
    {"read serial", BOOT_READ_SERIAL, sizeof(BOOT_READ_SERIAL)},
    {"read activation", BOOT_READ_ACTIVATION, sizeof(BOOT_READ_ACTIVATION)},
};
const size_t BOOTSTRAP_FRAME_COUNT = sizeof(BOOTSTRAP_FRAMES) / sizeof(BOOTSTRAP_FRAMES[0]);

uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xEE;
    for (size_t i = 0; i < len; i++) {
        crc ^= reflect8(data[i]);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return reflect8(crc);
}

uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x496C;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(reflect8(data[i]) << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return reflect16(crc);
}

size_t build_frame(uint8_t *out, size_t cap, uint8_t cmd, const uint8_t *payload, size_t payload_len,
                   uint16_t seq, uint8_t sender, uint8_t receiver)
{
    const size_t length = 11 + payload_len + 2;
    if (length > 0xFFFF || length > cap || (payload_len && !payload)) return 0;

    out[0] = FRAME_MAGIC;
    out[1] = (uint8_t)(length & 0xFF);
    out[2] = (length > 0xFF) ? FRAME_TYPE_EXT_APP_WRITE : FRAME_TYPE_APP_WRITE;
    out[3] = crc8(out, 3);
    out[4] = sender;
    out[5] = receiver;
    put_u16(&out[6], seq);
    put_u16(&out[8], PROTO);
    out[10] = cmd;
    if (payload_len) memcpy(&out[11], payload, payload_len);
    put_u16(&out[length - 2], crc16(out, length - 2));
    return length;
}

size_t build_param_write(uint8_t *out, size_t cap, uint16_t param, const uint8_t *value, size_t value_len, uint16_t seq)
{
    uint8_t payload[2 + 2 + 8];
    if (value_len > 8) return 0;
    payload[0] = 0x01;
    payload[1] = 0x00;
    put_u16(&payload[2], param);
    memcpy(&payload[4], value, value_len);
    return build_frame(out, cap, CMD_PARAM_WRITE, payload, 4 + value_len, seq);
}

size_t build_param_write_u8(uint8_t *out, size_t cap, uint16_t param, uint8_t value, uint16_t seq)
{
    return build_param_write(out, cap, param, &value, 1, seq);
}

size_t build_param_write_u16(uint8_t *out, size_t cap, uint16_t param, uint16_t value, uint16_t seq)
{
    uint8_t v[2];
    put_u16(v, value);
    return build_param_write(out, cap, param, v, 2, seq);
}

size_t build_param_write_i16(uint8_t *out, size_t cap, uint16_t param, int16_t value, uint16_t seq)
{
    return build_param_write_u16(out, cap, param, (uint16_t)value, seq);
}

size_t build_vendor(uint8_t *out, size_t cap, const uint8_t *payload, size_t payload_len, uint16_t seq)
{
    return build_frame(out, cap, CMD_VENDOR, payload, payload_len, seq);
}

size_t build_param_read(uint8_t *out, size_t cap, const uint16_t *ids, size_t count, uint16_t seq)
{
    uint8_t payload[2 + 2 * 32];
    if (count == 0 || count > 32) return 0;
    put_u16(payload, (uint16_t)count);
    for (size_t i = 0; i < count; i++) put_u16(&payload[2 + 2 * i], ids[i]);
    return build_frame(out, cap, CMD_PARAM_READ, payload, 2 + 2 * count, seq);
}

size_t build_app_hello(uint8_t *out, size_t cap, const char *app_name)
{
    // 21-byte zero-padded name followed by a fixed 7-byte trailer captured from the Beyond+ app.
    static const uint8_t trailer[] = {0x84, 0xab, 0x1a, 0x5f, 0x29, 0x20, 0x01};
    uint8_t payload[21 + sizeof(trailer)];
    memset(payload, 0, sizeof(payload));
    size_t n = app_name ? strlen(app_name) : 0;
    if (n > 20) n = 20;
    memcpy(payload, app_name, n);
    memcpy(&payload[21], trailer, sizeof(trailer));
    return build_frame(out, cap, CMD_DEVICE_NAME, payload, sizeof(payload), 0, 0x01, DEVICE_RECEIVER);
}

int expected_frame_length(const uint8_t *buf, size_t len)
{
    if (len == 0) return 0;
    if (buf[0] != FRAME_MAGIC) return -1;
    if (len < 3) return 0;
    int declared = buf[1];
    uint8_t type = buf[2];
    if (type == FRAME_TYPE_EXT_APP_WRITE || type == 0x09) declared += 0x100;
    if (declared < (int)MIN_FRAME) return -1;
    return declared;
}

bool parse_packet(const uint8_t *frame, size_t len, Packet &out)
{
    if (len < MIN_FRAME || frame[0] != FRAME_MAGIC) return false;
    int expected = expected_frame_length(frame, len);
    if (expected <= 0 || (size_t)expected != len) return false;

    out.type = frame[2];
    out.sender = frame[4];
    out.receiver = frame[5];
    out.seq = get_u16(&frame[6]);
    out.proto = get_u16(&frame[8]);
    out.cmd = frame[10];
    out.payload = &frame[11];
    out.payload_len = len - FRAME_OVERHEAD;
    out.crc_ok = (crc8(frame, 3) == frame[3]) && (crc16(frame, len - 2) == get_u16(&frame[len - 2]));
    return true;
}

size_t decode_params(const Packet &pkt, ParamValue *out, size_t max)
{
    if (pkt.cmd != CMD_PARAM_READ && pkt.cmd != CMD_ASYNC_STATE) return 0;

    const uint8_t *p = pkt.payload;
    size_t n = pkt.payload_len;

    // PARAM_READ responses carry a leading status byte; ASYNC_STATE pushes do not.
    size_t start = 0;
    if (pkt.cmd == CMD_PARAM_READ && n >= 5 && p[0] == 0x00 && find_param(get_u16(&p[3]))) {
        start = 1;
    }
    if (n < start + 2) return 0;

    size_t count = get_u16(&p[start]);
    size_t off = start + 2;
    size_t written = 0;
    for (size_t i = 0; i < count && written < max; i++) {
        if (off + 2 > n) break;
        uint16_t id = get_u16(&p[off]);
        off += 2;
        const ParamDef *def = find_param(id);
        if (!def || off + def->len > n) break;   // unknown size: cannot continue
        int32_t value = 0;
        switch (def->len) {
            case 1: value = p[off]; break;
            case 2: value = def->is_signed ? (int32_t)(int16_t)get_u16(&p[off]) : (int32_t)get_u16(&p[off]); break;
            case 4: value = (int32_t)((uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24)); break;
            default: return written;
        }
        out[written].id = id;
        out[written].value = value;
        written++;
        off += def->len;
    }
    return written;
}

int parse_workout_status(const Packet &pkt)
{
    if (pkt.cmd != CMD_TELEMETRY || pkt.payload_len < 5) return -1;
    const uint8_t *p = pkt.payload;
    if (p[0] != 0x80 || p[1] != 0x25 || p[2] != 0x01) return -1;
    return p[4];
}

bool parse_rep_telemetry(const Packet &pkt, RepTelemetry &out)
{
    if (pkt.cmd != CMD_TELEMETRY || pkt.payload_len < 6) return false;
    const uint8_t *p = pkt.payload;
    if (p[0] != 0x81 || p[1] != 0x2B) return false;
    out.phase = p[2];
    out.set_count = p[3];
    out.rep_count = (uint16_t)((p[4] << 8) | p[5]);   // big-endian
    if (out.rep_count > 10000) return false;
    return true;
}

int parse_activation(const Packet &pkt)
{
    if (pkt.cmd != CMD_ACTIVATION || pkt.payload_len < 2 || pkt.payload[0] != 0) return -1;
    if (pkt.payload[1] == 1) return 1;
    if (pkt.payload[1] == 0) return 0;
    return -1;
}

void FrameAssembler::accept(const uint8_t *data, size_t len, Sink sink, void *ctx)
{
    if (!data || len == 0) return;
    if (len_ + len > sizeof(buf_)) {
        len_ = 0;   // overflow: resync
        if (len > sizeof(buf_)) return;
    }
    memcpy(&buf_[len_], data, len);
    len_ += len;

    for (;;) {
        if (len_ == 0) return;
        int need = expected_frame_length(buf_, len_);
        if (need < 0) {
            // Not a frame start: drop bytes until the next magic byte.
            size_t skip = 1;
            while (skip < len_ && buf_[skip] != FRAME_MAGIC) skip++;
            memmove(buf_, &buf_[skip], len_ - skip);
            len_ -= skip;
            continue;
        }
        if (need == 0 || (size_t)need > len_) return;   // wait for more data
        if (sink) sink(ctx, buf_, (size_t)need);
        memmove(buf_, &buf_[need], len_ - need);
        len_ -= (size_t)need;
    }
}

}  // namespace voltra
