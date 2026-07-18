// Ct485Thermostat.cpp — see Ct485Thermostat.h.

#include "Ct485Thermostat.h"

#include <cstring>

namespace ct485 {

namespace {

constexpr size_t kDiscoveryRespPayloadLen = 17;  // PROVISIONAL, see header
constexpr size_t kSetAddressPayloadLen    = 18;
// #178: a channel that has never been granted yet is still establishing after a
// (re)join (a reboot re-join can briefly exceed the 60 s refresh window). Give
// it this much longer grace before crying starvation; a genuinely never-granted
// node still alarms, just not on a normal reboot.
constexpr uint32_t kJoinStarvationGraceMs = 300000;  // 5 min

inline bool timeReached(uint32_t nowMs, uint32_t deadlineMs) {
  // Wrap-safe: valid while |now - deadline| < 2^31 ms (~24 days).
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

inline uint8_t encodeDemandByte(float pct) {
  if (!(pct > 0.0f)) return 0;  // also catches NaN (docs/04: degrade to no-demand)
  if (pct > 100.0f) pct = 100.0f;
  return static_cast<uint8_t>(pct * 2.0f + 0.5f);  // percent*2, 0-200
}

inline size_t idx(DemandChannel ch) { return static_cast<size_t>(ch); }

}  // namespace

Command demandCommand(DemandChannel ch) {
  switch (ch) {
    case DemandChannel::kCool:       return Command::kCoolDemand;
    case DemandChannel::kFan:        return Command::kFanDemand;
    case DemandChannel::kBackupHeat: return Command::kBackupHeatDemand;
    case DemandChannel::kAuxHeat:    return Command::kAuxHeatDemand;
    case DemandChannel::kHeat:
    default:                         return Command::kHeatDemand;
  }
}

Ct485Thermostat::Ct485Thermostat(const Config& cfg) : cfg_(cfg) {
  if (!(cfg_.refreshFraction >= 0.1f && cfg_.refreshFraction <= 0.9f))
    cfg_.refreshFraction = dettson::kDemandRefreshFraction;
  if (cfg_.assumeAddressed) {
    join_   = JoinState::kAddressed;
    addr_   = cfg_.assumedAddress;
    subnet_ = cfg_.assumedSubnet;
  }
  // silent_ defaults true: boot = silent + no demands (docs/04 §1).
}

// ---------------- TX plumbing ----------------

Frame Ct485Thermostat::baseFrame(uint8_t dst, uint8_t msgType) const {
  Frame f;
  f.dst         = dst;
  f.src         = addressed() ? addr_ : 0x00;
  f.subnet      = addressed() ? subnet_ : kSubnetBroadcast;
  f.sendMethod  = 0x00;
  f.srcNodeType = static_cast<uint8_t>(NodeType::kThermostat);
  f.msgType     = msgType;
  f.packetNum   = versionBit_;
  return f;
}

void Ct485Thermostat::enqueueTx(const Frame& f) {
  if (silent_) return;  // hard gate: silence wins over everything
  if (txCount_ == kTxQueueDepth) {
    txDropped_++;  // drop, never block / never babble
    return;
  }
  tx_[(txHead_ + txCount_) % kTxQueueDepth] = f;
  txCount_++;
}

void Ct485Thermostat::transmit(Frame f, uint32_t nowMs) {
  (void)nowMs;
  f.src    = addressed() ? addr_ : 0x00;
  f.subnet = addressed() ? subnet_ : kSubnetBroadcast;
  f.packetNum = static_cast<uint8_t>((f.packetNum & ~kPktNumVersionBit) | versionBit_);
  enqueueTx(f);
}

bool Ct485Thermostat::popTx(Frame& out) {
  if (txCount_ == 0) return false;
  out = tx_[txHead_];
  txHead_ = (txHead_ + 1) % kTxQueueDepth;
  txCount_--;
  return true;
}

// ---------------- Safety switch ----------------

void Ct485Thermostat::goSilent() {
  silent_  = true;
  txCount_ = 0;
  txHead_  = 0;
  cmdCount_ = 0;
  cmdHead_  = 0;
  out_ = Outstanding{};
  clearDemands();
  // A pending discovery slot is cancelled too: a late 0xF9 after resume()
  // would be talk outside the coordinator's window.
  if (join_ == JoinState::kSlotWait || join_ == JoinState::kDiscoveryResponded)
    join_ = JoinState::kUnaddressed;
}

void Ct485Thermostat::resume(uint32_t nowMs) {
  (void)nowMs;
  silent_ = false;  // alarms stay latched until clearAlarms()
}

void Ct485Thermostat::clearAlarms() {
  pairingAlarm_ = commsLossAlarm_ = starvationAlarm_ = false;
}

void Ct485Thermostat::clearDemands() {
  for (auto& c : chan_) c = ChannelState{};
  if (out_.isDemand) out_ = Outstanding{};
}

void Ct485Thermostat::commsLoss() {
  commsLossAlarm_ = true;
  goSilent();  // comms-loss state = all channels silent (docs/02 §6 timing)
}

// ---------------- Demand API ----------------

bool Ct485Thermostat::setDemand(DemandChannel ch, float pct, uint32_t nowMs) {
  return setDemandInternal(ch, pct, chan_[idx(ch)].fanMode, nowMs);
}

bool Ct485Thermostat::setFanDemand(float pct, uint8_t fanMode, uint32_t nowMs) {
  return setDemandInternal(DemandChannel::kFan, pct, fanMode, nowMs);
}

bool Ct485Thermostat::setDemandInternal(DemandChannel ch, float pct, uint8_t fanMode,
                                        uint32_t nowMs) {
  if (silent_) return false;
  if (cfg_.offsetVariant == OffsetVariant::kUnset) return false;  // Phase 2 gate
  if (!(pct > 0.0f)) pct = 0.0f;  // NaN/negative -> 0, the safe direction
  if (pct > 100.0f) pct = 100.0f;

  ChannelState& cs = chan_[idx(ch)];
  // Only (re)assert sendNeeded on a real change to the wire demand. The control
  // loop calls setDemand() every tick; without this a steady demand re-sent on
  // every grant (~5 s) — ~60x the OEM refresh rate. An unchanged active demand is
  // instead refreshed by the timer path (dueMs); a genuine change re-asserts
  // immediately. Compare at wire resolution (percent x2) so float jitter that
  // maps to the same demand byte doesn't spuriously re-send.
  auto wireByte = [](float p) -> int {
    if (p <= 0.0f) return 0;
    if (p >= 100.0f) return 200;
    return static_cast<int>(p * 2.0f + 0.5f);
  };
  const bool changed = !cs.active || wireByte(cs.pct) != wireByte(pct) ||
                       cs.fanMode != fanMode;
  cs.fanMode = fanMode;
  if (pct > 0.0f) {
    if (!cs.active) {
      cs.activatedMs = nowMs;
      cs.everSent    = false;
    }
    cs.active      = true;
    cs.pct         = pct;
    if (changed) cs.sendNeeded = true;  // else the refresh timer (dueMs) resends
    cs.zeroPending = false;
  } else {
    const bool onWire = cs.everSent;
    cs.active     = false;
    cs.pct        = 0.0f;
    cs.sendNeeded = false;
    // Explicit zero only if the equipment ever heard a nonzero demand;
    // otherwise there is nothing on the wire to cancel.
    cs.zeroPending = onWire;
  }
  return true;
}

bool Ct485Thermostat::queueCommand(const Frame& f) {
  if (silent_) return false;
  if (cmdCount_ == kCmdFifoDepth) return false;
  cmdFifo_[(cmdHead_ + cmdCount_) % kCmdFifoDepth] = f;
  cmdCount_++;
  return true;
}

bool Ct485Thermostat::setSystemSwitch(SystemSwitch sw, uint32_t nowMs) {
  (void)nowMs;
  Frame f = baseFrame(cfg_.demandDst, static_cast<uint8_t>(MsgType::kSetControlCmd));
  f.sendMethod  = 0x01;  // routed-by-priority/control-command (docs/02 §2)
  f.sendParamHi = static_cast<uint8_t>(Command::kSystemSwitchModify);
  f.payload[0]  = f.sendParamHi;  // 16-bit LE command-code echo, frame [10..11]
  f.payload[1]  = 0x00;
  f.payload[2]  = static_cast<uint8_t>(sw);  // value at frame [12]
  f.payloadLen  = 3;
  return queueCommand(f);
}

bool Ct485Thermostat::setHeatSetpoint(uint8_t raw, uint32_t nowMs) {
  (void)nowMs;
  Frame f = baseFrame(cfg_.demandDst, static_cast<uint8_t>(MsgType::kSetControlCmd));
  f.sendMethod  = 0x01;
  f.sendParamHi = static_cast<uint8_t>(Command::kHeatSetPointModify);
  f.payload[0]  = f.sendParamHi;
  f.payload[1]  = 0x00;
  f.payload[2]  = 0x00;
  f.payload[3]  = raw;  // 1-byte temp at frame [13] (docs/02 §5a)
  f.payloadLen  = 4;
  return queueCommand(f);
}

bool Ct485Thermostat::setCoolSetpoint(uint8_t raw, uint32_t nowMs) {
  (void)nowMs;
  Frame f = baseFrame(cfg_.demandDst, static_cast<uint8_t>(MsgType::kSetControlCmd));
  f.sendMethod  = 0x01;
  f.sendParamHi = static_cast<uint8_t>(Command::kCoolSetPointModify);
  f.payload[0]  = f.sendParamHi;
  f.payload[1]  = 0x00;
  f.payload[2]  = 0x00;
  f.payload[3]  = raw;
  f.payloadLen  = 4;
  return queueCommand(f);
}

bool Ct485Thermostat::buildDemandFrame(DemandChannel ch, Frame& out) const {
  if (silent_ || cfg_.offsetVariant == OffsetVariant::kUnset) return false;
  const ChannelState& cs = chan_[idx(ch)];
  const uint8_t cmd = static_cast<uint8_t>(demandCommand(ch));
  const uint8_t demandByte = cs.active ? encodeDemandByte(cs.pct) : 0;

  out = baseFrame(cfg_.demandDst, static_cast<uint8_t>(MsgType::kSetControlCmd));
  out.sendMethod  = 0x01;
  out.sendParamHi = cmd;
  out.payload[0]  = cmd;  // command-code echo, frame [10..11] little-endian
  out.payload[1]  = 0x00;

  // Demand bytes land under the capture-confirmed variant ONLY (docs/02 §5a:
  // [12]/[13] vs [13]/[14]; variant B zero-fills frame [12]).
  const bool varA = cfg_.offsetVariant == OffsetVariant::kVarA;
  size_t p = varA ? 2 : 3;  // payload index of the refresh-timer byte
  if (!varA) out.payload[2] = 0x00;
  out.payload[p++] = cfg_.refreshTimerByte;
  if (ch == DemandChannel::kFan) out.payload[p++] = cs.fanMode;  // [.]=mode, then pct
  out.payload[p++] = demandByte;
  out.payloadLen = static_cast<uint8_t>(p);
  return true;
}

// ---------------- RX dispatch ----------------

void Ct485Thermostat::onFrame(const Frame& f, uint32_t nowMs) {
  if (f.src == kAddrCoordinator)
    versionBit_ = f.packetNum & kPktNumVersionBit;  // mirror the bus CT version

  switch (f.msgType) {
    case static_cast<uint8_t>(MsgType::kR2R): {
      if (!(f.packetNum & kPktNumDataflowBit)) { unexpected_++; return; }
      if (silent_ || !addressed() || f.dst != addr_) return;  // not ours: stay quiet
      handleGrant(nowMs, f.src, GrantKind::kR2R);
      return;
    }
    case static_cast<uint8_t>(MsgType::kTokenOffer): {
      if (silent_ || !addressed() || f.dst != addr_) return;
      handleGrant(nowMs, f.src, GrantKind::kTokenOffer);
      return;
    }
    case static_cast<uint8_t>(MsgType::kVersionAnnounce): {
      if (f.payloadLen >= 1) busVersion_ = f.payload[0];
      if (f.payloadLen >= 2) busRevision_ = f.payload[1];
      return;
    }
    case static_cast<uint8_t>(MsgType::kNodeDiscovery):
      handleDiscovery(f, nowMs);
      return;
    case static_cast<uint8_t>(MsgType::kSetAddress):
      handleSetAddress(f, nowMs);
      return;
    case static_cast<uint8_t>(MsgType::kSetControlCmd) | kResponseFlag:
      handleControlResponse(f, nowMs);
      return;
    default:
      // Anything we don't explicitly own: silence (docs/04 §1 never babble).
      unexpected_++;
      return;
  }
}

void Ct485Thermostat::handleDiscovery(const Frame& f, uint32_t nowMs) {
  if (silent_) return;
  const uint8_t filter = f.payloadLen >= 1 ? f.payload[0] : 0x00;
  if (filter != 0x00 && filter != static_cast<uint8_t>(NodeType::kThermostat)) return;

  if (addressed()) {
    reenumerations_++;
    // Impersonation mode (go-live): we deliberately ARE a fixed node — the OEM
    // thermostat we replaced, whose node-1 slot this coordinator remembers and
    // R2R-polls directly. Observed on the real bus (v1.0/1.0.1): this coordinator
    // broadcasts Node Discovery ROUTINELY, not only on restart. Surrendering
    // node 1 on each one strands us in AutoNet forever (it will not re-grant an
    // occupied slot), flapping addressed<->slot_wait. Hold our address; we stay
    // on the node list by answering the coordinator's R2R polls, not discovery.
    if (cfg_.assumeAddressed) return;
    // Fresh-join mode: Node Discovery while addressed = the coordinator is
    // re-enumerating (restart/replacement). Drop everything and rejoin from
    // scratch — demanding into a network being rebuilt is unsafe (docs/02 §6).
    clearDemands();
    cmdCount_ = 0;
    cmdHead_  = 0;
    out_   = Outstanding{};
    addr_  = 0;
    subnet_ = kSubnetBroadcast;
  }
  uint32_t d = cfg_.randomMs ? cfg_.randomMs(kAnetSlotLoMs, kAnetSlotHiMs) : kAnetSlotLoMs;
  if (d < kAnetSlotLoMs) d = kAnetSlotLoMs;  // clamp injected entropy into the
  if (d > kAnetSlotHiMs) d = kAnetSlotHiMs;  //  6-30 s slot window (docs/02 §6)
  slotDeadlineMs_ = nowMs + d;
  join_ = JoinState::kSlotWait;
}

void Ct485Thermostat::handleSetAddress(const Frame& f, uint32_t nowMs) {
  if (silent_) return;
  if (f.payloadLen < kSetAddressPayloadLen) { unexpected_++; return; }
  if (std::memcmp(f.payload + 2, cfg_.mac, 8) != 0) return;  // someone else's MAC

  addr_   = f.payload[0];
  subnet_ = f.payload[1];
  join_   = JoinState::kAddressed;

  Frame r = baseFrame(f.src, static_cast<uint8_t>(MsgType::kSetAddress) | kResponseFlag);
  std::memcpy(r.payload, f.payload, kSetAddressPayloadLen);  // echo the assignment
  r.payloadLen = kSetAddressPayloadLen;
  transmit(r, nowMs);
}

void Ct485Thermostat::handleControlResponse(const Frame& f, uint32_t nowMs) {
  (void)nowMs;
  if (!addressed() || f.dst != addr_) { unexpected_++; return; }
  if (!out_.active) { unexpected_++; return; }  // nothing outstanding: ignore

  const uint8_t code = f.payloadLen >= 1 ? f.payload[0] : 0x00;
  lastResponseCode_ = code;
  switch (code) {
    case kAck1:
    case kAck2:  // delivered, param flagged — diagnostics via lastResponseCode()
    case kAck3:
      // A successful demand ACK proves the bus round-trip is working again, so
      // auto-clear a latched starvation alarm. Starvation is a "recovering"
      // condition (a refresh window slipped, the equipment briefly reverted);
      // leaving it latched until a manual ack/reboot left "Furnace link
      // interrupted" stuck on the screen/HA while the furnace was in fact
      // running normally. Comms-loss / pairing stay latched (they need
      // operator attention); starvation self-heals.
      if (out_.isDemand) starvationAlarm_ = false;
      out_ = Outstanding{};
      return;
    case kNak1:  // bad CRC: retransmit, bounded by the attempt budget
      if (out_.attempts >= kMsgResendAttempts) commsLoss();
      else out_.retryQueued = true;
      return;
    case kNak2:
      // Pairing/ownership rejection (docs/02 §9): we are not this equipment's
      // controller. Stop ALL demands; retrying would be two masters.
      pairingAlarm_ = true;
      out_ = Outstanding{};
      clearDemands();
      cmdCount_ = 0;
      cmdHead_  = 0;
      return;
    default:
      // #162: a real Dettson demand/command response is the 0x83 ECHO of our
      // request — payload[0] is the command code WE sent (not an ack/nak code),
      // and the actual ack/nak rides in a SEPARATE 17-byte dataflow frame
      // (docs/02 "Response contract"), which is not routed here. So an echo whose
      // payload[0] matches the outstanding command IS the delivery ack. This is
      // what really clears the round-trip on the live bus (the kAck* cases above
      // only fire if a dataflow ack/nak frame ever routes here).
      // DELIBERATELY NOT reading ack/nak from payload[2]: that byte is the
      // refresh-timer echo (docs/02 §5a), whose legal values include 0x15/0x1B —
      // the exact NAK1/NAK2 codes — so treating it as ack/nak would misfire
      // kNak2 -> clearDemands()/pairing-latch -> furnace shutdown on a normal
      // demand refresh. A genuine rejection (NAK2 0x1B) arrives as a dataflow
      // frame that never reaches this handler; wiring that up needs a real
      // captured rejection and is tracked in #162 (we are the accepted paired
      // controller now, so rejection is unlikely to occur in practice).
      if (f.payloadLen >= 1 && code == out_.cmdCode) {
        if (out_.isDemand) starvationAlarm_ = false;  // echo proves the round-trip
        out_ = Outstanding{};
        return;
      }
      // Genuinely unrecognized response to an outstanding request: surface it
      // (counter + lastResponseCode_) rather than silently assuming delivery.
      unexpected_++;
      out_ = Outstanding{};
      return;
  }
}

// ---------------- Token grant ----------------

// NOTE: dryRunGrantResponse() below mirrors this function's branch order for
// the passive #28 turnaround probe. Any change to the grant-reply logic here
// must be reflected there (and vice versa).
void Ct485Thermostat::handleGrant(uint32_t nowMs, uint8_t replyTo, GrantKind kind) {
  // 1) A retransmit owed (response timeout / NAK1) goes first, verbatim.
  if (out_.active && out_.retryQueued) {
    out_.retryQueued = false;
    out_.attempts++;
    out_.sentMs = nowMs;
    if (out_.isDemand) {
      ChannelState& cs = chan_[idx(out_.ch)];
      cs.lastSentMs = nowMs;
      cs.everSent   = true;
    }
    transmit(out_.frame, nowMs);
    return;
  }

  if (!out_.active) {
    // 2) Demand channels needing (re)send — built fresh from current state.
    for (size_t i = 0; i < kDemandChannelCount; i++) {
      ChannelState& cs = chan_[i];
      if (!cs.sendNeeded && !cs.zeroPending) continue;
      Frame d;
      if (!buildDemandFrame(static_cast<DemandChannel>(i), d)) continue;
      cs.sendNeeded  = false;
      cs.zeroPending = false;
      cs.lastSentMs  = nowMs;
      cs.everSent    = true;
      out_ = Outstanding{};
      out_.active   = true;
      out_.isDemand = true;
      out_.ch       = static_cast<DemandChannel>(i);
      out_.cmdCode  = d.sendParamHi;
      out_.attempts = 1;
      out_.sentMs   = nowMs;
      out_.frame    = d;
      transmit(d, nowMs);
      return;
    }
    // 3) Queued persistent-state commands.
    if (cmdCount_ > 0) {
      Frame c = cmdFifo_[cmdHead_];
      cmdHead_ = (cmdHead_ + 1) % kCmdFifoDepth;
      cmdCount_--;
      out_ = Outstanding{};
      out_.active   = true;
      out_.isDemand = false;
      out_.cmdCode  = c.sendParamHi;
      out_.attempts = 1;
      out_.sentMs   = nowMs;
      out_.frame    = c;
      transmit(c, nowMs);
      return;
    }
  }

  // 4) Nothing to send (or still awaiting a response): acknowledge the poll
  //    so the coordinator keeps us on the node list (silent >120 s = dropped).
  if (kind == GrantKind::kR2R) {
    Frame a = baseFrame(replyTo, static_cast<uint8_t>(MsgType::kR2R));
    a.packetNum = static_cast<uint8_t>(kPktNumDataflowBit | versionBit_);
    a.payload[0] = kAck1;  // R2R_ACK = 0x06 (docs/02 §4)
    a.payloadLen = 1;
    transmit(a, nowMs);
  } else {
    Frame t = baseFrame(replyTo, static_cast<uint8_t>(MsgType::kTokenOffer) | kResponseFlag);
    t.payload[0] = addr_;  // decline/echo: addr, subnet, MAC, session (PROVISIONAL)
    t.payload[1] = subnet_;
    std::memcpy(t.payload + 2, cfg_.mac, 8);
    std::memcpy(t.payload + 10, cfg_.sessionId, 8);
    t.payloadLen = kSetAddressPayloadLen;
    transmit(t, nowMs);
  }
}

// ---------------- Passive turnaround probe (issue #28) ----------------

// See the header contract. Pure/const dry-run of handleGrant()'s reply, with
// no enqueue/state change and no silent()/addressed() gating. Kept in lockstep
// with handleGrant() (branch order: retry > demand > queued command > ACK).
bool Ct485Thermostat::dryRunGrantResponse(uint8_t grantSrc, bool tokenOffer,
                                          Frame& out) const {
  // 1) A retransmit owed would go out first, verbatim.
  if (out_.active && out_.retryQueued) { out = out_.frame; return true; }

  if (!out_.active) {
    // 2) Demand channels needing (re)send — built fresh from current state.
    //    buildDemandFrame() self-gates on silent_/offsetVariant, so in shadow
    //    mode this yields nothing and we fall through to the ACK reply.
    for (size_t i = 0; i < kDemandChannelCount; i++) {
      const ChannelState& cs = chan_[i];
      if (!cs.sendNeeded && !cs.zeroPending) continue;
      if (buildDemandFrame(static_cast<DemandChannel>(i), out)) return true;
    }
    // 3) Queued persistent-state command.
    if (cmdCount_ > 0) { out = cmdFifo_[cmdHead_]; return true; }
  }

  // 4) Nothing to send: acknowledge the poll / echo the token offer.
  if (!tokenOffer) {
    out = baseFrame(grantSrc, static_cast<uint8_t>(MsgType::kR2R));
    out.packetNum  = static_cast<uint8_t>(kPktNumDataflowBit | versionBit_);
    out.payload[0] = kAck1;
    out.payloadLen = 1;
  } else {
    out = baseFrame(grantSrc, static_cast<uint8_t>(MsgType::kTokenOffer) | kResponseFlag);
    out.payload[0] = addr_;
    out.payload[1] = subnet_;
    std::memcpy(out.payload + 2, cfg_.mac, 8);
    std::memcpy(out.payload + 10, cfg_.sessionId, 8);
    out.payloadLen = kSetAddressPayloadLen;
  }
  return true;
}

// ---------------- Timers ----------------

void Ct485Thermostat::tick(uint32_t nowMs) {
  if (silent_) return;

  // AutoNet discovery-slot expiry -> 0xF9 (the slot IS the arbitration; no
  // token exists for an unaddressed node).
  if (join_ == JoinState::kSlotWait && timeReached(nowMs, slotDeadlineMs_)) {
    Frame r = baseFrame(kAddrCoordinator,
                        static_cast<uint8_t>(MsgType::kNodeDiscovery) | kResponseFlag);
    r.payload[0] = static_cast<uint8_t>(NodeType::kThermostat);
    std::memcpy(r.payload + 1, cfg_.mac, 8);
    std::memcpy(r.payload + 9, cfg_.sessionId, 8);
    r.payloadLen = kDiscoveryRespPayloadLen;
    transmit(r, nowMs);
    join_ = JoinState::kDiscoveryResponded;
  }

  // Response timeout (3000 ms x 3 attempts per Net485, docs/02 §6).
  if (out_.active && !out_.retryQueued &&
      timeReached(nowMs, out_.sentMs + kResponseTimeoutMs)) {
    if (out_.attempts >= kMsgResendAttempts) {
      commsLoss();
      return;
    }
    out_.retryQueued = true;  // actual retransmit waits for the next grant
  }

  // Demand refresh + starvation watchdog (per channel, docs/04 §3).
  const uint32_t windowMs = refreshTimerMs(cfg_.refreshTimerByte);
  const uint32_t dueMs =
      static_cast<uint32_t>(static_cast<float>(windowMs) * cfg_.refreshFraction);
  for (size_t i = 0; i < kDemandChannelCount; i++) {
    ChannelState& cs = chan_[i];
    if (!cs.active) continue;
    const uint32_t refMs = cs.everSent ? cs.lastSentMs : cs.activatedMs;
    // #178: tight refresh window once we've been granted at least once; a long
    // join grace while still waiting for the first grant after a (re)join, so a
    // normal reboot re-join doesn't false-positive.
    const uint32_t limitMs = cs.everSent ? windowMs : kJoinStarvationGraceMs;
    if (timeReached(nowMs, refMs + limitMs)) {
      // A full window elapsed with no successful (re)send: the equipment is
      // reverting (or never started). Alarm and GO SILENT — the failsafe is
      // quiet, not a retry storm (docs/04 §1).
      starvationAlarm_ = true;
      starvedCh_ = static_cast<DemandChannel>(i);
      goSilent();
      return;
    }
    if (timeReached(nowMs, refMs + dueMs)) cs.sendNeeded = true;
  }
  // Reaching here means NO active channel is past its refresh window — either it
  // recovered or there's no active demand (idle in the deadband). Starvation is a
  // transient "recovering" condition, so auto-clear the latch. Without this it
  // stuck on "Furnace link interrupted" while idle (no demand ACK to clear it via
  // handleControlResponse). comms-loss/pairing still latch — they need operator
  // attention.
  starvationAlarm_ = false;
}

// ---------------- State getters ----------------

bool Ct485Thermostat::channelActive(DemandChannel ch) const {
  return chan_[idx(ch)].active;
}

uint32_t Ct485Thermostat::nextRefreshDueMs(DemandChannel ch) const {
  const ChannelState& cs = chan_[idx(ch)];
  if (!cs.active) return 0;
  const uint32_t windowMs = refreshTimerMs(cfg_.refreshTimerByte);
  const uint32_t refMs = cs.everSent ? cs.lastSentMs : cs.activatedMs;
  return refMs + static_cast<uint32_t>(static_cast<float>(windowMs) * cfg_.refreshFraction);
}

}  // namespace ct485
