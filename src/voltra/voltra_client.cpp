#include "voltra_client.h"
#include "sweep_params.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace voltra {

namespace {

constexpr uint32_t WRITE_PACING_MS      = 90;     // same pacing as the official app captures
constexpr uint32_t STATUS_POLL_MS       = 3000;
constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 8000;
constexpr uint32_t RECONNECT_MIN_MS     = 1500;
constexpr uint32_t RECONNECT_MAX_MS     = 15000;
constexpr uint32_t SCAN_PERIOD_MS       = 4000;
constexpr uint16_t PREFERRED_MTU        = 247;
constexpr size_t   PARAMS_PER_READ      = 5;
/** Skip the post-write state read-back when more than this many frames are queued. */
constexpr size_t   READBACK_MAX_QUEUE_DEPTH = 3;

/** How long a commanded fitness mode outranks a contradicting async echo. */
constexpr uint32_t MODE_ECHO_GUARD_MS = 2500;

/** Re-issue a load/unload this often until the device reaches the target state. */
constexpr uint32_t LOAD_RETRY_MS = 600;
constexpr uint8_t  LOAD_MAX_RETRIES = 4;

/** Auto load: how long to follow it after the trigger, and how often to read its countdown. */
constexpr uint32_t AUTO_LOAD_WATCH_MS = 45000;
constexpr uint32_t AUTO_LOAD_POLL_MS  = 250;

/**
 * Dump raw parameter traffic to the serial log. On only in the diagnostic build
 * (pio run -e remote_diag), which also sweeps every BP/MC register periodically.
 */
bool s_log_params = VOLTRA_DIAG;

#if VOLTRA_DIAG
constexpr uint32_t SWEEP_PERIOD_MS = 20000;   // start a full register sweep this often
// Undocumented registers have unknown widths; small batches keep the offline width
// inference unambiguous.
constexpr size_t   SWEEP_CHUNK     = 6;       // ids per read request
constexpr size_t   SWEEP_MAX_QUEUE = 6;       // keep room in the tx queue for other traffic
#endif
constexpr const char *APP_NAME          = "Voltra Remote";

const uint16_t STATUS_PARAMS[] = {
    PARAM_BP_BASE_WEIGHT,
    PARAM_BP_CHAINS_WEIGHT,
    PARAM_BP_ECCENTRIC_WEIGHT,
    PARAM_FITNESS_INVERSE_CHAIN,
    PARAM_BP_SET_FITNESS_MODE,
    PARAM_FITNESS_WORKOUT_STATE,
    PARAM_BMS_RSOC,
    PARAM_BMS_RSOC_LEGACY,
    PARAM_BP_RUNTIME_WIRE_WEIGHT_LBS,
    PARAM_BP_RUNTIME_POSITION_CM,
    PARAM_OVERDRIVE_FORCE_MAX,
    PARAM_MOUNTAIN_CURVE,      // never pushed by the device, so it must be polled
    PARAM_ACCESSORY_DISPLAY_PCT,   // likewise
    PARAM_DIRECT_LOAD_SAFETY_ST,   // tells a finished auto load from one still waiting
#if VOLTRA_DIAG
    0x5467,                        // FITNESS_ONGOING_UI: candidate set / rest indicator
#endif
};
constexpr size_t STATUS_PARAM_COUNT = sizeof(STATUS_PARAMS) / sizeof(STATUS_PARAMS[0]);

/** Accessory amounts as percentages, and the device's limits for them. */
const uint16_t PERCENT_PARAMS[] = {
    PARAM_CHAINS_PCT_X100_S32,
    PARAM_ECCENTRIC_PCT_X100_S32,
    PARAM_MAX_CHAINS_PCT,
    PARAM_MAX_ECCENTRIC_PCT,
};
constexpr size_t PERCENT_PARAM_COUNT = sizeof(PERCENT_PARAMS) / sizeof(PERCENT_PARAMS[0]);

class ScanCb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *dev) override
    {
        std::string name = dev->haveName() ? dev->getName() : std::string();
        bool is_voltra = name.rfind(DEVICE_NAME_PREFIX, 0) == 0 ||
                         name.find("VOLTRA") != std::string::npos ||
                         name.find("Voltra") != std::string::npos ||
                         dev->isAdvertisingService(NimBLEUUID(SERVICE_UUID));
        Client::instance().onScanResult(name, dev->getAddress().toString(), dev->getAddress().getType(),
                                        dev->getRSSI(), is_voltra);
    }
    void onScanEnd(const NimBLEScanResults &, int) override { Client::instance().onScanEnd(); }
};

class ClientCb : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *) override { log_i("GATT connected"); }
    void onConnectFail(NimBLEClient *, int reason) override { log_w("connect failed, reason %d", reason); }
    void onDisconnect(NimBLEClient *, int reason) override { Client::instance().onDisconnected(reason); }
};

ScanCb s_scan_cb;
ClientCb s_client_cb;

void task_entry(void *) { Client::instance().task(); }

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

Client &Client::instance()
{
    static Client c;
    return c;
}

// Null-safe so that reading state before begin() returns defaults instead of
// asserting inside FreeRTOS.
void Client::lockState() const
{
    if (mutex_) xSemaphoreTake((SemaphoreHandle_t)mutex_, portMAX_DELAY);
}

void Client::unlockState() const
{
    if (mutex_) xSemaphoreGive((SemaphoreHandle_t)mutex_);
}

