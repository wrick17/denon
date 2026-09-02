#include <assert.h>
#include <stdint.h>

#include "../src/volume_target.h"

int main() {
  assert(!volumeMovementInDirection(72, 72, 1));
  assert(volumeMovementInDirection(72, 73, 1));
  assert(!volumeMovementInDirection(72, 71, 1));
  assert(volumeMovementInDirection(72, 71, -1));
  assert(!volumeSettleWindowElapsed(299, 0, 0));
  assert(!volumeSettleWindowElapsed(599, 0, 300));
  assert(volumeSettleWindowElapsed(600, 0, 300));
  assert(!volumeSettleWindowElapsed(300, 0, 299));
  assert(!volumeSettleWindowElapsed(598, 0, 299));
  assert(volumeSettleWindowElapsed(599, 0, 299));
  const int freshMeasuredDeltaRaw = 0;
  const int freshMeasuredClicks = 0;
  assert(volumeRapidClickCount(29, freshMeasuredDeltaRaw,
                               freshMeasuredClicks) == 2);
  assert(volumeRapidClickCount(30, freshMeasuredDeltaRaw,
                               freshMeasuredClicks) == 6);
  assert(volumeRapidClickCount(31, freshMeasuredDeltaRaw,
                               freshMeasuredClicks) == 6);
  assert(volumeRapidClickCount(28, 2, 2) == 5);
  assert(volumeRapidClickCount(20, 10, 2) == 3);
  assert(volumeRapidClickCount(12, 2, 2) == 2);
  assert(volumeRapidClickCount(11, 2, 2) == 2);
  assert(volumeRapidClickCount(10, 2, 2) == 2);
  assert(volumeRapidClickCount(10, 10, 2) == 2);
  assert(volumeRapidClickCount(9, 2, 2) == 0);
  assert(volumeRapidClickCount(15, 2, 2) == 3);
  assert(volumeRapidClickCount(20, 2, 2) == 4);
  assert(volumeRapidClickCount(25, 2, 2) == 5);
  assert(volumeRapidClickCount(29, 2, 2) == 5);
  assert(volumeRapidClickCount(30, 2, 2) == 6);
  assert(volumeRapidClickCount(31, 2, 2) == 6);
  assert(volumeRapidClickCount(29, 2, 2) * kVolumeRapidWorstRawPerClick <= 29);
  assert(volumeRapidClickCount(30, 2, 2) * kVolumeRapidWorstRawPerClick <= 30);
  assert(volumeRapidClickCount(31, 2, 2) * kVolumeRapidWorstRawPerClick <= 31);
  assert(volumeRapidClickCount(10, 6, 2) == 2);
  assert(volumeRapidClickCount(30, 30, 5) == 4);
  assert(volumeRapidGainWithinBound(2, 2));
  assert(volumeRapidGainWithinBound(25, 5));
  assert(volumeRapidGainWithinBound(30, 6));
  assert(!volumeRapidGainWithinBound(0, 2));
  assert(!volumeRapidGainWithinBound(6, 1));
  assert(!volumeRapidGainWithinBound(11, 2));
  assert(!volumeRapidGainWithinBound(31, 6));
  assert(!volumeMovementInDirection(110, 109, 1));
  assert(!volumeMovementInDirection(110, 100, 1));
  assert(volumeRapidFeedbackValid(100, 100, 110, 1, 2));
  assert(!volumeRapidFeedbackValid(100, 110, 109, 1, 2));
  assert(!volumeRapidFeedbackValid(100, 110, 100, 1, 2));
  assert(volumeRapidFeedbackValid(90, 115, 120, 1, 6));
  assert(!volumeRapidFeedbackValid(90, 120, 121, 1, 6));
  assert(volumeDirection(120, 120) != 1);
  assert(volumeDirection(121, 120) != 1);
  assert(!volumeRapidExtensionAllowed(94, 90, -1, true, false, 6, 196));
  assert(volumeRapidExtensionAllowed(95, 90, -1, true, false, 6, 196));
  assert(volumeRapidExtensionAllowed(99, 90, -1, true, false, 6, 196));
  assert(!volumeRapidExtensionAllowed(95, 90, -1, false, false, 6, 196));
  assert(!volumeRapidExtensionAllowed(95, 90, -1, true, true, 6, 196));
  assert(!volumeRapidExtensionAllowed(95, 90, -1, true, false, 196, 196));
  assert(!volumeRapidExtensionAllowed(95, -1, -1, true, false, 6, 196));
  assert(!volumeRapidExtensionAllowed(90, 90, -1, true, false, 6, 196));
  assert(!volumeRapidExtensionAllowed(89, 90, -1, true, false, 6, 196));
  assert(volumeRapidFeedbackValid(98, 98, 96, -1, 1));
  assert(volumeRapidExtensionAllowed(96, 90, -1, true, false, 7, 196));
  assert(volumeRapidFeedbackValid(96, 96, 94, -1, 1));
  assert(!volumeRapidExtensionAllowed(94, 90, -1, true, false, 8, 196));
  assert(!volumeRapidFeedbackValid(98, 98, 92, -1, 1));
  assert(!volumeRapidFeedbackValid(98, 98, 99, -1, 1));
  bool extensionPending = true;
  int bufferedRaw = 98;
  int extensionBaseline = -1;
  int extensionSends = 0;
  const auto observeBeforeExtension = [&](int raw) {
    if (raw == bufferedRaw) return;
    const bool continuedInDirection =
        volumeMovementInDirection(bufferedRaw, raw, -1);
    bufferedRaw = raw;
    if (extensionPending &&
        (!continuedInDirection ||
         !volumeRapidExtensionAllowed(raw, 90, -1, true, false, 6, 196))) {
      extensionPending = false;
    }
  };
  observeBeforeExtension(96);
  observeBeforeExtension(95);
  assert(extensionPending && bufferedRaw == 95);
  if (extensionPending &&
      volumeRapidExtensionAllowed(bufferedRaw, 90, -1, true, false, 6, 196)) {
    extensionBaseline = bufferedRaw;
    extensionPending = false;
    ++extensionSends;
  }
  assert(extensionSends == 1 && extensionBaseline == 95 && !extensionPending);

  extensionPending = true;
  bufferedRaw = 98;
  observeBeforeExtension(98);
  assert(extensionPending && bufferedRaw == 98);
  observeBeforeExtension(94);
  assert(!extensionPending && bufferedRaw == 94);
  extensionPending = true;
  bufferedRaw = 98;
  observeBeforeExtension(90);
  assert(!extensionPending);
  extensionPending = true;
  bufferedRaw = 98;
  observeBeforeExtension(89);
  assert(!extensionPending);
  extensionPending = true;
  bufferedRaw = 98;
  observeBeforeExtension(99);
  assert(!extensionPending);
  int rapidObservedRaw = 100;
  bool rapidFailed = false;
  const auto observeRapid = [&](int raw) {
    if (rapidFailed) return false;
    if (!volumeRapidFeedbackValid(100, rapidObservedRaw, raw, 1, 2)) {
      rapidFailed = true;
      return false;
    }
    rapidObservedRaw = raw;
    return true;
  };
  assert(!observeRapid(111));
  assert(!observeRapid(109));
  assert(rapidFailed && rapidObservedRaw == 100);

  int spotifyRaw = 90;
  int netflixRaw = 120;
  bool learningSuppressed = true;
  int blockedRaw = 90;
  const auto learnNetflix = [&](uint8_t raw) {
    if (learningSuppressed) {
      if (!canResumeLearningAfterFailure(true, blockedRaw, raw)) return;
      learningSuppressed = false;
    }
    netflixRaw = raw;
  };
  learnNetflix(90);
  learnNetflix(90);
  assert(spotifyRaw == 90);
  assert(netflixRaw == 120);
  learnNetflix(91);
  assert(!learningSuppressed);
  assert(netflixRaw == 91);

  netflixRaw = 120;
  learningSuppressed = true;
  blockedRaw = 90;
  bool automaticFeedbackPending = false;
  const auto observeCancelledRestore = [&](uint8_t raw) {
    if (automaticFeedbackPending) {
      blockedRaw = raw;
      return;
    }
    if (!canResumeLearningAfterFailure(true, blockedRaw, raw)) return;
    learningSuppressed = false;
    netflixRaw = raw;
  };
  observeCancelledRestore(90);
  assert(learningSuppressed && netflixRaw == 120);
  observeCancelledRestore(91);
  assert(!learningSuppressed && netflixRaw == 91);

  netflixRaw = 120;
  learningSuppressed = true;
  blockedRaw = 90;
  automaticFeedbackPending = true;
  observeCancelledRestore(91);
  assert(learningSuppressed && blockedRaw == 91 && netflixRaw == 120);
  automaticFeedbackPending = false;
  observeCancelledRestore(91);
  assert(learningSuppressed && netflixRaw == 120);
  observeCancelledRestore(92);
  assert(!learningSuppressed && netflixRaw == 92);

  assert(!manualMuteLockClearsOnVolume(false, 72, 73, false));
  assert(!manualMuteLockClearsOnVolume(true, -1, 73, false));
  assert(!manualMuteLockClearsOnVolume(true, 72, 72, false));
  assert(manualMuteLockClearsOnVolume(true, 72, 73, false));
  assert(!manualMuteLockClearsOnVolume(true, 72, 73, true));
  assert(!manualVolumeFeedbackMatches(72, 73, -1));
  assert(manualVolumeFeedbackMatches(73, 72, -1));
  assert(!manualVolumeFeedbackMatches(72, 71, 1));
  assert(manualVolumeFeedbackMatches(71, 72, 1));
  assert(!manualVolumeFeedbackMatches(72, 72, 0));
  assert(volumeDirection(72, 56) == -1);
  assert(!manualVolumeFeedbackMatches(72, 73, volumeDirection(72, 56)));
  assert(manualVolumeFeedbackMatches(73, 72, volumeDirection(72, 56)));
  assert(!volumeCommandAllowed(false, false, false));
  assert(!volumeCommandAllowed(true, true, false));
  assert(!volumeCommandAllowed(true, false, true));
  assert(volumeCommandAllowed(true, false, false));
  assert(playbackIdleAuthorizationFresh(true, 100, 5100, 5000));
  assert(!playbackIdleAuthorizationFresh(true, 100, 5101, 5000));
  assert(!playbackIdleAuthorizationFresh(false, 100, 101, 5000));
  const char firstEvent[] = "0123456789abcdef0123456789abcdef";
  const char nextEvent[] = "fedcba9876543210fedcba9876543210";
  const char emptyEvent[33] = {};
  assert(validPlaybackEventId(firstEvent, 32));
  assert(validPlaybackEventId(nextEvent, 32));
  assert(!validPlaybackEventId("", 0));
  assert(!validPlaybackEventId("0123456789abcdef", 16));
  assert(!validPlaybackEventId("0123456789abcdef0123456789abcdeF", 32));
  assert(playbackIdleEventGrants(true, false, firstEvent, 32, emptyEvent));
  assert(!playbackIdleEventGrants(true, false, firstEvent, 32, firstEvent));
  assert(playbackIdleEventGrants(true, false, nextEvent, 32, firstEvent));
  assert(!playbackIdleEventGrants(true, false, "", 0, firstEvent));
  assert(!playbackIdleEventGrants(true, true, nextEvent, 32, firstEvent));
  assert(!playbackIdleEventReady(false, true, 90, true, true, true, false,
                                false));
  assert(!playbackIdleEventReady(true, false, 90, true, true, true, false,
                                false));
  assert(!playbackIdleEventReady(true, true, -1, true, true, true, false,
                                false));
  assert(!playbackIdleEventReady(true, true, 90, false, true, true, false,
                                false));
  assert(!playbackIdleEventReady(true, true, 90, true, true, true, true,
                                false));
  assert(!playbackIdleEventReady(true, true, 90, true, false, false, false,
                                true));
  assert(playbackIdleEventReady(true, true, 90, true, false, false, false,
                               false));
  assert(playbackIdleEventReady(true, true, 90, true, true, true, false,
                               false));
  assert(!playbackAuthorizationExpiryFails(90, 90));
  assert(playbackAuthorizationExpiryFails(91, 90));
  assert(playbackAuthorizationExpiryFails(90, -1));
  assert(!automaticRemuteConfirmed(true, false, true, true));
  assert(!automaticRemuteConfirmed(true, true, true, false));
  assert(!automaticRemuteConfirmed(true, true, false, true));
  assert(automaticRemuteConfirmed(true, true, true, true));
  assert(!automaticRemuteFeedbackChanged(true, false, 106, 106));
  assert(!automaticRemuteFeedbackChanged(true, true, 90, 106));
  assert(automaticRemuteFeedbackChanged(true, false, 90, 106));
  assert(!volumeTargetMayHavePendingCommand(-1, 1));
  assert(!volumeTargetMayHavePendingCommand(120, 0));
  assert(volumeTargetMayHavePendingCommand(120, 1));
  assert(!appVolumeLearningAllowed(-1, false, 90, false, false, false));
  assert(!appVolumeLearningAllowed(-1, false, 90, true, true, true));
  assert(!appVolumeLearningAllowed(-1, false, 90, true, false, true));
  assert(!appVolumeLearningAllowed(120, false, 90, true, false, false));
  assert(!appVolumeLearningAllowed(-1, true, 90, true, false, false));
  assert(!appVolumeLearningAllowed(-1, false, -1, true, false, false));
  assert(appVolumeLearningAllowed(-1, false, 90, true, false, false));
  assert(!appClearCancelsVolumeTarget(-1, false));
  assert(!appClearCancelsVolumeTarget(-1, true));
  assert(!appClearCancelsVolumeTarget(72, false));
  assert(appClearCancelsVolumeTarget(72, true));
  uint32_t generation = 7;
  if (appClearCancelsVolumeTarget(-1, false)) ++generation;
  if (appClearCancelsVolumeTarget(-1, false)) ++generation;
  assert(generation == 7);
  if (volumeTargetCancellationAdvancesGeneration(-1)) ++generation;
  if (volumeTargetCancellationAdvancesGeneration(-1)) ++generation;
  assert(generation == 7);
  if (volumeTargetCancellationAdvancesGeneration(72)) ++generation;
  assert(generation == 8);

  assert(volumeDirection(99, 100) == 1);
  assert(volumeDirection(101, 100) == -1);
  assert(volumeDirection(100, 100) == 0);
  assert(volumeDirection(98, 90) == -1);
  assert(volumeDirection(90, 90) != -1);
  assert(volumeDirection(89, 90) != -1);
  assert(volumeFeedbackImproved(100, 90, 91));
  assert(volumeFeedbackImproved(100, 90, 101));
  assert(!volumeFeedbackImproved(100, 99, 101));
  assert(!volumeFeedbackImproved(100, 90, 90));
  assert(!volumeFeedbackImproved(100, 90, 89));

  uint8_t raw = 0;
  assert(parseDisplayedVolume("0", 1, raw) && raw == 0);
  assert(parseDisplayedVolume("50", 2, raw) && raw == 100);
  assert(parseDisplayedVolume("50.5", 4, raw) && raw == 101);
  assert(parseDisplayedVolume("98.0", 4, raw) && raw == 196);
  assert(!parseDisplayedVolume("", 0, raw));
  assert(!parseDisplayedVolume("50.1", 4, raw));
  assert(!parseDisplayedVolume("98.5", 4, raw));
  assert(!parseDisplayedVolume("-1", 2, raw));
  assert(!parseDisplayedVolume("50x", 3, raw));
  return 0;
}
