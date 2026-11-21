#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp32-hal-bt.h>
#include <esp_a2dp_api.h>
#include <esp_bt.h>
#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_core_dump.h>
#include <esp_flash.h>
#include <esp_gap_bt_api.h>
#include <esp_ota_ops.h>
#include <esp_spp_api.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <freertos/message_buffer.h>
#include <freertos/queue.h>
#include <atomic>

#include "config.h"
#include "bt_diagnostics.h"
#include "network_logs.h"
#include "recovery_policy.h"
#include "volume_target.h"

// Arduino marks a pending OTA image valid at boot by default; this firmware
// confirms it only after the health check in maintainOtaGuard().
extern "C" bool verifyRollbackLater() { return true; }

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error "Bluetooth Classic requires the original ESP32 and Bluedroid."
#endif

#if !defined(CONFIG_BT_SPP_ENABLED)
#error "Denon control needs Bluetooth Classic SPP, available only on the original ESP32."
#endif

namespace {

constexpr char kProduct[] = "denon-volume-memory";
constexpr char kMdnsService[] = "denon-volume";
constexpr char kApiVersion[] = "1";
constexpr uint16_t kHttpPort = 80;
constexpr unsigned long kWifiTimeoutMs = 15000;
constexpr uint32_t kWifiReconnectMs = 30000;
constexpr unsigned long kReconnectMs = 5000;
constexpr unsigned long kStatusIntervalMs = 5000;
constexpr unsigned long kBondRemovalTimeoutMs = 3000;
constexpr uint8_t kDenonSppChannel = 2;
constexpr unsigned long kBtAttemptTimeoutMs = 30000;
constexpr unsigned long kBtWriteStallMs = 5000;
constexpr size_t kBtTxQueueLength = 8;
constexpr unsigned long kBtProfileStartMs = 10000;
constexpr unsigned long kBtProfileWaitMs = 2000;
constexpr unsigned long kA2dpTransitionTimeoutMs = 60000;
constexpr unsigned long kSppServerRetryMs = 60000;
constexpr size_t kBtRxChunkBytes = 128;
constexpr unsigned long kFirmwareBodyTimeoutMs = 120000;
constexpr uint32_t kBtLifecycleMarker = 0xB7C1F00D;
constexpr unsigned long kCleanShutdownMs = 2000;
constexpr uint32_t kLoopWatchdogSeconds = 60;
constexpr size_t kMaxDiscovered = 20;
constexpr size_t kBtEventQueueLength = 32;
constexpr size_t kBtRxBufferBytes = 2048;
constexpr char kBtDeviceName[] = "Denon ESP32";
constexpr unsigned long kAppStableMs = 750;
constexpr unsigned long kAppPersistDelayMs = 2000;
constexpr unsigned long kPlaybackIdleFreshMs = 5000;
constexpr unsigned long kVolumeMovementStatusDelayMs = 450;
constexpr unsigned long kVolumeStepResponseTimeoutMs = 1800;
constexpr unsigned long kVolumeStatusResponseTimeoutMs = 1200;
constexpr unsigned long kRestoreDeadlineMs = 30000;
constexpr unsigned long kRestoreFailureCooldownMs = 2000;
constexpr unsigned long kApiClaimWindowMs = 10UL * 60UL * 1000UL;
constexpr unsigned long kTokenResetHoldMs = 10UL * 1000UL;
constexpr uint8_t kBootButtonPin = 0;
constexpr uint16_t kRestoreMaxSteps = kMaxVolumeRaw;
constexpr size_t kMaxApps = 16;
constexpr size_t kMaxAppIdLength = 96;
constexpr size_t kMaxAppNameLength = 64;
constexpr size_t kMaxBackupBodyLength = 24576;
constexpr uint32_t kStoredAppMagic = 0x44564131;  // DVA1

constexpr uint8_t kVolumeUp[] = {0x41, 0x54, 0x07, 0x00, 0x00, 0x00};
constexpr uint8_t kVolumeDown[] = {0x41, 0x54, 0x07, 0x01, 0x00, 0x00};
constexpr uint8_t kMuteOn[] = {0x41, 0x54, 0x07, 0x1D, 0x01, 0x01, 0xFE};
constexpr uint8_t kControlHandshake[] = {0x41, 0x54, 0x07, 0x25,
                                         0x01, 0x20, 0xDF};
constexpr uint8_t kGetSources[] = {0x41, 0x54, 0x00, 0x04, 0x00, 0x00};
constexpr uint8_t kGetStatus[] = {0x41, 0x54, 0x00, 0x06, 0x00, 0x00};

DNSServer dnsServer;
Preferences preferences;
Preferences otaPreferences;  // "ota" namespace; opened before the self-checks
// Set while the Bluetooth lifecycle task runs; survives panic/watchdog resets.
RTC_NOINIT_ATTR uint32_t rtcBtLifecycleMarker;
bool stationHttpClaimed = false;

bool isSetupApIngress(bool apRunning, bool matchesAp, bool matchesStation) {
  return apRunning && matchesAp && !matchesStation;
}

bool isStationIngress(bool connected, bool matchesStation, bool matchesAp) {
  return connected && matchesStation && !matchesAp;
}

class StationAwareWebServer : public WebServer {
 public:
  explicit StationAwareWebServer(int port) : WebServer(port) {}

  // client() returns a copy; stopping a copy leaves the request socket open.
  WiFiClient &requestClient() { return _currentClient; }

 protected:
  size_t _currentClientWrite(const char *buffer, size_t length) override {
    const size_t written = WebServer::_currentClientWrite(buffer, length);
    if (length > 0 && written == length) markStationRequest();
    return written;
  }

  size_t _currentClientWrite_P(PGM_P buffer, size_t length) override {
    const size_t written = WebServer::_currentClientWrite_P(buffer, length);
    if (length > 0 && written == length) markStationRequest();
    return written;
  }

