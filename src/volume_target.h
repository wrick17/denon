#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr uint8_t kMaxVolumeRaw = 196;
constexpr uint32_t kVolumeCommandMinGapMs = 300;
constexpr uint32_t kVolumeReportQuietMs = 300;
constexpr uint32_t kVolumeRapidIntervalMs = 100;
constexpr int kVolumeRapidProbeClicks = 2;
constexpr int kVolumeRapidMaxClicks = 6;
constexpr int kVolumeRapidWorstRawPerClick = 5;
constexpr int kVolumeRapidFineAimRaw = 3;

inline int volumeDirection(int current, int target) {
  return current < target ? 1 : (current > target ? -1 : 0);
}

inline bool volumeFeedbackImproved(int target, int commandRaw, int observedRaw) {
  const int previousDistance =
      target > commandRaw ? target - commandRaw : commandRaw - target;
  const int observedDistance =
      target > observedRaw ? target - observedRaw : observedRaw - target;
  return observedDistance < previousDistance;
}

inline bool volumeMovementInDirection(int commandRaw, int observedRaw,
                                      int direction) {
  return direction > 0 ? observedRaw > commandRaw : observedRaw < commandRaw;
}

inline bool volumeSettleWindowElapsed(uint32_t now, uint32_t commandAt,
                                     uint32_t lastChangedAt) {
  return now - commandAt >= kVolumeCommandMinGapMs &&
         now - lastChangedAt >= kVolumeReportQuietMs;
}

inline int volumeRapidClickCount(int distanceRaw, int measuredDeltaRaw,
                                 int measuredClicks) {
  if (distanceRaw <= kVolumeRapidFineAimRaw) return 0;
  const int noOvershootClicks =
      distanceRaw / kVolumeRapidWorstRawPerClick;
  if (noOvershootClicks < kVolumeRapidProbeClicks) return 0;
  if ((measuredDeltaRaw <= 0 || measuredClicks <= 0) &&
      noOvershootClicks >= kVolumeRapidMaxClicks) {
    return kVolumeRapidMaxClicks;
  }
  int clicks = kVolumeRapidProbeClicks;
  if (measuredDeltaRaw > 0 && measuredClicks > 0) {
    clicks = ((distanceRaw - kVolumeRapidFineAimRaw) * measuredClicks) /
             measuredDeltaRaw;
  }
  if (clicks < kVolumeRapidProbeClicks) clicks = kVolumeRapidProbeClicks;
  if (clicks > noOvershootClicks) clicks = noOvershootClicks;
  if (clicks > kVolumeRapidMaxClicks) clicks = kVolumeRapidMaxClicks;
  return clicks >= kVolumeRapidProbeClicks ? clicks : 0;
}

inline bool volumeRapidGainWithinBound(int movedRaw, int sentClicks) {
  return movedRaw > 0 && sentClicks > 0 &&
         movedRaw <= kVolumeRapidWorstRawPerClick * sentClicks;
}

inline bool volumeRapidFeedbackValid(int startRaw, int previousRaw,
                                     int observedRaw, int direction,
                                     int sentClicks) {
  const int movedRaw = observedRaw > startRaw ? observedRaw - startRaw
                                               : startRaw - observedRaw;
  return volumeMovementInDirection(previousRaw, observedRaw, direction) &&
         volumeRapidGainWithinBound(movedRaw, sentClicks);
}

inline bool volumeRapidExtensionAllowed(int currentRaw, int targetRaw,
                                        int commandDirection,
                                        bool commandAllowed,
                                        bool deadlineReached, int steps,
                                        int maxSteps) {
  if (targetRaw < 0 || !commandAllowed || deadlineReached ||
      steps >= maxSteps ||
      volumeDirection(currentRaw, targetRaw) != commandDirection) {
    return false;
  }
  const int remainingRaw = targetRaw > currentRaw ? targetRaw - currentRaw
                                                   : currentRaw - targetRaw;
  return remainingRaw >= kVolumeRapidWorstRawPerClick;
}

inline bool canResumeLearningAfterFailure(bool cooldownElapsed, int failedRaw,
                                          uint8_t observedRaw) {
  return cooldownElapsed && failedRaw >= 0 && observedRaw != failedRaw;
}

