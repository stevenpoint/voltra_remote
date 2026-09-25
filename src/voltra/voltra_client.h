#pragma once
/*
 * BLE client for the Beyond Power Voltra (NimBLE, runs in its own FreeRTOS task).
 *
 * The UI talks to it through "desired state" requests that are coalesced, so a
 * fast-turning dial never floods the link; the task paces writes at ~90 ms like
 * the official app and reads the device state back after every write.
 */
#include <stdint.h>

#include <atomic>
#include <string>
#include <vector>

#include "voltra_protocol.h"

namespace voltra {

enum class ConnState : uint8_t {
    Idle,          // no target, not scanning
    Scanning,      // looking for Voltras
    Connecting,    // GATT connect + discovery
    Handshaking,   // bootstrap sent, waiting for first parsed response
    Ready,         // connected and validated
    Reconnecting,  // link lost, retrying
};

struct FoundDevice {
    std::string name;
    std::string address;
    uint8_t addr_type = 0;
    int rssi = 0;
    uint32_t last_seen_ms = 0;
};

struct DeviceState {
    ConnState conn = ConnState::Idle;
    char device_name[24] = {0};
    char address[20] = {0};
    char status[40] = {0};
    bool protocol_ok = false;

    int battery = -1;          // %
    int weight = -1;           // base weight lb (-1 unknown)
    // Chains and eccentric exist in both units. Which pair is authoritative follows the
    // Voltra's lb/% setting (accessory_display); the device derives the other pair.
    int chains = -1;           // percent of the base weight
    int eccentric = 0;         // percent of the base weight, +/-
    int chains_lb = 0;         // pounds
    int eccentric_lb = 0;      // pounds, +/-
    bool eccentric_known = false;
    int inverse_chains = -1;   // 1/0, -1 unknown
    int mountain = -1;         // 1 on / 0 off / -1 unknown
    /** The Voltra's lb/% display setting for accessories: 1 = percent, 0 = pounds, -1 unknown. */
    int accessory_display = -1;
    int max_weight = MAX_TARGET_LB;   // effective ceiling, raised by overdrive firmware
    int max_chains_pct = 100;  // device limit when in percent mode
    int max_ecc_pct = 60;      // device limit when in percent mode
    int fitness_mode = -1;     // raw PARAM_BP_SET_FITNESS_MODE
    int direct_load_countdown_ms = 0;   // auto load: time left before it loads, 0 = none
    int direct_load_status = -1;        // auto load progress (DIRECT_LOAD_ST_*), -1 unknown
    int workout_state = -1;    // raw PARAM_FITNESS_WORKOUT_STATE
    int activation = -1;       // 1 activated / 0 / -1 unknown
    int force_lb = 0;
    bool force_known = false;
    int position_cm = 0;
    /** Bumped each time a load gives up without the weight going on (e.g. cable pulled out). */
    uint32_t load_refused = 0;
    uint16_t reps = 0;
    uint8_t sets = 0;
    uint8_t rep_phase = 0;
    int workout_status = -1;   // WORKOUT_STATUS_*, -1 not seen yet
    uint32_t last_rx_ms = 0;
    /** Twin mode (docs/PROTOCOL.md, "Twin mode"): TWIN_STATE_*, -1 unknown. */
    int twin_state = -1;
    char twin_peer[20] = {0};   // the other unit's address while twinned

    bool twinned() const { return twin_state == TWIN_STATE_TWINNED; }
    bool connected() const { return conn == ConnState::Handshaking || conn == ConnState::Ready; }
    /** The Voltra's auto load is running: waiting for the cable pull, or counting down. */
    bool auto_loading() const { return voltra_auto_loading(fitness_mode, direct_load_status); }
    /** Weight on: a set active, idle/resting between sets, or loaded by auto load. */
    bool loaded() const { return voltra_loaded(fitness_mode, direct_load_status); }
    /**
     * True when the Voltra is set to pounds for accessories. In that mode the pound
     * registers hold the setting and the percentages are derived; in percent mode it is
     * the other way round, and a write to the derived pair is reverted within ~100 ms.
     */
    bool accessory_lb() const { return accessory_display == 0; }
    const char *accessory_unit() const { return accessory_lb() ? "lb" : "%"; }
};

class Client {
public:
    static Client &instance();

