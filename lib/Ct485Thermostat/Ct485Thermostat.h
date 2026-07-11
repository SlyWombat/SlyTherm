// Ct485Thermostat.h — CT-485 TX-side protocol state machine (Phase 3;
// docs/02-protocol-climatetalk.md §4-§6 + §9): AutoNet join, token/R2R
// discipline, Set Control Command demand TX, the per-channel demand-refresh
// watchdog, ACK/NAK correlation, and the go-silent hard switch.
//
// PURE protocol logic — no UART, no Arduino, no wall clock. The caller feeds
// decoded ct485::Frame inputs (onFrame) plus monotonic milliseconds (tick)
// and drains authorized TX frames (popTx) to the wire. A frame enters the TX
// queue ONLY when the bus rules allow it (docs/04 §1 "never babble"):
//   * application frames go out only on a coordinator grant — R2R (msgType
//     0x00, dataflow bit) or Token Offer (0x77) — addressed to our node ID;
//   * AutoNet traffic is the protocol's own arbitration: the Node Discovery
//     reply (0xF9) waits a random 6-30 s slot (kAnetSlotLoMs/HiMs), and the
//     Set Address reply (0xFA) answers a frame addressed to our MAC;
//   * everything unexpected gets silence (counted, never answered).
//
// Safety invariants (docs/04 §1 prime directive — the most safety-critical
// module in the project):
//   * boot state = SILENT + no demands; the caller must resume() on purpose.
//   * goSilent() flushes every queue, clears every demand, and refuses all
//     TX until resume(); the safety/watchdog layer wires straight into it.
//   * demand frames are REFUSED until Config::offsetVariant is explicitly
//     set to A or B — the docs/02 §5a sniff-confirmation gate, in code.
//   * an active demand that cannot be re-sent within one full refresh window
//     (token starvation) raises an alarm AND goes silent — never a retry
//     storm. Comms loss (response timeout x kMsgResendAttempts) and NAK2
//     pairing rejection (docs/02 §9) likewise end in no-demand.
//
// PROVISIONAL payload layouts (reconstructed from the Net485 structures, not
// yet capture-confirmed — same confidence rule as the §5a demand offsets;
// centralize here so a capture diff fixes one place):
//   0x79 Node Discovery req : payload[0] = node-type filter (0x00 = any)
//   0xF9 Node Discovery resp: [0]=nodeType, [1..8]=MAC, [9..16]=sessionId
//   0x7A Set Address        : [0]=nodeId, [1]=subnet, [2..9]=MAC, [10..17]=sessionId
//   0xFA Set Address resp   : echoes the 0x7A payload
//   0x78 Version Announce   : [0]=version, [1]=revision, [2]=FFD flag
//   0x83 Set Control resp   : payload[0] = ACK/NAK code
//
// Pure C++17, no Arduino dependencies; time injected as uint32_t nowMs.

#pragma once
#include <cstddef>
#include <cstdint>

#include "Ct485Core.h"
#include "DettsonConfig.h"

namespace ct485 {

// docs/02 §5a hard TX gate: which provisional demand-offset layout real
// captures confirmed. kUnset = Phase 2 not done -> demand TX refused.
enum class OffsetVariant : uint8_t {
  kUnset = 0,
  kVarA,  // frame [12]=refresh timer, [13]=demand
  kVarB,  // frame [13]=refresh timer, [14]=demand
};

// Demand channels we may originate (docs/04 §1: DEFROST 0x68 is the
// interface board's channel, never ours).
enum class DemandChannel : uint8_t { kHeat = 0, kCool, kFan, kBackupHeat, kAuxHeat };
constexpr size_t kDemandChannelCount = 5;

Command demandCommand(DemandChannel ch);

// Refresh-timer byte -> milliseconds: high nibble minutes + low nibble in
// 3.75 s units (docs/02 §5a).
constexpr uint32_t refreshTimerMs(uint8_t timerByte) {
  return static_cast<uint32_t>(timerByte >> 4) * 60000u +
         static_cast<uint32_t>(timerByte & 0x0F) * 3750u;
}

enum class JoinState : uint8_t {
  kUnaddressed = 0,
  kSlotWait,            // Node Discovery seen; 0xF9 queued for the slot deadline
  kDiscoveryResponded,  // 0xF9 sent; waiting for Set Address
  kAddressed,
};

class Ct485Thermostat {
 public:
  // AutoNet slot-delay entropy is injected: the caller supplies the random
  // source (hardware RNG in firmware, scripted in tests). Returned values
  // are clamped into [loMs, hiMs]; nullptr -> deterministic loMs.
  using RandomFn = uint32_t (*)(uint32_t loMs, uint32_t hiMs);