inline bool manualMuteLockClearsOnVolume(bool locked, int lockedRaw,
                                         int observedRaw,
                                         bool automaticFeedbackPending) {
  return locked && !automaticFeedbackPending && lockedRaw >= 0 &&
         observedRaw != lockedRaw;
}

inline bool manualVolumeFeedbackMatches(int baselineRaw, int observedRaw,
                                        int direction) {
  return direction != 0 &&
         volumeMovementInDirection(baselineRaw, observedRaw, direction);
}

inline bool volumeCommandAllowed(bool muteKnown, bool muted,
                                 bool manualMuteLocked) {
  return muteKnown && !muted && !manualMuteLocked;
}

inline bool playbackIdleAuthorizationFresh(bool authorized,
                                           uint32_t receivedAt,
                                           uint32_t now, uint32_t ttl) {
  return authorized && now - receivedAt <= ttl;
}

inline bool validPlaybackEventId(const char *eventId, size_t length) {
  if (eventId == nullptr || length != 32) return false;
  for (size_t index = 0; index < length; ++index) {
    const char value = eventId[index];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f'))) {
      return false;
    }
  }
  return true;
}

inline bool playbackIdleEventGrants(bool playbackKnown, bool playbackActive,
                                    const char *eventId, size_t length,
                                    const char *lastGrantedEventId) {
  if (!playbackKnown || playbackActive ||
      !validPlaybackEventId(eventId, length)) {
    return false;
  }
  if (lastGrantedEventId == nullptr) return true;
  for (size_t index = 0; index < length; ++index) {
    if (eventId[index] != lastGrantedEventId[index]) return true;
  }
  return lastGrantedEventId[length] != '\0';
}

inline bool playbackIdleEventReady(bool connected, bool sessionInitialized,
                                   int volumeRaw, bool muteKnown, bool muted,
                                   bool manualMuteLocked,
                                   bool remuteRequired,
                                   bool manualTargetActive) {
  if (!connected || !sessionInitialized || volumeRaw < 0 || !muteKnown ||
      remuteRequired || manualTargetActive) {
    return false;
  }
  return volumeCommandAllowed(muteKnown, muted, manualMuteLocked) ||
         (muted && manualMuteLocked);
}

inline bool playbackAuthorizationExpiryFails(int currentRaw, int targetRaw) {
  return targetRaw < 0 || currentRaw != targetRaw;
}

inline bool automaticRemuteConfirmed(bool remuteRequired,
                                     bool confirmationPending,
                                     bool muteKnown, bool muted) {
  return remuteRequired && confirmationPending && muteKnown && muted;
}

inline bool automaticRemuteFeedbackChanged(bool remuteRequired,
                                            bool automaticMuteCycle,
                                            int baselineRaw,
                                            int observedRaw) {
  return remuteRequired && !automaticMuteCycle && baselineRaw != observedRaw;
}

inline bool volumeTargetMayHavePendingCommand(int targetRaw, uint16_t steps) {
  return targetRaw >= 0 && steps > 0;
}

inline bool appVolumeLearningAllowed(int targetRaw, bool learningSuppressed,
                                     int volumeRaw, bool muteKnown, bool muted,
                                     bool manualMuteLocked) {
  return targetRaw < 0 && !learningSuppressed && volumeRaw >= 0 &&
         volumeCommandAllowed(muteKnown, muted, manualMuteLocked);
}

inline bool appClearCancelsVolumeTarget(int targetRaw, bool automatic) {
  return targetRaw >= 0 && automatic;
}

inline bool volumeTargetCancellationAdvancesGeneration(int targetRaw) {
  return targetRaw >= 0;
}

inline bool parseDisplayedVolume(const char *text, size_t length, uint8_t &raw) {
  if (text == nullptr || length == 0) return false;

  unsigned whole = 0;
  size_t position = 0;
  while (position < length && text[position] >= '0' && text[position] <= '9') {
    whole = whole * 10 + static_cast<unsigned>(text[position++] - '0');
    if (whole > kMaxVolumeRaw / 2) return false;
  }
  if (position == 0) return false;

  unsigned half = 0;
  if (position < length) {
    if (text[position++] != '.' || position + 1 != length ||
        (text[position] != '0' && text[position] != '5')) {
      return false;
    }
    half = text[position] == '5' ? 1 : 0;
  }

  const unsigned encoded = whole * 2 + half;
  if (encoded > kMaxVolumeRaw) return false;
  raw = static_cast<uint8_t>(encoded);
  return true;
}
