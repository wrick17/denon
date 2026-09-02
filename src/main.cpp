#include <Arduino.h>
#include <BluetoothSerial.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_a2dp_api.h>
#include <esp_gap_bt_api.h>
#include <esp_system.h>

#include "config.h"
#include "volume_target.h"

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
constexpr unsigned long kReconnectMs = 5000;
constexpr unsigned long kStatusIntervalMs = 5000;
constexpr unsigned long kBondRemovalTimeoutMs = 3000;
constexpr unsigned long kAppStableMs = 1500;
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

BluetoothSerial serialBt;
DNSServer dnsServer;
Preferences preferences;
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

const char kPage[] PROGMEM = R"HTML(
<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Denon volume</title><style>
body{font-family:system-ui,sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem;background:#111;color:#f4f4f4}main{background:#1d1d1d;border-radius:1rem;padding:1.25rem}button,input{box-sizing:border-box;font:inherit;border-radius:.65rem;padding:.8rem;border:0}button{background:#e9b949;color:#111;font-weight:700;min-width:5rem}button:disabled{opacity:.45}.row{display:flex;gap:.75rem;align-items:center;justify-content:center;margin:1rem 0}.volume{font-size:3.5rem;text-align:center}.muted{color:#aaa;font-size:.9rem;text-align:center}.mute-status{width:max-content;margin:.25rem auto 0;padding:.35rem .65rem;border-radius:99rem;background:#333;color:#ddd;font-weight:700}.mute-status.is-muted{background:#762727;color:#fff}.mute-status.is-unmuted{background:#194a34;color:#b8f5d4}.note{color:#bbb;font-size:.9rem;line-height:1.4}form{display:grid;gap:.6rem;margin-top:1rem}input{display:block;width:100%;margin-top:.25rem}#target{display:flex;align-items:center}#target label{white-space:nowrap}#target input{min-width:0;margin:0;flex:1}#target button{white-space:nowrap}#devices button{width:100%;margin-top:.4rem;text-align:left;background:#ddd;overflow-wrap:anywhere;white-space:normal}section{border-top:1px solid #444;margin-top:1.25rem;padding-top:.75rem}.secondary{background:#555;color:#fff;width:100%;margin-top:.6rem}.pairing{text-align:center;border:2px solid #e9b949;border-radius:.75rem;padding:1rem;margin-top:1rem}.pairing-number{font-size:3rem;font-weight:800;letter-spacing:.12em;margin:.4rem 0;color:#e9b949}.pairing-action{font-size:1.15rem;font-weight:700}table{width:100%;border-collapse:collapse;font-size:.9rem}th,td{text-align:left;padding:.55rem .3rem;border-bottom:1px solid #444}th:nth-child(n+2),td:nth-child(n+2){text-align:right}.active{color:#e9b949;font-weight:700}@media(max-width:22rem){#target{display:grid;grid-template-columns:1fr auto}#target label{grid-column:1/-1}#target input{width:100%}}
</style><main><h1>Denon volume</h1><p id=connection class=muted>Starting…</p><div id=volume class=volume>—</div><p id=db class=muted></p><p id=mute-status class=mute-status role=status aria-live=polite>Mute state unknown</p><div class=row><button onclick="send('down')" disabled>−</button><button onclick="send('up')" disabled>+</button></div><form id=target onsubmit=setTargetVolume(event)><label for=target-volume>Target volume</label><input id=target-volume type=number min=0 max=98 step=.5 required inputmode=decimal><button disabled>Set</button></form><p id=target-status class=note></p><section><h2>App volume memory</h2><p id=app-status class=note>Waiting for Home Assistant…</p><table><thead><tr><th>App</th><th>Volume</th><th>dB</th></tr></thead><tbody id=apps></tbody></table></section><section id=receiver><p class=note>On the Denon remote, hold <b>Bluetooth</b> for 3 seconds until Pairing appears, then select the receiver below. When the same six-digit number appears here and on the Denon, confirm it on the receiver. Set Bluetooth Auto-Select to Off if this control connection should not change inputs.</p><button id=scan onclick=scan()>Find receiver</button><div id=devices></div><div id=pairing-confirm class=pairing hidden><p>Confirm this number matches the Denon:</p><div id=pairing-number class=pairing-number></div><p class=pairing-action>Press ENTER on the Denon now</p><p class=note>The ESP32 has already accepted this number. No phone confirmation is needed.</p></div><p id=pairing-message class=note></p><button id=retry class=secondary onclick=retryReceiver() hidden>Retry pairing</button><button id=forget class=secondary onclick=forgetReceiver() hidden>Forget receiver</button></section><section id=wifi><form onsubmit=saveWifi(event)><label>Wi-Fi name<input id=ssid required maxlength=32 autocomplete=off></label><label>Wi-Fi password<input id=password type=password maxlength=63></label><label>Preferred IP address (optional)<input id=preferred-ip inputmode=decimal maxlength=15 placeholder="Leave blank for DHCP" autocomplete=off></label><button>Save Wi-Fi</button></form></section></main><script>
const q=s=>document.querySelector(s);let provisioning=false,targetRequest=null;const pairingMessages={waiting:'Waiting for the Denon pairing number…',confirm_on_denon:'Confirm the matching number on the Denon.',timed_out:'Pairing attempt ended. Put the Denon in pairing mode, then tap Retry pairing.'};async function api(url,opt){let r=await fetch(url,opt);if(!r.ok)throw Error(await r.text());return r.status===204?null:r.json()}async function send(dir){try{await api('/api/volume/'+dir,{method:'POST'});targetRequest=null;q('#target-status').textContent='';setTimeout(state,300)}catch(e){alert(e.message)}}async function setTargetVolume(e){e.preventDefault();let input=q('#target-volume');if(!input.reportValidity())return;let value=Number(input.value);q('#target-status').textContent='Setting volume to '+value.toFixed(1)+'…';try{let result=await api('/api/volume',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'volume='+encodeURIComponent(input.value)});targetRequest={value:value,id:result.target_id};state()}catch(e){targetRequest=null;q('#target-status').textContent=e.message}}async function state(){if(provisioning)return;try{let s=await api('/api/state');q('#connection').textContent=s.connected?'Receiver connected':s.connecting?'Connecting to receiver…':s.receiver_configured?'Receiver disconnected':'Select a receiver';q('#volume').textContent=s.volume===null?'—':s.volume;q('#db').textContent=s.volume_db===null?'':s.volume_db+' dB';let mute=q('#mute-status'),muteText=!s.mute_known?'Mute state unknown':s.muted?'Denon muted'+(s.manual_mute_lock?', automation locked':''):'Denon unmuted';if(mute.textContent!==muteText)mute.textContent=muteText;mute.classList.toggle('is-muted',s.mute_known&&s.muted);mute.classList.toggle('is-unmuted',s.mute_known&&!s.muted);document.querySelectorAll('.row button,#target input,#target button').forEach(b=>b.disabled=!s.connected||s.restoring);if(targetRequest!==null){if(s.volume_target_id!==targetRequest.id){q('#target-status').textContent='Volume change was interrupted';targetRequest=null}else if(s.restore_state==='error'){q('#target-status').textContent='Could not set volume: '+s.restore_error;targetRequest=null}else if(s.restoring){q('#target-status').textContent='Setting volume to '+targetRequest.value.toFixed(1)+'…'}else if(s.volume===targetRequest.value){q('#target-status').textContent='Volume set to '+targetRequest.value.toFixed(1);targetRequest=null}else{q('#target-status').textContent='Volume change was interrupted';targetRequest=null}}q('#scan').hidden=s.receiver_configured;q('#retry').hidden=!s.receiver_configured||s.connected||s.connecting;q('#retry').textContent=s.receiver_bonded?'Retry connection':'Retry pairing';q('#forget').hidden=!s.receiver_configured;q('#wifi').hidden=!s.setup_ap;let confirming=s.pairing_status==='confirm_on_denon'&&s.pairing_number;q('#pairing-confirm').hidden=!confirming;q('#pairing-number').textContent=confirming?s.pairing_number:'';q('#pairing-message').textContent=pairingMessages[s.pairing_status]||''}catch(e){q('#connection').textContent='ESP32 unavailable';let mute=q('#mute-status');mute.textContent='Mute state unavailable';mute.classList.remove('is-muted','is-unmuted')}}async function appTable(){if(provisioning)return;try{let j=await api('/api/apps');q('#apps').replaceChildren(...j.apps.map(x=>{let r=document.createElement('tr'),n=document.createElement('td'),v=document.createElement('td'),d=document.createElement('td'),name=x.app_name||x.app_id;if(name==='MemoryPoster'||name==='com.apple.IdleScreen.MemoryPoster')name='Screensaver';n.textContent=(x.active?'● ':'')+name;if(x.active)n.className='active';v.textContent=x.volume;d.textContent=x.volume_db;r.append(n,v,d);return r}));q('#app-status').textContent=j.restoring?'Setting volume…':j.apps.length?'Volumes update after manual changes.':'Waiting for Home Assistant…'}catch(e){q('#app-status').textContent='App memory unavailable'}}async function scan(){let d=q('#devices');d.textContent='Scanning Bluetooth for 8 seconds…';try{let j=await api('/api/discover');d.replaceChildren(...j.devices.map(x=>{let b=document.createElement('button');b.textContent=(x.name||'Bluetooth device')+' '+x.mac;b.onclick=()=>receiver(x.mac);return b}));if(!j.devices.length)d.textContent='No devices found. Check Denon pairing mode and try again.'}catch(e){d.textContent=e.message}}async function receiver(mac){try{await api('/api/denon',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mac='+encodeURIComponent(mac)});q('#devices').textContent='Saved. Connecting…';state()}catch(e){alert(e.message)}}async function retryReceiver(){try{await api('/api/denon/reconnect',{method:'POST'});state()}catch(e){alert(e.message)}}async function forgetReceiver(){if(!confirm('Forget the saved receiver?'))return;await fetch('/api/denon',{method:'DELETE'});q('#connection').textContent='Restarting…'}async function saveWifi(e){e.preventDefault();try{let r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(q('#ssid').value)+'&password='+encodeURIComponent(q('#password').value)+'&preferred_ip='+encodeURIComponent(q('#preferred-ip').value)});if(!r.ok)throw Error(await r.text());provisioning=true;q('#wifi').hidden=true;q('#connection').textContent='Wi-Fi saved. Rejoin your home Wi-Fi. Home Assistant will discover this ESP32.'}catch(e){alert(e.message)}}state();appTable();setInterval(()=>{state();appTable()},1000);
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
  delay(250);
  ESP.restart();
  return true;
}

void startWifi() {
  String ssid = preferences.getString("wifi_ssid", WIFI_SSID);
  String password = preferences.getString("wifi_password", WIFI_PASSWORD);
  if (ssid.isEmpty()) {
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
  Serial.printf("Connecting to Wi-Fi %s\n", ssid.c_str());
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
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
    if (mdnsRunning) {
      MDNS.end();
      mdnsRunning = false;
    }
  }
  if (!setupApRunning && millis() - wifiStartedAt >= kWifiTimeoutMs) startSetupAp();
}

bool sendDenon(const uint8_t *command, size_t length) {
  if (!serialBt.connected()) return false;
  return serialBt.write(command, length) == length;
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
  restoreFailureRaw = volumeRaw;
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
    const bool cooldownElapsed =
        timeReached(millis(), restoreLearningResumeAt);
    if (!cooldownElapsed) return;
    if (restoreFailureRaw < 0) {
      restoreFailureRaw = raw;
      return;
    }
    if (!canResumeLearningAfterFailure(cooldownElapsed, restoreFailureRaw,
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
      !serialBt.connected() || !sessionInitialized || !muteStateKnown) {
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
  if (!restoreSessionArmed || !serialBt.connected() || !sessionInitialized) {
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
    const uint8_t *command = direction > 0 ? kVolumeUp : kVolumeDown;
    const size_t length = direction > 0 ? sizeof(kVolumeUp)
                                         : sizeof(kVolumeDown);
    const int rapidClicks = volumeRapidClickCount(
        abs(restoreTargetRaw - volumeRaw), 0, 0);
    if (!sendDenon(command, length)) {
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
    if (sendExtension) {
      restoreBurstActive = true;
      restoreBurstStartRaw = restoreObservedRaw;
      restoreBurstClicksPlanned = 1;
      restoreBurstClicksSent = 0;
      restoreBurstExtensionPending = false;
    }
    if (!sendDenon(command, length)) {
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
  const uint8_t *command = direction > 0 ? kVolumeUp : kVolumeDown;
  const size_t length = direction > 0 ? sizeof(kVolumeUp) : sizeof(kVolumeDown);
  const int distanceRaw = abs(restoreTargetRaw - restoreObservedRaw);
  const int rapidClicks =
      restoreCoarseEnabled
          ? volumeRapidClickCount(distanceRaw, restoreBurstMeasuredDeltaRaw,
                                  restoreBurstMeasuredClicks)
          : 0;
  if (sendDenon(command, length)) {
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

void confirmDenonPairing(uint32_t number) {
  if (forgetInProgress || (!a2dpConnecting && !connectInProgress) ||
      !hasDenonMac) {
    if (hasDenonMac) esp_bt_gap_ssp_confirm_reply(denonMac, false);
    return;
  }
  if (esp_bt_gap_ssp_confirm_reply(denonMac, true) != ESP_OK) return;
  pairingNumber = number;
  pairingNumberReady = true;
}

int32_t provideSilentAudio(uint8_t *data, int32_t length) {
  if (length <= 0) return 0;
  memset(data, 0, static_cast<size_t>(length));
  return length;
}

void a2dpEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  if (event == ESP_A2D_PROF_STATE_EVT) {
    a2dpReady = param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS;
    if (a2dpReady) nextA2dpReconnectAt = 0;
    return;
  }
  if (event != ESP_A2D_CONNECTION_STATE_EVT) return;

  const bool target = isDenonAddress(param->conn_stat.remote_bda);
  if (!target) {
    if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING ||
        param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
      esp_a2d_source_disconnect(param->conn_stat.remote_bda);
    }
    return;
  }
  if (forgetInProgress &&
      (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING ||
       param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)) {
    a2dpConnecting = false;
    a2dpConnected = false;
    a2dpDisconnecting = true;
    clearPairingNumber();
    esp_a2d_source_disconnect(param->conn_stat.remote_bda);
    return;
  }

  switch (param->conn_stat.state) {
    case ESP_A2D_CONNECTION_STATE_CONNECTING:
      a2dpConnecting = true;
      a2dpDisconnecting = false;
      break;
    case ESP_A2D_CONNECTION_STATE_CONNECTED:
      a2dpConnecting = false;
      a2dpConnected = true;
      a2dpDisconnecting = false;
      pairingTimedOut = false;
      clearPairingNumber();
      nextReconnectAt = 0;
      Serial.println("Denon A2DP connected");
      break;
    case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
      a2dpConnecting = false;
      a2dpDisconnecting = true;
      break;
    case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
      pairingTimedOut = !forgetInProgress && a2dpConnecting;
      a2dpConnecting = false;
      a2dpConnected = false;
      a2dpDisconnecting = false;
      clearPairingNumber();
      nextA2dpReconnectAt = millis() + kReconnectMs;
      Serial.println("Denon A2DP disconnected");
      break;
  }
}

bool startA2dp() {
  if (esp_a2d_register_callback(a2dpEvent) != ESP_OK ||
      esp_a2d_source_register_data_callback(provideSilentAudio) != ESP_OK ||
      esp_a2d_source_init() != ESP_OK) {
    Serial.println("Could not start A2DP source profile");
    return false;
  }
  return true;
}

void startA2dpConnect() {
  if (!a2dpReady || !hasDenonMac || !denonBondStateKnown || denonBonded ||
      a2dpConnecting || a2dpConnected || a2dpDisconnecting ||
      connectInProgress || forgetInProgress) {
    return;
  }
  clearPairingNumber();
  pairingTimedOut = false;
  a2dpConnecting = true;
  if (esp_a2d_source_connect(denonMac) != ESP_OK) {
    a2dpConnecting = false;
    nextA2dpReconnectAt = millis() + kReconnectMs;
    Serial.println("Could not start Denon A2DP connection");
  }
}

void connectDenonTask(void *) {
  uint8_t address[6];
  memcpy(address, denonMac, sizeof(address));
  if ((!denonBonded && !a2dpConnected) || forgetInProgress ||
      !isDenonAddress(address)) {
    connectInProgress = false;
    vTaskDelete(nullptr);
    return;
  }
  Serial.printf("Connecting to Denon at %s\n", macString().c_str());

  // The captured Denon 500-series service uses RFCOMM channel 2. Connecting
  // directly also avoids the X580BT rejecting SDP before it is paired.
  const bool connected = serialBt.connect(
      address, 2, ESP_SPP_SEC_ENCRYPT | ESP_SPP_SEC_AUTHENTICATE,
      ESP_SPP_ROLE_MASTER);
  if (connected && !denonBonded) refreshDenonBondState();
  const bool targetStillConnected =
      (denonBonded || a2dpConnected) && !forgetInProgress;
  if (connected && !targetStillConnected) serialBt.disconnect();
  Serial.println(connected && targetStillConnected ? "Denon connected"
                                                    : "Denon connection failed");
  clearPairingNumber();
  nextReconnectAt = millis() + kReconnectMs;
  connectInProgress = false;
  vTaskDelete(nullptr);
}

void startDenonConnect() {
  if ((!denonBonded && !a2dpConnected) || connectInProgress ||
      forgetInProgress) {
    return;
  }
  if (a2dpConnected && !denonBonded) refreshDenonBondState();
  clearPairingNumber();
  pairingTimedOut = false;
  connectInProgress = true;
  if (xTaskCreate(connectDenonTask, "denon_connect", 4096, nullptr, 1, nullptr) !=
      pdPASS) {
    clearPairingNumber();
    connectInProgress = false;
    nextReconnectAt = millis() + kReconnectMs;
    Serial.println("Could not start Denon connection task");
  }
}

void maintainDenon() {
  const unsigned long now = millis();
  if (!bluetoothReady || !hasDenonMac || !denonBondStateKnown) return;

  const bool connected = serialBt.connected();
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
      if (!sessionInitialized) {
        sendDenon(kControlHandshake, sizeof(kControlHandshake));
        sendDenon(kGetSources, sizeof(kGetSources));
        sessionInitialized = true;
      }
      if (restoreTargetRaw < 0) sendDenon(kGetStatus, sizeof(kGetStatus));
      nextStatusAt = now + kStatusIntervalMs;
    }
    return;
  }

  if (wasDenonConnected) {
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
  static size_t length = 0;
  while (serialBt.available()) {
    if (length == sizeof(buffer)) {
      memmove(buffer, buffer + 1, --length);
    }
    buffer[length++] = static_cast<uint8_t>(serialBt.read());

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
      delay(100);
      ESP.restart();
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
          serialBt.connected(), sessionInitialized, volumeRaw, muteStateKnown,
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
  const bool connected = serialBt.connected();
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
          "\",\"hostname\":\"" + jsonEscape(hostName) + ".local\"}";
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
  if (!sendDenon(command, length)) {
    server.send(503, "text/plain", "Receiver is not connected");
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
  if (!serialBt.connected() || volumeRaw < 0) {
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
  if (serialBt.connected()) {
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
      connectInProgress || serialBt.connected()) {
    server.send(409, "text/plain", "A receiver connection is already active");
    return;
  }
  if (hasDenonMac) {
    server.send(409, "text/plain", "Forget the saved receiver before scanning again");
    return;
  }
  Serial.println("Bluetooth discovery started");
  BTScanResults *results = serialBt.discover(8000);
  String body = "{\"devices\":[";
  if (results) {
    for (int i = 0; i < results->getCount(); ++i) {
      BTAdvertisedDevice *device = results->getDevice(i);
      if (i) body += ',';
      body += "{\"name\":\"" + jsonEscape(String(device->getName().c_str())) + "\",\"mac\":\"" +
              device->getAddress().toString() + "\"}";
    }
  }
  body += "]}";
  serialBt.discoverClear();
  server.send(200, "application/json", body);
}

void saveDenon() {
  if (!requireProvisioningAuthorization()) return;
  if (forgetInProgress || a2dpConnecting || a2dpConnected || a2dpDisconnecting ||
      connectInProgress || serialBt.connected()) {
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
  if (serialBt.connected() && !serialBt.disconnect()) {
    forgetFailed(409, "Could not disconnect the receiver; try again");
    return;
  }
  if (a2dpConnecting || a2dpConnected || a2dpDisconnecting) {
    if (esp_a2d_source_disconnect(denonMac) != ESP_OK) {
      forgetFailed(409, "Could not disconnect the receiver; try again");
      return;
    }
    const unsigned long deadline = millis() + kBondRemovalTimeoutMs;
    while ((a2dpConnecting || a2dpConnected || a2dpDisconnecting) &&
           !timeReached(millis(), deadline)) {
      delay(25);
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
      delay(25);
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
  delay(250);
  ESP.restart();
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
  delay(250);
  ESP.restart();
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
  delay(250);
  ESP.restart();
}

void clearNetwork() {
  if (!requireProvisioningAuthorization()) return;
  if (!clearStaticNetworkStorage()) {
    server.send(500, "text/plain",
                "Could not clear fixed network settings; previous settings restored");
    return;
  }
  server.send(202, "text/plain", "Restarting with DHCP");
  delay(250);
  ESP.restart();
}

void setupWeb() {
  const char *headers[] = {"Authorization", "If-Match", "Origin", "Host"};
  server.collectHeaders(headers, 4);
  server.on("/", HTTP_GET, sendPage);
  server.on("/api/info", HTTP_GET, sendInfo);
  server.on("/api/pair", HTTP_POST, pairApi);
  server.on("/api/unpair", HTTP_POST, unpairApi);
  server.on("/api/app", HTTP_POST, receiveApp);
  server.on("/api/apps", HTTP_GET, sendApps);
  server.on("/api/backup", HTTP_GET, sendBackup);
  server.on("/api/backup", HTTP_PUT, restoreBackup);
  server.on("/api/state", HTTP_GET, sendState);
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
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302); });
  server.begin();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kBootButtonPin, INPUT_PULLUP);
  if (!protocolSelfCheck() || !appStateSelfCheck() || !jsonSelfCheck() ||
      !networkSelfCheck()) {
    Serial.println("Firmware self-check failed");
    return;
  }
  preferences.begin("denon", false);
  automaticRemuteRequired = preferences.getBool("remute", false);
  automaticRemuteJournalPersisted = automaticRemuteRequired;
  restoreLearningSuppressed = automaticRemuteRequired;
  loadDeviceIdentity();
  loadAppVolumes();
  loadDenonMac();
  startWifi();
  serialBt.enableSSP();
  serialBt.onConfirmRequest(confirmDenonPairing);
  serialBt.setPin("0000");
  bluetoothReady = serialBt.begin("Denon ESP32", true);
  if (!bluetoothReady) {
    Serial.println("Bluetooth Classic failed to start");
  } else {
    if (!refreshDenonBondState() || !startA2dp()) bluetoothReady = false;
  }
  setupWeb();
}

void loop() {
  maintainTokenResetButton();
  server.handleClient();
  if (setupApRunning) dnsServer.processNextRequest();
  maintainWifi();
  if (bluetoothReady) readDenon();
  maintainManualMuteLock();
  maintainAppSwitch();
  maintainVolumeRestore();
  maintainAppStorage();
  maintainDenon();
}
