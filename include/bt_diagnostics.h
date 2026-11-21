#pragma once

#include <stdint.h>

// Passive ACL diagnostics for the configured receiver. The GAP callback only
// queues events; the loop task records them here, so no locking is needed.
namespace btDiagnostics {
uint32_t connectCount = 0;
int lastConnectStatus = -1;
uint32_t lastConnectAtMs = 0;
int lastDisconnectReason = -1;
uint32_t lastDisconnectAtMs = 0;
int establishedHandle = -1;

void recordConnect(int status, uint16_t handle, uint32_t at, bool success) {
  lastConnectAtMs = at;
  lastConnectStatus = status;
  ++connectCount;
  if (success) establishedHandle = handle;
}

// Only a disconnect of the established link counts; failed pages also report
// a disconnect with the page-timeout status and an invalid handle.
void recordDisconnect(int reason, uint16_t handle, uint32_t at) {
  if (establishedHandle != handle) return;
  lastDisconnectAtMs = at;
  lastDisconnectReason = reason;
  establishedHandle = -1;
}
}  // namespace btDiagnostics