 private:
  void markStationRequest() {
    const IPAddress localAddress = _currentClient.localIP();
    if (isStationIngress(WiFi.status() == WL_CONNECTED,
                         localAddress == WiFi.localIP(),
                         localAddress == WiFi.softAPIP())) {
      stationHttpClaimed = true;
    }
  }
};

StationAwareWebServer server(kHttpPort);

struct StoredAppVolume {
  uint32_t magic = 0;
  uint32_t sequence = 0;
  uint8_t raw = 0;
  char appId[kMaxAppIdLength + 1] = {};
  char appName[kMaxAppNameLength + 1] = {};
};

enum class VolumeTargetPhase : uint8_t {
  idle,
  needFreshStatus,
  waitFreshStatus,
  waitAutomaticUnmute,
  ready,
  waitBurstSecond,
  waitMovement,
  waitMovementStatus,
  settling,
  waitConfirmation,
};

StoredAppVolume appVolumes[kMaxApps];
bool appDirty[kMaxApps] = {};
unsigned long appPersistAt[kMaxApps] = {};
uint32_t appSequence = 0;
uint8_t activeAppBank = 0;
String deviceId;
String hostName;
String setupApName;
String apiToken;
String currentAppId;
String currentAppName;
String pendingAppId;
String pendingAppName;
unsigned long pendingAppAt = 0;
bool pendingPlaybackIdleAuthorized = false;
unsigned long pendingPlaybackAt = 0;
bool pendingRestoreAllowed = true;
bool currentPlaybackIdleAuthorized = false;
unsigned long currentPlaybackAt = 0;
char lastPlaybackIdleEventId[33] = {};
unsigned long apiClaimUntil = 0;
bool wifiWasConnected = false;
bool wifiConfigured = false;
uint32_t wifiLastRetryAt = 0;
uint32_t wifiRetryCount = 0;
std::atomic<int> lastWifiDisconnectReason{-1};
bool staticNetworkEnabled = false;
IPAddress staticIp;
IPAddress staticGateway;
IPAddress staticSubnet;
IPAddress staticDns;
int restoreTargetRaw = -1;
bool restoreSessionArmed = false;
bool restoreAutomatic = false;
bool restoreManualMuteOverride = false;
bool restoreLearningSuppressed = false;
uint16_t restoreSteps = 0;
VolumeTargetPhase restorePhase = VolumeTargetPhase::idle;
int restoreObservedRaw = -1;
int restoreCommandRaw = -1;
int restoreCommandDirection = 0;
bool restoreCorrectiveReversalUsed = false;
bool restoreCoarseEnabled = true;
bool restoreBurstActive = false;
bool restoreBurstExtensionPending = false;
bool restoreAutomaticMuteCycle = false;
bool restoreAutomaticUnmuteObserved = false;
bool automaticRemuteRequired = false;
bool automaticMuteConfirmationPending = false;
bool automaticRemuteJournalPersisted = false;
unsigned long automaticMuteCommandAt = 0;
unsigned long automaticRemuteNotBefore = 0;
int automaticRemuteBaselineRaw = -1;
int restoreBurstStartRaw = -1;
uint8_t restoreBurstClicksPlanned = 0;
uint8_t restoreBurstClicksSent = 0;
int restoreBurstMeasuredDeltaRaw = 0;
int restoreBurstMeasuredClicks = 0;
unsigned long restoreCommandAt = 0;
unsigned long restoreLastChangedAt = 0;
unsigned long restorePhaseAt = 0;
unsigned long restoreDeadlineAt = 0;
String restoreError;
int restoreFailureRaw = -1;
unsigned long restoreLearningResumeAt = 0;
uint32_t volumeTargetGeneration = 0;
bool muteStateKnown = false;
bool denonMuted = false;
bool manualMuteLocked = false;
int manualMuteLockRaw = -1;
bool muteAutomaticFeedbackPending = false;
unsigned long muteAutomaticFeedbackUntil = 0;
int muteManualFeedbackDirection = 0;
unsigned long bootButtonPressedAt = 0;
bool tokenResetArmed = false;

uint8_t denonMac[6] = {};
bool hasDenonMac = false;
bool setupApRunning = false;
bool mdnsRunning = false;
bool bluetoothReady = false;
volatile bool denonBonded = false;
bool denonBondStateKnown = false;
volatile bool a2dpReady = false;
volatile bool a2dpConnecting = false;
volatile bool a2dpConnected = false;
volatile bool a2dpDisconnecting = false;
volatile bool forgetInProgress = false;
bool wasDenonConnected = false;
bool sessionInitialized = false;
volatile bool connectInProgress = false;
volatile bool pairingTimedOut = false;
volatile bool pairingNumberReady = false;
volatile uint32_t pairingNumber = 0;
int volumeRaw = -1;
unsigned long wifiStartedAt = 0;
volatile unsigned long nextReconnectAt = 0;
volatile unsigned long nextA2dpReconnectAt = 0;
unsigned long nextStatusAt = 0;

// Bluetooth runtime. Bluedroid callbacks only copy events into btEvents (and
// received bytes into btRx) or set the atomics below; every state change
// happens in the loop task. The bt_lifecycle task only starts/stops the stack
// while the loop leaves Bluetooth alone (btLifecycleBusy).
enum class BtEventType : uint8_t {
  aclConnect,
  aclDisconnect,
  authComplete,
  confirmRequest,
  pinRequest,
  discoveryResult,
  discoveryState,
  sppInit,
  sppServerStart,
  sppClientInit,
  sppOpen,
  sppServerOpen,
  sppClose,
  sppCongestion,
  sppWrite,
  a2dpProfile,
  a2dpConnection,
};

struct BtEvent {
  BtEventType type;
  uint8_t generation;
  uint8_t address[6];
  int32_t status;
  uint32_t handle;
  uint32_t value;
  uint32_t at;
  uint32_t connection;  // SPP connection sequence, assigned in the callback
  char name[33];
};

struct DiscoveredDevice {
  uint8_t address[6];
  char name[33];
};

// Each RX message is generation, handle, connection sequence, then the bytes.
struct BtRxHeader {
  uint8_t generation;
  uint32_t handle;
  uint32_t connection;
} __attribute__((packed));

// Control traffic only (handshake, sources, status, mute). Volume steps are
// never queued; see sendDenonStep().
struct BtTxCommand {
  const uint8_t *data;
  uint8_t length;
};

QueueHandle_t btEvents = nullptr;
MessageBufferHandle_t btRx = nullptr;
std::atomic<uint8_t> btGeneration{0};
std::atomic<uint32_t> btEventDrops{0};
std::atomic<uint32_t> btRxDrops{0};
std::atomic<bool> btLifecycleBusy{false};
std::atomic<const char *> btLifecycleError{nullptr};
std::atomic<bool> sppUninitDone{false};
std::atomic<bool> btTeardownFailed{false};
std::atomic<bool> a2dpDeinitDone{false};
bool btLifecycleResultPending = false;
unsigned long btLifecycleStartedAt = 0;
unsigned long btProfilesDeadline = 0;
const char *btLastError = nullptr;
bool sppReady = false;
bool sppServerRunning = false;
unsigned long sppServerRetryAt = 0;
bool sppOpen = false;
uint32_t sppHandle = 0;
uint32_t sppConnection = 0;
bool sppCongested = false;
unsigned long sppTxProgressAt = 0;  // last submit, completion, or idle enqueue
bool sppWriteInFlight = false;
BtTxCommand sppTx[kBtTxQueueLength];
size_t sppTxHead = 0;
size_t sppTxCount = 0;
size_t denonRxLength = 0;
unsigned long a2dpTransitionAt = 0;
uint32_t attemptHandle = 0;
unsigned long attemptStartedAt = 0;
bool attemptPageTimeout = false;
BtLadder btLadder;
uint32_t btStartFailures = 0;
unsigned long nextStackRetryAt = 0;
bool firmwareRestartPending = false;
bool selfCheckPassed = false;
uint32_t btEventDropsHandled = 0;
bool discoveryActive = false;
bool discoveryRunning = false;
bool discoveryStarted = false;
DiscoveredDevice discovered[kMaxDiscovered];
size_t discoveredCount = 0;

// OTA boot guard state for this boot.
const char *otaBootState = "stable";
String otaLastResult;
uint8_t otaTrialBoots = 0;
bool otaTrialActive = false;
bool otaPendingVerify = false;

void restartDevice(bool clean);

const char kPage[] PROGMEM = R"HTML(
<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Denon volume</title><style>
body{font-family:system-ui,sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem;background:#111;color:#f4f4f4}main{background:#1d1d1d;border-radius:1rem;padding:1.25rem}button,input{box-sizing:border-box;font:inherit;border-radius:.65rem;padding:.8rem;border:0}button{background:#e9b949;color:#111;font-weight:700;min-width:5rem}button:disabled{opacity:.45}.row{display:flex;gap:.75rem;align-items:center;justify-content:center;margin:1rem 0}.volume{font-size:3.5rem;text-align:center}.muted{color:#aaa;font-size:.9rem;text-align:center}.mute-status{width:max-content;margin:.25rem auto 0;padding:.35rem .65rem;border-radius:99rem;background:#333;color:#ddd;font-weight:700}.mute-status.is-muted{background:#762727;color:#fff}.mute-status.is-unmuted{background:#194a34;color:#b8f5d4}.note{color:#bbb;font-size:.9rem;line-height:1.4}form{display:grid;gap:.6rem;margin-top:1rem}input{display:block;width:100%;margin-top:.25rem}#target{display:flex;align-items:center}#target label{white-space:nowrap}#target input{min-width:0;margin:0;flex:1}#target button{white-space:nowrap}#devices button{width:100%;margin-top:.4rem;text-align:left;background:#ddd;overflow-wrap:anywhere;white-space:normal}section{border-top:1px solid #444;margin-top:1.25rem;padding-top:.75rem}.secondary{background:#555;color:#fff;width:100%;margin-top:.6rem}.pairing{text-align:center;border:2px solid #e9b949;border-radius:.75rem;padding:1rem;margin-top:1rem}.pairing-number{font-size:3rem;font-weight:800;letter-spacing:.12em;margin:.4rem 0;color:#e9b949}.pairing-action{font-size:1.15rem;font-weight:700}table{width:100%;border-collapse:collapse;font-size:.9rem}th,td{text-align:left;padding:.55rem .3rem;border-bottom:1px solid #444}th:nth-child(n+2),td:nth-child(n+2){text-align:right}.active{color:#e9b949;font-weight:700}@media(max-width:22rem){#target{display:grid;grid-template-columns:1fr auto}#target label{grid-column:1/-1}#target input{width:100%}}
</style><main><h1>Denon volume</h1><p id=connection class=muted>Starting…</p><div id=volume class=volume>—</div><p id=db class=muted></p><p id=mute-status class=mute-status role=status aria-live=polite>Mute state unknown</p><div class=row><button onclick="send('down')" disabled>−</button><button onclick="send('up')" disabled>+</button></div><form id=target onsubmit=setTargetVolume(event)><label for=target-volume>Target volume</label><input id=target-volume type=number min=0 max=98 step=.5 required inputmode=decimal><button disabled>Set</button></form><p id=target-status class=note></p><section><h2>App volume memory</h2><p id=app-status class=note>Waiting for Home Assistant…</p><table><thead><tr><th>App</th><th>Volume</th><th>dB</th></tr></thead><tbody id=apps></tbody></table></section><section id=receiver><p class=note>On the Denon remote, hold <b>Bluetooth</b> for 3 seconds until Pairing appears, then select the receiver below. When the same six-digit number appears here and on the Denon, confirm it on the receiver. Set Bluetooth Auto-Select to Off if this control connection should not change inputs.</p><button id=scan onclick=scan()>Find receiver</button><div id=devices></div><div id=pairing-confirm class=pairing hidden><p>Confirm this number matches the Denon:</p><div id=pairing-number class=pairing-number></div><p class=pairing-action>Press ENTER on the Denon now</p><p class=note>The ESP32 has already accepted this number. No phone confirmation is needed.</p></div><p id=pairing-message class=note></p><button id=retry class=secondary onclick=retryReceiver() hidden>Retry pairing</button><button id=forget class=secondary onclick=forgetReceiver() hidden>Forget receiver</button></section><section id=wifi><form onsubmit=saveWifi(event)><label>Wi-Fi name<input id=ssid required maxlength=32 autocomplete=off></label><label>Wi-Fi password<input id=password type=password maxlength=63></label><label>Preferred IP address (optional)<input id=preferred-ip inputmode=decimal maxlength=15 placeholder="Leave blank for DHCP" autocomplete=off></label><button>Save Wi-Fi</button></form></section></main><script>
const q=s=>document.querySelector(s);let provisioning=false,targetRequest=null;const pairingMessages={waiting:'Waiting for the Denon pairing number…',confirm_on_denon:'Confirm the matching number on the Denon.',timed_out:'Pairing attempt ended. Put the Denon in pairing mode, then tap Retry pairing.'};async function api(url,opt={},timeoutMs=3000){let c=new AbortController,t=setTimeout(()=>c.abort(),timeoutMs);try{let r=await fetch(url,{...opt,signal:c.signal});if(!r.ok)throw Error(await r.text());return r.status===204?null:await r.json()}finally{clearTimeout(t)}}async function send(dir){try{await api('/api/volume/'+dir,{method:'POST'});targetRequest=null;q('#target-status').textContent='';setTimeout(refresh,300)}catch(e){alert(e.message)}}async function setTargetVolume(e){e.preventDefault();let input=q('#target-volume');if(!input.reportValidity())return;let value=Number(input.value);q('#target-status').textContent='Setting volume to '+value.toFixed(1)+'…';try{let result=await api('/api/volume',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'volume='+encodeURIComponent(input.value)});targetRequest={value:value,id:result.target_id};refresh()}catch(e){targetRequest=null;q('#target-status').textContent=e.message}}async function state(){if(provisioning)return false;try{let s=await api('/api/state');q('#connection').textContent=s.connected?'Receiver connected':s.connecting?'Connecting to receiver…':s.receiver_configured?'Receiver disconnected':'Select a receiver';q('#volume').textContent=s.volume===null?'—':s.volume;q('#db').textContent=s.volume_db===null?'':s.volume_db+' dB';let mute=q('#mute-status'),muteText=!s.mute_known?'Mute state unknown':s.muted?'Denon muted'+(s.manual_mute_lock?', automation locked':''):'Denon unmuted';if(mute.textContent!==muteText)mute.textContent=muteText;mute.classList.toggle('is-muted',s.mute_known&&s.muted);mute.classList.toggle('is-unmuted',s.mute_known&&!s.muted);document.querySelectorAll('.row button,#target input,#target button').forEach(b=>b.disabled=!s.connected||s.restoring);if(targetRequest!==null){if(s.volume_target_id!==targetRequest.id){q('#target-status').textContent='Volume change was interrupted';targetRequest=null}else if(s.restore_state==='error'){q('#target-status').textContent='Could not set volume: '+s.restore_error;targetRequest=null}else if(s.restoring){q('#target-status').textContent='Setting volume to '+targetRequest.value.toFixed(1)+'…'}else if(s.volume===targetRequest.value){q('#target-status').textContent='Volume set to '+targetRequest.value.toFixed(1);targetRequest=null}else{q('#target-status').textContent='Volume change was interrupted';targetRequest=null}}q('#scan').hidden=s.receiver_configured;q('#retry').hidden=!s.receiver_configured||s.connected||s.connecting;q('#retry').textContent=s.receiver_bonded?'Retry connection':'Retry pairing';q('#forget').hidden=!s.receiver_configured;q('#wifi').hidden=!s.setup_ap;let confirming=s.pairing_status==='confirm_on_denon'&&s.pairing_number;q('#pairing-confirm').hidden=!confirming;q('#pairing-number').textContent=confirming?s.pairing_number:'';q('#pairing-message').textContent=pairingMessages[s.pairing_status]||'';return true}catch(e){q('#connection').textContent='ESP32 unavailable';let mute=q('#mute-status');mute.textContent='Mute state unavailable';mute.classList.remove('is-muted','is-unmuted');return false}}async function appTable(){if(provisioning)return false;try{let j=await api('/api/apps');q('#apps').replaceChildren(...j.apps.map(x=>{let r=document.createElement('tr'),n=document.createElement('td'),v=document.createElement('td'),d=document.createElement('td'),name=x.app_name||x.app_id;if(name==='MemoryPoster'||name==='com.apple.IdleScreen.MemoryPoster')name='Screensaver';n.textContent=(x.active?'● ':'')+name;if(x.active)n.className='active';v.textContent=x.volume;d.textContent=x.volume_db;r.append(n,v,d);return r}));q('#app-status').textContent=j.restoring?'Setting volume…':j.apps.length?'Volumes update after manual changes.':'Waiting for Home Assistant…';return true}catch(e){q('#app-status').textContent='App memory unavailable';return false}}async function scan(){let d=q('#devices');d.textContent='Scanning Bluetooth for 8 seconds…';try{let j=await api('/api/discover',{},10000);d.replaceChildren(...j.devices.map(x=>{let b=document.createElement('button');b.textContent=(x.name||'Bluetooth device')+' '+x.mac;b.onclick=()=>receiver(x.mac);return b}));if(!j.devices.length)d.textContent='No devices found. Check Denon pairing mode and try again.'}catch(e){d.textContent=e.message}}async function receiver(mac){try{await api('/api/denon',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mac='+encodeURIComponent(mac)});q('#devices').textContent='Saved. Connecting…';refresh()}catch(e){alert(e.message)}}async function retryReceiver(){try{await api('/api/denon/reconnect',{method:'POST'});refresh()}catch(e){alert(e.message)}}async function forgetReceiver(){if(!confirm('Forget the saved receiver?'))return;await fetch('/api/denon',{method:'DELETE'});q('#connection').textContent='Restarting…'}async function saveWifi(e){e.preventDefault();try{let r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(q('#ssid').value)+'&password='+encodeURIComponent(q('#password').value)+'&preferred_ip='+encodeURIComponent(q('#preferred-ip').value)});if(!r.ok)throw Error(await r.text());provisioning=true;q('#wifi').hidden=true;q('#connection').textContent='Wi-Fi saved. Rejoin your home Wi-Fi. Home Assistant will discover this ESP32.'}catch(e){alert(e.message)}}let refreshing=false,retryAt=0;async function refresh(){if(provisioning||refreshing||document.hidden||Date.now()<retryAt)return;refreshing=true;try{let ok=await state();if(ok)ok=await appTable();retryAt=ok?0:Date.now()+4000}finally{refreshing=false}}document.addEventListener('visibilitychange',()=>{if(!document.hidden){retryAt=0;refresh()}});refresh();setInterval(refresh,1000);
</script>
)HTML";

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') escaped += '\\';
    if (c >= 0x20) escaped += static_cast<char>(c);
  }
  return escaped;
}

bool parseMac(const String &text, uint8_t output[6]) {
  int consumed = 0;
  return text.length() == 17 &&
         sscanf(text.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx%n", &output[0],
                &output[1], &output[2], &output[3], &output[4], &output[5],
                &consumed) == 6 &&
         consumed == 17;
}

bool timeReached(unsigned long now, unsigned long target) {
  return static_cast<long>(now - target) >= 0;
}

bool parseIpv4(String text, IPAddress &address) {
  text.trim();
  uint8_t octets[4] = {};
  size_t octet = 0;
  unsigned value = 0;
  size_t digits = 0;
  for (size_t i = 0; i <= text.length(); ++i) {
    const char c = i < text.length() ? text[i] : '.';
    if (c >= '0' && c <= '9') {
      value = value * 10 + static_cast<unsigned>(c - '0');
      if (++digits > 3 || value > 255) return false;
      continue;
    }
    if (c != '.' || digits == 0 || octet >= 4) return false;
    octets[octet++] = static_cast<uint8_t>(value);
    value = 0;
    digits = 0;
  }
  if (octet != 4) return false;
  address = IPAddress(octets[0], octets[1], octets[2], octets[3]);
  return true;
}

uint32_t ipv4Value(const IPAddress &address) {
  return (static_cast<uint32_t>(address[0]) << 24) |
         (static_cast<uint32_t>(address[1]) << 16) |
         (static_cast<uint32_t>(address[2]) << 8) |
         static_cast<uint32_t>(address[3]);
}

bool isUnicastIpv4(const IPAddress &address) {
  return address[0] > 0 && address[0] < 224 && address[0] != 127;
}

bool validStaticNetwork(const IPAddress &ip, const IPAddress &gateway,
                        const IPAddress &subnet, const IPAddress &dns) {
  const uint32_t ipValue = ipv4Value(ip);
  const uint32_t gatewayValue = ipv4Value(gateway);
  const uint32_t mask = ipv4Value(subnet);
  const uint32_t hostMask = ~mask;
  if (!isUnicastIpv4(ip) || !isUnicastIpv4(gateway) || !isUnicastIpv4(dns) ||
      mask == 0 || mask == UINT32_MAX || (hostMask & (hostMask + 1)) != 0 ||
      (ipValue & mask) != (gatewayValue & mask)) {
    return false;
  }
  const uint32_t ipHost = ipValue & hostMask;
  const uint32_t gatewayHost = gatewayValue & hostMask;
  return ipHost != 0 && ipHost != hostMask && gatewayHost != 0 &&
         gatewayHost != hostMask && ipValue != gatewayValue;
}

bool shouldStopSetupAp(bool staticConfigured, bool stationClaimed) {
  return !staticConfigured || stationClaimed;
}

bool networkSelfCheck() {
  IPAddress parsed;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns;
  return parseIpv4("203.0.113.1", gateway) &&
         parseIpv4("255.255.255.0", subnet) &&
         parseIpv4("203.0.113.53", dns) &&
         parseIpv4("203.0.113.7", parsed) && parsed[0] == 203 &&
         parsed[3] == 7 && validStaticNetwork(parsed, gateway, subnet, dns) &&
         !validStaticNetwork(gateway, gateway, subnet, dns) &&
         shouldStopSetupAp(false, false) &&
         !shouldStopSetupAp(true, false) && shouldStopSetupAp(true, true) &&
         isSetupApIngress(true, true, false) &&
         !isSetupApIngress(true, false, true) &&
         !isSetupApIngress(true, true, true) &&
         isStationIngress(true, true, false) &&
         !isStationIngress(true, false, true) &&
         !parseIpv4("203.0.113", parsed) &&
         !parseIpv4("203.0.113.256", parsed) &&
         !parseIpv4("203.0.113.7.extra", parsed);
}

bool isHexText(const String &value, size_t expectedLength) {
  if (value.length() != expectedLength) return false;
  for (char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

String randomHex(size_t bytes) {
  String value;
  value.reserve(bytes * 2);
  while (bytes) {
    const uint32_t random = esp_random();
    for (size_t i = 0; i < 4 && bytes; ++i, --bytes) {
      char pair[3];
      snprintf(pair, sizeof(pair), "%02x",
               static_cast<unsigned>((random >> (i * 8)) & 0xFF));
      value += pair;
    }
  }
  return value;
}

void loadDeviceIdentity() {
  deviceId = preferences.getString("device_id", "");
  if (!isHexText(deviceId, 16)) {
    deviceId = randomHex(8);
    if (preferences.putString("device_id", deviceId) == 0) {
      Serial.println("Could not persist device ID");
    }
  }
  hostName = "denon-volume-" + deviceId.substring(0, 8);
  setupApName = "Denon-Setup-" + deviceId.substring(0, 8);
  apiToken = preferences.getString("api_token", "");
  if (!apiToken.isEmpty() && !isHexText(apiToken, 64)) {
    Serial.println("Ignoring invalid API token in storage");
    apiToken = "";
  }
}

bool constantTimeEquals(const String &left, const String &right) {
  if (left.length() != right.length()) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < left.length(); ++i) {
    difference |= static_cast<uint8_t>(left[i] ^ right[i]);
  }
  return difference == 0;
}

bool hasAppAuthorization() {
  return !apiToken.isEmpty() &&
         constantTimeEquals(server.header("Authorization"),
                            "Bearer " + apiToken);
}

bool currentRequestUsesSetupAp() {
  const IPAddress localAddress = server.client().localIP();
  return isSetupApIngress(
      setupApRunning, localAddress == WiFi.softAPIP(),
      WiFi.status() == WL_CONNECTED && localAddress == WiFi.localIP());
}

bool isLocalControlHost(String host) {
  const int port = host.indexOf(':');
  if (port >= 0) host.remove(port);
  return host == hostName || host == hostName + ".local" ||
         (WiFi.status() == WL_CONNECTED &&
          host == WiFi.localIP().toString());
}

bool hasSameOriginAuthorization() {
  const String origin = server.header("Origin");
  const String host = server.header("Host");
  return isLocalControlHost(host) &&
         constantTimeEquals(origin, "http://" + host);
}

bool requireManualVolumeAuthorization() {
  if (currentRequestUsesSetupAp() || hasAppAuthorization() ||
      hasSameOriginAuthorization()) {
    return true;
  }
  server.sendHeader("WWW-Authenticate", "Bearer");
  server.send(401, "text/plain",
              "Same-origin request or bearer token required for volume control");
  return false;
}

bool apiClaimWindowOpen() {
  return currentRequestUsesSetupAp() ||
         (apiClaimUntil != 0 && !timeReached(millis(), apiClaimUntil));
}

bool requireProvisioningAuthorization() {
  if (currentRequestUsesSetupAp() ||
      (apiToken.isEmpty() && apiClaimWindowOpen()) || hasAppAuthorization()) {
    return true;
  }
  if (!apiToken.isEmpty()) {
    server.sendHeader("WWW-Authenticate", "Bearer");
    server.send(401, "text/plain", "Bearer token required for receiver setup");
  } else {
    server.send(403, "text/plain",
                "Receiver setup is closed; restart and retry within 10 minutes");
  }
  return false;
}

String macString() {
  char value[18];
  snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X", denonMac[0],
           denonMac[1], denonMac[2], denonMac[3], denonMac[4], denonMac[5]);
  return value;
}

enum class BondState { Error, Absent, Present };

BondState bondState(const uint8_t address[6]) {
  int count = esp_bt_gap_get_bond_device_num();
  if (count < 0) return BondState::Error;
  if (count == 0) return BondState::Absent;
  auto *devices = new esp_bd_addr_t[count];
  if (!devices) return BondState::Error;
  int returned = count;
  if (esp_bt_gap_get_bond_device_list(&returned, devices) != ESP_OK) {
    delete[] devices;
    return BondState::Error;
  }
  bool found = false;
  for (int i = 0; i < returned; ++i) {
    if (memcmp(address, devices[i], sizeof(denonMac)) == 0) {
      found = true;
      break;
    }
  }
  delete[] devices;
  return found ? BondState::Present : BondState::Absent;
}

bool refreshDenonBondState() {
  if (!hasDenonMac) {
    denonBonded = false;
    denonBondStateKnown = true;
    return true;
  }
  const BondState state = bondState(denonMac);
  if (state == BondState::Error) {
    Serial.println("Could not read Denon Bluetooth bond");
    return false;
  }
  denonBonded = state == BondState::Present;
  denonBondStateKnown = true;
  Serial.printf("Denon bond: %s\n", denonBonded ? "present" : "absent");
  return true;
}

void loadDenonMac() {
  String saved = preferences.getString("denon_mac", DENON_MAC);
  hasDenonMac = parseMac(saved, denonMac);
  if (hasDenonMac) Serial.printf("Denon MAC: %s\n", macString().c_str());
}

void startSetupAp() {
  if (setupApRunning) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(setupApName.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  setupApRunning = true;
  Serial.printf("Setup AP: %s at %s\n", setupApName.c_str(),
                WiFi.softAPIP().toString().c_str());
}

void stopSetupAp() {
  if (!setupApRunning) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  setupApRunning = false;
}

enum NetworkStorageKey : size_t {
  kWifiSsidKey,
  kWifiPasswordKey,
  kStaticIpKey,
  kStaticGatewayKey,
  kStaticSubnetKey,
  kStaticDnsKey,
  kPendingIpKey,
  kNetworkStorageKeyCount,
};

constexpr const char *kNetworkStorageKeys[kNetworkStorageKeyCount] = {
    "wifi_ssid",  "wifi_password", "static_ip",  "static_gw",
    "static_mask", "static_dns",    "pending_ip",
};

struct NetworkStorageState {
  bool present[kNetworkStorageKeyCount] = {};
  String value[kNetworkStorageKeyCount];
};

NetworkStorageState readNetworkStorage() {
  NetworkStorageState state;
  for (size_t i = 0; i < kNetworkStorageKeyCount; ++i) {
    state.present[i] = preferences.isKey(kNetworkStorageKeys[i]);
    if (state.present[i]) {
      state.value[i] = preferences.getString(kNetworkStorageKeys[i], "");
    }
  }
  return state;
}

bool writeNetworkStorage(const NetworkStorageState &state) {
  for (size_t i = 0; i < kNetworkStorageKeyCount; ++i) {
    if (!state.present[i]) continue;
    const size_t written =
        preferences.putString(kNetworkStorageKeys[i], state.value[i]);
    if ((!state.value[i].isEmpty() && written == 0) ||
        !preferences.isKey(kNetworkStorageKeys[i]) ||
        preferences.getString(kNetworkStorageKeys[i], "") != state.value[i]) {
      return false;
    }
  }
  for (size_t i = 0; i < kNetworkStorageKeyCount; ++i) {
    if (state.present[i] || !preferences.isKey(kNetworkStorageKeys[i])) continue;
    if (!preferences.remove(kNetworkStorageKeys[i]) ||
        preferences.isKey(kNetworkStorageKeys[i])) {
      return false;
    }
  }
  return true;
}

bool commitNetworkStorage(const NetworkStorageState &desired) {
  const NetworkStorageState previous = readNetworkStorage();
  if (writeNetworkStorage(desired)) return true;
  if (!writeNetworkStorage(previous)) {
    Serial.println("Network settings rollback failed");
  }
  return false;
}

bool clearStaticNetworkStorage() {
  NetworkStorageState desired = readNetworkStorage();
  for (size_t i = kStaticIpKey; i <= kPendingIpKey; ++i) {
    desired.present[i] = false;
    desired.value[i] = "";
  }
  if (!commitNetworkStorage(desired)) return false;
  staticNetworkEnabled = false;
  return true;
}

bool persistStaticNetwork(const IPAddress &ip, const IPAddress &gateway,
                          const IPAddress &subnet, const IPAddress &dns) {
  NetworkStorageState desired = readNetworkStorage();
  const String values[] = {ip.toString(), gateway.toString(), subnet.toString(),
                           dns.toString()};
  for (size_t i = 0; i < 4; ++i) {
    desired.present[kStaticIpKey + i] = true;
    desired.value[kStaticIpKey + i] = values[i];
  }
  desired.present[kPendingIpKey] = false;
  desired.value[kPendingIpKey] = "";
  if (!commitNetworkStorage(desired)) return false;
  staticIp = ip;
  staticGateway = gateway;
  staticSubnet = subnet;
  staticDns = dns;
  staticNetworkEnabled = true;
  return true;
}

void loadStaticNetwork() {
  const String ipText = preferences.getString("static_ip", "");
  const String gatewayText = preferences.getString("static_gw", "");
  const String subnetText = preferences.getString("static_mask", "");
  const String dnsText = preferences.getString("static_dns", "");
  if (ipText.isEmpty() && gatewayText.isEmpty() && subnetText.isEmpty() &&
      dnsText.isEmpty()) {
    return;
  }
  if (!parseIpv4(ipText, staticIp) ||
      !parseIpv4(gatewayText, staticGateway) ||
      !parseIpv4(subnetText, staticSubnet) ||
      !parseIpv4(dnsText, staticDns) ||
      !validStaticNetwork(staticIp, staticGateway, staticSubnet, staticDns)) {
    staticNetworkEnabled = false;
    Serial.println("Stored static network configuration is invalid; using DHCP");
    return;
  }
  staticNetworkEnabled = true;
}

bool finishPendingStaticNetwork() {
  const String pendingText = preferences.getString("pending_ip", "");
  if (pendingText.isEmpty()) return false;
  IPAddress preferred;
  const IPAddress gateway = WiFi.gatewayIP();
  const IPAddress subnet = WiFi.subnetMask();
  const IPAddress dns = WiFi.dnsIP(0);
  if (!parseIpv4(pendingText, preferred) ||
      !validStaticNetwork(preferred, gateway, subnet, dns) ||
      !persistStaticNetwork(preferred, gateway, subnet, dns)) {
    if (!clearStaticNetworkStorage()) {
      Serial.println("Could not clear failed preferred network settings");
    }
    Serial.println("Preferred IP could not use the DHCP network defaults; staying on DHCP");
    return false;
  }
  Serial.println("Preferred IP saved with DHCP network defaults; restarting");
  restartDevice(true);
  return true;
}

void startWifi() {
  String ssid = preferences.getString("wifi_ssid", WIFI_SSID);
  String password = preferences.getString("wifi_password", WIFI_PASSWORD);
  wifiConfigured = !ssid.isEmpty();
  if (!wifiConfigured) {
    startSetupAp();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName.c_str());
  stationHttpClaimed = false;
  loadStaticNetwork();
  if (staticNetworkEnabled &&
      !WiFi.config(staticIp, staticGateway, staticSubnet, staticDns, staticDns)) {
    staticNetworkEnabled = false;
    Serial.println("Could not apply static network configuration; using DHCP");
  }
  if (staticNetworkEnabled) startSetupAp();
  WiFi.begin(ssid.c_str(), password.c_str());
  wifiStartedAt = millis();
  wifiLastRetryAt = static_cast<uint32_t>(wifiStartedAt);
  Serial.printf("Connecting to Wi-Fi %s\n", ssid.c_str());
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      wifiLastRetryAt = static_cast<uint32_t>(millis());
      if (!staticNetworkEnabled && finishPendingStaticNetwork()) return;
      if (apiToken.isEmpty()) apiClaimUntil = millis() + kApiClaimWindowMs;
    }
    if (shouldStopSetupAp(staticNetworkEnabled, stationHttpClaimed)) {
      stopSetupAp();
    } else {
      startSetupAp();
    }
    if (!mdnsRunning) {
      mdnsRunning = MDNS.begin(hostName.c_str());
      if (mdnsRunning) {
        MDNS.addService("http", "tcp", kHttpPort);
        MDNS.addService(kMdnsService, "tcp", kHttpPort);
        MDNS.addServiceTxt(kMdnsService, "tcp", "api", kProduct);
        MDNS.addServiceTxt(kMdnsService, "tcp", "version", kApiVersion);
        MDNS.addServiceTxt(kMdnsService, "tcp", "id", deviceId.c_str());
        Serial.printf("Web UI: http://%s.local/ or http://%s/\n", hostName.c_str(),
                      WiFi.localIP().toString().c_str());
      }
    }
    return;
  }
  if (wifiWasConnected) {
    wifiWasConnected = false;
    if (staticNetworkEnabled) stationHttpClaimed = false;
    wifiStartedAt = millis();
    wifiLastRetryAt = static_cast<uint32_t>(wifiStartedAt);
    if (mdnsRunning) {
      MDNS.end();
      mdnsRunning = false;
    }
  }
  if (!setupApRunning && millis() - wifiStartedAt >= kWifiTimeoutMs) startSetupAp();
  const uint32_t now = static_cast<uint32_t>(millis());
  if (wifiConfigured &&
      static_cast<uint32_t>(now - wifiLastRetryAt) >= kWifiReconnectMs) {
    wifiLastRetryAt = now;
    wifi_ap_record_t associatedAp;
    if (esp_wifi_sta_get_ap_info(&associatedAp) == ESP_OK) return;
    ++wifiRetryCount;
    const esp_err_t result = esp_wifi_connect();
    Serial.printf("Wi-Fi retry %lu: connect result 0x%x\n",
                  static_cast<unsigned long>(wifiRetryCount),
                  static_cast<unsigned>(result));
  }
}

void dropSppLink(const char *reason);

// One SDK write in flight; the next starts on its completion or when
// congestion clears. Commands are static constants, so pointers stay valid.
void pumpDenonTx() {
  const uint32_t now = millis();
  if (!sppOpen || sppWriteInFlight || sppCongested || sppTxCount == 0) return;
  const BtTxCommand command = sppTx[sppTxHead];
  if (esp_spp_write(sppHandle, command.length, const_cast<uint8_t *>(command.data)) !=
      ESP_OK) {
    dropSppLink("write could not start");
    return;
  }
  sppTxHead = (sppTxHead + 1) % kBtTxQueueLength;
  --sppTxCount;
  sppWriteInFlight = true;
  sppTxProgressAt = now;
}

// Bounded and non-blocking: queues up to kBtTxQueueLength commands.
bool sendDenon(const uint8_t *command, size_t length) {
  if (!sppOpen || sppTxCount == kBtTxQueueLength) return false;
  if (sppTxCount == 0 && !sppWriteInFlight) sppTxProgressAt = millis();
  sppTx[(sppTxHead + sppTxCount) % kBtTxQueueLength] = {
      command, static_cast<uint8_t>(length)};
  ++sppTxCount;
  pumpDenonTx();
  return sppOpen;
}

// A volume step goes out in this pass or not at all: only with nothing
// queued, nothing in flight, and no congestion. Callers retry from fresh
// feedback on a later pass.
bool denonStepReady() {
  return sppOpen && !sppWriteInFlight && !sppCongested && sppTxCount == 0;
}

bool sendDenonStep(const uint8_t *command, size_t length) {
  if (!denonStepReady()) return false;
  if (esp_spp_write(sppHandle, static_cast<int>(length),
                    const_cast<uint8_t *>(command)) != ESP_OK) {
    dropSppLink("write could not start");
    return false;
  }
  sppWriteInFlight = true;
  sppTxProgressAt = millis();
  return true;
}

void appStorageKey(uint8_t bank, size_t index, char key[7]) {
  snprintf(key, 7, bank == 0 ? "app%02u" : "bak%02u",
           static_cast<unsigned>(index));
}

const char *appSequenceKey(uint8_t bank) {
  return bank == 0 ? "app_seq" : "bak_seq";
}

void copyText(char *destination, size_t capacity, const String &value) {
  if (capacity == 0) return;
  const size_t length = min(value.length(), capacity - 1);
  memcpy(destination, value.c_str(), length);
  destination[length] = '\0';
}

void loadAppVolumes() {
  activeAppBank = preferences.getUChar("app_bank", 0);
  if (activeAppBank > 1) {
    Serial.println("Stored app bank is invalid; using the original table");
    activeAppBank = 0;
  }
  appSequence = preferences.getUInt(appSequenceKey(activeAppBank), 0);
  for (size_t i = 0; i < kMaxApps; ++i) {
    char key[7];
    appStorageKey(activeAppBank, i, key);
    StoredAppVolume stored;
    if (preferences.getBytesLength(key) != sizeof(stored) ||
        preferences.getBytes(key, &stored, sizeof(stored)) != sizeof(stored) ||
        stored.magic != kStoredAppMagic || stored.raw > kMaxVolumeRaw) {
      continue;
    }
    stored.appId[kMaxAppIdLength] = '\0';
    stored.appName[kMaxAppNameLength] = '\0';
    if (stored.appId[0] == '\0') continue;
    appVolumes[i] = stored;
    if (stored.sequence > appSequence) appSequence = stored.sequence;
  }
}