    /** Initialise NimBLE and start the worker task. Auto-connects to the saved device if any. */
    void begin();

    // --- requests (safe from any task) ---------------------------------------
    void scanStart();
    void scanStop();
    void connectTo(const FoundDevice &dev);
    void disconnect();                 // user initiated; disables auto-reconnect
    void forgetSavedDevice();

    void setWeight(int lb);
    /** Amounts are in pounds when `lb` is true, else percent of the base weight. */
    void setChains(int value, bool lb);
    void setEccentric(int value, bool lb);
    void setInverseChains(bool on);
    void setMountain(bool on);
    void load();
    void unload();
    /** Start the Voltra's auto load: it loads once the cable is pulled out and held. */
    void autoLoad();
    /**
     * Load with the cable pulled out, which a plain load() cannot (the Voltra refuses it).
     * Starts auto load, then writes 2 ("bypass") to DIRECT_LOAD_SAFETY_CHECK_CTRL while it
     * waits, so it loads at once without the pull-and-hold countdown. The watch sends this
     * only when the user taps "override" after a refused load. See docs/PROTOCOL.md,
     * "Loading with the cable out".
     */
    void loadOverride();
    void refresh();
    /**
     * Twin the connected Voltra (the host) with `follower`, the way Beyond+ does: open a
     * short second connection to the follower and tell it to join this host. The watch
     * keeps its connection to the host, which stops advertising while it hosts, so the
     * pair can only be driven through a connection made before twinning.
     */
    void twinWith(const FoundDevice &follower);
    /** Un-twin: tell the host to drop its follower. */
    void untwin();

    // --- state --------------------------------------------------------------
    DeviceState state() const;
    uint32_t version() const { return version_.load(std::memory_order_relaxed); }
    std::vector<FoundDevice> devices() const;
    uint32_t devicesVersion() const { return devices_version_.load(std::memory_order_relaxed); }
    bool hasSavedDevice() const { return !saved_addr_.empty(); }
    std::string savedDeviceName() const;

    // internal (public for the static callbacks)
    void onScanResult(const std::string &name, const std::string &addr, uint8_t addr_type, int rssi, bool is_voltra);
    void onScanEnd();
    void onDisconnected(int reason);
    void onNotify(uint8_t slot, const uint8_t *data, size_t len);
    void onAuxNotify(uint8_t slot, const uint8_t *data, size_t len);
    void onAuxDisconnected() { aux_disconnected_ = true; }
    void task();

private:
    Client() = default;

    struct Requests {
        bool scan_start = false;
        bool scan_stop = false;
        bool connect = false;
        bool disconnect = false;
        bool refresh = false;
        bool load = false;
        bool unload = false;
        bool auto_load = false;
        bool load_override = false;
        bool twin = false;
        bool untwin = false;
        FoundDevice twin_target;
        bool has_weight = false;
        bool has_chains = false;
        bool has_eccentric = false;
        bool has_inverse = false;
        bool has_mountain = false;
        int weight = 0;
        int chains = 0;
        int eccentric = 0;
        bool inverse = false;
        bool mountain = false;
        bool chains_lb = false;      // chains/eccentric given in pounds rather than percent
        bool eccentric_lb = false;
        FoundDevice target;
    };

    struct Frame {
        uint8_t data[64];
        uint8_t len;
    };

    void noteLoadRefused();
    bool isTwinned() const;
    /** Fitness mode to write for a load or unload: the twin values while twinned. */
    uint16_t loadModeValue(bool load) const;
    bool joinFollower(const FoundDevice &follower, const std::string &host_addr);
    void queueTwinStatusRead();
    void lockState() const;
    void unlockState() const;
    void bump() { version_++; }

    bool takeRequests(Requests &out);
    void handleFrame(const uint8_t *frame, size_t len);

