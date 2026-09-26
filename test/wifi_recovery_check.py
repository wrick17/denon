"""Run the firmware's actual Wi-Fi maintenance function against a fake radio."""

from pathlib import Path
import subprocess
import tempfile


source = (Path(__file__).parents[1] / "src" / "main.cpp").read_text()
maintain_wifi = source[source.index("void maintainWifi()") : source.index("bool sendDenon(")]

harness = r'''
#include <cassert>
#include <cstdint>
#include <string>

constexpr int WL_CONNECTED = 3;
constexpr uint32_t kWifiTimeoutMs = 15000;
constexpr uint32_t kWifiReconnectMs = 30000;
constexpr int kHttpPort = 80;
const char *kMdnsService = "denon-volume";
const char *kProduct = "denon-volume-memory";
const char *kApiVersion = "1";
std::string hostName = "denon-test";
std::string deviceId = "test";
struct Token { bool isEmpty() { return true; } } apiToken;
uint32_t nowMs = 0;
unsigned long millis() { return nowMs; }
bool wifiWasConnected = false;
bool wifiConfigured = false;
bool staticNetworkEnabled = false;
bool stationHttpClaimed = false;
bool setupApRunning = false;
bool mdnsRunning = false;
unsigned long wifiStartedAt = 0;
uint32_t wifiLastRetryAt = 0;
uint32_t wifiRetryCount = 0;
unsigned long apiClaimUntil = 0;
constexpr unsigned long kApiClaimWindowMs = 600000;

struct Address { std::string toString() { return "192.0.2.10"; } };
struct Radio {
  int connectionStatus = 0;
  int retries = 0;
  bool associated = false;
  int status() { return connectionStatus; }
  Address localIP() { return {}; }
} WiFi;
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
struct wifi_ap_record_t {};
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *) {
  return WiFi.associated ? ESP_OK : -1;
}
esp_err_t esp_wifi_connect() { ++WiFi.retries; return -1; }
struct Mdns {
  bool begin(const char *) { return true; }
  void end() {}
  void addService(const char *, const char *, int) {}
  void addServiceTxt(const char *, const char *, const char *, const char *) {}
} MDNS;
struct Logger {
  template <typename... Args> void printf(const char *, Args...) {}
} Serial;
bool finishPendingStaticNetwork() { return false; }
bool shouldStopSetupAp(bool configured, bool claimed) { return !configured || claimed; }
void startSetupAp() { setupApRunning = true; }
void stopSetupAp() { setupApRunning = false; }

__MAINTAIN_WIFI__

int main() {
  nowMs = 30000;
  maintainWifi();
  assert(setupApRunning && WiFi.retries == 0);  // No saved SSID.

  wifiConfigured = true;
  setupApRunning = false;
  nowMs = 29999;
  maintainWifi();
  assert(WiFi.retries == 0);
  nowMs = 30000;
  maintainWifi();
  assert(setupApRunning && WiFi.retries == 1 && wifiRetryCount == 1);
  nowMs = 59999;
  maintainWifi();
  assert(WiFi.retries == 1);
  nowMs = 60000;
  maintainWifi();
  assert(WiFi.retries == 2 && wifiRetryCount == 2);  // Failed calls stay spaced.

  WiFi.connectionStatus = WL_CONNECTED;
  nowMs = 61000;
  maintainWifi();
  assert(!setupApRunning && wifiWasConnected && WiFi.retries == 2);
  nowMs = 500000;
  maintainWifi();
  assert(!setupApRunning && WiFi.retries == 2);  // Healthy link stays untouched.
  WiFi.connectionStatus = 0;
  nowMs = 501000;
  maintainWifi();
  nowMs = 530999;
  maintainWifi();
  assert(WiFi.retries == 2);
  nowMs = 531000;
  maintainWifi();
  assert(WiFi.retries == 3);

  // Model ESP32's 32-bit millis rollover, regardless of host unsigned long width.
  wifiLastRetryAt = UINT32_MAX - 10000;
  nowMs = 19998;
  maintainWifi();
  assert(WiFi.retries == 3);
  nowMs = 19999;
  maintainWifi();
  assert(WiFi.retries == 4);

  staticNetworkEnabled = true;
  stationHttpClaimed = false;
  setupApRunning = true;
  wifiWasConnected = true;
  WiFi.connectionStatus = WL_CONNECTED;
  nowMs = 100000;
  maintainWifi();
  assert(setupApRunning && WiFi.retries == 4);
  WiFi.connectionStatus = 0;
  nowMs = 101000;
  maintainWifi();
  assert(setupApRunning && !stationHttpClaimed);
  WiFi.connectionStatus = WL_CONNECTED;
  nowMs = 132000;
  maintainWifi();
  assert(setupApRunning && WiFi.retries == 4);
  stationHttpClaimed = true;
  maintainWifi();
  assert(!setupApRunning);

  WiFi.connectionStatus = 0;
  nowMs = 200000;
  maintainWifi();
  WiFi.associated = true;  // Associated to router, still waiting for DHCP/IP.
  nowMs = 230000;
  maintainWifi();
  assert(setupApRunning && WiFi.retries == 4 && wifiRetryCount == 4);
  nowMs = 260000;
  maintainWifi();
  assert(WiFi.retries == 4);
  WiFi.associated = false;
  nowMs = 289999;
  maintainWifi();
  assert(WiFi.retries == 4);
  nowMs = 290000;
  maintainWifi();
  assert(WiFi.retries == 5 && wifiRetryCount == 5);
}
'''.replace("__MAINTAIN_WIFI__", maintain_wifi)

with tempfile.TemporaryDirectory() as temporary:
    executable = Path(temporary) / "wifi_recovery_check"
    subprocess.run(
        ["c++", "-std=c++17", "-x", "c++", "-", "-o", str(executable)],
        input=harness,
        text=True,
        check=True,
    )
    subprocess.run([str(executable)], check=True)

print("Wi-Fi recovery checks passed")