bool persistApp(size_t index) {
  if (index >= kMaxApps || appVolumes[index].appId[0] == '\0') return false;
  char key[7];
  appStorageKey(activeAppBank, index, key);
  appVolumes[index].magic = kStoredAppMagic;
  if (preferences.putBytes(key, &appVolumes[index], sizeof(appVolumes[index])) !=
      sizeof(appVolumes[index])) {
    Serial.printf("Could not persist app volume slot %u\n",
                  static_cast<unsigned>(index));
    return false;
  }
  preferences.putUInt(appSequenceKey(activeAppBank), appSequence);
  appDirty[index] = false;
  return true;
}

void markAppDirty(size_t index, bool immediate) {
  appDirty[index] = true;
  appPersistAt[index] = millis() + kAppPersistDelayMs;
  if (immediate) persistApp(index);
}

int findApp(const String &appId) {
  for (size_t i = 0; i < kMaxApps; ++i) {
    if (appVolumes[i].appId[0] != '\0' && appId == appVolumes[i].appId) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int allocateApp(const String &appId, const String &appName, uint8_t raw) {
  size_t selected = kMaxApps;
  uint32_t oldest = UINT32_MAX;
  for (size_t i = 0; i < kMaxApps; ++i) {
    if (appVolumes[i].appId[0] == '\0') {
      selected = i;
      break;
    }
    if (appVolumes[i].sequence < oldest) {
      oldest = appVolumes[i].sequence;
      selected = i;
    }
  }
  if (selected == kMaxApps) return -1;
  appVolumes[selected] = {};
  appVolumes[selected].magic = kStoredAppMagic;
  appVolumes[selected].raw = raw;
  appVolumes[selected].sequence = ++appSequence;
  copyText(appVolumes[selected].appId, sizeof(appVolumes[selected].appId), appId);
  copyText(appVolumes[selected].appName, sizeof(appVolumes[selected].appName), appName);
  markAppDirty(selected, false);
  return static_cast<int>(selected);
}

int rememberAppVolume(const String &appId, const String &appName, uint8_t raw,
                      bool immediate) {
  int index = findApp(appId);
  if (index < 0) index = allocateApp(appId, appName, raw);
  if (index < 0) return -1;
  StoredAppVolume &stored = appVolumes[index];
  const bool changed = stored.raw != raw || appName != stored.appName;
  if (!changed) {
    if (immediate && appDirty[index]) persistApp(static_cast<size_t>(index));
    return index;
  }
  stored.raw = raw;
  stored.sequence = ++appSequence;
  copyText(stored.appName, sizeof(stored.appName), appName);
  markAppDirty(static_cast<size_t>(index), immediate);
  return index;
}

void maintainAppStorage() {
  const unsigned long now = millis();
  for (size_t i = 0; i < kMaxApps; ++i) {
    if (appDirty[i] && timeReached(now, appPersistAt[i])) persistApp(i);
  }
}

bool isIgnoredAppId(String appId) {
  appId.trim();
  if (appId.isEmpty() || appId.length() > kMaxAppIdLength) return true;
  for (char c : appId) {
    if (static_cast<unsigned char>(c) < 0x20) return true;
  }
  return appId.equalsIgnoreCase("unknown") ||
         appId.equalsIgnoreCase("unavailable") ||
         appId.equalsIgnoreCase("none") || appId.equalsIgnoreCase("null");
}

bool appStateSelfCheck() {
  return isIgnoredAppId("") && isIgnoredAppId("unknown") &&
         isIgnoredAppId("unavailable") && isIgnoredAppId("none") &&
         isIgnoredAppId("null") &&
         !isIgnoredAppId("com.example.video") && volumeDirection(10, 12) == 1 &&
         volumeDirection(12, 10) == -1 && volumeDirection(10, 10) == 0 &&
         !canResumeLearningAfterFailure(false, 100, 101) &&
         !canResumeLearningAfterFailure(true, 100, 100) &&
         !canResumeLearningAfterFailure(true, -1, 101) &&
         canResumeLearningAfterFailure(true, 100, 101);
}

void resetVolumeRestore(bool clearError) {
  restoreTargetRaw = -1;
  restoreSessionArmed = false;
  restoreAutomatic = false;
  restoreManualMuteOverride = false;
  restoreSteps = 0;
  restorePhase = VolumeTargetPhase::idle;
  restoreObservedRaw = -1;
  restoreCommandRaw = -1;
  restoreCommandDirection = 0;
  restoreCorrectiveReversalUsed = false;
  restoreCoarseEnabled = true;
  restoreBurstActive = false;
  restoreBurstExtensionPending = false;
  restoreBurstStartRaw = -1;
  restoreBurstClicksPlanned = 0;
  restoreBurstClicksSent = 0;
  restoreBurstMeasuredDeltaRaw = 0;
  restoreBurstMeasuredClicks = 0;
  restoreAutomaticMuteCycle = false;
  restoreAutomaticUnmuteObserved = false;
  if (automaticRemuteRequired) {
    automaticRemuteBaselineRaw = volumeRaw;
    automaticRemuteNotBefore = millis() + kVolumeStepResponseTimeoutMs;
  }
  if (clearError) restoreError = "";
}

bool currentPlaybackIdleFresh(unsigned long now) {
  return playbackIdleAuthorizationFresh(currentPlaybackIdleAuthorized,
                                        currentPlaybackAt, now,
                                        kPlaybackIdleFreshMs);
}

bool automaticRestoreCommandAllowed() {
  return volumeCommandAllowed(muteStateKnown, denonMuted, manualMuteLocked) ||
         (restoreAutomaticMuteCycle && automaticRemuteRequired &&
          muteStateKnown && !denonMuted);
}

bool clearAutomaticRemuteJournal() {
  if (automaticRemuteJournalPersisted && !preferences.remove("remute")) {
    return false;
  }
  automaticRemuteRequired = false;
  automaticMuteConfirmationPending = false;
  automaticRemuteJournalPersisted = false;
  automaticRemuteNotBefore = 0;
  automaticRemuteBaselineRaw = -1;
  return true;
}

bool armAutomaticRemuteRecovery(unsigned long now) {
  automaticRemuteRequired = true;
  automaticMuteConfirmationPending = false;
  automaticRemuteBaselineRaw = volumeRaw;
  automaticRemuteNotBefore = now + kVolumeStepResponseTimeoutMs;
  if (automaticRemuteJournalPersisted) return true;
  automaticRemuteJournalPersisted = preferences.putBool("remute", true) > 0;
  return automaticRemuteJournalPersisted;
}

void advanceVolumeTargetGeneration() {
  if (++volumeTargetGeneration == 0) ++volumeTargetGeneration;
}

void armVolumeTargetSession(unsigned long now) {
  restoreSessionArmed = true;
  restoreSteps = 0;
  restorePhase = VolumeTargetPhase::needFreshStatus;
  restoreObservedRaw = -1;
  restoreCommandRaw = -1;
  restoreCommandDirection = 0;
  restoreCorrectiveReversalUsed = false;
  restoreCoarseEnabled = true;
  restoreBurstActive = false;
  restoreBurstExtensionPending = false;
  restoreBurstStartRaw = -1;
  restoreBurstClicksPlanned = 0;
  restoreBurstClicksSent = 0;
  restoreBurstMeasuredDeltaRaw = 0;
  restoreBurstMeasuredClicks = 0;
  restoreCommandAt = now;
  restoreLastChangedAt = now;
  restorePhaseAt = now;
  restoreDeadlineAt = now + kRestoreDeadlineMs;
}

void pauseVolumeTargetSession() {
  if (restoreTargetRaw < 0) return;
  restoreSessionArmed = false;
  restoreSteps = 0;
  restorePhase = VolumeTargetPhase::needFreshStatus;
  restoreObservedRaw = -1;
  restoreCommandRaw = -1;
  restoreCommandDirection = 0;
  restoreCorrectiveReversalUsed = false;
  restoreCoarseEnabled = true;
  restoreBurstActive = false;
  restoreBurstExtensionPending = false;
  restoreBurstStartRaw = -1;
  restoreBurstClicksPlanned = 0;
  restoreBurstClicksSent = 0;
  restoreBurstMeasuredDeltaRaw = 0;
  restoreBurstMeasuredClicks = 0;
  restoreDeadlineAt = 0;
}

void cancelVolumeRestore() {
  if (volumeTargetCancellationAdvancesGeneration(restoreTargetRaw)) {
    advanceVolumeTargetGeneration();
  }
  resetVolumeRestore(true);
  restoreLearningSuppressed = automaticRemuteRequired;
  restoreFailureRaw = -1;
  restoreLearningResumeAt = 0;
}

void completeVolumeRestore() {
  resetVolumeRestore(true);
  restoreLearningSuppressed = automaticRemuteRequired;
  restoreFailureRaw = -1;
  restoreLearningResumeAt = 0;
}

bool setVolume(int targetRaw, bool automatic) {
  if (targetRaw < 0 || targetRaw > kMaxVolumeRaw) return false;
  if (automaticRemuteRequired) return false;
  if (!automatic &&
      !volumeCommandAllowed(muteStateKnown, denonMuted, manualMuteLocked)) {
    return false;
  }
  const bool mutedIdleRestore =
      automatic && muteStateKnown && denonMuted && manualMuteLocked &&
      currentPlaybackIdleFresh(millis());
  if (automatic &&
      (manualMuteLocked || (muteStateKnown && denonMuted)) &&
      !mutedIdleRestore) {
    restoreLearningSuppressed = true;
    restoreFailureRaw = volumeRaw;
    restoreLearningResumeAt = millis();
    return false;
  }
  advanceVolumeTargetGeneration();
  restoreTargetRaw = targetRaw;
  restoreAutomatic = automatic;
  restoreManualMuteOverride = !automatic && manualMuteLocked;
  restoreLearningSuppressed = true;
  restoreError = "";
  armVolumeTargetSession(millis());
  if (mutedIdleRestore) {
    restoreAutomaticMuteCycle = true;
    restorePhase = VolumeTargetPhase::waitAutomaticUnmute;
  }
  return true;
}

void failVolumeRestore(const char *reason) {
  restoreError = reason;
  restoreFailureRaw = -1;
  restoreLearningResumeAt = millis() + kRestoreFailureCooldownMs;
  resetVolumeRestore(false);
  restoreLearningSuppressed = true;
  Serial.printf("Volume restore stopped: %s\n", reason);
}

void startRestoreForCurrentApp() {
  if (restoreTargetRaw >= 0 && !restoreAutomatic) return;
  const int index = findApp(currentAppId);
  if (index < 0) {
    cancelVolumeRestore();
    return;
  }
  setVolume(appVolumes[index].raw, true);
}

void clearCurrentApp() {
  pendingAppId = "";
  pendingAppName = "";
  pendingPlaybackIdleAuthorized = false;
  pendingRestoreAllowed = true;
  currentAppId = "";
  currentAppName = "";
  currentPlaybackIdleAuthorized = false;
  if (appClearCancelsVolumeTarget(restoreTargetRaw, restoreAutomatic)) {
    cancelVolumeRestore();
  }
}

void queueAppCandidate(String appId, String appName, bool playbackKnown,
                       bool playbackActive, String eventId) {
  appId.trim();
  appName.trim();
  if (isIgnoredAppId(appId)) return;
  const unsigned long now = millis();
  const bool validIdleEvent =
      playbackKnown && !playbackActive &&
      validPlaybackEventId(eventId.c_str(), eventId.length());
  const bool idleAuthorized = playbackIdleEventGrants(
      playbackKnown, playbackActive, eventId.c_str(), eventId.length(),
      lastPlaybackIdleEventId);
  const bool duplicateIdleEvent = validIdleEvent && !idleAuthorized;
  if (idleAuthorized) {
    copyText(lastPlaybackIdleEventId, sizeof(lastPlaybackIdleEventId), eventId);
  }
  if (appName.isEmpty()) appName = appId;
  if (appName.length() > kMaxAppNameLength) {
    appName = appName.substring(0, kMaxAppNameLength);
  }

  if (appId == currentAppId) {
    const bool hadPendingSwitch = !pendingAppId.isEmpty();
    pendingAppId = "";
    pendingAppName = "";
    pendingRestoreAllowed = true;
    currentAppName = appName;
    if (!duplicateIdleEvent) {
      currentPlaybackIdleAuthorized = idleAuthorized;
      currentPlaybackAt = now;
      if (!idleAuthorized && restoreAutomaticMuteCycle) cancelVolumeRestore();
    }
    const int index = findApp(currentAppId);
    if (index >= 0 && appName != appVolumes[index].appName) {
      appVolumes[index].sequence = ++appSequence;
      copyText(appVolumes[index].appName, sizeof(appVolumes[index].appName),
               appName);
      markAppDirty(static_cast<size_t>(index), false);
    }
    if (!duplicateIdleEvent && (hadPendingSwitch || idleAuthorized)) {
      startRestoreForCurrentApp();
    }
    return;
  }

  if (appId == pendingAppId) {
    pendingAppName = appName;
    if (!duplicateIdleEvent) {
      pendingPlaybackIdleAuthorized = idleAuthorized;
      pendingPlaybackAt = now;
    }
    return;
  }

  if (!currentAppId.isEmpty() && appVolumeLearningAllowed(
                                         restoreTargetRaw,
                                         restoreLearningSuppressed, volumeRaw,
                                         muteStateKnown, denonMuted,
                                         manualMuteLocked)) {
    rememberAppVolume(currentAppId, currentAppName,
                      static_cast<uint8_t>(volumeRaw), true);
  }
  if (restoreTargetRaw < 0 || restoreAutomatic) cancelVolumeRestore();
  pendingAppId = appId;
  pendingAppName = appName;
  pendingPlaybackIdleAuthorized = idleAuthorized;
  pendingPlaybackAt = now;
  pendingRestoreAllowed = !duplicateIdleEvent;
  pendingAppAt = now;
}

void activatePendingApp() {
  if (pendingAppId.isEmpty()) return;
  if (!currentAppId.isEmpty() && appVolumeLearningAllowed(
                                         restoreTargetRaw,
                                         restoreLearningSuppressed, volumeRaw,
                                         muteStateKnown, denonMuted,
                                         manualMuteLocked)) {
    rememberAppVolume(currentAppId, currentAppName,
                      static_cast<uint8_t>(volumeRaw), true);
  }
  currentAppId = pendingAppId;
  currentAppName = pendingAppName;
  currentPlaybackIdleAuthorized = pendingPlaybackIdleAuthorized;
  currentPlaybackAt = pendingPlaybackAt;
  pendingAppId = "";
  pendingAppName = "";
  pendingPlaybackIdleAuthorized = false;
  const bool restoreAllowed = pendingRestoreAllowed;
  pendingRestoreAllowed = true;

  const int existing = findApp(currentAppId);
  if (existing >= 0) {
    if (currentAppName != appVolumes[existing].appName) {
      appVolumes[existing].sequence = ++appSequence;
      copyText(appVolumes[existing].appName,
               sizeof(appVolumes[existing].appName), currentAppName);
      markAppDirty(static_cast<size_t>(existing), false);
    }
    if (restoreAllowed) startRestoreForCurrentApp();
  } else if (appVolumeLearningAllowed(
                 restoreTargetRaw, restoreLearningSuppressed, volumeRaw,
                 muteStateKnown, denonMuted, manualMuteLocked)) {
    rememberAppVolume(currentAppId, currentAppName,
                      static_cast<uint8_t>(volumeRaw), false);
    cancelVolumeRestore();
  }
}

void maintainAppSwitch() {
  if (restoreTargetRaw >= 0 && !restoreAutomatic) return;
  if (!pendingAppId.isEmpty() &&
      millis() - pendingAppAt >= kAppStableMs) {
    activatePendingApp();
  }
}

void clearManualMuteLock() {
  manualMuteLocked = false;
  manualMuteLockRaw = -1;
  muteAutomaticFeedbackPending = false;
  muteAutomaticFeedbackUntil = 0;
  muteManualFeedbackDirection = 0;
}

void observeMute(bool muted) {
  const bool activeTargetCommand =
      volumeTargetMayHavePendingCommand(restoreTargetRaw, restoreSteps);
  const bool remuteJournalReady =
      !muted || !activeTargetCommand || automaticRemuteRequired ||
      armAutomaticRemuteRecovery(millis());
  const bool becameMuted = !muteStateKnown || !denonMuted;
  muteStateKnown = true;
  denonMuted = muted;
  if (!muted) {
    if (automaticRemuteRequired) {
      if (restoreAutomaticMuteCycle) restoreAutomaticUnmuteObserved = true;
      return;
    }
    clearManualMuteLock();
    return;
  }
  if (automaticRemuteConfirmed(automaticRemuteRequired,
                               automaticMuteConfirmationPending,
                               muteStateKnown, denonMuted) &&
      clearAutomaticRemuteJournal()) {
    restoreLearningSuppressed = false;
  }
  if (restoreAutomaticMuteCycle && automaticRemuteRequired &&
      !restoreAutomaticUnmuteObserved) {
    return;
  }
  if (!becameMuted) return;

  const bool cancelTarget =
      restoreTargetRaw >= 0 &&
      (restoreAutomatic || !restoreManualMuteOverride);
  muteAutomaticFeedbackPending =
      restoreAutomatic && restoreTargetRaw >= 0 && restoreSteps > 0;
  muteAutomaticFeedbackUntil = muteAutomaticFeedbackPending
                                   ? millis() + kVolumeStepResponseTimeoutMs
                                   : 0;
  manualMuteLocked = true;
  manualMuteLockRaw = volumeRaw;
  if (cancelTarget) {
    cancelVolumeRestore();
    restoreLearningSuppressed = true;
    restoreFailureRaw = volumeRaw;
    restoreLearningResumeAt = millis();
    if (!remuteJournalReady) restoreError = "remute_journal_failed";
  }
}

void maintainManualMuteLock() {
  if (!muteAutomaticFeedbackPending ||
      !timeReached(millis(), muteAutomaticFeedbackUntil)) {
    return;
  }
  muteAutomaticFeedbackPending = false;
  muteAutomaticFeedbackUntil = 0;
  manualMuteLockRaw = volumeRaw;
}

void acceptManualVolumeFeedback(int direction) {
  if (!manualMuteLocked) return;
  muteAutomaticFeedbackPending = false;
  muteAutomaticFeedbackUntil = 0;
  manualMuteLockRaw = volumeRaw;
  muteManualFeedbackDirection = direction;
}

bool validateActiveBurstFeedback(uint8_t raw) {
  if (!restoreBurstActive || restoreObservedRaw < 0 ||
      raw == restoreObservedRaw) {
    return true;
  }
  if (volumeRapidFeedbackValid(restoreBurstStartRaw, restoreObservedRaw, raw,
                               restoreCommandDirection,
                               restoreBurstClicksSent)) {
    return true;
  }
  const char *reason =
      volumeMovementInDirection(restoreObservedRaw, raw,
                                restoreCommandDirection)
          ? "rapid_gain_exceeded"
          : "wrong_direction";
  failVolumeRestore(reason);
  return false;
}

void observeVolume(uint8_t raw) {
  if (automaticRemuteFeedbackChanged(
          automaticRemuteRequired, restoreAutomaticMuteCycle,
          automaticRemuteBaselineRaw, raw)) {
    automaticRemuteBaselineRaw = raw;
    automaticMuteConfirmationPending = false;
    automaticRemuteNotBefore = millis() + kVolumeStepResponseTimeoutMs;
  }
  if (!restoreAutomaticMuteCycle && !automaticRemuteRequired &&
      manualMuteLocked && muteManualFeedbackDirection != 0 &&
      manualMuteLockRaw != raw) {
    if (manualVolumeFeedbackMatches(manualMuteLockRaw, raw,
                                    muteManualFeedbackDirection)) {
      clearManualMuteLock();
    } else {
      manualMuteLockRaw = raw;
    }
  } else if (!restoreAutomaticMuteCycle && !automaticRemuteRequired &&
             manualMuteLockClearsOnVolume(
          manualMuteLocked, manualMuteLockRaw, raw,
          muteAutomaticFeedbackPending)) {
    clearManualMuteLock();
  } else if (!restoreAutomaticMuteCycle && !automaticRemuteRequired &&
             manualMuteLocked && manualMuteLockRaw != raw) {
    manualMuteLockRaw = raw;
  }
  if (restoreTargetRaw >= 0) {
    const unsigned long now = millis();
    const int previousRaw = restoreObservedRaw;
    if (!validateActiveBurstFeedback(raw)) return;
    if (restorePhase == VolumeTargetPhase::waitFreshStatus) {
      restoreObservedRaw = raw;
      restoreLastChangedAt = now;
      if (raw == restoreTargetRaw) {
        completeVolumeRestore();
      } else {
        restorePhase = VolumeTargetPhase::ready;
      }
      return;
    }
    if (restorePhase == VolumeTargetPhase::waitMovement ||
        restorePhase == VolumeTargetPhase::waitMovementStatus) {
      const int feedbackBaseline =
          restoreBurstActive ? restoreObservedRaw : restoreCommandRaw;
      if (raw == feedbackBaseline) return;
      if (!restoreBurstActive &&
          !volumeMovementInDirection(feedbackBaseline, raw,
                                     restoreCommandDirection)) {
        failVolumeRestore("wrong_direction");
        return;
      }
      if (restoreBurstActive) {
        restoreBurstMeasuredDeltaRaw = abs(raw - restoreBurstStartRaw);
        restoreBurstMeasuredClicks = restoreBurstClicksSent;
      }
      restoreObservedRaw = raw;
      restoreLastChangedAt = now;
      if (restoreBurstActive) {
        restoreBurstActive = false;
        restoreBurstExtensionPending = volumeRapidExtensionAllowed(
            raw, restoreTargetRaw, restoreCommandDirection,
            automaticRestoreCommandAllowed(),
            timeReached(now, restoreDeadlineAt), restoreSteps,
            kRestoreMaxSteps);
        restorePhase = restoreBurstExtensionPending
                           ? VolumeTargetPhase::waitBurstSecond
                           : VolumeTargetPhase::settling;
      } else {
        restorePhase = VolumeTargetPhase::settling;
      }
      return;
    }
    if (restorePhase == VolumeTargetPhase::waitBurstSecond) {
      if (raw == restoreObservedRaw) return;
      const bool continuedInDirection = volumeMovementInDirection(
          restoreObservedRaw, raw, restoreCommandDirection);
      restoreObservedRaw = raw;
      restoreLastChangedAt = now;
      if (restoreBurstExtensionPending) {
        if (!continuedInDirection ||
            !volumeRapidExtensionAllowed(
                raw, restoreTargetRaw, restoreCommandDirection,
                automaticRestoreCommandAllowed(),
                timeReached(now, restoreDeadlineAt), restoreSteps,
                kRestoreMaxSteps)) {
          restoreBurstExtensionPending = false;
          restorePhase = VolumeTargetPhase::settling;
        }
        return;
      }
      const int remainingDirection = volumeDirection(raw, restoreTargetRaw);
      if (remainingDirection == 0 ||
          remainingDirection != restoreCommandDirection) {
        restoreCoarseEnabled = false;
        restoreBurstClicksPlanned = restoreBurstClicksSent;
        restorePhase = VolumeTargetPhase::settling;
      }
      return;
    }
    if (restorePhase == VolumeTargetPhase::settling) {
      if (previousRaw != raw) {
        restoreObservedRaw = raw;
        restoreLastChangedAt = now;
      }
      return;
    }
    if (restorePhase == VolumeTargetPhase::waitConfirmation) {
      if (previousRaw != raw) {
        restoreObservedRaw = raw;
        restoreLastChangedAt = now;
        restorePhase = VolumeTargetPhase::settling;
        return;
      }
      if (restoreBurstActive) {
        const int movedRaw = abs(raw - restoreBurstStartRaw);
        if (!volumeRapidGainWithinBound(movedRaw,
                                        restoreBurstClicksSent)) {
          failVolumeRestore("rapid_gain_exceeded");
          return;
        }
        restoreBurstMeasuredDeltaRaw = movedRaw;
        restoreBurstMeasuredClicks = restoreBurstClicksSent;
        const int remainingDirection = volumeDirection(raw, restoreTargetRaw);
        if (remainingDirection != 0 &&
            remainingDirection != restoreCommandDirection) {
          restoreCoarseEnabled = false;
        }
        restoreBurstActive = false;
      }
      if (raw == restoreTargetRaw) {
        completeVolumeRestore();
      } else {
        restorePhase = VolumeTargetPhase::ready;
      }
      return;
    }
    if (restorePhase == VolumeTargetPhase::ready && previousRaw != raw) {
      restoreObservedRaw = raw;
      restoreLastChangedAt = now;
      restorePhase = VolumeTargetPhase::settling;
    }
    return;
  }
  if (restoreLearningSuppressed) {
    if (muteAutomaticFeedbackPending) {
      restoreFailureRaw = raw;
      return;
    }
    const unsigned long now = millis();
    const bool quietElapsed = timeReached(now, restoreLearningResumeAt);
    if (learningFailureBaselineNeedsRefresh(quietElapsed, restoreFailureRaw,
                                            raw)) {
      restoreFailureRaw = raw;
      const unsigned long quietUntil = now + kRestoreFailureCooldownMs;
      if (timeReached(quietUntil, restoreLearningResumeAt)) {
        restoreLearningResumeAt = quietUntil;
      }
      return;
    }
    if (!canResumeLearningAfterFailure(quietElapsed, restoreFailureRaw,
                                       raw)) {
      return;
    }
    restoreLearningSuppressed = false;
    restoreFailureRaw = -1;
    restoreLearningResumeAt = 0;
    restoreError = "";
  }
  if (currentAppId.isEmpty() || !pendingAppId.isEmpty() ||
      !appVolumeLearningAllowed(
          restoreTargetRaw, restoreLearningSuppressed, raw, muteStateKnown,
          denonMuted, manualMuteLocked)) {
    return;
  }
  rememberAppVolume(currentAppId, currentAppName, raw, false);
}

void maintainAutomaticRemute(unsigned long now) {
  if (!automaticRemuteRequired || restoreAutomaticMuteCycle ||
      !sppOpen || !sessionInitialized || !muteStateKnown) {
    return;
  }
  if (!timeReached(now, automaticRemuteNotBefore)) return;
  if (now - automaticMuteCommandAt < kVolumeMovementStatusDelayMs) return;
  if (sendDenon(kMuteOn, sizeof(kMuteOn))) {
    automaticMuteConfirmationPending = true;
    muteStateKnown = false;
    automaticMuteCommandAt = now;
    nextStatusAt = now + kVolumeMovementStatusDelayMs;
  }
}

void maintainVolumeRestore() {
  const unsigned long now = millis();
  maintainAutomaticRemute(now);
  if (automaticRemuteRequired && !restoreAutomaticMuteCycle) return;
  if (restoreTargetRaw < 0) return;
  if (!restoreSessionArmed || !sppOpen || !sessionInitialized) {
    return;
  }
  if (timeReached(now, restoreDeadlineAt)) {
    failVolumeRestore("deadline_exceeded");
    return;
  }

  if (restorePhase == VolumeTargetPhase::needFreshStatus) {
    if (!sendDenon(kGetStatus, sizeof(kGetStatus))) {
      failVolumeRestore("status_request_failed");
      return;
    }
    restorePhase = VolumeTargetPhase::waitFreshStatus;
    restorePhaseAt = now;
    nextStatusAt = now + kStatusIntervalMs;
    return;
  }
  if (restorePhase == VolumeTargetPhase::waitFreshStatus) {
    if (now - restorePhaseAt >= kVolumeStatusResponseTimeoutMs) {
      failVolumeRestore("fresh_status_timeout");
    }
    return;
  }
  if (restorePhase == VolumeTargetPhase::waitAutomaticUnmute) {
    if (!currentPlaybackIdleFresh(now)) {
      failVolumeRestore("playback_authorization_expired");
      return;
    }
    if (!muteStateKnown || !denonMuted || !manualMuteLocked || volumeRaw < 0) {
      failVolumeRestore("mute_state_changed");
      return;
    }
    const int direction = volumeDirection(volumeRaw, restoreTargetRaw);
    if (direction == 0) {
      completeVolumeRestore();
      return;
    }
    if (!armAutomaticRemuteRecovery(now)) {
      failVolumeRestore("remute_journal_failed");
      return;
    }
    if (!denonStepReady()) return;  // retry next pass
    const uint8_t *command = direction > 0 ? kVolumeUp : kVolumeDown;
    const size_t length = direction > 0 ? sizeof(kVolumeUp)
                                         : sizeof(kVolumeDown);
    const int rapidClicks = volumeRapidClickCount(
        abs(restoreTargetRaw - volumeRaw), 0, 0);
    if (!sendDenonStep(command, length)) {
      failVolumeRestore("command_write_failed");
      return;
    }
    muteStateKnown = false;
    restoreObservedRaw = volumeRaw;
    restoreCommandRaw = volumeRaw;
    restoreCommandDirection = direction;
    ++restoreSteps;
    restoreCommandAt = now;
    restorePhaseAt = now;
    if (rapidClicks > 0) {
      restoreBurstActive = true;
      restoreBurstStartRaw = volumeRaw;
      restoreBurstClicksPlanned = static_cast<uint8_t>(rapidClicks);
      restoreBurstClicksSent = 1;
      restorePhase = VolumeTargetPhase::waitBurstSecond;
    } else {
      restorePhase = VolumeTargetPhase::waitMovement;
    }
    return;
  }
  if (restoreAutomaticMuteCycle && !currentPlaybackIdleFresh(now)) {
    if (playbackAuthorizationExpiryFails(volumeRaw, restoreTargetRaw)) {
      failVolumeRestore("playback_authorization_expired");
    } else {
      completeVolumeRestore();
    }
    return;
  }
  if (restoreBurstExtensionPending &&
      !automaticRestoreCommandAllowed()) {
    restoreBurstExtensionPending = false;
    restorePhase = VolumeTargetPhase::settling;
    return;
  }
  if (!automaticRestoreCommandAllowed()) {
    return;
  }
  if (restorePhase == VolumeTargetPhase::waitBurstSecond) {
    const bool sendExtension = restoreBurstExtensionPending;
    if (sendExtension &&
        !volumeRapidExtensionAllowed(
            restoreObservedRaw, restoreTargetRaw, restoreCommandDirection,
            true, timeReached(now, restoreDeadlineAt), restoreSteps,
            kRestoreMaxSteps)) {
      restoreBurstExtensionPending = false;
      restorePhase = VolumeTargetPhase::settling;
      return;
    }
    if (now - restoreCommandAt < kVolumeRapidIntervalMs) return;
    if (!sendExtension &&
        restoreBurstClicksSent >= restoreBurstClicksPlanned) {
      restorePhase = VolumeTargetPhase::waitMovement;
      return;
    }
    if (restoreSteps >= kRestoreMaxSteps) {
      failVolumeRestore("step_budget_exceeded");
      return;
    }
    const uint8_t *command =
        restoreCommandDirection > 0 ? kVolumeUp : kVolumeDown;
    const size_t length = restoreCommandDirection > 0 ? sizeof(kVolumeUp)
                                                       : sizeof(kVolumeDown);
    if (!denonStepReady()) return;  // retry next pass
    if (sendExtension) {
      restoreBurstActive = true;
      restoreBurstStartRaw = restoreObservedRaw;
      restoreBurstClicksPlanned = 1;
      restoreBurstClicksSent = 0;
      restoreBurstExtensionPending = false;
    }
    if (!sendDenonStep(command, length)) {
      failVolumeRestore("command_write_failed");
      return;
    }
    ++restoreSteps;
    ++restoreBurstClicksSent;
    restoreCommandAt = now;
    restorePhaseAt = now;
    if (restoreBurstClicksSent >= restoreBurstClicksPlanned) {
      restorePhase = VolumeTargetPhase::waitMovement;
    }
    return;
  }
  if (restorePhase == VolumeTargetPhase::waitMovement ||
      restorePhase == VolumeTargetPhase::waitMovementStatus) {
    if (now - restoreCommandAt >= kVolumeStepResponseTimeoutMs) {
      failVolumeRestore("movement_timeout");
      return;
    }
    if (restorePhase == VolumeTargetPhase::waitMovement &&
        now - restoreCommandAt >= kVolumeMovementStatusDelayMs) {
      if (!sendDenon(kGetStatus, sizeof(kGetStatus))) {
        failVolumeRestore("status_request_failed");
        return;
      }
      restorePhase = VolumeTargetPhase::waitMovementStatus;
      restorePhaseAt = now;
      nextStatusAt = now + kStatusIntervalMs;
    }
    return;
  }
  if (restorePhase == VolumeTargetPhase::settling) {
    if (!volumeSettleWindowElapsed(now, restoreCommandAt,
                                   restoreLastChangedAt)) {
      return;
    }
    const int remainingDirection =
        volumeDirection(restoreObservedRaw, restoreTargetRaw);
    if (!restoreBurstActive &&
        remainingDirection == restoreCommandDirection) {
      restorePhase = VolumeTargetPhase::ready;
      return;
    }
    if (!sendDenon(kGetStatus, sizeof(kGetStatus))) {
      failVolumeRestore("status_request_failed");
      return;
    }
    restorePhase = VolumeTargetPhase::waitConfirmation;
    restorePhaseAt = now;
    nextStatusAt = now + kStatusIntervalMs;
    return;
  }
  if (restorePhase == VolumeTargetPhase::waitConfirmation) {
    if (now - restorePhaseAt >= kVolumeStatusResponseTimeoutMs) {
      failVolumeRestore("confirmation_timeout");
    }
    return;
  }
  if (restorePhase != VolumeTargetPhase::ready || restoreObservedRaw < 0) return;

  const int direction = volumeDirection(restoreObservedRaw, restoreTargetRaw);
  if (direction == 0) {
    completeVolumeRestore();
    return;
  }
  const bool movementObserved =
      restoreCommandDirection != 0 &&
      volumeMovementInDirection(restoreCommandRaw, restoreObservedRaw,
                                restoreCommandDirection);
  if (restoreSteps > 0 &&
      (!movementObserved ||
       !volumeSettleWindowElapsed(now, restoreCommandAt,
                                  restoreLastChangedAt))) return;
  if (restoreCommandDirection != 0 && direction != restoreCommandDirection) {
    if (restoreCorrectiveReversalUsed) {
      failVolumeRestore("corrective_reversal_exceeded");
      return;
    }
    restoreCorrectiveReversalUsed = true;
  }
  if (restoreSteps >= kRestoreMaxSteps) {
    failVolumeRestore("step_budget_exceeded");
    return;
  }
  if (!denonStepReady()) return;  // retry next pass
  const uint8_t *command = direction > 0 ? kVolumeUp : kVolumeDown;
  const size_t length = direction > 0 ? sizeof(kVolumeUp) : sizeof(kVolumeDown);
  const int distanceRaw = abs(restoreTargetRaw - restoreObservedRaw);
  const int rapidClicks =
      restoreCoarseEnabled
          ? volumeRapidClickCount(distanceRaw, restoreBurstMeasuredDeltaRaw,
                                  restoreBurstMeasuredClicks)
          : 0;
  if (sendDenonStep(command, length)) {
    restoreCommandRaw = restoreObservedRaw;
    restoreCommandDirection = direction;
    ++restoreSteps;
    restoreCommandAt = now;
    restorePhaseAt = now;
    if (rapidClicks > 0) {
      restoreBurstActive = true;
      restoreBurstStartRaw = restoreObservedRaw;
      restoreBurstClicksPlanned = static_cast<uint8_t>(rapidClicks);
      restoreBurstClicksSent = 1;
      restorePhase = VolumeTargetPhase::waitBurstSecond;
    } else {
      restorePhase = VolumeTargetPhase::waitMovement;
    }
    nextStatusAt = now + kStatusIntervalMs;
  } else {
    failVolumeRestore("command_write_failed");
  }
}

bool parseVolumePacket(const uint8_t *packet, size_t length, uint8_t &raw) {
  // ponytail: Byte 8 semantics are unknown and intentionally unvalidated;
  // add validation only when cross-device captures prove it is compatible.
  if (length != 9 || packet[0] != 0x41 || packet[1] != 0x54 ||
      (packet[2] != 0x07 && packet[2] != 0x57) || packet[3] != 0x02 ||
      packet[4] != 0x03 || packet[5] != 0xC5 || packet[7] != 0 ||
      packet[6] > kMaxVolumeRaw) {
    return false;
  }
  raw = packet[6];
  return true;
}

bool parseMutePacket(const uint8_t *packet, size_t length, bool &muted) {
  if (length != 7 || packet[0] != 0x41 || packet[1] != 0x54 ||
      (packet[2] != 0x07 && packet[2] != 0x57) || packet[3] != 0x1D ||
      packet[4] != 0x01 ||
      packet[5] > 1 || packet[6] != static_cast<uint8_t>(0xFF - packet[5])) {
    return false;
  }
  muted = packet[5] == 1;
  return true;
}

bool protocolSelfCheck() {
  constexpr uint8_t captured50[] = {0x41, 0x54, 0x07, 0x02, 0x03,
                                    0xC5, 0x64, 0x00, 0xD4};
  constexpr uint8_t captured50_5[] = {0x41, 0x54, 0x07, 0x02, 0x03,
                                      0xC5, 0x65, 0x00, 0xD3};
  constexpr uint8_t capturedTvAudio50[] = {0x41, 0x54, 0x57, 0x02, 0x03,
                                           0xC5, 0x64, 0x00, 0xD4};
  constexpr uint8_t wrongHeader[] = {0x41, 0x54, 0x06, 0x02, 0x03,
                                     0xC5, 0x64, 0x00, 0xD4};
  constexpr uint8_t outOfRange[] = {0x41, 0x54, 0x07, 0x02, 0x03,
                                    0xC5, 0xC5, 0x00, 0x00};
  constexpr uint8_t capturedMuteOn[] = {0x41, 0x54, 0x07, 0x1D,
                                        0x01, 0x01, 0xFE};
  constexpr uint8_t capturedMuteOff[] = {0x41, 0x54, 0x07, 0x1D,
                                         0x01, 0x00, 0xFF};
  constexpr uint8_t capturedX580MuteOn[] = {0x41, 0x54, 0x57, 0x1D,
                                            0x01, 0x01, 0xFE};
  constexpr uint8_t capturedX580MuteOff[] = {0x41, 0x54, 0x57, 0x1D,
                                             0x01, 0x00, 0xFF};
  constexpr uint8_t wrongMuteChecksum[] = {0x41, 0x54, 0x07, 0x1D,
                                           0x01, 0x01, 0xFF};
  uint8_t raw = 0;
  bool muted = false;
  return parseVolumePacket(captured50, sizeof(captured50), raw) && raw == 0x64 &&
         parseVolumePacket(captured50_5, sizeof(captured50_5), raw) &&
         raw == 0x65 &&
         parseVolumePacket(capturedTvAudio50, sizeof(capturedTvAudio50), raw) &&
         raw == 0x64 &&
         !parseVolumePacket(wrongHeader, sizeof(wrongHeader), raw) &&
         !parseVolumePacket(outOfRange, sizeof(outOfRange), raw) &&
         !parseVolumePacket(captured50, sizeof(captured50) - 1, raw) &&
         parseMutePacket(capturedMuteOn, sizeof(capturedMuteOn), muted) &&
         muted &&
         parseMutePacket(capturedMuteOff, sizeof(capturedMuteOff), muted) &&
         !muted &&
         parseMutePacket(capturedX580MuteOn, sizeof(capturedX580MuteOn),
                         muted) &&
         muted &&
         parseMutePacket(capturedX580MuteOff, sizeof(capturedX580MuteOff),
                         muted) &&
         !muted &&
         !parseMutePacket(wrongMuteChecksum, sizeof(wrongMuteChecksum), muted);
}

void clearPairingNumber() {
  pairingNumberReady = false;
  pairingNumber = 0;
}

bool isDenonAddress(const uint8_t address[6]) {
  return hasDenonMac && memcmp(address, denonMac, sizeof(denonMac)) == 0;
}

// ---- Bluedroid callbacks: they run in Bluetooth tasks, so they only copy
// data into btEvents/btRx or set atomics. No logging or state changes here.

void postBtEvent(BtEvent &event) {
  event.generation = btGeneration.load(std::memory_order_relaxed);
  event.at = millis();
  if (!btEvents || xQueueSend(btEvents, &event, 0) != pdTRUE) {
    btEventDrops.fetch_add(1, std::memory_order_relaxed);
  }
}

void copyBtName(char output[33], const uint8_t *text, size_t length) {
  if (length > 32) length = 32;
  memcpy(output, text, length);
  output[length] = '\0';
}

void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  BtEvent e{};
  switch (event) {
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
      e.type = BtEventType::aclConnect;
      memcpy(e.address, param->acl_conn_cmpl_stat.bda, sizeof(e.address));
      e.status = param->acl_conn_cmpl_stat.stat;
      e.handle = param->acl_conn_cmpl_stat.handle;
      break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
      e.type = BtEventType::aclDisconnect;
      memcpy(e.address, param->acl_disconn_cmpl_stat.bda, sizeof(e.address));
      e.status = param->acl_disconn_cmpl_stat.reason;
      e.handle = param->acl_disconn_cmpl_stat.handle;
      break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      e.type = BtEventType::authComplete;
      memcpy(e.address, param->auth_cmpl.bda, sizeof(e.address));
      e.status = param->auth_cmpl.stat;
      break;
    case ESP_BT_GAP_CFM_REQ_EVT:
      e.type = BtEventType::confirmRequest;
      memcpy(e.address, param->cfm_req.bda, sizeof(e.address));
      e.value = param->cfm_req.num_val;
      break;
    case ESP_BT_GAP_PIN_REQ_EVT:
      e.type = BtEventType::pinRequest;
      memcpy(e.address, param->pin_req.bda, sizeof(e.address));
      e.value = param->pin_req.min_16_digit;
      break;
    case ESP_BT_GAP_DISC_RES_EVT:
      // Keep queue room for connection events; scan results are best effort.
      if (!btEvents || uxQueueSpacesAvailable(btEvents) < 8) return;
      e.type = BtEventType::discoveryResult;
      memcpy(e.address, param->disc_res.bda, sizeof(e.address));
      for (int i = 0; i < param->disc_res.num_prop; ++i) {
        const esp_bt_gap_dev_prop_t &prop = param->disc_res.prop[i];
        const uint8_t *value = static_cast<const uint8_t *>(prop.val);
        if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME && prop.len > 0) {
          copyBtName(e.name, value,
                     strnlen(reinterpret_cast<const char *>(value), prop.len));
        } else if (prop.type == ESP_BT_GAP_DEV_PROP_EIR && !e.name[0]) {
          uint8_t length = 0;
          uint8_t *eir = static_cast<uint8_t *>(prop.val);
          const uint8_t *text = esp_bt_gap_resolve_eir_data(
              eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &length);
          if (!text) {
            text = esp_bt_gap_resolve_eir_data(
                eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &length);
          }
          if (text) copyBtName(e.name, text, length);
        }
      }
      break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      e.type = BtEventType::discoveryState;
      e.status = param->disc_st_chg.state;
      break;
    default:
      return;
  }
  postBtEvent(e);
}