    void startScan();
    void stopScan();
    bool doConnect(const std::string &addr, uint8_t addr_type);
    void teardown();
    void enqueue(const uint8_t *data, size_t len);
    bool queueEmpty() const { return q_head_ == q_tail_; }
    size_t queueDepth() const
    {
        const size_t cap = sizeof(queue_) / sizeof(queue_[0]);
        return (q_tail_ + cap - q_head_) % cap;
    }
    void queueBootstrap();
    void queueStatusRead();
    void queueParamWriteU16(uint16_t param, uint16_t value);
    void queueParamWriteU8(uint16_t param, uint8_t value);
    void queueParamWriteI32(uint16_t param, int32_t value);
    bool writeNext();
    uint16_t nextSeq() { return seq_++; }

    /** Start the window in which async fitness-mode echoes are ignored. */
    void guardModeEcho();

    /** Last unfiltered fitness mode reported by the device. */
    int reportedMode() const;

    void loadSaved();
    void saveDevice(const std::string &addr, uint8_t type, const std::string &name);
    void setStatus(const char *s);

    // state (protected by mutex_)
    mutable void *mutex_ = nullptr;
    DeviceState state_;
    std::atomic<uint32_t> version_{0};
    std::vector<FoundDevice> devices_;
    std::atomic<uint32_t> devices_version_{0};
    Requests req_;
    // Guards against the device echoing a stale fitness mode after a settings write.
    uint32_t mode_guard_until_ = 0;
    // The Voltra steps 0 -> 1 -> 5 rather than jumping straight to loaded, so a single
    // write often lands on an intermediate state. Re-issue until the target is reached.
    bool load_intent_ = false;
    bool load_intent_target_ = false;   // true = want loaded
    uint8_t load_retries_ = 0;
    uint32_t load_next_retry_ms_ = 0;
    /**
     * Last fitness mode the device actually reported. `state_.fitness_mode` is the
     * UI-facing value and is held at "loaded" while a re-assert is in flight, so the
     * retry logic needs the unfiltered one to know whether it is done.
     */
    int reported_mode_ = -1;
    /** While set, poll the auto-load countdown quickly so the screen can count along. */
    uint32_t auto_load_watch_until_ = 0;
    uint32_t auto_load_started_ms_ = 0;
    bool bypass_on_wait_ = false;   // override: write the bypass once auto load waits
    uint32_t last_auto_load_poll_ms_ = 0;
    void queueDirectLoadRead();

    // diagnostic register sweep (VOLTRA_DIAG builds only)
    bool sweep_active_ = false;
    size_t sweep_pos_ = 0;
    uint32_t sweep_next_ms_ = 0;

    // twin: the short second connection to the follower (joinFollower)
    FrameAssembler aux_assemblers_[3];
    std::atomic<int> aux_link_reply_{-1};   // CMD_TWIN_LINK reply's first byte, -1 none yet
    std::atomic<bool> aux_disconnected_{false};
    // Twinned, the host echoes FITNESS_MODE_TWIN_LOAD at once when it accepts a load;
    // a refused one never does (the idle state reads the same as a refusal).
    std::atomic<bool> twin_load_ack_{false};
    uint32_t last_twin_poll_ms_ = 0;
    uint32_t last_refresh_ms_ = 0;

    // worker-task-private
    void *client_ = nullptr;        // NimBLEClient*
    void *write_chr_ = nullptr;     // NimBLERemoteCharacteristic*
    FrameAssembler assemblers_[3];
    Frame queue_[32];
    uint8_t q_head_ = 0, q_tail_ = 0;
    uint16_t seq_ = 7;
    uint32_t last_write_ms_ = 0;
    uint32_t last_poll_ms_ = 0;
    uint32_t handshake_started_ms_ = 0;
    uint32_t reconnect_at_ms_ = 0;
    uint32_t reconnect_delay_ms_ = 0;
    bool scanning_ = false;
    bool want_scan_ = false;
    bool auto_reconnect_ = false;
    std::atomic<bool> disconnected_flag_{false};
    std::atomic<bool> scan_ended_flag_{false};
    std::string target_addr_;
    uint8_t target_type_ = 0;
    std::string target_name_;
    std::string saved_addr_;
    uint8_t saved_type_ = 0;
    std::string saved_name_;
};

}  // namespace voltra