void Client::guardModeEcho()
{
    lockState();
    mode_guard_until_ = millis() + MODE_ECHO_GUARD_MS;
    unlockState();
}

int Client::reportedMode() const
{
    lockState();
    const int m = reported_mode_;
    unlockState();
    return m;
}

void Client::setStatus(const char *s)
{
    lockState();
    strncpy(state_.status, s, sizeof(state_.status) - 1);
    state_.status[sizeof(state_.status) - 1] = 0;
    bump();
    unlockState();
    log_i("status: %s", s);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
void Client::loadSaved()
{
    Preferences p;
    if (p.begin("voltra", true)) {
        saved_addr_ = p.getString("addr", "").c_str();
        saved_type_ = p.getUChar("type", 0);
        saved_name_ = p.getString("name", "").c_str();
        p.end();
    }
}

void Client::saveDevice(const std::string &addr, uint8_t type, const std::string &name)
{
    saved_addr_ = addr;
    saved_type_ = type;
    saved_name_ = name;
    Preferences p;
    if (p.begin("voltra", false)) {
        p.putString("addr", addr.c_str());
        p.putUChar("type", type);
        p.putString("name", name.c_str());
        p.end();
    }
}

void Client::forgetSavedDevice()
{
    lockState();
    saved_addr_.clear();
    saved_name_.clear();
    unlockState();
    Preferences p;
    if (p.begin("voltra", false)) {
        p.clear();
        p.end();
    }
}

std::string Client::savedDeviceName() const
{
    lockState();
    std::string n = saved_name_.empty() ? saved_addr_ : saved_name_;
    unlockState();
    return n;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Client::begin()
{
    mutex_ = xSemaphoreCreateMutex();
    loadSaved();

    NimBLEDevice::init(APP_NAME);
    NimBLEDevice::setMTU(PREFERRED_MTU);
    NimBLEDevice::setPower(9);

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_scan_cb, false);
    scan->setActiveScan(true);
    scan->setInterval(45);
    scan->setWindow(30);
    scan->setMaxResults(0);

    if (!saved_addr_.empty()) {
        target_addr_ = saved_addr_;
        target_type_ = saved_type_;
        target_name_ = saved_name_;
        auto_reconnect_ = true;
        reconnect_delay_ms_ = RECONNECT_MIN_MS;
        reconnect_at_ms_ = millis() + 500;
        lockState();
        state_.conn = ConnState::Reconnecting;
        strncpy(state_.device_name, target_name_.c_str(), sizeof(state_.device_name) - 1);
        strncpy(state_.address, target_addr_.c_str(), sizeof(state_.address) - 1);
        snprintf(state_.status, sizeof(state_.status), "Connecting to %s", target_name_.c_str());
        bump();
        unlockState();
    } else {
        setStatus("Tap to connect");
    }

    xTaskCreatePinnedToCore(task_entry, "voltra", 8192, nullptr, 3, nullptr, 0);
}

void Client::scanStart()
{
    lockState();
    req_.scan_start = true;
    req_.scan_stop = false;
    unlockState();
}

void Client::scanStop()
{
    lockState();
    req_.scan_stop = true;
    req_.scan_start = false;
    unlockState();
}

void Client::connectTo(const FoundDevice &dev)
{
    lockState();
    req_.connect = true;
    req_.target = dev;
    unlockState();
}

void Client::disconnect()
{
    lockState();
    req_.disconnect = true;
    unlockState();
}

void Client::setWeight(int lb)
{
    lockState();
    req_.has_weight = true;
    req_.weight = lb;
    unlockState();
}

void Client::setChains(int value, bool lb)
{
    lockState();
    req_.has_chains = true;
    req_.chains = value;
    req_.chains_lb = lb;
    unlockState();
}

void Client::setEccentric(int value, bool lb)
{
    lockState();
    req_.has_eccentric = true;
    req_.eccentric = value;
    req_.eccentric_lb = lb;
    unlockState();
}

void Client::setInverseChains(bool on)
{
    lockState();
    req_.has_inverse = true;
    req_.inverse = on;
    unlockState();
}

void Client::setMountain(bool on)
{
    lockState();
    req_.has_mountain = true;
    req_.mountain = on;
    unlockState();
}

void Client::load()
{
    lockState();
    req_.load = true;
    req_.unload = false;
    unlockState();
}

void Client::unload()
{
    lockState();
    req_.unload = true;
    req_.load = false;
    req_.auto_load = false;
    unlockState();
}

void Client::autoLoad()
{
    lockState();
    req_.auto_load = true;
    req_.load = false;
    req_.unload = false;
    unlockState();
}

void Client::queueDirectLoadRead()
{
    static const uint16_t ids[] = {PARAM_BP_SET_FITNESS_MODE, PARAM_DIRECT_LOAD_SAFETY_CHECK,
                                   PARAM_DIRECT_LOAD_SAFETY_ST, PARAM_DIRECT_LOAD_COUNTDOWN_MS,
                                   PARAM_DIRECT_LOAD_SAFETY_CTRL};
    uint8_t buf[64];
    const size_t n = build_param_read(buf, sizeof(buf), ids, sizeof(ids) / sizeof(ids[0]), nextSeq());
    enqueue(buf, n);
}

void Client::refresh()
{
    lockState();
    req_.refresh = true;
    unlockState();
}

DeviceState Client::state() const
{
    lockState();
    DeviceState s = state_;
    unlockState();
    return s;
}

std::vector<FoundDevice> Client::devices() const
{
    lockState();
    std::vector<FoundDevice> d = devices_;
    unlockState();
    return d;
}

bool Client::takeRequests(Requests &out)
{
    lockState();
    out = req_;
    req_ = Requests();
    unlockState();
    return out.scan_start || out.scan_stop || out.connect || out.disconnect || out.refresh || out.load ||
           out.unload || out.auto_load || out.has_weight || out.has_chains || out.has_eccentric ||
           out.has_inverse || out.has_mountain;
}

// ---------------------------------------------------------------------------
// Callbacks from the NimBLE host task
// ---------------------------------------------------------------------------
void Client::onScanResult(const std::string &name, const std::string &addr, uint8_t addr_type, int rssi, bool is_voltra)
{
    if (!is_voltra) return;
    lockState();
    bool found = false;
    for (FoundDevice &d : devices_) {
        if (d.address == addr) {
            d.rssi = rssi;
            d.last_seen_ms = millis();
            if (!name.empty()) d.name = name;
            found = true;
            break;
        }
    }
    if (!found) {
        FoundDevice d;
        d.name = name.empty() ? addr : name;
        d.address = addr;
        d.addr_type = addr_type;
        d.rssi = rssi;
        d.last_seen_ms = millis();
        devices_.push_back(d);
        log_i("found %s (%s) rssi %d", d.name.c_str(), addr.c_str(), rssi);
    }
    devices_version_++;
    unlockState();
}

void Client::onScanEnd() { scan_ended_flag_ = true; }

void Client::onDisconnected(int reason)
{
    log_w("disconnected, reason %d", reason);
    disconnected_flag_ = true;
}

void Client::onNotify(uint8_t slot, const uint8_t *data, size_t len)
{
    if (slot >= 3) return;
    assemblers_[slot].accept(data, len,
        [](void *ctx, const uint8_t *frame, size_t n) { static_cast<Client *>(ctx)->handleFrame(frame, n); }, this);
}

void Client::handleFrame(const uint8_t *frame, size_t len)
{
    Packet pkt;
    if (!parse_packet(frame, len, pkt)) return;
    if (!pkt.crc_ok) log_d("frame with bad crc, cmd 0x%02x", pkt.cmd);

    ParamValue vals[16];
    size_t n = decode_params(pkt, vals, 16);

    if (s_log_params && (pkt.cmd == CMD_PARAM_READ || pkt.cmd == CMD_ASYNC_STATE)) {
        // Raw payload so parameters missing from the registry can still be decoded
        // offline while hunting for a reliable "is loaded" signal.
        // Static: long sweep responses would not fit comfortably on the NimBLE host
        // task's stack, and handleFrame only ever runs on that one task.
        static char hex[2 * 300 + 1];
        int o = 0;
        for (size_t i = 0; i < pkt.payload_len && o < (int)sizeof(hex) - 3; i++) {
            o += snprintf(hex + o, sizeof(hex) - o, "%02x", pkt.payload[i]);
        }
        log_i("rx %02x [%u] %s", pkt.cmd, (unsigned)pkt.payload_len, hex);
    }
    if (s_log_params && pkt.cmd == CMD_TELEMETRY) {
        // Workout telemetry streams quickly; log a frame only when its first bytes change.
        static uint8_t last[12];
        static size_t last_len = 0;
        const size_t k = std::min(pkt.payload_len, sizeof(last));
        if (k != last_len || memcmp(last, pkt.payload, k) != 0) {
            memcpy(last, pkt.payload, k);
            last_len = k;
            static char hex[2 * 40 + 1];
            int o = 0;
            for (size_t i = 0; i < pkt.payload_len && i < 40; i++) {
                o += snprintf(hex + o, sizeof(hex) - o, "%02x", pkt.payload[i]);
            }
            log_i("rx aa [%u] %s", (unsigned)pkt.payload_len, hex);
        }
    }

    RepTelemetry rep;
    bool has_rep = parse_rep_telemetry(pkt, rep);
    const int workout_status = parse_workout_status(pkt);
    int activation = parse_activation(pkt);

    lockState();
    state_.last_rx_ms = millis();
    bool changed = false;

    switch (pkt.cmd) {
        case CMD_PARAM_READ:
        case CMD_ASYNC_STATE:
        case CMD_DEVICE_NAME:
        case CMD_COMMON_STATE:
        case CMD_FIRMWARE_INFO:
        case CMD_SERIAL_INFO:
        case CMD_ACTIVATION:
            if (!state_.protocol_ok) {
                state_.protocol_ok = true;
                changed = true;
            }
            break;
        default:
            break;
    }

    for (size_t i = 0; i < n; i++) {
        int v = (int)vals[i].value;
        switch (vals[i].id) {
            case PARAM_BP_BASE_WEIGHT:
                if (state_.weight != v) { state_.weight = v; changed = true; }
                break;
            case PARAM_BP_CHAINS_WEIGHT:
                if (state_.chains_lb != v) { state_.chains_lb = v; changed = true; }
                break;
            case PARAM_CHAINS_PCT_X100_S32: {
                const int pct = v / 100;
                if (state_.chains != pct) { state_.chains = pct; changed = true; }
                break;
            }
            case PARAM_ECCENTRIC_PCT_X100_S32: {
                const int pct = v / 100;
                if (state_.eccentric != pct || !state_.eccentric_known) {
                    state_.eccentric = pct;
                    state_.eccentric_known = true;
                    changed = true;
                }
                break;
            }
            case PARAM_MAX_CHAINS_PCT:
                if (v > 0 && state_.max_chains_pct != v) { state_.max_chains_pct = v; changed = true; }
                break;
            case PARAM_MAX_ECCENTRIC_PCT:
                if (v > 0 && state_.max_ecc_pct != v) { state_.max_ecc_pct = v; changed = true; }
                break;
            case PARAM_BP_ECCENTRIC_WEIGHT:
                if (state_.eccentric_lb != v) { state_.eccentric_lb = v; changed = true; }
                break;
            case PARAM_MOUNTAIN_CURVE: {
                const int on = (v == 1) ? 1 : 0;
                if (state_.mountain != on) { state_.mountain = on; changed = true; }
                break;
            }
            case PARAM_ACCESSORY_DISPLAY_PCT: {
                const int pct = (v == 1) ? 1 : 0;
                if (state_.accessory_display != pct) { state_.accessory_display = pct; changed = true; }
                break;
            }
            case PARAM_OVERDRIVE_FORCE_MAX: {
                // Overdrive firmware raises the ceiling; ignore values below the classic
                // limit, which is what the device reports when overdrive is not set up.
                const int ceiling = (v >= MAX_TARGET_LB_CLASSIC && v <= MAX_TARGET_LB_OVERDRIVE)
                                        ? v : MAX_TARGET_LB;
                if (state_.max_weight != ceiling) { state_.max_weight = ceiling; changed = true; }
                break;
            }
            case PARAM_FITNESS_INVERSE_CHAIN:
                v = (v == 1) ? 1 : 0;
                if (state_.inverse_chains != v) { state_.inverse_chains = v; changed = true; }
                break;
            case PARAM_BP_SET_FITNESS_MODE: {
                // The Voltra asynchronously echoes its whole settings block after a
                // settings write and after reps, and the fitness mode in that echo is
                // stale. Captured 100 ms apart while the device was loaded:
                //
                //   0x0f read        3E89 = 5   (truth)
                //   0x10 push, 9 par 3E89 = 1   (stale echo)
                //
                // A single-parameter async push is a genuine mode-change notification;
                // a wider one is an echo. Anything we skip here is corrected by the
                // parameter read that follows every write and repeats every 3 s.
                if (pkt.cmd == CMD_ASYNC_STATE && n > 1) {
                    log_d("ignoring fitness mode %d in %u-parameter async echo", v, (unsigned)n);
                    break;
                }
                if (pkt.cmd == CMD_ASYNC_STATE && (int32_t)(millis() - mode_guard_until_) < 0) {
                    log_d("ignoring async fitness mode echo %d during guard window", v);
                    break;
                }
                reported_mode_ = v;
                // While a load is in progress keep showing the loaded state rather than
                // flickering through the 0 it may start from.
                if (load_intent_ && load_intent_target_ && !fitness_mode_loaded(v)) {
                    break;
                }
                if (state_.fitness_mode != v) { state_.fitness_mode = v; changed = true; }
                break;
            }
            case PARAM_DIRECT_LOAD_SAFETY_ST:
                if (state_.direct_load_status != v) { state_.direct_load_status = v; changed = true; }
                break;
            case PARAM_DIRECT_LOAD_COUNTDOWN_MS:
                if (state_.direct_load_countdown_ms != v) { state_.direct_load_countdown_ms = v; changed = true; }
                break;
            case PARAM_FITNESS_WORKOUT_STATE:
                if (state_.workout_state != v) { state_.workout_state = v; changed = true; }
                break;
            case PARAM_BMS_RSOC:
            case PARAM_BMS_RSOC_LEGACY:
                if (v >= 0 && v <= 100 && state_.battery != v) { state_.battery = v; changed = true; }
                break;
            case PARAM_BP_RUNTIME_WIRE_WEIGHT_LBS:
                if (state_.force_lb != v || !state_.force_known) { state_.force_lb = v; state_.force_known = true; changed = true; }
                break;
            case PARAM_BP_RUNTIME_POSITION_CM:
                if (state_.position_cm != v) { state_.position_cm = v; changed = true; }
                break;
            default:
                break;
        }
    }

    if (has_rep) {
        if (state_.reps != rep.rep_count || state_.sets != rep.set_count || state_.rep_phase != rep.phase) {
            state_.reps = rep.rep_count;
            state_.sets = rep.set_count;
            state_.rep_phase = rep.phase;
            changed = true;
        }
    }
    if (workout_status >= 0 && state_.workout_status != workout_status) {
        state_.workout_status = workout_status;
        changed = true;
    }
    if (activation >= 0 && state_.activation != activation) {
        state_.activation = activation;
        changed = true;
    }

    if (state_.conn == ConnState::Handshaking && state_.protocol_ok) {
        state_.conn = ConnState::Ready;
        snprintf(state_.status, sizeof(state_.status), "Connected");
        changed = true;
    }
    if (changed) bump();
    unlockState();
}

// ---------------------------------------------------------------------------
// Worker task helpers
// ---------------------------------------------------------------------------
void Client::enqueue(const uint8_t *data, size_t len)
{
    if (len == 0 || len > sizeof(queue_[0].data)) return;
    uint8_t next = (uint8_t)((q_tail_ + 1) % (sizeof(queue_) / sizeof(queue_[0])));
    if (next == q_head_) {
        log_w("tx queue full, dropping frame");
        return;
    }
    memcpy(queue_[q_tail_].data, data, len);
    queue_[q_tail_].len = (uint8_t)len;
    q_tail_ = next;
}

void Client::queueBootstrap()
{
    for (size_t i = 0; i < BOOTSTRAP_FRAME_COUNT; i++) {
        enqueue(BOOTSTRAP_FRAMES[i].data, BOOTSTRAP_FRAMES[i].len);
    }
    // battery + mode/feature state, same order as the official app
    uint8_t buf[64];
    const uint16_t bat[] = {PARAM_BMS_RSOC, PARAM_BMS_RSOC_LEGACY};
    size_t n = build_param_read(buf, sizeof(buf), bat, 2, 5);
    enqueue(buf, n);
    seq_ = 6;
    queueStatusRead();
    seq_ = std::max<uint16_t>(seq_, 7);
}

void Client::queueStatusRead()
{
    uint8_t buf[64];
    for (size_t i = 0; i < STATUS_PARAM_COUNT; i += PARAMS_PER_READ) {
        size_t cnt = std::min(PARAMS_PER_READ, STATUS_PARAM_COUNT - i);
        size_t n = build_param_read(buf, sizeof(buf), &STATUS_PARAMS[i], cnt, nextSeq());
        enqueue(buf, n);
    }
    for (size_t i = 0; i < PERCENT_PARAM_COUNT; i += PARAMS_PER_READ) {
        const size_t cnt = std::min(PARAMS_PER_READ, PERCENT_PARAM_COUNT - i);
        size_t n = build_param_read(buf, sizeof(buf), &PERCENT_PARAMS[i], cnt, nextSeq());
        enqueue(buf, n);
    }
}

void Client::queueParamWriteU16(uint16_t param, uint16_t value)
{
    uint8_t buf[64];
    size_t n = build_param_write_u16(buf, sizeof(buf), param, value, nextSeq());
    enqueue(buf, n);
}

void Client::queueParamWriteI32(uint16_t param, int32_t value)
{
    uint8_t buf[64];
    const uint32_t u = (uint32_t)value;
    const uint8_t v[4] = {(uint8_t)(u & 0xFF), (uint8_t)((u >> 8) & 0xFF),
                          (uint8_t)((u >> 16) & 0xFF), (uint8_t)((u >> 24) & 0xFF)};
    size_t n = build_param_write(buf, sizeof(buf), param, v, sizeof(v), nextSeq());
    enqueue(buf, n);
}

void Client::queueParamWriteU8(uint16_t param, uint8_t value)
{
    uint8_t buf[64];
    size_t n = build_param_write_u8(buf, sizeof(buf), param, value, nextSeq());
    enqueue(buf, n);
}

bool Client::writeNext()
{
    if (queueEmpty() || !write_chr_) return true;
    auto *chr = static_cast<NimBLERemoteCharacteristic *>(write_chr_);
    Frame &f = queue_[q_head_];
#if VOLTRA_DIAG
    if (s_log_params && f.len >= 17 && f.data[10] == CMD_PARAM_WRITE) {
        // payload: 01 00 | param lo hi | value...
        const uint16_t param = (uint16_t)(f.data[13] | (f.data[14] << 8));
        char hex[24];
        int o = 0;
        for (size_t i = 15; i + 2 < f.len && o < (int)sizeof(hex) - 3; i++) {
            o += snprintf(hex + o, sizeof(hex) - o, "%02x", f.data[i]);
        }
        log_i("tx write %04X = %s", param, hex);
    }
#endif
    bool ok = chr->writeValue(f.data, f.len, chr->canWrite());
    if (!ok) {
        log_w("write failed (cmd 0x%02x)", f.data[10]);
    }
    q_head_ = (uint8_t)((q_head_ + 1) % (sizeof(queue_) / sizeof(queue_[0])));
    return ok;
}

void Client::startScan()
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan->isScanning()) return;
    scan->clearResults();
    if (scan->start(SCAN_PERIOD_MS, false, true)) {
        scanning_ = true;
        lockState();
        if (!client_) {
            state_.conn = ConnState::Scanning;
            snprintf(state_.status, sizeof(state_.status), "Scanning");
        }
        bump();
        unlockState();
    } else {
        log_w("scan start failed");
    }
}