// Only the BTC task touches this table, so it needs no lock. Handles can be
// reused within one stack generation; the sequence tells sessions apart.
struct CallbackLink {
  uint32_t handle;
  uint32_t connection;
};
CallbackLink callbackLinks[4] = {};
uint32_t callbackConnectionSeq = 0;

uint32_t callbackConnection(uint32_t handle) {
  for (const CallbackLink &link : callbackLinks) {
    if (link.handle == handle && link.connection) return link.connection;
  }
  return 0;
}

uint32_t openCallbackConnection(uint32_t handle) {
  CallbackLink *slot = &callbackLinks[0];
  for (CallbackLink &link : callbackLinks) {
    if (link.handle == handle || !link.connection) {
      slot = &link;
      break;
    }
  }
  if (++callbackConnectionSeq == 0) ++callbackConnectionSeq;
  *slot = {handle, callbackConnectionSeq};
  return callbackConnectionSeq;
}

void sppCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  BtEvent e{};
  switch (event) {
    case ESP_SPP_DATA_IND_EVT: {
      // Tag every chunk with its stack generation and connection handle so
      // the loop parses only bytes of the accepted session.
      uint8_t message[sizeof(BtRxHeader) + kBtRxChunkBytes];
      const BtRxHeader header{btGeneration.load(std::memory_order_relaxed),
                              param->data_ind.handle,
                              callbackConnection(param->data_ind.handle)};
      memcpy(message, &header, sizeof(header));
      size_t remaining = param->data_ind.len;
      const uint8_t *data = param->data_ind.data;
      while (remaining) {
        const size_t chunk = remaining < kBtRxChunkBytes ? remaining : kBtRxChunkBytes;
        memcpy(message + sizeof(header), data, chunk);
        if (!btRx || xMessageBufferSend(btRx, message, sizeof(header) + chunk, 0) == 0) {
          btRxDrops.fetch_add(remaining, std::memory_order_relaxed);
          break;
        }
        data += chunk;
        remaining -= chunk;
      }
      return;
    }
    case ESP_SPP_UNINIT_EVT:
      sppUninitDone.store(true, std::memory_order_release);
      return;
    case ESP_SPP_INIT_EVT:
      e.type = BtEventType::sppInit;
      e.status = param->init.status;
      break;
    case ESP_SPP_START_EVT:
      e.type = BtEventType::sppServerStart;
      e.status = param->start.status;
      break;
    case ESP_SPP_CL_INIT_EVT:
      e.type = BtEventType::sppClientInit;
      e.status = param->cl_init.status;
      e.handle = param->cl_init.handle;
      break;
    case ESP_SPP_OPEN_EVT:
      e.type = BtEventType::sppOpen;
      e.status = param->open.status;
      e.handle = param->open.handle;
      if (e.status == ESP_SPP_SUCCESS) e.connection = openCallbackConnection(e.handle);
      memcpy(e.address, param->open.rem_bda, sizeof(e.address));
      break;
    case ESP_SPP_SRV_OPEN_EVT:
      e.type = BtEventType::sppServerOpen;
      e.status = param->srv_open.status;
      e.handle = param->srv_open.handle;
      if (e.status == ESP_SPP_SUCCESS) e.connection = openCallbackConnection(e.handle);
      memcpy(e.address, param->srv_open.rem_bda, sizeof(e.address));
      break;
    case ESP_SPP_CLOSE_EVT:
      e.type = BtEventType::sppClose;
      e.status = param->close.status;
      e.handle = param->close.handle;
      e.value = param->close.async;
      e.connection = callbackConnection(e.handle);
      for (CallbackLink &link : callbackLinks) {
        if (link.handle == e.handle) link = {};
      }
      break;
    case ESP_SPP_CONG_EVT:
      e.type = BtEventType::sppCongestion;
      e.handle = param->cong.handle;
      e.connection = callbackConnection(e.handle);
      e.value = param->cong.cong;
      break;
    case ESP_SPP_WRITE_EVT:
      e.type = BtEventType::sppWrite;
      e.status = param->write.status;
      e.handle = param->write.handle;
      e.connection = callbackConnection(e.handle);
      e.value = param->write.cong;
      break;
    default:
      return;
  }
  postBtEvent(e);
}

