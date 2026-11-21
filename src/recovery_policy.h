#pragma once

#include <stddef.h>
#include <stdint.h>

// Host-testable recovery decisions: the Bluetooth reconnect ladder, the OTA
// boot guard, and firmware upload checks. No SDK calls live here.

// ---- Bluetooth reconnect ladder ----

constexpr uint32_t kBtRetryFastMs = 5000;
constexpr uint32_t kBtRetrySlowMs = 15000;
constexpr uint32_t kBtRetryPassiveMs = 60000;
constexpr uint32_t kBtFastAttempts = 6;    // about one minute of fast retries
constexpr uint32_t kBtSlowAttempts = 12;   // then about 1.5 minutes of slow retries
constexpr uint32_t kBtStackRestartAfterMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kBtStackRestartRepeatMs = 30UL * 60UL * 1000UL;
constexpr uint32_t kBtStackRestartMinFailures = 3;
constexpr uint32_t kBtNoResponseRestartCount = 2;

enum class BtFailure : uint8_t { pageTimeout, other, noResponse };

struct BtLadder {
  uint32_t consecutiveFailures = 0;
  uint32_t consecutivePageTimeouts = 0;
  uint32_t consecutiveNoResponse = 0;
  uint32_t stackRestarts = 0;
  uint32_t failingSinceMs = 0;
  uint32_t lastStackRestartMs = 0;
  bool restartedThisWindow = false;
  bool connected = false;
  bool everConnected = false;
  uint32_t lastConnectedMs = 0;
};

inline void btLadderConnected(BtLadder &ladder, uint32_t now) {
  ladder.consecutiveFailures = 0;
  ladder.consecutivePageTimeouts = 0;
  ladder.consecutiveNoResponse = 0;
  ladder.restartedThisWindow = false;
  ladder.connected = true;
  ladder.everConnected = true;
  ladder.lastConnectedMs = now;
}

inline void btLadderDisconnected(BtLadder &ladder, uint32_t now) {
  if (ladder.connected) ladder.lastConnectedMs = now;
  ladder.connected = false;
}

inline void btLadderFailure(BtLadder &ladder, uint32_t now, BtFailure failure) {
  if (ladder.consecutiveFailures == 0) ladder.failingSinceMs = now;
  ++ladder.consecutiveFailures;
  if (failure == BtFailure::pageTimeout) {
    ++ladder.consecutivePageTimeouts;
  } else {
    ladder.consecutivePageTimeouts = 0;
  }
  ladder.consecutiveNoResponse =
      failure == BtFailure::noResponse ? ladder.consecutiveNoResponse + 1 : 0;
}

inline uint32_t btLadderRetryDelayMs(const BtLadder &ladder) {
  if (ladder.consecutiveFailures <= kBtFastAttempts) return kBtRetryFastMs;
  if (ladder.consecutiveFailures <= kBtSlowAttempts) return kBtRetrySlowMs;
  return kBtRetryPassiveMs;
}

// A wedged stack (attempts that never report a result) restarts at once; a
// receiver that keeps ignoring pages restarts after 10 min, then every 30 min.
inline bool btLadderShouldRestartStack(const BtLadder &ladder, uint32_t now) {
  if (ladder.connected) return false;
  if (ladder.consecutiveNoResponse >= kBtNoResponseRestartCount) return true;
  if (ladder.consecutiveFailures < kBtStackRestartMinFailures) return false;
  if (now - ladder.failingSinceMs < kBtStackRestartAfterMs) return false;
  return !ladder.restartedThisWindow ||
         now - ladder.lastStackRestartMs >= kBtStackRestartRepeatMs;
}

inline void btLadderStackRestarted(BtLadder &ladder, uint32_t now) {
  ++ladder.stackRestarts;
  ladder.lastStackRestartMs = now;
  ladder.restartedThisWindow = true;
  ladder.consecutiveNoResponse = 0;
}

