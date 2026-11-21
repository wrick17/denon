#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/recovery_policy.h"

int main() {
  // Ladder: fast, slow, then passive retries; stack restart after 10 min of
  // sustained failures, then every 30 min; immediately when the stack is wedged.
  BtLadder ladder;
  uint32_t now = 1000;
  assert(!btLadderShouldRestartStack(ladder, now));
  for (uint32_t i = 1; i <= kBtFastAttempts; ++i) {
    btLadderFailure(ladder, now += 10000, BtFailure::pageTimeout);
    assert(btLadderRetryDelayMs(ladder) == kBtRetryFastMs);
  }
  btLadderFailure(ladder, now += 10000, BtFailure::pageTimeout);
  assert(btLadderRetryDelayMs(ladder) == kBtRetrySlowMs);
  while (ladder.consecutiveFailures <= kBtSlowAttempts) {
    btLadderFailure(ladder, now += 20000, BtFailure::pageTimeout);
  }
  assert(btLadderRetryDelayMs(ladder) == kBtRetryPassiveMs);
  assert(ladder.consecutivePageTimeouts == ladder.consecutiveFailures);
  assert(strcmp(btLadderStage(ladder, true, false), "passive") == 0);
  assert(!btLadderShouldRestartStack(ladder, ladder.failingSinceMs + kBtStackRestartAfterMs - 1));
  now = ladder.failingSinceMs + kBtStackRestartAfterMs;
  assert(btLadderShouldRestartStack(ladder, now));
  btLadderStackRestarted(ladder, now);
  assert(ladder.stackRestarts == 1);
  assert(!btLadderShouldRestartStack(ladder, now + kBtStackRestartRepeatMs - 1));
  assert(btLadderShouldRestartStack(ladder, now + kBtStackRestartRepeatMs));
  btLadderFailure(ladder, now, BtFailure::other);
  assert(ladder.consecutivePageTimeouts == 0);

  btLadderConnected(ladder, now);
  assert(ladder.consecutiveFailures == 0 && !btLadderShouldRestartStack(ladder, now + 3600000));
  assert(strcmp(btLadderStage(ladder, true, false), "connected") == 0);
  btLadderDisconnected(ladder, now + 5000);
  assert(ladder.lastConnectedMs == now + 5000);
  btLadderFailure(ladder, now + 6000, BtFailure::noResponse);
  assert(!btLadderShouldRestartStack(ladder, now + 6000));
  btLadderFailure(ladder, now + 7000, BtFailure::noResponse);
  assert(btLadderShouldRestartStack(ladder, now + 7000));

  // Wrap-around safe timing.
  BtLadder wrapped;
  for (int i = 0; i < 3; ++i) btLadderFailure(wrapped, UINT32_MAX - 1000, BtFailure::pageTimeout);
  assert(btLadderShouldRestartStack(wrapped, kBtStackRestartAfterMs));

  // OTA boot guard.
  assert(otaBootAction(false, true, 9) == OtaBootAction::stable);
  assert(otaBootAction(true, false, 0) == OtaBootAction::rolledBack);
  assert(otaBootAction(true, true, 0) == OtaBootAction::trial);
  assert(otaBootAction(true, true, kOtaMaxTrialBoots - 1) == OtaBootAction::trial);
  assert(otaBootAction(true, true, kOtaMaxTrialBoots) == OtaBootAction::fallback);
  assert(!otaTrialTimedOut(kOtaTrialTimeoutMs - 1) && otaTrialTimedOut(kOtaTrialTimeoutMs));

  // Bluetooth start failures back off and cap; a reset during the Bluetooth
  // lifecycle delays the next start so HTTP/OTA stays available.
  assert(btStartRetryDelayMs(0) == kBtStartRetryMinMs);
  assert(btStartRetryDelayMs(1) == kBtStartRetryMinMs);
  assert(btStartRetryDelayMs(2) == 2 * kBtStartRetryMinMs);
  assert(btStartRetryDelayMs(30) == kBtStartRetryMaxMs);
  assert(btBootStartDelayMs(false) == 0);
  assert(btBootStartDelayMs(true) == kBtRecoveryBootDelayMs);

  // Firmware image checks, using the header of a real esp32dev build.
  uint8_t header[kFirmwareHeaderBytes] = {0xe9, 0x05, 0x02, 0x20, 0x5c, 0x2d, 0x08, 0x40,
                                          0xee, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01,
                                          0x20, 0x00, 0x40, 0x3f, 0x5c, 0x55, 0x03, 0x00,
                                          0x32, 0x54, 0xcd, 0xab};
  assert(firmwareHeaderValid(header, sizeof(header)));
  assert(!firmwareHeaderValid(header, sizeof(header) - 1));
  uint8_t bad[kFirmwareHeaderBytes];
  const size_t corrupt[] = {0, 1, 12, 23, 34};
  for (size_t offset : corrupt) {
    memcpy(bad, header, sizeof(bad));
    bad[offset] ^= 0x5a;
    assert(!firmwareHeaderValid(bad, sizeof(bad)));
  }
  assert(firmwareSizeAcceptable(1656505, 0x1E0000));
  assert(firmwareSizeAcceptable(0x1E0000, 0x1E0000));
  assert(!firmwareSizeAcceptable(0x1E0001, 0x1E0000));
  assert(!firmwareSizeAcceptable(0, 0x1E0000));
  assert(!firmwareSizeAcceptable(-1, 0x1E0000));
}