void a2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  BtEvent e{};
  if (event == ESP_A2D_PROF_STATE_EVT) {
    if (param->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS) {
      a2dpDeinitDone.store(true, std::memory_order_release);
    }
    e.type = BtEventType::a2dpProfile;
    e.status = param->a2d_prof_stat.init_state;
  } else if (event == ESP_A2D_CONNECTION_STATE_EVT) {
    e.type = BtEventType::a2dpConnection;
    e.status = param->conn_stat.state;
    memcpy(e.address, param->conn_stat.remote_bda, sizeof(e.address));
  } else {
    return;
  }
  postBtEvent(e);
}

int32_t provideSilentAudio(uint8_t *data, int32_t length) {
  if (length <= 0) return 0;
  memset(data, 0, static_cast<size_t>(length));
  return length;
}

// ---- Bluetooth lifecycle task: stack start/stop only, never on the loop ----

bool waitFlag(std::atomic<bool> &flag, unsigned long timeoutMs) {
  const unsigned long started = millis();
  while (!flag.load(std::memory_order_acquire)) {
    if (millis() - started >= timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

// Every transition is checked; a failure stops here so nothing is reset
// while a host callback could still run.
const char *btTeardown() {
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
    a2dpDeinitDone.store(false, std::memory_order_relaxed);
    if (esp_a2d_source_deinit() == ESP_OK &&
        !waitFlag(a2dpDeinitDone, kBtProfileWaitMs)) {
      return "a2dp_deinit_timeout";
    }
    sppUninitDone.store(false, std::memory_order_relaxed);
    if (esp_spp_deinit() == ESP_OK && !waitFlag(sppUninitDone, kBtProfileWaitMs)) {
      return "spp_deinit_timeout";
    }
    if (esp_bluedroid_disable() != ESP_OK) return "bluedroid_disable_failed";
  }
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED &&
      esp_bluedroid_deinit() != ESP_OK) {
    return "bluedroid_deinit_failed";
  }
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    return "bluedroid_still_running";
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED &&
      esp_bt_controller_disable() != ESP_OK) {
    return "controller_disable_failed";
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED &&
      esp_bt_controller_deinit() != ESP_OK) {
    return "controller_deinit_failed";
  }
  return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE
             ? nullptr
             : "controller_still_running";
}

const char *btStartup() {
  esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE &&
      esp_bt_controller_init(&config) != ESP_OK) {
    return "controller_init_failed";
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED &&
      esp_bt_controller_enable(static_cast<esp_bt_mode_t>(config.mode)) != ESP_OK) {
    return "controller_enable_failed";
  }
  if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
    return "controller_not_enabled";
  }
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED &&
      esp_bluedroid_init() != ESP_OK) {
    return "bluedroid_init_failed";
  }
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED &&
      esp_bluedroid_enable() != ESP_OK) {
    return "bluedroid_enable_failed";
  }
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    return "bluedroid_not_enabled";
  }
  if (esp_bt_gap_register_callback(gapCallback) != ESP_OK ||
      esp_spp_register_callback(sppCallback) != ESP_OK ||
      esp_spp_init(ESP_SPP_MODE_CB) != ESP_OK) {
    return "spp_start_failed";
  }
  esp_bt_dev_set_device_name(kBtDeviceName);
  esp_bt_pin_code_t pin;
  memset(pin, '0', sizeof(pin));
  esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);
  esp_bt_io_cap_t ioCapability = ESP_BT_IO_CAP_IO;
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &ioCapability,
                                sizeof(ioCapability));
  // Same class of device as the previous Arduino Bluetooth serial build.
  esp_bt_cod_t cod = {};
  cod.major = 0b00001;
  cod.minor = 0b000100;
  cod.service = 0b00000010110;
  esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);
  // Connectable but hidden, so the bonded receiver can open the link to us.
  if (esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE) != ESP_OK) {
    return "scan_mode_failed";
  }
  if (esp_a2d_register_callback(a2dpCallback) != ESP_OK ||
      esp_a2d_source_register_data_callback(provideSilentAudio) != ESP_OK ||
      esp_a2d_source_init() != ESP_OK) {
    return "a2dp_start_failed";
  }
  return nullptr;
}