  struct Config {
    OffsetVariant offsetVariant = OffsetVariant::kUnset;  // Phase 2 gate
    uint8_t  refreshTimerByte = dettson::kDemandRefreshTimerByte;
    float    refreshFraction  = dettson::kDemandRefreshFraction;  // clamped 0.1-0.9
    uint8_t  demandDst        = kAddrBroadcast;  // 0x00 + sendMethod 0x01 = routed
    uint8_t  mac[8]           = {};
    uint8_t  sessionId[8]     = {};
    RandomFn randomMs         = nullptr;
    // Already-addressed fast path: a coordinator that remembers us skips
    // AutoNet and polls R2R directly (thermostat is always node 1, docs/02 §6).
    bool     assumeAddressed  = false;
    uint8_t  assumedAddress   = kAddrThermostat;
    uint8_t  assumedSubnet    = kSubnetV2;
  };

  explicit Ct485Thermostat(const Config& cfg);

  // ---- I/O (the caller's UART glue) ----
  void onFrame(const Frame& f, uint32_t nowMs);  // one decoded, Fletcher-valid RX frame
  void tick(uint32_t nowMs);                     // drive timers (slot, timeouts, refresh)
  bool popTx(Frame& out);                        // drain authorized TX frames, FIFO
  size_t txPending() const { return txCount_; }

  // ---- Safety hard switch (docs/04 §1) ----
  void goSilent();              // flush + refuse all TX; demands cleared
  void resume(uint32_t nowMs);  // deliberate re-enable; alarms stay latched
  bool silent() const { return silent_; }

  // ---- Demand API (the DemandArbiter-facing actuator surface) ----
  // pct 0-100 (non-finite/negative coerced to 0, >100 clamped); encoded as
  // percent*2 on the wire. Returns false when refused (silent, or
  // offsetVariant unset). pct = 0 deactivates: one explicit zero-demand
  // frame is emitted at the next grant, then the channel stops refreshing.
  bool setDemand(DemandChannel ch, float pct, uint32_t nowMs);
  bool setFanDemand(float pct, uint8_t fanMode, uint32_t nowMs);

  // Persistent-state writes (no refresh timer, docs/02 §5a); queued FIFO
  // until a grant. Refused only while silent. Setpoint values are the raw
  // wire byte (frame [13]; unit scaling unconfirmed until Phase 2).
  bool setSystemSwitch(SystemSwitch sw, uint32_t nowMs);
  bool setHeatSetpoint(uint8_t raw, uint32_t nowMs);
  bool setCoolSetpoint(uint8_t raw, uint32_t nowMs);

  // ---- Join / network state ----
  JoinState joinState() const { return join_; }
  bool addressed() const { return join_ == JoinState::kAddressed; }
  uint8_t nodeAddress() const { return addr_; }
  uint8_t nodeSubnet() const { return subnet_; }
  uint32_t slotDeadlineMs() const { return slotDeadlineMs_; }
  uint8_t busVersion() const { return busVersion_; }    // from 0x78
  uint8_t busRevision() const { return busRevision_; }
  uint32_t reenumerations() const { return reenumerations_; }

  // ---- Demand / refresh state ----
  bool channelActive(DemandChannel ch) const;
  // Absolute nowMs deadline by which the channel's next refresh should TX
  // (refreshFraction * window past the last send); 0 when inactive.
  uint32_t nextRefreshDueMs(DemandChannel ch) const;

  // ---- Alarms (latched; survive resume() until clearAlarms()) ----
  bool pairingAlarm() const { return pairingAlarm_; }        // NAK2 0x1B (docs/02 §9)
  bool commsLossAlarm() const { return commsLossAlarm_; }    // response timeout exhausted
  bool starvationAlarm() const { return starvationAlarm_; }  // refresh window missed (no token)
  DemandChannel starvedChannel() const { return starvedCh_; }
  void clearAlarms();