inline const char *btLadderStage(const BtLadder &ladder, bool active,
                                 bool connecting) {
  if (ladder.connected) return "connected";
  if (!active) return "idle";
  if (connecting) return "connecting";
  if (ladder.consecutiveFailures == 0) return "reconnecting";
  if (ladder.consecutiveFailures <= kBtFastAttempts) return "retrying";
  if (ladder.consecutiveFailures <= kBtSlowAttempts) return "backoff";
  return "passive";  // mostly page-scanning so the receiver can reach us
}

// Bluetooth start/restart failures back off from 1 min to 10 min; HTTP and
// OTA keep running meanwhile. After a reset that hit the Bluetooth lifecycle
// (panic, watchdog, a hung start or a failed teardown while Bluetooth was
// active), the next boot waits 3 min before touching Bluetooth so recovery
// HTTP can serve an update first.
constexpr uint32_t kBtStartRetryMinMs = 60000;
constexpr uint32_t kBtStartRetryMaxMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kBtRecoveryBootDelayMs = 3UL * 60UL * 1000UL;
constexpr uint32_t kBtLifecycleTimeoutMs = 45000;

inline uint32_t btStartRetryDelayMs(uint32_t failures) {
  uint32_t delay = kBtStartRetryMinMs;
  for (uint32_t i = 1; i < failures && delay < kBtStartRetryMaxMs; ++i) delay *= 2;
  return delay < kBtStartRetryMaxMs ? delay : kBtStartRetryMaxMs;
}

inline uint32_t btBootStartDelayMs(bool lifecycleInterruptedLastBoot) {
  return lifecycleInterruptedLastBoot ? kBtRecoveryBootDelayMs : 0;
}

// ---- OTA boot guard ----

constexpr uint8_t kOtaMaxTrialBoots = 3;

enum class OtaBootAction : uint8_t { stable, rolledBack, trial, fallback };

// trialRecorded: an uploaded image is still unconfirmed. runningIsTrial: the
// running slot is that image. previousBoots: trial boots already counted.
inline OtaBootAction otaBootAction(bool trialRecorded, bool runningIsTrial,
                                   uint8_t previousBoots) {
  if (!trialRecorded) return OtaBootAction::stable;
  if (!runningIsTrial) return OtaBootAction::rolledBack;
  return previousBoots >= kOtaMaxTrialBoots ? OtaBootAction::fallback
                                            : OtaBootAction::trial;
}

// Uptime limit for an unconfirmed image to be confirmed by
// POST /api/firmware/confirm; after it the board restarts and rolls back.
constexpr uint32_t kOtaTrialTimeoutMs = 10UL * 60UL * 1000UL;
inline bool otaTrialTimedOut(uint32_t uptimeMs) {
  return uptimeMs >= kOtaTrialTimeoutMs;
}

// ---- Firmware upload checks ----

// Image header (24) + first segment header (8) + esp_app_desc_t magic (4).
constexpr size_t kFirmwareHeaderBytes = 36;
constexpr uint8_t kImageMagic = 0xE9;
constexpr uint32_t kAppDescMagic = 0xABCD5432;
constexpr uint8_t kImageMaxSegments = 16;
constexpr uint16_t kChipIdEsp32 = 0;

inline bool firmwareSizeAcceptable(long contentLength, size_t slotSize) {
  return contentLength >= static_cast<long>(kFirmwareHeaderBytes) &&
         static_cast<unsigned long>(contentLength) <= slotSize;
}

inline bool firmwareHeaderValid(const uint8_t *data, size_t length) {
  if (length < kFirmwareHeaderBytes) return false;
  const uint16_t chipId = static_cast<uint16_t>(data[12] | (data[13] << 8));
  const uint32_t descMagic = static_cast<uint32_t>(data[32]) |
                             (static_cast<uint32_t>(data[33]) << 8) |
                             (static_cast<uint32_t>(data[34]) << 16) |
                             (static_cast<uint32_t>(data[35]) << 24);
  return data[0] == kImageMagic && data[1] > 0 &&
         data[1] <= kImageMaxSegments && chipId == kChipIdEsp32 &&
         data[23] == 1 &&  // SHA-256 appended, so the SDK verifies it
         descMagic == kAppDescMagic;
}
