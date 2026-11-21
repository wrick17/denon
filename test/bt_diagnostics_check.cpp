// Exercise the loop-side ACL diagnostics recorder.
#include <cassert>
#include "bt_diagnostics.h"

namespace {
constexpr int kSuccess = 0;
void connect(int status, uint16_t handle, uint32_t at) {
  btDiagnostics::recordConnect(status, handle, at, status == kSuccess);
}
}  // namespace

int main() {
  using namespace btDiagnostics;
  assert(lastConnectStatus == -1 && lastDisconnectReason == -1);

  connect(260, 0xffff, 100);
  recordDisconnect(260, 0xffff, 100);
  assert(connectCount == 1 && lastConnectStatus == 260 && lastConnectAtMs == 100);
  assert(lastDisconnectReason == -1);

  connect(kSuccess, 7, 200);
  recordDisconnect(0x108, 8, 250);
  assert(lastDisconnectReason == -1);
  recordDisconnect(0x108, 7, 300);
  assert(lastDisconnectReason == 0x108 && lastDisconnectAtMs == 300);

  connect(260, 0xffff, 400);
  recordDisconnect(260, 7, 400);
  assert(connectCount == 3 && lastConnectAtMs == 400);
  assert(lastDisconnectReason == 0x108 && lastDisconnectAtMs == 300);

  connect(kSuccess, 8, 500);
  recordDisconnect(0x113, 8, 600);
  assert(lastDisconnectReason == 0x113 && lastDisconnectAtMs == 600);
}