void Client::stopScan()
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan->isScanning()) scan->stop();
    scanning_ = false;
}

void Client::teardown()
{
    write_chr_ = nullptr;
    if (client_) {
        auto *c = static_cast<NimBLEClient *>(client_);
        client_ = nullptr;
        if (c->isConnected()) c->disconnect();
        NimBLEDevice::deleteClient(c);
    }
    for (FrameAssembler &a : assemblers_) a.clear();
    q_head_ = q_tail_ = 0;
    lockState();
    state_.protocol_ok = false;
    state_.reps = 0;
    state_.sets = 0;
    state_.force_known = false;
    bump();
    unlockState();
}

bool Client::doConnect(const std::string &addr, uint8_t addr_type)
{
    NimBLEClient *c = NimBLEDevice::createClient();
    if (!c) return false;
    c->setClientCallbacks(&s_client_cb, false);
    c->setConnectTimeout(10000);
    c->setConnectionParams(12, 24, 0, 400);

    NimBLEAddress address(addr, addr_type);
    log_i("connecting to %s", addr.c_str());
    if (!c->connect(address, true, false, true)) {
        log_w("connect() failed");
        NimBLEDevice::deleteClient(c);
        return false;
    }
    client_ = c;

    NimBLERemoteService *svc = c->getService(NimBLEUUID(SERVICE_UUID));
    if (!svc) {
        log_w("Voltra service not found");
        return false;
    }
    NimBLERemoteCharacteristic *wr = svc->getCharacteristic(NimBLEUUID(CHAR_TRANSPORT_UUID));
    if (!wr) {
        log_w("transport characteristic not found");
        return false;
    }
    write_chr_ = wr;

    // The Voltra only accepts the app hello for a short window after connecting,
    // so send it before spending time on descriptor writes.
    uint8_t hello[64];
    size_t n = build_app_hello(hello, sizeof(hello), APP_NAME);
    if (!wr->writeValue(hello, n, wr->canWrite())) {
        log_w("hello write failed");
        return false;
    }
    log_i("hello sent (mtu %u)", c->getMTU());

    const char *uuids[3] = {CHAR_COMMAND_UUID, CHAR_NOTIFY_UUID, CHAR_TRANSPORT_UUID};
    for (uint8_t slot = 0; slot < 3; slot++) {
        NimBLERemoteCharacteristic *ch = svc->getCharacteristic(NimBLEUUID(uuids[slot]));
        if (ch && ch->canNotify()) {
            bool ok = ch->subscribe(true,
                [slot](NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
                    Client::instance().onNotify(slot, data, len);
                }, true);
            log_i("subscribe %s: %s", uuids[slot], ok ? "ok" : "failed");
            (void)ok;   // only logged
        }
    }

    for (FrameAssembler &a : assemblers_) a.clear();
    q_head_ = q_tail_ = 0;
    queueBootstrap();
    last_write_ms_ = millis();
    handshake_started_ms_ = millis();
    last_poll_ms_ = millis();

    lockState();
    state_.conn = ConnState::Handshaking;
    state_.protocol_ok = false;
    strncpy(state_.device_name, target_name_.c_str(), sizeof(state_.device_name) - 1);
    strncpy(state_.address, addr.c_str(), sizeof(state_.address) - 1);
    snprintf(state_.status, sizeof(state_.status), "Handshaking");
    bump();
    unlockState();
    return true;
}