  // ---- Diagnostics ----
  uint8_t lastResponseCode() const { return lastResponseCode_; }  // last 0x83 ACK/NAK byte
  uint32_t unexpectedFrames() const { return unexpected_; }
  uint32_t txDropped() const { return txDropped_; }  // TX-queue overflow (never babbles)

  // ---- Passive TX-turnaround probe (issue #28, Phase 3 bench gate) ----
  // Build the response frame handleGrant() WOULD transmit for a coordinator
  // grant addressed to our slot, with NO enqueue, NO state change, and
  // independent of the silent()/addressed() gates — the shadow-mode dry-run
  // that lets the firmware time its own grant->DE-ready turnaround without ever
  // driving the bus. Branch order mirrors handleGrant() (retry > demand >
  // queued command > ACK/token echo) so the build cost is representative; in
  // shadow mode the demand branches are gated off so it always lands on the
  // ACK/token-echo frame, and encode() (the Fletcher pass) dominates and is
  // content-independent at microsecond resolution. tokenOffer=false => R2R
  // grant, true => Token Offer. Pure/const; MUST be kept in lockstep with
  // handleGrant(). Always returns true (a grant to us always has a would-be
  // reply); `out` is filled with the frame.
  bool dryRunGrantResponse(uint8_t grantSrc, bool tokenOffer, Frame& out) const;

 private:
  enum class GrantKind : uint8_t { kR2R, kTokenOffer };

  struct ChannelState {
    bool     active      = false;  // desired pct > 0
    bool     sendNeeded  = false;  // (re)send awaiting a grant
    bool     zeroPending = false;  // one-shot explicit zero frame after deactivation
    bool     everSent    = false;
    float    pct         = 0.0f;
    uint8_t  fanMode     = 0;      // FAN_DEMAND only
    uint32_t activatedMs = 0;
    uint32_t lastSentMs  = 0;
  };

  struct Outstanding {
    bool     active      = false;
    bool     retryQueued = false;  // timed out / NAK1: retransmit at next grant
    bool     isDemand    = false;
    DemandChannel ch     = DemandChannel::kHeat;
    uint8_t  cmdCode     = 0;
    uint8_t  attempts    = 0;      // transmissions so far (max kMsgResendAttempts)
    uint32_t sentMs      = 0;
    Frame    frame;
  };

  Frame baseFrame(uint8_t dst, uint8_t msgType) const;
  bool  buildDemandFrame(DemandChannel ch, Frame& out) const;
  bool  queueCommand(const Frame& f);
  void  enqueueTx(const Frame& f);
  void  transmit(Frame f, uint32_t nowMs);  // patches src/subnet, enqueues
  void  handleGrant(uint32_t nowMs, uint8_t replyTo, GrantKind kind);
  void  handleDiscovery(const Frame& f, uint32_t nowMs);
  void  handleSetAddress(const Frame& f, uint32_t nowMs);
  void  handleControlResponse(const Frame& f, uint32_t nowMs);
  void  clearDemands();
  void  commsLoss();
  bool  setDemandInternal(DemandChannel ch, float pct, uint8_t fanMode, uint32_t nowMs);

  static constexpr size_t kTxQueueDepth  = 6;
  static constexpr size_t kCmdFifoDepth  = 4;

  Config       cfg_;
  bool         silent_ = true;  // boot = silent (docs/04 §1)
  JoinState    join_   = JoinState::kUnaddressed;
  uint8_t      addr_   = 0;
  uint8_t      subnet_ = kSubnetBroadcast;
  uint8_t      versionBit_ = 0;  // packet-number version bit mirrored from the coordinator
  uint8_t      busVersion_ = 0, busRevision_ = 0;
  uint32_t     slotDeadlineMs_ = 0;
  uint32_t     reenumerations_ = 0;

  ChannelState chan_[kDemandChannelCount];
  Outstanding  out_;

  Frame        cmdFifo_[kCmdFifoDepth];
  size_t       cmdHead_ = 0, cmdCount_ = 0;

  Frame        tx_[kTxQueueDepth];
  size_t       txHead_ = 0, txCount_ = 0;

  bool         pairingAlarm_    = false;
  bool         commsLossAlarm_  = false;
  bool         starvationAlarm_ = false;
  DemandChannel starvedCh_      = DemandChannel::kHeat;
  uint8_t      lastResponseCode_ = 0;
  uint32_t     unexpected_ = 0;
  uint32_t     txDropped_  = 0;
};

}  // namespace ct485