void btLifecycleTask(void *restart) {
  const char *error = restart ? btTeardown() : nullptr;
  if (error) btTeardownFailed.store(true, std::memory_order_relaxed);
  if (!error) {
    // The old host is fully stopped: anything it queued is stale now.
    btGeneration.fetch_add(1, std::memory_order_relaxed);
    error = btStartup();
  }
  btLifecycleError.store(error, std::memory_order_relaxed);
  btLifecycleBusy.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

// ---- Loop-side Bluetooth state machine ----

void resetBtLinkState() {
  sppReady = false;
  sppServerRunning = false;
  sppOpen = false;
  sppHandle = 0;
  sppCongested = false;
  sppWriteInFlight = false;
  sppTxCount = 0;
  denonRxLength = 0;
  attemptHandle = 0;
  connectInProgress = false;
  a2dpReady = false;
  a2dpConnecting = false;
  a2dpConnected = false;
  a2dpDisconnecting = false;
  discoveryRunning = false;
  clearPairingNumber();
}

void requestBtLifecycle(bool restart, const char *reason) {
  Serial.printf("Bluetooth %s (%s)\n", restart ? "stack restart" : "start", reason);
  bluetoothReady = false;  // the loop stops calling Bluetooth APIs
  if (sppOpen) btLadderDisconnected(btLadder, millis());
  resetBtLinkState();
  btTeardownFailed.store(false, std::memory_order_relaxed);
  btLifecycleBusy.store(true, std::memory_order_release);
  btLifecycleStartedAt = millis();
  // Stays armed while Bluetooth runs; only a clean stop or an intentional
  // restart clears it, so any Bluetooth-time crash defers the next start.
  rtcBtLifecycleMarker = kBtLifecycleMarker;
  if (xTaskCreatePinnedToCore(btLifecycleTask, "bt_lifecycle", 6144,
                              restart ? reinterpret_cast<void *>(1) : nullptr, 1,
                              nullptr, ARDUINO_RUNNING_CORE) != pdPASS) {
    btLifecycleError.store("lifecycle_task_failed", std::memory_order_relaxed);
    btLifecycleBusy.store(false, std::memory_order_release);
  }
  btLifecycleResultPending = true;
}

void failBtStart(const char *error) {
  bluetoothReady = false;
  btLastError = error;
  ++btStartFailures;
  nextStackRetryAt = millis() + btStartRetryDelayMs(btStartFailures);
  Serial.printf("Bluetooth unavailable (%s); retry in %lus\n", error,
                static_cast<unsigned long>(btStartRetryDelayMs(btStartFailures) / 1000));
}

bool refreshDenonBondState();

void maintainBtLifecycle() {
  const unsigned long now = millis();
  if (btLifecycleBusy.load(std::memory_order_acquire)) {
    if (now - btLifecycleStartedAt >= kBtLifecycleTimeoutMs) {
      // The stack hung. The RTC marker stays set, so the next boot keeps
      // Bluetooth off for a while and HTTP/OTA stays reachable.
      Serial.println("Bluetooth lifecycle hung; rebooting into recovery");
      delay(50);
      esp_restart();
    }
    return;
  }
  if (btLifecycleResultPending) {
    btLifecycleResultPending = false;
    const char *error = btLifecycleError.load(std::memory_order_relaxed);
    if (btTeardownFailed.load(std::memory_order_relaxed)) {
      // A half-stopped stack is not safe to reuse. Reboot with the marker
      // still armed, so the next boot serves HTTP before Bluetooth.
      Serial.printf("Bluetooth teardown failed (%s); rebooting into recovery\n", error);
      delay(50);
      esp_restart();
    }
    if (error) {
      failBtStart(error);
      return;
    }
    bluetoothReady = true;
    btProfilesDeadline = now + kBtProfileStartMs;
    nextReconnectAt = now + 1000;
    if (!refreshDenonBondState()) failBtStart("bond_read_failed");
    return;
  }
  if (!bluetoothReady && nextStackRetryAt != 0 && timeReached(now, nextStackRetryAt)) {
    nextStackRetryAt = 0;
    requestBtLifecycle(true, btLastError ? btLastError : "start");
  }
}

void confirmDenonPairing(uint8_t address[6], uint32_t number) {
  if (forgetInProgress || (!a2dpConnecting && !connectInProgress) ||
      !isDenonAddress(address)) {
    esp_bt_gap_ssp_confirm_reply(address, false);
    return;
  }
  if (esp_bt_gap_ssp_confirm_reply(address, true) != ESP_OK) return;
  pairingNumber = number;
  pairingNumberReady = true;
}

bool aclStatusSucceeded(int status) {
  return status == ESP_BT_STATUS_SUCCESS || status == ESP_BT_STATUS_HCI_SUCCESS;
}

void finishConnectAttempt(bool connected, BtFailure failure) {
  const unsigned long now = millis();
  connectInProgress = false;
  attemptHandle = 0;
  clearPairingNumber();
  if (connected) {
    btLadderConnected(btLadder, now);
    Serial.println("Denon connected");
    return;
  }
  btLadderFailure(btLadder, now, failure);
  nextReconnectAt = now + btLadderRetryDelayMs(btLadder);
  Serial.printf("Denon connection failed (%s; %lu consecutive, %lu page timeouts)\n",
                failure == BtFailure::pageTimeout
                    ? "page timeout"
                    : (failure == BtFailure::noResponse ? "no response" : "error"),
                static_cast<unsigned long>(btLadder.consecutiveFailures),
                static_cast<unsigned long>(btLadder.consecutivePageTimeouts));
}

// Every session change resets the TX queue and the RX frame parser.
void openSppSession(uint32_t handle, uint32_t connection) {
  sppOpen = true;
  sppHandle = handle;
  sppConnection = connection;
  sppCongested = false;
  sppWriteInFlight = false;
  sppTxCount = 0;
  denonRxLength = 0;
}

void closeSppLocally() {
  sppOpen = false;
  sppHandle = 0;
  sppConnection = 0;
  sppCongested = false;
  sppWriteInFlight = false;
  sppTxCount = 0;
  denonRxLength = 0;
  btLadderDisconnected(btLadder, millis());
}

// Used when the link can no longer be trusted (lost events, failed or stalled
// writes): ask the stack to close it and treat it as closed now.
void dropSppLink(const char *reason) {
  if (!sppOpen) return;
  Serial.printf("Dropping Denon link: %s\n", reason);
  esp_spp_disconnect(sppHandle);
  closeSppLocally();
}

void acceptSppLink(BtEvent &e, bool incoming) {
  if (e.status != ESP_SPP_SUCCESS) {
    if (!incoming && connectInProgress) {
      finishConnectAttempt(false, attemptPageTimeout ? BtFailure::pageTimeout
                                                     : BtFailure::other);
    }
    return;
  }
  bool accept = !sppOpen && !forgetInProgress && isDenonAddress(e.address);
  if (accept && !denonBonded) refreshDenonBondState();
  // Incoming links are only accepted from the bonded receiver.
  accept = accept && (denonBonded || (!incoming && a2dpConnected));
  if (!accept) {
    esp_spp_disconnect(e.handle);
    if (!incoming && connectInProgress && !sppOpen) {
      finishConnectAttempt(false, BtFailure::other);
    }
    return;
  }
  openSppSession(e.handle, e.connection);
  if (connectInProgress) {
    finishConnectAttempt(true, BtFailure::other);
  } else {
    btLadderConnected(btLadder, millis());
  }
  if (incoming) Serial.println("Denon opened the control link");
}

bool currentSession(const BtEvent &e) {
  return sppOpen && e.handle == sppHandle && e.connection == sppConnection;
}

void handleSppClose(const BtEvent &e) {
  if (currentSession(e)) {
    closeSppLocally();
    return;
  }
  if (connectInProgress && attemptHandle != 0 && e.handle == attemptHandle) {
    finishConnectAttempt(false, attemptPageTimeout ? BtFailure::pageTimeout
                                                   : BtFailure::other);
  }
}

void setA2dpTransition(bool connecting, bool disconnecting) {
  a2dpConnecting = connecting;
  a2dpDisconnecting = disconnecting;
  if (connecting || disconnecting) a2dpTransitionAt = millis();
}

void handleA2dpConnection(BtEvent &e) {
  const auto state = static_cast<esp_a2d_connection_state_t>(e.status);
  const bool opening = state == ESP_A2D_CONNECTION_STATE_CONNECTING ||
                       state == ESP_A2D_CONNECTION_STATE_CONNECTED;
  if (!isDenonAddress(e.address)) {
    if (opening) esp_a2d_source_disconnect(e.address);
    return;
  }
  if (forgetInProgress && opening) {
    a2dpConnected = false;
    setA2dpTransition(false, true);
    clearPairingNumber();
    esp_a2d_source_disconnect(e.address);
    return;
  }

  switch (state) {
    case ESP_A2D_CONNECTION_STATE_CONNECTING:
      setA2dpTransition(true, false);
      break;
    case ESP_A2D_CONNECTION_STATE_CONNECTED:
      setA2dpTransition(false, false);
      a2dpConnected = true;
      pairingTimedOut = false;
      clearPairingNumber();
      nextReconnectAt = 0;
      Serial.println("Denon A2DP connected");
      break;
    case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
      setA2dpTransition(false, true);
      break;
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
      pairingTimedOut = !forgetInProgress && a2dpConnecting;
      setA2dpTransition(false, false);
      a2dpConnected = false;
      clearPairingNumber();
      nextA2dpReconnectAt = millis() + kReconnectMs;
      Serial.println("Denon A2DP disconnected");
      break;
  }
}

void addDiscoveredDevice(const BtEvent &e) {
  if (!discoveryActive) return;
  for (size_t i = 0; i < discoveredCount; ++i) {
    if (memcmp(discovered[i].address, e.address, sizeof(e.address)) == 0) {
      if (!discovered[i].name[0]) memcpy(discovered[i].name, e.name, sizeof(e.name));
      return;
    }
  }
  if (discoveredCount == kMaxDiscovered) return;
  memcpy(discovered[discoveredCount].address, e.address, sizeof(e.address));
  memcpy(discovered[discoveredCount].name, e.name, sizeof(e.name));
  ++discoveredCount;
}

void startSppServer() {
  sppServerRetryAt = millis() + kSppServerRetryMs;
  if (esp_spp_start_srv(ESP_SPP_SEC_ENCRYPT | ESP_SPP_SEC_AUTHENTICATE,
                        ESP_SPP_ROLE_SLAVE, 0, "Denon control") != ESP_OK) {
    btLastError = "spp_server_failed";
    Serial.println("Could not start the SPP server");
  }
}

void handleBtEvent(BtEvent &e) {
  switch (e.type) {
    case BtEventType::aclConnect: {
      if (!isDenonAddress(e.address)) return;
      const bool success = aclStatusSucceeded(e.status);
      btDiagnostics::recordConnect(e.status, static_cast<uint16_t>(e.handle), e.at,
                                   success);
      Serial.printf("BT_DIAG ACL_CONNECT t=%u status=%d handle=%u\n", e.at,
                    static_cast<int>(e.status), static_cast<unsigned>(e.handle));
      if (e.status == ESP_BT_STATUS_HCI_PAGE_TIMEOUT && connectInProgress) {
        attemptPageTimeout = true;
      }
      // The receiver may open the ACL itself; use it for SPP right away.
      if (success && denonBonded && !sppOpen && !connectInProgress) nextReconnectAt = 0;
      return;
    }
    case BtEventType::aclDisconnect:
      if (!isDenonAddress(e.address)) return;
      btDiagnostics::recordDisconnect(e.status, static_cast<uint16_t>(e.handle), e.at);
      Serial.printf("BT_DIAG ACL_DISCONNECT t=%u reason=%d handle=%u\n", e.at,
                    static_cast<int>(e.status), static_cast<unsigned>(e.handle));
      return;
    case BtEventType::authComplete:
      if (!isDenonAddress(e.address)) return;
      if (e.status == ESP_BT_STATUS_SUCCESS) {
        refreshDenonBondState();
      } else {
        Serial.printf("Denon authentication failed, status %d\n",
                      static_cast<int>(e.status));
      }
      return;
    case BtEventType::confirmRequest:
      confirmDenonPairing(e.address, e.value);
      return;
    case BtEventType::pinRequest: {
      esp_bt_pin_code_t pin;
      memset(pin, '0', sizeof(pin));
      esp_bt_gap_pin_reply(e.address, true, e.value ? 16 : 4, pin);
      return;
    }
    case BtEventType::discoveryResult:
      addDiscoveredDevice(e);
      return;
    case BtEventType::discoveryState:
      discoveryRunning = e.status == ESP_BT_GAP_DISCOVERY_STARTED;
      if (discoveryRunning) discoveryStarted = true;
      return;
    case BtEventType::sppInit:
      sppReady = e.status == ESP_SPP_SUCCESS;
      if (sppReady) {
        startSppServer();
      } else {
        failBtStart("spp_init_failed");
      }
      return;
    case BtEventType::sppServerStart:
      sppServerRunning = e.status == ESP_SPP_SUCCESS;
      if (!sppServerRunning) {
        btLastError = "spp_server_failed";
        Serial.printf("SPP server did not start, status %d\n", static_cast<int>(e.status));
      }
      return;
    case BtEventType::sppClientInit:
      if (!connectInProgress) return;
      if (e.status != ESP_SPP_SUCCESS) {
        finishConnectAttempt(false, BtFailure::other);
      } else {
        attemptHandle = e.handle;
      }
      return;
    case BtEventType::sppOpen:
      acceptSppLink(e, false);
      return;
    case BtEventType::sppServerOpen:
      acceptSppLink(e, true);
      return;
    case BtEventType::sppClose:
      handleSppClose(e);
      return;
    case BtEventType::sppCongestion:
      if (!currentSession(e)) return;
      sppCongested = e.value != 0;
      pumpDenonTx();
      return;
    case BtEventType::sppWrite:
      if (!currentSession(e)) return;
      sppWriteInFlight = false;
      sppTxProgressAt = millis();
      if (e.status != ESP_SPP_SUCCESS) {
        dropSppLink("write failed");
        return;
      }
      sppCongested = e.value != 0;
      pumpDenonTx();
      return;
    case BtEventType::a2dpProfile:
      a2dpReady = e.status == ESP_A2D_INIT_SUCCESS;
      if (a2dpReady) nextA2dpReconnectAt = 0;
      return;
    case BtEventType::a2dpConnection:
      handleA2dpConnection(e);
      return;
  }
}

void processBtEvents() {
  // Events are handled only after a successful lifecycle handoff; until then
  // they wait in the queue (and a later generation bump discards them).
  if (!btEvents || !bluetoothReady || btLifecycleBusy.load(std::memory_order_acquire) ||
      btLifecycleResultPending) {
    return;
  }
  BtEvent e;
  const uint8_t generation = btGeneration.load(std::memory_order_relaxed);
  while (xQueueReceive(btEvents, &e, 0) == pdTRUE) {
    if (e.generation == generation) handleBtEvent(e);
  }
  const uint32_t drops = btEventDrops.load(std::memory_order_relaxed);
  if (drops != btEventDropsHandled) {
    btEventDropsHandled = drops;
    Serial.printf("Bluetooth event queue overflow (%lu dropped)\n",
                  static_cast<unsigned long>(drops));
    dropSppLink("events were lost");
  }
}

// Waits in the loop task while still handling Bluetooth events.
void btPause(unsigned long ms) {
  esp_task_wdt_reset();
  delay(ms);
  processBtEvents();
}

template <typename Done>
bool waitBt(Done done, unsigned long timeoutMs) {
  const unsigned long deadline = millis() + timeoutMs;
  while (!done()) {
    if (timeReached(millis(), deadline)) return false;
    btPause(10);
  }
  return true;
}

// Ends the receiver link cleanly so the Denon sees a normal disconnect
// instead of a supervision timeout.
void shutdownBluetoothLinks() {
  if (!bluetoothReady) return;
  forgetInProgress = true;  // reject new links and close A2DP while waiting
  if (sppOpen) esp_spp_disconnect(sppHandle);
  if (connectInProgress && attemptHandle) esp_spp_disconnect(attemptHandle);
  if (a2dpConnecting || a2dpConnected) esp_a2d_source_disconnect(denonMac);
  const bool closed = waitBt(
      [] {
        return !sppOpen && !a2dpConnected && !a2dpConnecting && !a2dpDisconnecting;
      },
      kCleanShutdownMs);
  Serial.println(closed ? "Bluetooth links closed" : "Bluetooth close timed out");
  // Disabling Bluedroid disconnects any remaining ACL link. The board resets
  // next, and the loop watchdog bounds this call if the stack is wedged.
  if (esp_bluedroid_disable() == ESP_OK) rtcBtLifecycleMarker = 0;  // clean stop
}

void restartDevice(bool clean) {
  delay(250);  // let the HTTP response leave first
  if (clean) shutdownBluetoothLinks();
  rtcBtLifecycleMarker = 0;  // an intentional restart is not a Bluetooth fault
  delay(50);
  esp_restart();
}

void startA2dpConnect() {
  if (!a2dpReady || !hasDenonMac || !denonBondStateKnown || denonBonded ||
      a2dpConnecting || a2dpConnected || a2dpDisconnecting ||
      connectInProgress || forgetInProgress || discoveryActive) {
    return;
  }
  clearPairingNumber();
  pairingTimedOut = false;
  setA2dpTransition(true, false);
  if (esp_a2d_source_connect(denonMac) != ESP_OK) {
    setA2dpTransition(false, false);
    nextA2dpReconnectAt = millis() + kReconnectMs;
    Serial.println("Could not start Denon A2DP connection");
  }
}

void startDenonConnect() {
  if ((!denonBonded && !a2dpConnected) || connectInProgress ||
      forgetInProgress || !sppReady || sppOpen || discoveryActive) {
    return;
  }
  if (a2dpConnected && !denonBonded) refreshDenonBondState();
  clearPairingNumber();
  pairingTimedOut = false;
  const unsigned long now = millis();
  // The captured Denon 500-series service uses RFCOMM channel 2. Connecting
  // directly also avoids the X580BT rejecting SDP before it is paired.
  if (esp_spp_connect(ESP_SPP_SEC_ENCRYPT | ESP_SPP_SEC_AUTHENTICATE,
                      ESP_SPP_ROLE_MASTER, kDenonSppChannel, denonMac) != ESP_OK) {
    btLadderFailure(btLadder, now, BtFailure::other);
    nextReconnectAt = now + btLadderRetryDelayMs(btLadder);
    Serial.println("Could not start Denon connection");
    return;
  }
  connectInProgress = true;
  attemptHandle = 0;
  attemptStartedAt = now;
  attemptPageTimeout = false;
  Serial.printf("Connecting to Denon at %s\n", macString().c_str());
}

// Missing completions are failures too: profile start, server start,
// writes, connect attempts, and A2DP transitions all have deadlines.
void checkBtDeadlines(unsigned long now) {
  if (sppReady && a2dpReady) {
    btStartFailures = 0;
  } else if (timeReached(now, btProfilesDeadline)) {
    failBtStart(!sppReady ? "spp_init_timeout" : "a2dp_init_timeout");
    return;
  }
  if (sppReady && !sppServerRunning && timeReached(now, sppServerRetryAt)) {
    startSppServer();
  }
  // Fresh clock: an event handled earlier in this pass may have just
  // submitted a write, which must not read as "time since" wrapping around.
  if (sppOpen && (sppWriteInFlight || sppTxCount) &&
      static_cast<long>(millis() - sppTxProgressAt) >=
          static_cast<long>(kBtWriteStallMs)) {
    dropSppLink("transmit made no progress");
  }
  if (connectInProgress && now - attemptStartedAt >= kBtAttemptTimeoutMs) {
    if (attemptHandle) esp_spp_disconnect(attemptHandle);
    finishConnectAttempt(false, BtFailure::noResponse);
  }
  if ((a2dpConnecting || a2dpDisconnecting) && !forgetInProgress &&
      now - a2dpTransitionAt >= kA2dpTransitionTimeoutMs) {
    Serial.println("Denon A2DP transition timed out");
    btLastError = "a2dp_timeout";
    esp_a2d_source_disconnect(denonMac);
    pairingTimedOut = a2dpConnecting;
    setA2dpTransition(false, false);
    a2dpConnected = false;
    clearPairingNumber();
    nextA2dpReconnectAt = now + kReconnectMs;
    btLadderFailure(btLadder, now, BtFailure::noResponse);
  }
}

void noteDenonDisconnected(unsigned long now) {
  wasDenonConnected = false;
  sessionInitialized = false;
  volumeRaw = -1;
  muteStateKnown = false;
  currentPlaybackIdleAuthorized = false;
  pendingPlaybackIdleAuthorized = false;
  if (restoreAutomaticMuteCycle || automaticRemuteRequired) {
    automaticMuteConfirmationPending = false;
    cancelVolumeRestore();
  } else {
    pauseVolumeTargetSession();
  }
  nextReconnectAt = now + 1000;
  Serial.println("Denon disconnected");
}

void maintainDenon() {
  const unsigned long now = millis();
  // Also covers links lost to a stack restart while Bluetooth is down.
  if (!sppOpen && wasDenonConnected) noteDenonDisconnected(now);
  if (!bluetoothReady) return;
  checkBtDeadlines(now);
  if (!bluetoothReady || !hasDenonMac || !denonBondStateKnown) return;

  const bool connected = sppOpen;
  if (connected) {
    if (!wasDenonConnected) {
      wasDenonConnected = true;
      sessionInitialized = false;
      volumeRaw = -1;
      muteStateKnown = false;
      if (restoreTargetRaw >= 0) armVolumeTargetSession(now);
      nextStatusAt = now + 300;
    }
    if (timeReached(now, nextStatusAt)) {
      // The TX queue sends in order, one write at a time, so the handshake
      // and source request complete before the first status request.
      if (!sessionInitialized) {
        sessionInitialized =
            sendDenon(kControlHandshake, sizeof(kControlHandshake)) &&
            sendDenon(kGetSources, sizeof(kGetSources));
      }
      if (sessionInitialized && restoreTargetRaw < 0) {
        sendDenon(kGetStatus, sizeof(kGetStatus));
      }
      nextStatusAt = now + kStatusIntervalMs;
    }
    return;
  }

  if (wasDenonConnected) noteDenonDisconnected(now);

  if (!connectInProgress && !forgetInProgress && !a2dpConnecting &&
      !discoveryActive && btLadderShouldRestartStack(btLadder, now)) {
    btLadderStackRestarted(btLadder, now);
    requestBtLifecycle(true, btLadder.consecutiveNoResponse ? "no response"
                                                            : "sustained failures");
    return;
  }

  // A saved bond already provides the authenticated link key. Use only SPP so
  // background control does not select the Denon's Bluetooth audio input.
  if (denonBonded) {
    if (!connectInProgress && !forgetInProgress &&
        timeReached(now, nextReconnectAt)) {
      startDenonConnect();
    }
    return;
  }

  if (!a2dpConnected) {
    if (!a2dpConnecting && !a2dpDisconnecting && !connectInProgress &&
        !forgetInProgress && timeReached(now, nextA2dpReconnectAt)) {
      startA2dpConnect();
    }
    return;
  }

  if (!connectInProgress && !forgetInProgress &&
      timeReached(now, nextReconnectAt)) {
    startDenonConnect();
  }
}

void readDenon() {
  static uint8_t buffer[128];
  uint8_t message[sizeof(BtRxHeader) + kBtRxChunkBytes];
  size_t received;
  const uint8_t generation = btGeneration.load(std::memory_order_relaxed);
  while ((received = xMessageBufferReceive(btRx, message, sizeof(message), 0)) > 0) {
    BtRxHeader header;
    memcpy(&header, message, sizeof(header));
    // Only bytes of the accepted session reach the parser.
    if (received <= sizeof(header) || !sppOpen || header.generation != generation ||
        header.handle != sppHandle || header.connection != sppConnection) {
      continue;
    }
    size_t &length = denonRxLength;
    for (size_t k = sizeof(header); k < received; ++k) {
      if (length == sizeof(buffer)) {
        memmove(buffer, buffer + 1, --length);
      }
      buffer[length++] = message[k];

      bool consumed;
      do {
        consumed = false;
        for (size_t i = 0; i + 7 <= length; ++i) {
          bool muted = false;
          if (parseMutePacket(buffer + i, 7, muted)) {
            observeMute(muted);
            Serial.printf("Mute: %s\n", muted ? "on" : "off");
            memmove(buffer, buffer + i + 7, length - (i + 7));
            length -= i + 7;
            consumed = true;
            break;
          }
          if (i + 9 > length) continue;
          uint8_t raw = 0;
          if (!parseVolumePacket(buffer + i, 9, raw)) continue;
          volumeRaw = raw;
          observeVolume(raw);
          Serial.printf("Volume: %.1f\n", raw / 2.0f);
          memmove(buffer, buffer + i + 9, length - (i + 9));
          length -= i + 9;
          consumed = true;
          break;
        }
      } while (consumed);
    }
  }
}

void skipJsonWhitespace(const String &json, size_t &position) {
  while (position < json.length() &&
         (json[position] == ' ' || json[position] == '\t' ||
          json[position] == '\r' || json[position] == '\n')) {
    ++position;
  }
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool readJsonCodeUnit(const String &json, size_t &position, uint16_t &value) {
  if (position + 4 > json.length()) return false;
  value = 0;
  for (size_t i = 0; i < 4; ++i) {
    const int digit = hexValue(json[position++]);
    if (digit < 0) return false;
    value = static_cast<uint16_t>((value << 4) | digit);
  }
  return true;
}

bool appendUtf8(String &output, uint32_t codePoint) {
  if (codePoint < 0x20 || codePoint > 0x10FFFF ||
      (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
    return false;
  }
  if (codePoint <= 0x7F) {
    output += static_cast<char>(codePoint);
  } else if (codePoint <= 0x7FF) {
    output += static_cast<char>(0xC0 | (codePoint >> 6));
    output += static_cast<char>(0x80 | (codePoint & 0x3F));
  } else if (codePoint <= 0xFFFF) {
    output += static_cast<char>(0xE0 | (codePoint >> 12));
    output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
    output += static_cast<char>(0x80 | (codePoint & 0x3F));
  } else {
    output += static_cast<char>(0xF0 | (codePoint >> 18));
    output += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
    output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
    output += static_cast<char>(0x80 | (codePoint & 0x3F));
  }
  return true;
}

bool parseJsonString(const String &json, size_t &position, String &output) {
  if (position >= json.length() || json[position++] != '"') return false;
  output = "";
  while (position < json.length()) {
    const unsigned char c = json[position++];
    if (c == '"') return true;
    if (c < 0x20) return false;
    if (c != '\\') {
      output += static_cast<char>(c);
      continue;
    }
    if (position >= json.length()) return false;
    const char escaped = json[position++];
    if (escaped == '"' || escaped == '\\' || escaped == '/') {
      output += escaped;
    } else if (escaped == 'b' || escaped == 'f' || escaped == 'n' ||
               escaped == 'r' || escaped == 't') {
      return false;
    } else if (escaped == 'u') {
      uint16_t first = 0;
      if (!readJsonCodeUnit(json, position, first)) return false;
      uint32_t codePoint = first;
      if (first >= 0xD800 && first <= 0xDBFF) {
        if (position + 2 > json.length() || json[position] != '\\' ||
            json[position + 1] != 'u') {
          return false;
        }
        position += 2;
        uint16_t second = 0;
        if (!readJsonCodeUnit(json, position, second) || second < 0xDC00 ||
            second > 0xDFFF) {
          return false;
        }
        codePoint = 0x10000 +
                    ((static_cast<uint32_t>(first) - 0xD800) << 10) +
                    (static_cast<uint32_t>(second) - 0xDC00);
      }
      if (!appendUtf8(output, codePoint)) return false;
    } else {
      return false;
    }
  }
  return false;
}

bool parseAppJson(const String &json, String &appId, String &appName,
                  String &eventId, bool &playbackKnown,
                  bool &playbackActive) {
  size_t position = 0;
  bool hasAppId = false;
  bool hasAppName = false;
  playbackKnown = false;
  playbackActive = false;
  eventId = "";
  skipJsonWhitespace(json, position);
  if (position >= json.length() || json[position++] != '{') return false;
  skipJsonWhitespace(json, position);
  while (position < json.length() && json[position] != '}') {
    String key;
    if (!parseJsonString(json, position, key)) return false;
    skipJsonWhitespace(json, position);
    if (position >= json.length() || json[position++] != ':') return false;
    skipJsonWhitespace(json, position);
    if (key == "playback_active") {
      if (json.substring(position, position + 4) == "true") {
        playbackActive = true;
        position += 4;
      } else if (json.substring(position, position + 5) == "false") {
        playbackActive = false;
        position += 5;
      } else {
        return false;
      }
      playbackKnown = true;
    } else {
      String value;
      if (!parseJsonString(json, position, value)) return false;
      if (key == "app_id") {
      appId = value;
      hasAppId = true;
      } else if (key == "app_name") {
        appName = value;
        hasAppName = true;
      } else if (key == "event_id") {
        eventId = value;
      }
    }
    skipJsonWhitespace(json, position);
    if (position < json.length() && json[position] == ',') {
      ++position;
      skipJsonWhitespace(json, position);
      if (position >= json.length() || json[position] == '}') return false;
    } else {
      break;
    }
  }
  if (position >= json.length() || json[position++] != '}') return false;
  skipJsonWhitespace(json, position);
  return position == json.length() && hasAppId && hasAppName;
}

bool parseJsonUnsigned(const String &json, size_t &position, uint32_t &value) {
  if (position >= json.length() || json[position] < '0' ||
      json[position] > '9') {
    return false;
  }
  if (json[position] == '0' && position + 1 < json.length() &&
      json[position + 1] >= '0' && json[position + 1] <= '9') {
    return false;
  }
  uint32_t parsed = 0;
  do {
    const uint8_t digit = static_cast<uint8_t>(json[position++] - '0');
    if (parsed > (UINT32_MAX - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  } while (position < json.length() && json[position] >= '0' &&
           json[position] <= '9');
  value = parsed;
  return true;
}

bool validBackupText(const String &appId, const String &appName) {
  return !appId.isEmpty() && appId.length() <= kMaxAppIdLength &&
         appName.length() <= kMaxAppNameLength;
}

bool parseBackupApp(const String &json, size_t &position,
                    StoredAppVolume &app) {
  bool hasAppId = false;
  bool hasAppName = false;
  bool hasVolumeRaw = false;
  String appId;
  String appName;
  uint32_t volumeRaw = 0;
  if (position >= json.length() || json[position++] != '{') return false;
  skipJsonWhitespace(json, position);
  while (position < json.length() && json[position] != '}') {
    String key;
    if (!parseJsonString(json, position, key)) return false;
    skipJsonWhitespace(json, position);
    if (position >= json.length() || json[position++] != ':') return false;
    skipJsonWhitespace(json, position);
    if (key == "app_id" && !hasAppId) {
      if (!parseJsonString(json, position, appId)) return false;
      hasAppId = true;
    } else if (key == "app_name" && !hasAppName) {
      if (!parseJsonString(json, position, appName)) return false;
      hasAppName = true;
    } else if (key == "volume_raw" && !hasVolumeRaw) {
      if (!parseJsonUnsigned(json, position, volumeRaw) ||
          volumeRaw > kMaxVolumeRaw) {
        return false;
      }
      hasVolumeRaw = true;
    } else {
      return false;
    }
    skipJsonWhitespace(json, position);
    if (position < json.length() && json[position] == ',') {
      ++position;
      skipJsonWhitespace(json, position);
      if (position >= json.length() || json[position] == '}') return false;
    } else {
      break;
    }
  }
  if (position >= json.length() || json[position++] != '}' || !hasAppId ||
      !hasAppName || !hasVolumeRaw || !validBackupText(appId, appName)) {
    return false;
  }
  app = {};
  app.magic = kStoredAppMagic;
  app.raw = static_cast<uint8_t>(volumeRaw);
  copyText(app.appId, sizeof(app.appId), appId);
  copyText(app.appName, sizeof(app.appName), appName);
  return true;
}

bool parseBackupApps(const String &json, size_t &position,
                     StoredAppVolume apps[kMaxApps], size_t &count) {
  if (position >= json.length() || json[position++] != '[') return false;
  count = 0;
  skipJsonWhitespace(json, position);
  if (position < json.length() && json[position] == ']') {
    ++position;
    return true;
  }
  while (position < json.length()) {
    if (count >= kMaxApps || !parseBackupApp(json, position, apps[count])) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      if (strcmp(apps[i].appId, apps[count].appId) == 0) return false;
    }
    ++count;
    skipJsonWhitespace(json, position);
    if (position < json.length() && json[position] == ',') {
      ++position;
      skipJsonWhitespace(json, position);
      if (position >= json.length() || json[position] == ']') return false;
      continue;
    }
    if (position >= json.length() || json[position++] != ']') return false;
    return true;
  }
  return false;
}

bool parseBackupJson(const String &json, StoredAppVolume apps[kMaxApps],
                     size_t &count) {
  size_t position = 0;
  bool hasSchema = false;
  bool hasApps = false;
  uint32_t schema = 0;
  skipJsonWhitespace(json, position);
  if (position >= json.length() || json[position++] != '{') return false;
  skipJsonWhitespace(json, position);
  while (position < json.length() && json[position] != '}') {
    String key;
    if (!parseJsonString(json, position, key)) return false;
    skipJsonWhitespace(json, position);
    if (position >= json.length() || json[position++] != ':') return false;
    skipJsonWhitespace(json, position);
    if (key == "schema" && !hasSchema) {
      if (!parseJsonUnsigned(json, position, schema)) return false;
      hasSchema = true;
    } else if (key == "apps" && !hasApps) {
      if (!parseBackupApps(json, position, apps, count)) return false;
      hasApps = true;
    } else {
      return false;
    }
    skipJsonWhitespace(json, position);
    if (position < json.length() && json[position] == ',') {
      ++position;
      skipJsonWhitespace(json, position);
      if (position >= json.length() || json[position] == '}') return false;
    } else {
      break;
    }
  }
  if (position >= json.length() || json[position++] != '}' || !hasSchema ||
      !hasApps || schema != 1) {
    return false;
  }
  skipJsonWhitespace(json, position);
  return position == json.length();
}

bool jsonSelfCheck() {
  String appId;
  String appName;
  String eventId;
  bool playbackKnown = false;
  bool playbackActive = false;
  StoredAppVolume apps[kMaxApps];
  size_t count = 0;
  return parseAppJson(
             "{\"app_id\":\"com.example.video\",\"app_name\":\"Video\","
             "\"event_id\":\"0123456789abcdef0123456789abcdef\","
             "\"playback_active\":false}",
             appId, appName, eventId, playbackKnown, playbackActive) &&
         appId == "com.example.video" && appName == "Video" &&
         eventId == "0123456789abcdef0123456789abcdef" && playbackKnown &&
         !playbackActive &&
         !parseAppJson("{\"app_id\":\"bad\",}", appId, appName,
                       eventId, playbackKnown, playbackActive) &&
         parseBackupJson(
             "{\"schema\":1,\"apps\":[{\"app_id\":\"one\","
             "\"app_name\":\"One\",\"volume_raw\":100},{\"app_id\":"
             "\"two\",\"app_name\":\"Two\",\"volume_raw\":101}]}",
             apps, count) &&
         count == 2 && strcmp(apps[0].appId, "one") == 0 &&
         apps[1].raw == 101 &&
         !parseBackupJson(
             "{\"schema\":1,\"apps\":[{\"app_id\":\"one\","
             "\"app_name\":\"One\",\"volume_raw\":197}]}",
             apps, count) &&
         !parseBackupJson(
             "{\"schema\":1,\"apps\":[{\"app_id\":\"one\","
             "\"app_name\":\"One\",\"volume_raw\":1},{\"app_id\":"
             "\"one\",\"app_name\":\"Again\",\"volume_raw\":2}]}",
             apps, count);
}

bool appTableEmpty() {
  for (const StoredAppVolume &app : appVolumes) {
    if (app.appId[0] != '\0') return false;
  }
  return true;
}

String backupEtag() { return "\"" + String(appSequence) + "\""; }

size_t orderedAppIndices(size_t indices[kMaxApps]) {
  size_t count = 0;
  for (size_t i = 0; i < kMaxApps; ++i) {
    if (appVolumes[i].appId[0] == '\0') continue;
    size_t position = count;
    while (position > 0 &&
           appVolumes[indices[position - 1]].sequence >
               appVolumes[i].sequence) {
      indices[position] = indices[position - 1];
      --position;
    }
    indices[position] = i;
    ++count;
  }
  return count;
}

bool writeBackupBank(uint8_t bank,
                     const StoredAppVolume apps[kMaxApps], size_t count,
                     uint32_t revision) {
  for (size_t i = 0; i < count; ++i) {
    char key[7];
    appStorageKey(bank, i, key);
    if (preferences.putBytes(key, &apps[i], sizeof(apps[i])) !=
        sizeof(apps[i])) {
      return false;
    }
  }
  for (size_t i = count; i < kMaxApps; ++i) {
    char key[7];
    appStorageKey(bank, i, key);
    if (preferences.isKey(key) && !preferences.remove(key)) return false;
  }
  if (preferences.putUInt(appSequenceKey(bank), revision) != sizeof(revision)) {
    return false;
  }

  for (size_t i = 0; i < count; ++i) {
    char key[7];
    StoredAppVolume readback;
    appStorageKey(bank, i, key);
    if (preferences.getBytesLength(key) != sizeof(readback) ||
        preferences.getBytes(key, &readback, sizeof(readback)) !=
            sizeof(readback) ||
        memcmp(&readback, &apps[i], sizeof(readback)) != 0) {
      return false;
    }
  }
  for (size_t i = count; i < kMaxApps; ++i) {
    char key[7];
    appStorageKey(bank, i, key);
    if (preferences.isKey(key)) return false;
  }
  return preferences.getUInt(appSequenceKey(bank), UINT32_MAX) == revision;
}

bool commitBackupApps(StoredAppVolume apps[kMaxApps], size_t count) {
  if (count == 0) return true;
  uint32_t base = appSequence;
  if (base > UINT32_MAX - count) base = 0;
  for (size_t i = 0; i < count; ++i) {
    apps[i].sequence = base + static_cast<uint32_t>(i) + 1;
  }
  const uint32_t revision = base + static_cast<uint32_t>(count);
  const uint8_t targetBank = activeAppBank == 0 ? 1 : 0;
  if (!writeBackupBank(targetBank, apps, count, revision)) return false;

  if (preferences.putUChar("app_bank", targetBank) != sizeof(targetBank) ||
      preferences.getUChar("app_bank", 0xFF) != targetBank) {
    return false;
  }
  memset(appVolumes, 0, sizeof(appVolumes));
  memcpy(appVolumes, apps, count * sizeof(apps[0]));
  memset(appDirty, 0, sizeof(appDirty));
  memset(appPersistAt, 0, sizeof(appPersistAt));
  activeAppBank = targetBank;
  appSequence = revision;
  startRestoreForCurrentApp();
  return true;
}

bool requireBackupAuthorization() {
  if (hasAppAuthorization()) return true;
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("WWW-Authenticate", "Bearer");
  server.send(401, "text/plain", "Missing or invalid bearer token");
  return false;
}

bool parseLogQueryUnsigned(const String &text, uint32_t maximum,
                           uint32_t &value) {
  size_t position = 0;
  return parseJsonUnsigned(text, position, value) &&
         position == text.length() && value <= maximum;
}

void appendLogJsonText(String &body, const char *text, size_t length) {
  constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (c == '"' || c == '\\') {
      body += '\\';
      body += static_cast<char>(c);
    } else if (c < 0x20 || c >= 0x7F) {
      body += "\\u00";
      body += hex[c >> 4];
      body += hex[c & 0x0F];
    } else {
      body += static_cast<char>(c);
    }
  }
}

void sendLogs() {
  if (!requireBackupAuthorization()) return;
  server.sendHeader("Cache-Control", "no-store");
  uint32_t after = 0;
  uint32_t limit = networkLogs::kMaxPage;
  if ((server.hasArg("after") &&
       !parseLogQueryUnsigned(server.arg("after"), UINT32_MAX, after)) ||
      (server.hasArg("limit") &&
       (!parseLogQueryUnsigned(server.arg("limit"), networkLogs::kMaxPage,
                              limit) || limit == 0))) {
    server.send(400, "text/plain", "Invalid log cursor or limit");
    return;
  }

  networkLogs::flush();
  const networkLogs::Stats stats = networkLogs::stats();
  constexpr size_t kMaxResponseBytes = 3072;
  String body;
  if (!body.reserve(kMaxResponseBytes)) {
    server.send(503, "text/plain", "Log response allocation failed");
    return;
  }
  body += "{\"boot_id\":\"" + String(networkLogs::bootId()) +
         "\",\"oldest_seq\":" + String(stats.oldestSeq) +
         ",\"next_seq\":" + String(stats.nextSeq) +
         ",\"dropped\":" + String(stats.dropped) + ",\"records\":[";
  networkLogs::Record record;
  for (uint32_t count = 0; count < limit &&
                           networkLogs::readAfter(after, record); ++count) {
    size_t encodedLength = 0;
    for (size_t i = 0; i < record.length; ++i) {
      const uint8_t c = static_cast<uint8_t>(record.text[i]);
      encodedLength += c < 0x20 || c >= 0x7F ? 6 :
                       c == '"' || c == '\\' ? 2 : 1;
    }
    if (body.length() + encodedLength + 128 + 2 > kMaxResponseBytes) break;
    if (count) body += ',';
    body += "{\"seq\":" + String(record.seq) +
            ",\"uptime_ms\":" + String(record.uptimeMs) +
            ",\"continued\":" + String(record.continued ? "true" : "false") +
            ",\"text\":\"";
    appendLogJsonText(body, record.text, record.length);
    body += "\"}";
    after = record.seq;
  }
  body += "]}";
  server.send(200, "application/json", body);
}

void sendBackup() {
  if (!requireBackupAuthorization()) return;
  size_t indices[kMaxApps];
  const size_t count = orderedAppIndices(indices);
  String body = "{\"schema\":1,\"revision\":" + String(appSequence) +
                ",\"apps\":[";
  for (size_t position = 0; position < count; ++position) {
    if (position) body += ',';
    const StoredAppVolume &app = appVolumes[indices[position]];
    body += "{\"app_id\":\"" + jsonEscape(app.appId) +
            "\",\"app_name\":\"" + jsonEscape(app.appName) +
            "\",\"volume_raw\":" + String(app.raw) + "}";
  }
  body += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("ETag", backupEtag());
  server.send(200, "application/json", body);
}

void restoreBackup() {
  if (!requireBackupAuthorization()) return;
  server.sendHeader("Cache-Control", "no-store");
  const String body = server.arg("plain");
  if (body.isEmpty() || body.length() > kMaxBackupBodyLength) {
    server.send(body.isEmpty() ? 400 : 413, "text/plain",
                body.isEmpty() ? "Expected a JSON backup" :
                                 "Backup is too large");
    return;
  }
  StoredAppVolume apps[kMaxApps];
  size_t count = 0;
  if (!parseBackupJson(body, apps, count)) {
    server.send(400, "text/plain", "Invalid schema 1 app backup");
    return;
  }
  if (!server.hasHeader("If-Match")) {
    server.send(428, "text/plain", "If-Match is required");
    return;
  }
  if (server.header("If-Match") != backupEtag()) {
    server.send(412, "text/plain", "Backup revision changed");
    return;
  }
  if (!appTableEmpty()) {
    server.send(409, "text/plain", "App volume table is not empty");
    return;
  }
  if (!commitBackupApps(apps, count)) {
    server.send(500, "text/plain",
                "Could not persist backup; current table was preserved");
    return;
  }
  server.sendHeader("ETag", backupEtag());
  server.send(204);
}

void sendInfo() {
  const String name = "Denon Volume " + deviceId.substring(0, 8);
  const String body =
      "{\"product\":\"" + String(kProduct) + "\",\"api_version\":" +
      kApiVersion + ",\"id\":\"" + deviceId + "\",\"name\":\"" +
      jsonEscape(name) + "\",\"device_id\":\"" + deviceId +
      "\",\"hostname\":\"" + jsonEscape(hostName + ".local") +
      "\",\"paired\":" +
      String(apiToken.isEmpty() ? "false" : "true") +
      ",\"pairing_available\":" +
      String(apiToken.isEmpty() && apiClaimWindowOpen() ? "true" : "false") +
      "}";
  server.send(200, "application/json", body);
}

void pairApi() {
  if (!apiToken.isEmpty()) {
    server.send(409, "text/plain", "Home Assistant is already paired");
    return;
  }
  if (!apiClaimWindowOpen()) {
    server.send(403, "text/plain",
                "Pairing is closed; restart the ESP32 and retry within 10 minutes");
    return;
  }
  const String token = randomHex(32);
  if (preferences.putString("api_token", token) == 0) {
    server.send(500, "text/plain", "Could not persist API token");
    return;
  }
  apiToken = token;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"token\":\"" + token + "\"}");
}

void unpairApi() {
  if (!hasAppAuthorization()) {
    server.sendHeader("WWW-Authenticate", "Bearer");
    server.send(401, "text/plain", "Missing or invalid bearer token");
    return;
  }
  if (!preferences.remove("api_token")) {
    server.send(500, "text/plain", "Could not remove API token");
    return;
  }
  apiToken = "";
  apiClaimUntil = millis() + kApiClaimWindowMs;
  server.send(204);
}

void maintainTokenResetButton() {
  if (digitalRead(kBootButtonPin) != LOW) {
    bootButtonPressedAt = 0;
    if (tokenResetArmed) {
      Serial.println("BOOT released; restarting with Home Assistant unpaired");
      restartDevice(true);
    }
    return;
  }
  if (tokenResetArmed || apiToken.isEmpty()) return;
  if (bootButtonPressedAt == 0) {
    bootButtonPressedAt = millis();
    return;
  }
  if (millis() - bootButtonPressedAt < kTokenResetHoldMs) return;
  if (!preferences.remove("api_token")) {
    Serial.println("Could not reset Home Assistant pairing token");
    bootButtonPressedAt = millis();
    return;
  }
  apiToken = "";
  tokenResetArmed = true;
  Serial.println("Home Assistant pairing reset; release BOOT to restart");
}

void receiveApp() {
  if (!hasAppAuthorization()) {
    server.sendHeader("WWW-Authenticate", "Bearer");
    server.send(401, "text/plain", "Missing or invalid bearer token");
    return;
  }
  String appId;
  String appName;
  String eventId;
  bool playbackKnown = false;
  bool playbackActive = false;
  if (!parseAppJson(server.arg("plain"), appId, appName, eventId,
                    playbackKnown, playbackActive)) {
    server.send(400, "text/plain",
                "Expected JSON string fields app_id and app_name");
    return;
  }
  appId.trim();
  appName.trim();
  if (appId.length() > kMaxAppIdLength || appName.length() > kMaxAppNameLength) {
    server.send(400, "text/plain", "App identity is too long");
    return;
  }
  if (appId.isEmpty()) {
    clearCurrentApp();
    server.send(202, "application/json",
                "{\"accepted\":true,\"cleared\":true}");
    return;
  }
  if (isIgnoredAppId(appId)) {
    server.send(204);
    return;
  }
  const bool validIdleEvent =
      playbackKnown && !playbackActive &&
      validPlaybackEventId(eventId.c_str(), eventId.length());
  const bool manualTargetActive = restoreTargetRaw >= 0 && !restoreAutomatic;
  if (validIdleEvent &&
      !playbackIdleEventReady(
          sppOpen, sessionInitialized, volumeRaw, muteStateKnown,
          denonMuted, manualMuteLocked, automaticRemuteRequired,
          manualTargetActive)) {
    server.send(503, "text/plain", "Receiver state is not ready for this event");
    return;
  }
  queueAppCandidate(appId, appName, playbackKnown, playbackActive, eventId);
  server.send(202, "application/json", "{\"accepted\":true}");
}

const char *restoreStateText() {
  if (automaticRemuteRequired && !restoreAutomaticMuteCycle) return "remuting";
  if (restoreTargetRaw >= 0) return "restoring";
  return restoreError.isEmpty() ? "idle" : "error";
}

void appendRestoreState(String &body) {
  body += ",\"restoring\":" +
          String((restoreTargetRaw >= 0 || automaticRemuteRequired) ? "true"
                                                                    : "false") +
          ",\"restore_state\":\"" + restoreStateText() +
          "\",\"restore_error\":";
  body += restoreError.isEmpty() ? "null" : "\"" + jsonEscape(restoreError) + "\"";
  body += ",\"mute_known\":" + String(muteStateKnown ? "true" : "false") +
          ",\"muted\":";
  body += muteStateKnown ? String(denonMuted ? "true" : "false") : "null";
  body += ",\"manual_mute_lock\":" +
          String(manualMuteLocked ? "true" : "false");
}

void sendApps() {
  String body = "{\"current_app_id\":";
  body += currentAppId.isEmpty() ? "null" : "\"" + jsonEscape(currentAppId) + "\"";
  appendRestoreState(body);
  body += ",\"apps\":[";
  bool first = true;
  for (size_t i = 0; i < kMaxApps; ++i) {
    const StoredAppVolume &stored = appVolumes[i];
    if (stored.appId[0] == '\0') continue;
    if (!first) body += ',';
    first = false;
    const bool active = currentAppId == stored.appId;
    body += "{\"app_id\":\"" + jsonEscape(stored.appId) +
            "\",\"app_name\":\"" + jsonEscape(stored.appName) +
            "\",\"volume_raw\":" + String(stored.raw) +
            ",\"volume\":" + String(stored.raw / 2.0f, 1) +
            ",\"volume_db\":" + String(stored.raw / 2.0f - 80.0f, 1) +
            ",\"active\":" + String(active ? "true" : "false") + "}";
  }
  body += "]}";
  server.send(200, "application/json", body);
}

void sendPage() { server.send_P(200, "text/html; charset=utf-8", kPage); }

void sendState() {
  const bool connected = sppOpen;
  const bool hasVolume = connected && volumeRaw >= 0;
  const bool connecting = a2dpConnecting || connectInProgress;
  const bool pairing = !denonBonded && a2dpConnecting;
  const bool hasPairingNumber = pairing && pairingNumberReady;
  const uint32_t currentPairingNumber = pairingNumber;
  const char *pairingStatus = hasPairingNumber
                                  ? "confirm_on_denon"
                                  : (pairing
                                         ? "waiting"
                                         : (!denonBonded && pairingTimedOut
                                                ? "timed_out"
                                                : "idle"));
  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                                   : WiFi.softAPIP().toString();
  String body = "{\"connected\":" + String(connected ? "true" : "false") +
                ",\"connecting\":" + String(connecting ? "true" : "false") +
                ",\"a2dp_connected\":" +
                String(a2dpConnected ? "true" : "false") +
                ",\"receiver_bonded\":" +
                String(denonBonded ? "true" : "false") +
                ",\"receiver_configured\":" +
                String(hasDenonMac ? "true" : "false") + ",\"setup_ap\":" +
                String(setupApRunning ? "true" : "false") +
                ",\"pairing_status\":\"" + pairingStatus + "\",\"pairing_number\":";
  if (hasPairingNumber) {
    char formatted[7];
    snprintf(formatted, sizeof(formatted), "%06lu",
             static_cast<unsigned long>(currentPairingNumber));
    body += "\"" + String(formatted) + "\"";
  } else {
    body += "null";
  }
  body += ",\"volume\":";
  body += hasVolume ? String(volumeRaw / 2.0f, 1) : "null";
  body += ",\"volume_db\":";
  body += hasVolume ? String(volumeRaw / 2.0f - 80.0f, 1) : "null";
  body += ",\"volume_raw\":";
  body += hasVolume ? String(volumeRaw) : "null";
  body += ",\"receiver_mac\":";
  body += hasDenonMac ? "\"" + macString() + "\"" : "null";
  body += ",\"current_app_id\":";
  body += currentAppId.isEmpty() ? "null" : "\"" + jsonEscape(currentAppId) + "\"";
  appendRestoreState(body);
  body += ",\"volume_target_id\":" + String(volumeTargetGeneration);
  body += ",\"ip\":\"" + jsonEscape(ip) + "\",\"network_mode\":\"" +
          String(staticNetworkEnabled ? "static" : "dhcp") +
          "\",\"hostname\":\"" + jsonEscape(hostName) + ".local\"";
  body += ",\"uptime_ms\":" + String(millis()) +
          ",\"free_heap_bytes\":" + String(ESP.getFreeHeap()) +
          ",\"min_free_heap_bytes\":" + String(ESP.getMinFreeHeap()) +
          ",\"wifi_retry_count\":" + String(wifiRetryCount) +
          ",\"wifi_rssi_dbm\":";
  body += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : "null";
  body += ",\"wifi_last_disconnect_reason\":";
  const int reason = lastWifiDisconnectReason.load(std::memory_order_relaxed);
  body += reason < 0 ? "null" : String(reason);
  body += ",\"bt_acl_connect_count\":" + String(btDiagnostics::connectCount);
  const int btStatus = btDiagnostics::lastConnectStatus;
  body += ",\"bt_acl_last_connect_status\":";
  body += btStatus < 0 ? "null" : String(btStatus);
  body += ",\"bt_acl_last_connect_at_ms\":";
  body += btStatus < 0 ? "null" : String(btDiagnostics::lastConnectAtMs);
  const int btReason = btDiagnostics::lastDisconnectReason;
  body += ",\"bt_acl_last_disconnect_reason\":";
  body += btReason < 0 ? "null" : String(btReason);
  body += ",\"bt_acl_last_disconnect_at_ms\":";
  body += btReason < 0 ? "null" : String(btDiagnostics::lastDisconnectAtMs);
  const uint32_t now = millis();
  const char *btStage =
      btLifecycleBusy.load(std::memory_order_relaxed)
          ? "stack_starting"
          : (!bluetoothReady
                 ? "bluetooth_down"
                 : btLadderStage(btLadder,
                                 hasDenonMac && (denonBonded || a2dpConnected),
                                 connectInProgress));
  body += ",\"bt_recovery_stage\":\"" + String(btStage) +
          "\",\"bt_page_timeouts\":" + String(btLadder.consecutivePageTimeouts) +
          ",\"bt_connect_failures\":" + String(btLadder.consecutiveFailures) +
          ",\"bt_stack_restarts\":" + String(btLadder.stackRestarts) +
          ",\"bt_start_failures\":" + String(btStartFailures) +
          ",\"bt_since_connected_ms\":";
  body += sppOpen ? "0"
                  : (btLadder.everConnected ? String(now - btLadder.lastConnectedMs)
                                            : "null");
  // btStarted() also links Arduino's strong btInUse(), which keeps
  // initArduino() from releasing the Bluetooth controller memory at boot.
  body += ",\"bt_controller_enabled\":" + String(btStarted() ? "true" : "false");
  body += ",\"bt_last_error\":";
  body += btLastError ? "\"" + String(btLastError) + "\"" : "null";
  body += ",\"bt_event_drops\":" +
          String(btEventDrops.load(std::memory_order_relaxed)) +
          ",\"bt_rx_drops\":" + String(btRxDrops.load(std::memory_order_relaxed));
  const esp_partition_t *running = esp_ota_get_running_partition();
  body += ",\"reset_reason\":" + String(static_cast<int>(esp_reset_reason())) +
          ",\"ota_slot\":";
  body += running ? "\"" + String(running->label) + "\"" : "null";
  body += ",\"ota_state\":\"" + String(otaBootState) +
          "\",\"ota_trial_boots\":" + String(otaTrialBoots) +
          ",\"ota_last_result\":";
  body += otaLastResult.isEmpty() ? "null" : "\"" + jsonEscape(otaLastResult) + "\"";
  body += "}";
  server.send(200, "application/json", body);
}

void sendVolume(const uint8_t *command, size_t length, int direction) {
  if (!requireManualVolumeAuthorization()) return;
  if (automaticRemuteRequired ||
      !volumeCommandAllowed(muteStateKnown, denonMuted, manualMuteLocked)) {
    server.send(423, "text/plain",
                "Unmute the Denon with its remote before changing volume");
    return;
  }
  if (restoreTargetRaw >= 0) {
    server.send(409, "text/plain", "Volume target is already in progress");
    return;
  }
  if (!sendDenonStep(command, length)) {
    server.send(503, "text/plain", "Receiver is not connected or busy; retry");
    return;
  }
  acceptManualVolumeFeedback(direction);
  cancelVolumeRestore();
  nextStatusAt = millis() + 300;
  server.send(204);
}

void sendTargetVolume() {
  if (!requireManualVolumeAuthorization()) return;
  if (automaticRemuteRequired ||
      !volumeCommandAllowed(muteStateKnown, denonMuted, manualMuteLocked)) {
    server.send(423, "text/plain",
                "Unmute the Denon with its remote before changing volume");
    return;
  }
  if (restoreTargetRaw >= 0) {
    server.send(409, "text/plain", "Volume target is already in progress");
    return;
  }
  String requested = server.arg("volume");
  requested.trim();
  uint8_t targetRaw = 0;
  if (!parseDisplayedVolume(requested.c_str(), requested.length(), targetRaw)) {
    server.send(400, "text/plain",
                "Volume must be from 0.0 to 98.0 in 0.5 steps");
    return;
  }
  if (!sppOpen || volumeRaw < 0) {
    server.send(503, "text/plain", "Receiver volume is not available");
    return;
  }
  acceptManualVolumeFeedback(volumeDirection(volumeRaw, targetRaw));
  if (!setVolume(targetRaw, false)) {
    server.send(400, "text/plain", "Volume is out of range");
    return;
  }
  server.send(202, "application/json",
              "{\"accepted\":true,\"target_volume\":" +
                  String(targetRaw / 2.0f, 1) + ",\"target_id\":" +
                  String(volumeTargetGeneration) + "}");
}

void reconnectDenon() {
  if (!bluetoothReady) {
    server.send(503, "text/plain", "Bluetooth Classic did not start");
    return;
  }
  if (!hasDenonMac) {
    server.send(409, "text/plain", "Select a receiver first");
    return;
  }
  if (forgetInProgress || a2dpConnecting || a2dpDisconnecting ||
      connectInProgress) {
    server.send(409, "text/plain", "A receiver connection is already active");
    return;
  }
  if (sppOpen) {
    server.send(204);
    return;
  }

  if (!denonBondStateKnown && !refreshDenonBondState()) {
    server.send(503, "text/plain", "Could not read the receiver Bluetooth bond");
    return;
  }

  if (denonBonded || a2dpConnected) {
    nextReconnectAt = 0;
    startDenonConnect();
    if (!connectInProgress) {
      server.send(503, "text/plain", "Could not start the receiver connection");
      return;
    }
  } else {
    nextA2dpReconnectAt = 0;
    startA2dpConnect();
    if (!a2dpConnecting) {
      server.send(503, "text/plain", "Could not start the Bluetooth connection");
      return;
    }
  }
  server.send(202, "application/json", "{\"connecting\":true}");
}

void discoverDenon() {
  if (!requireProvisioningAuthorization()) return;
  if (!bluetoothReady) {
    server.send(503, "text/plain", "Bluetooth Classic did not start");
    return;
  }
  if (forgetInProgress || a2dpConnecting || a2dpConnected || a2dpDisconnecting ||
      connectInProgress || sppOpen) {
    server.send(409, "text/plain", "A receiver connection is already active");
    return;
  }
  if (hasDenonMac) {
    server.send(409, "text/plain", "Forget the saved receiver before scanning again");
    return;
  }
  Serial.println("Bluetooth discovery started");
  discoveredCount = 0;
  discoveryStarted = false;
  discoveryRunning = false;
  discoveryActive = true;
  // 6 x 1.28 s inquiry, the same length as the previous 8 s scan.
  if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 6, 0) != ESP_OK) {
    discoveryActive = false;
    server.send(503, "text/plain", "Bluetooth discovery did not start");
    return;
  }
  waitBt([] { return discoveryStarted && !discoveryRunning; }, 9000);
  esp_bt_gap_cancel_discovery();
  discoveryActive = false;
  String body = "{\"devices\":[";
  for (size_t i = 0; i < discoveredCount; ++i) {
    const uint8_t *a = discovered[i].address;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2],
             a[3], a[4], a[5]);
    if (i) body += ',';
    body += "{\"name\":\"" + jsonEscape(String(discovered[i].name)) +
            "\",\"mac\":\"" + mac + "\"}";
  }
  body += "]}";
  server.send(200, "application/json", body);
}