// ---------------------------------------------------------------------------
// Worker task
// ---------------------------------------------------------------------------
void Client::task()
{
    for (;;) {
        Requests r;
        takeRequests(r);
        uint32_t now = millis();

        if (r.disconnect) {
            auto_reconnect_ = false;
            target_addr_.clear();
            teardown();
            lockState();
            state_.conn = ConnState::Idle;
            snprintf(state_.status, sizeof(state_.status), "Disconnected");
            bump();
            unlockState();
        }

        if (r.connect) {
            if (client_) teardown();
            stopScan();
            auto_reconnect_ = true;
            target_addr_ = r.target.address;
            target_type_ = r.target.addr_type;
            target_name_ = r.target.name;
            reconnect_delay_ms_ = RECONNECT_MIN_MS;
            reconnect_at_ms_ = now;
        }

        if (r.scan_stop) {
            want_scan_ = false;
            stopScan();
            lockState();
            if (state_.conn == ConnState::Scanning) {
                state_.conn = client_ ? ConnState::Ready : (auto_reconnect_ ? ConnState::Reconnecting : ConnState::Idle);
                bump();
            }
            unlockState();
        }
        if (r.scan_start) {
            want_scan_ = true;
            lockState();
            devices_.clear();
            devices_version_++;
            unlockState();
            startScan();
        }

        if (disconnected_flag_) {
            disconnected_flag_ = false;
            teardown();
            lockState();
            if (auto_reconnect_ && !target_addr_.empty()) {
                state_.conn = ConnState::Reconnecting;
                snprintf(state_.status, sizeof(state_.status), "Reconnecting");
                reconnect_at_ms_ = now + reconnect_delay_ms_;
            } else {
                state_.conn = ConnState::Idle;
                snprintf(state_.status, sizeof(state_.status), "Disconnected");
            }
            bump();
            unlockState();
        }

        if (scan_ended_flag_) {
            scan_ended_flag_ = false;
            scanning_ = false;
            // Keep scanning in repeating windows while the connect screen is open,
            // including while already connected (so the user can switch devices).
            if (want_scan_) startScan();
        }

        // (Re)connect attempt
        if (!client_ && auto_reconnect_ && !target_addr_.empty() && (int32_t)(now - reconnect_at_ms_) >= 0) {
            bool was_scanning = scanning_;
            stopScan();
            lockState();
            state_.conn = ConnState::Connecting;
            snprintf(state_.status, sizeof(state_.status), "Connecting");
            bump();
            unlockState();

            if (doConnect(target_addr_, target_type_)) {
                reconnect_delay_ms_ = RECONNECT_MIN_MS;
                saveDevice(target_addr_, target_type_, target_name_);
            } else {
                teardown();
                reconnect_delay_ms_ = std::min<uint32_t>(reconnect_delay_ms_ * 2, RECONNECT_MAX_MS);
                reconnect_at_ms_ = millis() + reconnect_delay_ms_;
                lockState();
                state_.conn = ConnState::Reconnecting;
                snprintf(state_.status, sizeof(state_.status), "Retrying in %lus", (unsigned long)(reconnect_delay_ms_ / 1000));
                bump();
                unlockState();
                if (want_scan_ || was_scanning) startScan();
            }
            continue;
        }

        if (client_) {
            bool wrote_setting = false;
            int base = state().weight;
            if (r.has_weight) {
                base = clampi(r.weight, MIN_TARGET_LB, MAX_TARGET_LB);
                queueParamWriteU16(PARAM_BP_BASE_WEIGHT, (uint16_t)base);
                wrote_setting = true;
            }
            if (base < MIN_TARGET_LB) base = MAX_TARGET_LB;   // unknown base: only clamp to absolute limits
            // Chains and eccentric are written in the unit the Voltra is set to. The other
            // pair is derived: in percent mode the device acknowledges a write to the pound
            // registers (0x3E87/0x3E88) and then reverts it ~100 ms later from percent x
            // base, and in pound mode a percent write goes nowhere.
            const DeviceState snap = state();
            // Chains, inverse chains and mountain share the chains amount; the two flags
            // pick which one it drives. Write them before the amount so the Voltra never
            // briefly applies the new amount in the old style.
            if (r.has_inverse) {
                queueParamWriteU8(PARAM_FITNESS_INVERSE_CHAIN, r.inverse ? 1 : 0);
                wrote_setting = true;
            }
            if (r.has_mountain) {
                queueParamWriteU8(PARAM_MOUNTAIN_CURVE, r.mountain ? 1 : 0);
                wrote_setting = true;
            }
            if (r.has_chains) {
                if (r.chains_lb) {
                    const int lb = clampi(r.chains, MIN_CHAINS_LB, MAX_CHAINS_LB);
                    queueParamWriteU16(PARAM_BP_CHAINS_WEIGHT, (uint16_t)lb);
                } else {
                    const int pct = clampi(r.chains, 0, snap.max_chains_pct);
                    queueParamWriteI32(PARAM_CHAINS_PCT_X100_S32, pct * 100);
                }
                wrote_setting = true;
            }
            if (r.has_eccentric) {
                if (r.eccentric_lb) {
                    const int lb = clampi(r.eccentric, MIN_ECCENTRIC_LB, MAX_ECCENTRIC_LB);
                    queueParamWriteU16(PARAM_BP_ECCENTRIC_WEIGHT, (uint16_t)(int16_t)lb);   // i16
                } else {
                    const int lim = snap.max_ecc_pct;
                    const int pct = clampi(r.eccentric, -lim, lim);
                    queueParamWriteI32(PARAM_ECCENTRIC_PCT_X100_S32, pct * 100);
                }
                wrote_setting = true;
            }
            if (r.load) {
                DeviceState s = state();
                if (s.workout_state <= WORKOUT_STATE_INACTIVE) {
                    queueParamWriteU8(PARAM_FITNESS_WORKOUT_STATE, WORKOUT_STATE_ACTIVE);
                }
                queueParamWriteU16(PARAM_BP_SET_FITNESS_MODE, FITNESS_MODE_STRENGTH_LOADED);
                guardModeEcho();
                load_intent_ = true;
                load_intent_target_ = true;
                load_retries_ = 0;
                load_next_retry_ms_ = now + LOAD_RETRY_MS;
                wrote_setting = true;
            }
            if (r.auto_load) {
                // Same sequence as the Android port's "Direct Load at distance": make sure
                // weight training is active, trigger, read the status, refresh the stream.
                DeviceState s = state();
                if (s.workout_state <= WORKOUT_STATE_INACTIVE) {
                    queueParamWriteU8(PARAM_FITNESS_WORKOUT_STATE, WORKOUT_STATE_ACTIVE);
                }
                uint8_t buf[32];
                const uint8_t trigger[] = {VENDOR_DIRECT_LOAD};
                enqueue(buf, build_vendor(buf, sizeof(buf), trigger, sizeof(trigger), nextSeq()));
                queueDirectLoadRead();
                const uint8_t refresh[] = {VENDOR_STATE_REFRESH, 0x01};
                enqueue(buf, build_vendor(buf, sizeof(buf), refresh, sizeof(refresh), nextSeq()));
                load_intent_ = false;
                auto_load_watch_until_ = now + AUTO_LOAD_WATCH_MS;
                auto_load_started_ms_ = now;
                last_auto_load_poll_ms_ = now;
            }
            if (r.unload) {
                auto_load_watch_until_ = 0;
                queueParamWriteU16(PARAM_BP_SET_FITNESS_MODE, FITNESS_MODE_STRENGTH_READY);
                guardModeEcho();
                load_intent_ = true;
                load_intent_target_ = false;
                load_retries_ = 0;
                load_next_retry_ms_ = now + LOAD_RETRY_MS;
                wrote_setting = true;
            }
            // Changing weight/chains/eccentric drops the Voltra from an active set (5) to
            // idle (1): a direct read right after a weight write returns "3E86=85 ...
            // 3E89=1". It stays loaded, but if a set was active, drive it back so the new
            // setting applies to the set in progress. Idle (resting between sets) is left
            // alone, so a change during rest does not cut the rest short.
            //
            // This can never start a load the user did not ask for, because the intent is
            // only armed when a set is already active, and the retry below re-checks the
            // reported mode before writing anything.
            const int mode_before = state().fitness_mode;
            if (wrote_setting && !r.load && !r.unload && mode_before >= 0 &&
                (mode_before & 0xFF) == FITNESS_MODE_STRENGTH_LOADED) {
                load_intent_ = true;
                load_intent_target_ = true;
                load_retries_ = 0;
                load_next_retry_ms_ = now + LOAD_RETRY_MS;
            }

            if (wrote_setting || r.refresh) {
                // Read the state back after a change, but not while writes are still
                // draining: a long turn of the dial produces a value change every few
                // hundred ms, and three frames per change would outrun the 90 ms write
                // pacing and eventually overflow the queue. The periodic poll below
                // reconciles a moment later instead.
                if (queueDepth() <= READBACK_MAX_QUEUE_DEPTH) {
                    queueStatusRead();
                    last_poll_ms_ = now;
                }
            }

            ConnState conn = state().conn;
            if (conn == ConnState::Handshaking && now - handshake_started_ms_ > HANDSHAKE_TIMEOUT_MS) {
                log_w("handshake timed out, reconnecting");
                teardown();
                reconnect_at_ms_ = millis() + RECONNECT_MIN_MS;
                lockState();
                state_.conn = ConnState::Reconnecting;
                snprintf(state_.status, sizeof(state_.status), "No response, retrying");
                bump();
                unlockState();
                continue;
            }
            // The Voltra advances 0 -> 1 -> 5 one command at a time, so a single write
            // frequently stops at an intermediate state. Keep re-issuing until the
            // reported mode matches what the user asked for.
            if (load_intent_ && queueEmpty() && (int32_t)(now - load_next_retry_ms_) >= 0) {
                const int mode = reportedMode();
                // A load is finished at an active set (5); an unload only once the device
                // has left both loaded states, since idle (1) still has the weight on.
                const bool done = load_intent_target_
                                      ? mode >= 0 && (mode & 0xFF) == FITNESS_MODE_STRENGTH_LOADED
                                      : mode >= 0 && !voltra_loaded(mode, state().direct_load_status);
                if (done) {
                    load_intent_ = false;
                } else if (load_retries_ < LOAD_MAX_RETRIES) {
                    load_retries_++;
                    log_i("%s retry %u (mode=%d)", load_intent_target_ ? "load" : "unload",
                          load_retries_, mode);
                    queueParamWriteU16(PARAM_BP_SET_FITNESS_MODE,
                                       load_intent_target_ ? FITNESS_MODE_STRENGTH_LOADED
                                                           : FITNESS_MODE_STRENGTH_READY);
                    guardModeEcho();
                    queueStatusRead();
                    last_poll_ms_ = now;
                    load_next_retry_ms_ = now + LOAD_RETRY_MS;
                } else {
                    load_intent_ = false;
                    log_w("%s did not reach the target state (mode=%d)",
                          load_intent_target_ ? "load" : "unload", mode);
                }
            }

#if VOLTRA_DIAG
            // Diagnostic sweep: read every BP/MC register so snapshots taken in different
            // device states can be diffed offline to find undocumented registers.
            if (conn == ConnState::Ready) {
                if (!sweep_active_ && !auto_load_watch_until_ && (int32_t)(now - sweep_next_ms_) >= 0) {
                    sweep_active_ = true;
                    sweep_pos_ = 0;
                    log_i("sweep begin (%u params)", (unsigned)SWEEP_PARAM_COUNT);
                }
                const bool poll_due = now - last_poll_ms_ >= STATUS_POLL_MS;
                while (sweep_active_ && !auto_load_watch_until_ && !poll_due &&
                       queueDepth() < SWEEP_MAX_QUEUE) {
                    const size_t cnt = std::min(SWEEP_CHUNK, SWEEP_PARAM_COUNT - sweep_pos_);
                    uint8_t buf[64];
                    const size_t n = build_param_read(buf, sizeof(buf), &SWEEP_PARAMS[sweep_pos_], cnt, nextSeq());
                    enqueue(buf, n);
                    sweep_pos_ += cnt;
                    if (sweep_pos_ >= SWEEP_PARAM_COUNT) {
                        sweep_active_ = false;
                        sweep_next_ms_ = now + SWEEP_PERIOD_MS;
                        log_i("sweep queued");
                    }
                }
            }
#endif

            // Auto load: poll its countdown quickly until it finishes or the window closes.
            if (conn == ConnState::Ready && auto_load_watch_until_) {
                const DeviceState s = state();
                const bool expired = (int32_t)(now - auto_load_watch_until_) >= 0;
                // Done once loaded and out of the auto-load modes. Wait a moment first: the
                // mode only changes after the trigger has gone out.
                const bool finished = now - auto_load_started_ms_ > 2000 && s.loaded() &&
                                      !s.auto_loading() && s.direct_load_countdown_ms == 0;
                if (expired || finished) {
                    auto_load_watch_until_ = 0;
                } else if (queueEmpty() && now - last_auto_load_poll_ms_ >= AUTO_LOAD_POLL_MS) {
                    queueDirectLoadRead();
                    last_auto_load_poll_ms_ = now;
                }
            }
            if (conn == ConnState::Ready && queueEmpty() && now - last_poll_ms_ >= STATUS_POLL_MS) {
                queueStatusRead();
                last_poll_ms_ = now;
            }
            if (!queueEmpty() && now - last_write_ms_ >= WRITE_PACING_MS) {
                if (!writeNext()) {
                    auto *c = static_cast<NimBLEClient *>(client_);
                    if (!c->isConnected()) disconnected_flag_ = true;
                }
                last_write_ms_ = millis();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace voltra