void saveDenon() {
  if (!requireProvisioningAuthorization()) return;
  if (!bluetoothReady) {
    server.send(503, "text/plain", "Bluetooth Classic is not ready");
    return;
  }
  if (forgetInProgress || a2dpConnecting || a2dpConnected || a2dpDisconnecting ||
      connectInProgress || sppOpen) {
    server.send(409, "text/plain", "Forget the current receiver first");
    return;
  }
  uint8_t selected[6];
  if (!parseMac(server.arg("mac"), selected)) {
    server.send(400, "text/plain", "Expected a Bluetooth MAC address");
    return;
  }
  memcpy(denonMac, selected, sizeof(denonMac));
  hasDenonMac = true;
  preferences.putString("denon_mac", macString());
  denonBondStateKnown = false;
  if (!refreshDenonBondState()) {
    server.send(503, "text/plain", "Could not read the receiver Bluetooth bond");
    return;
  }
  nextA2dpReconnectAt = 0;
  nextReconnectAt = 0;
  server.send(204);
}

void forgetFailed(int status, const char *message) {
  forgetInProgress = false;
  server.send(status, "text/plain", message);
}

void forgetDenon() {
  if (!requireProvisioningAuthorization()) return;
  if (forgetInProgress || connectInProgress) {
    server.send(409, "text/plain", "Wait for the current connection attempt to finish");
    return;
  }
  if (!hasDenonMac) {
    server.send(204);
    return;
  }
  if (!bluetoothReady) {
    server.send(503, "text/plain", "Bluetooth Classic is not ready; receiver was not forgotten");
    return;
  }
  forgetInProgress = true;
  if (sppOpen) {
    esp_spp_disconnect(sppHandle);
    if (!waitBt([] { return !sppOpen; }, kBondRemovalTimeoutMs)) {
      forgetFailed(409, "Could not disconnect the receiver; try again");
      return;
    }
  }
  if (a2dpConnecting || a2dpConnected || a2dpDisconnecting) {
    if (esp_a2d_source_disconnect(denonMac) != ESP_OK) {
      forgetFailed(409, "Could not disconnect the receiver; try again");
      return;
    }
    const unsigned long deadline = millis() + kBondRemovalTimeoutMs;
    while ((a2dpConnecting || a2dpConnected || a2dpDisconnecting) &&
           !timeReached(millis(), deadline)) {
      btPause(25);
    }
    if (a2dpConnecting || a2dpConnected || a2dpDisconnecting) {
      forgetFailed(504, "Bluetooth disconnect timed out; try again");
      return;
    }
  }

  BondState state = bondState(denonMac);
  if (state == BondState::Error) {
    forgetFailed(500, "Could not read Bluetooth bonds; receiver was not forgotten");
    return;
  }
  if (state == BondState::Present) {
    if (esp_bt_gap_remove_bond_device(denonMac) != ESP_OK) {
      forgetFailed(500, "Could not remove the Denon Bluetooth bond; receiver was not forgotten");
      return;
    }
    const unsigned long deadline = millis() + kBondRemovalTimeoutMs;
    do {
      btPause(25);
      state = bondState(denonMac);
      if (state == BondState::Error) {
        forgetFailed(500, "Could not verify Bluetooth bond removal; receiver was not forgotten");
        return;
      }
    } while (state == BondState::Present && !timeReached(millis(), deadline));
    if (state == BondState::Present) {
      forgetFailed(504, "Bluetooth bond removal timed out; try again");
      return;
    }
  }

  denonBonded = false;
  denonBondStateKnown = true;
  preferences.remove("denon_mac");
  clearPairingNumber();
  memset(denonMac, 0, sizeof(denonMac));
  hasDenonMac = false;
  volumeRaw = -1;
  server.send(202, "text/plain", "Restarting");
  restartDevice(true);
}

void saveWifi() {
  if (!currentRequestUsesSetupAp()) {
    server.send(403, "text/plain",
                "Wi-Fi provisioning is only available from the setup access point");
    return;
  }
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  String preferredText = server.arg("preferred_ip");
  preferredText.trim();
  if (ssid.isEmpty() || ssid.length() > 32) {
    server.send(400, "text/plain", "Wi-Fi name must contain 1 to 32 characters");
    return;
  }
  if ((!password.isEmpty() && password.length() < 8) || password.length() > 63) {
    server.send(400, "text/plain", "Wi-Fi password must be empty or contain 8 to 63 characters");
    return;
  }
  IPAddress preferred;
  if (!preferredText.isEmpty() &&
      (!parseIpv4(preferredText, preferred) || !isUnicastIpv4(preferred))) {
    server.send(400, "text/plain", "Preferred IP must be a complete unicast IPv4 address");
    return;
  }
  NetworkStorageState desired = readNetworkStorage();
  desired.present[kWifiSsidKey] = true;
  desired.value[kWifiSsidKey] = ssid;
  desired.present[kWifiPasswordKey] = true;
  desired.value[kWifiPasswordKey] = password;
  for (size_t i = kStaticIpKey; i <= kPendingIpKey; ++i) {
    desired.present[i] = false;
    desired.value[i] = "";
  }
  if (!preferredText.isEmpty()) {
    desired.present[kPendingIpKey] = true;
    desired.value[kPendingIpKey] = preferred.toString();
  }
  if (!commitNetworkStorage(desired)) {
    server.send(500, "text/plain",
                "Could not persist Wi-Fi settings; previous settings restored");
    return;
  }
  staticNetworkEnabled = false;
  server.send(202, "text/plain", "Restarting");
  restartDevice(true);
}

void saveNetwork() {
  if (!requireProvisioningAuthorization()) return;
  if (WiFi.status() != WL_CONNECTED) {
    server.send(409, "text/plain", "Connect with DHCP before setting a fixed IP");
    return;
  }
  IPAddress preferred;
  const IPAddress gateway = WiFi.gatewayIP();
  const IPAddress subnet = WiFi.subnetMask();
  const IPAddress dns = WiFi.dnsIP(0);
  if (!parseIpv4(server.arg("ip"), preferred) ||
      !validStaticNetwork(preferred, gateway, subnet, dns)) {
    server.send(400, "text/plain",
                "IP must be a complete usable IPv4 address on the current subnet");
    return;
  }
  if (!persistStaticNetwork(preferred, gateway, subnet, dns)) {
    server.send(500, "text/plain", "Could not persist fixed network settings");
    return;
  }
  server.send(202, "text/plain", "Restarting with fixed IP");
  restartDevice(true);
}

void clearNetwork() {
  if (!requireProvisioningAuthorization()) return;
  if (!clearStaticNetworkStorage()) {
    server.send(500, "text/plain",
                "Could not clear fixed network settings; previous settings restored");
    return;
  }
  server.send(202, "text/plain", "Restarting with DHCP");
  restartDevice(true);
}

// ---- OTA boot guard ----
// An uploaded image stays on trial until POST /api/firmware/confirm reaches it
// over the station network with the bearer token. The bootloader rolls back an
// unconfirmed image on its first failed boot; the "ota" NVS counter is the
// fallback if it does not, and the trial timeout restarts a silent image.

esp_ota_img_states_t runningImageState = ESP_OTA_IMG_UNDEFINED;

void readOtaImageState() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running || esp_ota_get_state_partition(running, &runningImageState) != ESP_OK) {
    runningImageState = ESP_OTA_IMG_UNDEFINED;
  }
  otaPendingVerify = runningImageState == ESP_OTA_IMG_PENDING_VERIFY;
  if (otaPendingVerify) otaBootState = "trial";
}

bool clearOtaTrial(const char *result) {
  const bool cleared = otaPreferences.remove("ota_trial") &&
                       otaPreferences.remove("ota_boots") &&
                       otaPreferences.putString("ota_last", result) > 0;
  if (cleared) {
    otaLastResult = result;
    otaTrialActive = false;
  }
  return cleared;
}

// Runs before the self-checks so a broken image still counts its boots.
void startOtaGuard() {
  readOtaImageState();
  if (!otaPreferences.begin("ota", false)) {
    Serial.println("Could not open OTA guard storage");
    return;
  }
  const esp_partition_t *running = esp_ota_get_running_partition();
  otaLastResult = otaPreferences.getString("ota_last", "");
  const String trial = otaPreferences.getString("ota_trial", "");
  const uint8_t previousBoots = otaPreferences.getUChar("ota_boots", 0);
  switch (otaBootAction(!trial.isEmpty(), running && trial == running->label,
                        previousBoots)) {
    case OtaBootAction::stable:
      return;
    case OtaBootAction::rolledBack:
      clearOtaTrial("rolled_back");
      otaBootState = "rolled_back";
      Serial.printf("Firmware in %s was not confirmed; bootloader rolled back\n",
                    trial.c_str());
      return;
    case OtaBootAction::trial:
      // Confirmed in otadata but the NVS clear did not land: reconcile.
      if (runningImageState == ESP_OTA_IMG_VALID) {
        otaTrialActive = true;
        clearOtaTrial("ok");
        otaBootState = "confirmed";
        return;
      }
      otaTrialBoots = previousBoots + 1;
      otaPreferences.putUChar("ota_boots", otaTrialBoots);
      otaTrialActive = true;
      otaBootState = "trial";
      Serial.printf("Firmware trial boot %u of %u in %s\n", otaTrialBoots,
                    kOtaMaxTrialBoots, running->label);
      return;
    case OtaBootAction::fallback: {
      const esp_partition_t *previous = esp_ota_get_next_update_partition(nullptr);
      // esp_ota_set_boot_partition verifies the previous image first.
      if (previous && esp_ota_set_boot_partition(previous) == ESP_OK) {
        clearOtaTrial("fallback");
        Serial.printf("Firmware in %s was never confirmed; booting %s\n",
                      running->label, previous->label);
        delay(100);
        esp_restart();
      }
      clearOtaTrial("fallback_failed");
      otaBootState = "fallback_failed";
      Serial.println("Previous firmware slot is not bootable; keeping this image");
      return;
    }
  }
}

void maintainOtaGuard() {
  // An unconfirmed image restarts, so the bootloader rollback or the trial
  // boot counter can return to the previous image.
  if ((otaTrialActive || otaPendingVerify) && otaTrialTimedOut(millis())) {
    Serial.println("Firmware trial was not confirmed; restarting");
    restartDevice(true);
  }
}

bool currentRequestUsesStation() {
  const IPAddress localAddress = server.client().localIP();
  return isStationIngress(WiFi.status() == WL_CONNECTED,
                          localAddress == WiFi.localIP(),
                          localAddress == WiFi.softAPIP());
}

void confirmFirmware() {
  if (!requireBackupAuthorization()) return;
  // Destination address alone is not a reliable ingress check, so the
  // setup access point must be down as well.
  if (setupApRunning || !currentRequestUsesStation()) {
    server.send(403, "text/plain",
                "Confirm firmware over the home network with the setup AP off");
    return;
  }
  if (!selfCheckPassed) {
    server.send(409, "text/plain", "Firmware self-check failed; not confirming");
    return;
  }
  if (!otaTrialActive && !otaPendingVerify) {
    server.send(409, "text/plain", "No unconfirmed firmware");
    return;
  }
  if (otaPendingVerify && esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    server.send(500, "text/plain", "Could not mark firmware valid");
    return;
  }
  otaPendingVerify = false;
  if (otaTrialActive && !clearOtaTrial("ok")) {
    server.send(500, "text/plain", "Could not clear the firmware trial record");
    return;
  }
  otaBootState = "confirmed";
  Serial.println("Firmware confirmed");
  server.send(200, "application/json", "{\"confirmed\":true}");
}

// ---- Firmware upload: POST /api/firmware, raw application/octet-stream ----

// Answers and closes the request socket, which also ends WebServer's body
// parsing, so a rejected upload is never read further.
void rejectFirmware(int status, const String &message) {
  if (Update.isRunning()) Update.abort();
  if (status == 401) server.sendHeader("WWW-Authenticate", "Bearer");
  server.send(status, "text/plain", message);
  server.requestClient().stop();
  Serial.printf("Firmware upload rejected: %s\n", message.c_str());
}

bool persistOtaTrial(const char *label) {
  return otaPreferences.putString("ota_trial", label) > 0 &&
         otaPreferences.putUChar("ota_boots", 0) > 0 &&
         otaPreferences.getString("ota_trial", "") == label &&
         otaPreferences.getUChar("ota_boots", 0xFF) == 0;
}

// Reads the whole body itself: reads are capped at the remaining declared
// length and the body has a total deadline.
void receiveFirmware() {
  // Token only: the setup access point never authorizes a firmware update.
  if (!hasAppAuthorization()) {
    rejectFirmware(401, "Missing or invalid bearer token");
    return;
  }
  // Writing the other slot now would overwrite the only rollback target.
  if (otaTrialActive || otaPendingVerify) {
    rejectFirmware(409, "Running firmware is not confirmed yet");
    return;
  }
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (!target) {
    rejectFirmware(503, "No update slot is available");
    return;
  }
  const long length = server.clientContentLength();
  if (!firmwareSizeAcceptable(length, target->size)) {
    rejectFirmware(413, "Image size must be " + String(kFirmwareHeaderBytes) +
                            " to " + String(target->size) + " bytes");
    return;
  }
  if (!Update.begin(static_cast<size_t>(length))) {
    rejectFirmware(500, String("Could not start update: ") + Update.errorString());
    return;
  }
  Serial.printf("Firmware upload started: %ld bytes to %s\n", length, target->label);
  WiFiClient &client = server.requestClient();
  static uint8_t chunk[1024];
  uint8_t header[kFirmwareHeaderBytes];
  size_t received = 0;
  const unsigned long deadline = millis() + kFirmwareBodyTimeoutMs;
  while (received < static_cast<size_t>(length)) {
    esp_task_wdt_reset();
    if (timeReached(millis(), deadline)) {
      rejectFirmware(408, "Upload took too long");
      return;
    }
    const size_t want = min(sizeof(chunk), static_cast<size_t>(length) - received);
    const int got = client.available() ? client.read(chunk, want) : 0;
    if (got <= 0) {
      if (!client.connected()) {
        rejectFirmware(400, "Upload aborted");
        return;
      }
      delay(1);
      continue;
    }
    if (received < kFirmwareHeaderBytes) {
      const size_t copy = min(static_cast<size_t>(got), kFirmwareHeaderBytes - received);
      memcpy(header + received, chunk, copy);
      if (received + copy == kFirmwareHeaderBytes &&
          !firmwareHeaderValid(header, sizeof(header))) {
        rejectFirmware(415, "Not an ESP32 application image");
        return;
      }
    }
    if (Update.write(chunk, got) != static_cast<size_t>(got)) {
      rejectFirmware(500, String("Flash write failed: ") + Update.errorString());
      return;
    }
    received += got;
  }
  // Arm the boot guard before the boot slot can change; refuse if it cannot.
  if (!persistOtaTrial(target->label)) {
    otaPreferences.remove("ota_trial");
    rejectFirmware(500, "Could not record the firmware trial");
    return;
  }
  // Update.end verifies the whole image (checksum and SHA-256) before it
  // switches the boot slot; on failure the running slot stays selected.
  if (!Update.end()) {
    otaPreferences.remove("ota_trial");
    otaPreferences.remove("ota_boots");
    rejectFirmware(422, String("Image verification failed: ") + Update.errorString());
    return;
  }
  Serial.printf("Firmware written to %s; restarting\n", target->label);
  server.send(202, "application/json",
              "{\"accepted\":true,\"slot\":\"" + String(target->label) +
                  "\",\"restarting\":true}");
  server.requestClient().stop();
  firmwareRestartPending = true;
}

// WebServer calls raw() only for non-multipart bodies and upload() only for
// multipart ones, so the body mode comes from its parser, not from headers.
class FirmwareHandler : public RequestHandler {
 public:
  bool canHandle(HTTPMethod method, String uri) override {
    return method == HTTP_POST && uri == "/api/firmware";
  }
  bool canUpload(String uri) override { return uri == "/api/firmware"; }
  bool canRaw(String uri) override { return uri == "/api/firmware"; }
  bool handle(WebServer &, HTTPMethod method, String uri) override {
    if (!canHandle(method, uri)) return false;
    // Reached only for a multipart body without a file part.
    server.send(415, "text/plain", "Send the image as application/octet-stream");
    return true;
  }
  void upload(WebServer &, String, HTTPUpload &upload) override {
    if (upload.status == UPLOAD_FILE_START) {
      rejectFirmware(415, "Send the image as application/octet-stream");
    }
  }
  void raw(WebServer &, String, HTTPRaw &raw) override {
    if (raw.status == RAW_START) receiveFirmware();
  }
};

// Streams the stored coredump in 1 KiB pieces; nothing is buffered beyond that.
void sendCoredump() {
  if (!requireBackupAuthorization()) return;
  size_t address = 0;
  size_t size = 0;
  if (esp_core_dump_image_get(&address, &size) != ESP_OK || size == 0) {
    server.send(404, "text/plain", "No coredump is stored");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  // Raw ESP-IDF flash coredump (ELF payload in its flash wrapper).
  server.sendHeader("Content-Disposition", "attachment; filename=coredump.bin");
  server.setContentLength(size);
  server.send(200, "application/octet-stream", "");
  static char chunk[1024];
  for (size_t offset = 0; offset < size;) {
    esp_task_wdt_reset();
    const size_t length = min(sizeof(chunk), size - offset);
    if (esp_flash_read(nullptr, chunk, address + offset, length) != ESP_OK ||
        !server.client().connected()) {
      server.requestClient().stop();
      return;
    }
    server.sendContent(chunk, length);
    offset += length;
  }
}

void eraseCoredump() {
  if (!requireBackupAuthorization()) return;
  if (esp_core_dump_image_erase() != ESP_OK) {
    server.send(500, "text/plain", "Could not erase the coredump");
    return;
  }
  server.send(204);
}

void restartApi() {
  if (!hasAppAuthorization()) {
    server.sendHeader("WWW-Authenticate", "Bearer");
    server.send(401, "text/plain", "Missing or invalid bearer token");
    return;
  }
  const String mode = server.arg("mode");
  if (mode != "clean" && mode != "abrupt") {
    server.send(400, "text/plain", "mode must be clean or abrupt");
    return;
  }
  Serial.printf("Restart requested (%s)\n", mode.c_str());
  server.send(202, "application/json",
              "{\"restarting\":true,\"mode\":\"" + mode + "\"}");
  // Abrupt skips the Bluetooth shutdown to simulate a crash.
  restartDevice(mode == "clean");
}

// Recovery routes (info, pairing, state, logs, firmware upload, restart,
// coredump) are always served; normal controls only after the self-checks.
void setupWeb(bool full) {
  const char *headers[] = {"Authorization", "If-Match", "Origin", "Host"};
  server.collectHeaders(headers, 4);
  server.on("/api/info", HTTP_GET, sendInfo);
  server.on("/api/pair", HTTP_POST, pairApi);
  server.on("/api/state", HTTP_GET, sendState);
  server.on("/api/logs", HTTP_GET, sendLogs);
  server.addHandler(new FirmwareHandler());
  server.on("/api/restart", HTTP_POST, restartApi);
  server.on("/api/coredump", HTTP_GET, sendCoredump);
  server.on("/api/coredump", HTTP_DELETE, eraseCoredump);
  if (!full) {
    server.begin();
    return;
  }
  server.on("/", HTTP_GET, sendPage);
  server.on("/api/unpair", HTTP_POST, unpairApi);
  server.on("/api/app", HTTP_POST, receiveApp);
  server.on("/api/apps", HTTP_GET, sendApps);
  server.on("/api/backup", HTTP_GET, sendBackup);
  server.on("/api/backup", HTTP_PUT, restoreBackup);
  server.on("/api/volume", HTTP_POST, sendTargetVolume);
  server.on("/api/volume/up", HTTP_POST,
            [] { sendVolume(kVolumeUp, sizeof(kVolumeUp), 1); });
  server.on("/api/volume/down", HTTP_POST,
            [] { sendVolume(kVolumeDown, sizeof(kVolumeDown), -1); });
  server.on("/api/denon/reconnect", HTTP_POST, reconnectDenon);
  server.on("/api/discover", HTTP_GET, discoverDenon);
  server.on("/api/denon", HTTP_POST, saveDenon);
  server.on("/api/denon", HTTP_DELETE, forgetDenon);
  server.on("/api/wifi", HTTP_POST, saveWifi);
  server.on("/api/network", HTTP_POST, saveNetwork);
  server.on("/api/network", HTTP_DELETE, clearNetwork);
  server.on("/api/firmware/confirm", HTTP_POST, confirmFirmware);
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302); });
  server.begin();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  networkLogs::begin();
  Serial.printf("Boot reset reason: %d\n", static_cast<int>(esp_reset_reason()));
  // Loop watchdog: longer than the longest legitimate block (8 s discovery).
  esp_task_wdt_init(kLoopWatchdogSeconds, true);
  esp_task_wdt_add(nullptr);
  startOtaGuard();
  // A set marker means the last boot died inside the Bluetooth lifecycle.
  const esp_reset_reason_t resetReason = esp_reset_reason();
  const bool btLifecycleInterrupted = resetReason != ESP_RST_POWERON &&
                                      rtcBtLifecycleMarker == kBtLifecycleMarker;
  // Kept armed through a deferred start, so a reset during the deferral
  // defers again; requestBtLifecycle() re-arms it when Bluetooth starts.
  if (resetReason == ESP_RST_POWERON) rtcBtLifecycleMarker = 0;
  pinMode(kBootButtonPin, INPUT_PULLUP);
  selfCheckPassed = protocolSelfCheck() && appStateSelfCheck() && jsonSelfCheck() &&
                    networkSelfCheck();
  preferences.begin("denon", false);
  loadDeviceIdentity();
  if (selfCheckPassed) {
    automaticRemuteRequired = preferences.getBool("remute", false);
    automaticRemuteJournalPersisted = automaticRemuteRequired;
    restoreLearningSuppressed = automaticRemuteRequired;
    loadAppVolumes();
    loadDenonMac();
  } else {
    // Keep Wi-Fi and the authenticated upload up so a fixed image can be
    // installed; Bluetooth, controls and firmware confirmation stay off.
    btLastError = "self_check_failed";
    Serial.println("Firmware self-check failed; recovery mode");
  }
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t info) {
        const int reason = info.wifi_sta_disconnected.reason;
        lastWifiDisconnectReason.store(reason, std::memory_order_relaxed);
        Serial.printf("Wi-Fi station disconnected, reason %d\n", reason);
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  startWifi();
  // HTTP (and so OTA) comes up before Bluetooth and never waits for it.
  setupWeb(selfCheckPassed);
  if (!selfCheckPassed) return;
  btEvents = xQueueCreate(kBtEventQueueLength, sizeof(BtEvent));
  btRx = xMessageBufferCreate(kBtRxBufferBytes);
  if (!btEvents || !btRx) {
    btLastError = "bt_queue_alloc_failed";
  } else if (btLifecycleInterrupted) {
    btLastError = "recovery_mode";
    nextStackRetryAt = millis() + btBootStartDelayMs(true);
    Serial.println("Last boot stopped in Bluetooth start; delaying Bluetooth");
  } else {
    requestBtLifecycle(false, "boot");
  }
}

void loop() {
  esp_task_wdt_reset();
  maintainTokenResetButton();
  // Receiver feedback (a mute report) is parsed before anything can send.
  if (bluetoothReady) readDenon();
  server.handleClient();
  if (setupApRunning) dnsServer.processNextRequest();
  if (firmwareRestartPending) restartDevice(true);
  maintainWifi();
  maintainOtaGuard();
  if (!selfCheckPassed) return;
  maintainBtLifecycle();  // consume the lifecycle result before any event
  if (bluetoothReady) readDenon();
  processBtEvents();
  maintainManualMuteLock();
  maintainAppSwitch();
  maintainVolumeRestore();
  maintainAppStorage();
  maintainDenon();
}
