// ESP8266 SNMP agent firmware — v1 + v2c, GET/GETNEXT/GETBULK/SET, traps.
// Engine: SNMP_Embedded v3.4.4 (vendored at lib/SNMP_Embedded, MIT).
// Device MIB: mibs/ESP8266-SNMP-MIB.mib (OIDs frozen, enterprise 99999).
// ADC samples internal supply (not A0) so espDeviceSupplyVoltage can be
// served; NodeMCU's external divider biases getVcc() LOW — indicative
// only, per the MIB. Macro must expand before the core reads it in init().
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <SNMP_Embedded.h>
#include <cstring>
#ifndef ESP_SNMP_NO_VCC
ADC_MODE(ADC_VCC)
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "changeme"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "changeme"
#endif

// Fixed NMS trap receivers; every trap goes to each. UDP/162.
static const IPAddress NMS_ADDRS[] = {
  IPAddress(10, 30, 30, 50),
  IPAddress(192, 168, 2, 211),
};
#define NMS_COUNT (sizeof(NMS_ADDRS) / sizeof(NMS_ADDRS[0]))

static WiFiUDP udp;
static SNMPAgent snmp("public", "private");

// ---------------- backing variables (library reads these at request time) ----

// RFC1213 system group
static const char sysDescr[] = "ESP8266 SNMP agent";
static char sysName[33] = "esp8266";
static char sysLocation[65] = "desk";
static int sysServices = 72;

// IF-MIB ifEntry.1 (WiFi adapter). ifPhysAddress uses a 6-byte buffer via
// addOpaqueHandler to carry the raw MAC as OCTET STRING.
static int ifNumber = 1;
static int ifIndex = 1;
static char ifDescr[10] = "WiFi STA";
static int ifType = 71;              // ieee80211
static int ifMtu = 1500;
static uint32_t ifSpeed = 54000000;
static uint8_t staMac[6];           // ifPhysAddress raw bytes
static int ifAdminStatus = 1;
static int ifOperStatus = 4;        // 1 up / 2 down / 4 dormant(associating)
static uint32_t ifLastChange = 0;   // ticks at last operStatus transition
// Counters below track the SNMP management plane. The platform's WiFi RX/TX
// runs inside the closed-source SDK blob and bypasses every observable stats
// hook (verified in-session by building instrumented lwIP three ways), so
// per-interface totals for non-SNMP traffic cannot be counted honestly.
static uint32_t ifInOctets = 0, ifInUcastPkts = 0, ifInDiscards = 0;
static uint32_t ifOutOctets = 0, ifOutUcastPkts = 0;
static uint32_t ifInNUcastPkts = 0, ifInErrors = 0, ifInUnknownProtos = 0;
static uint32_t ifOutNUcastPkts = 0, ifOutDiscards = 0, ifOutErrors = 0;
static uint32_t ifOutQLen = 0;

// enterprise 99999 device group
static const char* model = ARDUINO_BOARD;
static char firmware[8] = "1.0.0";
static const char* hardware = "ESP8266EX Xtensa LX106";
static uint32_t chipId = 0;
static uint32_t freeHeap = 0;
static uint32_t heapLimit = 16384;
static uint32_t maxFreeBlock = 0;
static uint32_t frag = 0;
static int ledState = 0;
static int resetRequest = 0;
static char resetReason[33] = "";
static const char* sdkVersion = "";   // assigned once at boot (getSdkVersion)
static char sdkVersionBuf[33];
static uint32_t cpuFreq = 80;
static uint32_t authFails = 0;
// CPU/memory extension (espDevice 15-21 per MIB)
static int cpuCores = 1;              // ESP8266EX single core
static uint32_t cpuUtilization = 0;   // loop-rate proxy, 0..100 %
static uint32_t supplyVoltage = 0;    // ESP.getVcc() mV (A0-divider biased)
static uint32_t processes = 1;        // NONOS: no scheduler, one image
static uint32_t heapTotal = 0;        // free heap snapshot at first loop
static uint32_t heapUtilization = 0;  // 100*(1-free/total)
static uint32_t ramTotal = 81920;     // ESP8266EX physical DRAM

// WiFi group
static uint32_t wifiState = 1;      // 0 down / 1 connecting / 2 up
static char ssid[33] = "";
static uint32_t channel = 0;
static int32_t rssi = 0;            // INTEGER (negative dBm)
static uint8_t bssidRaw[6];         // espWifiBssid, raw OCTET STRING
static IPAddress staIp;             // espWifiIpAddress (refreshed in loop)
static char ipStr[16] = "";          // dotted IP text for the console banner

// flash group
static uint32_t flashRealSize = 0, sketchSize = 0, sketchFree = 0;
static uint32_t flashChipSpeed = 0;

// ---------------- bookkeeping ------------------------------------------------

static uint8_t lastWifiState = 0xff;
static bool bootTrapSent = false;
static bool trapV1 = false;          // serial "trapv1"/"trapv2" toggle
static bool resetIsWarm = false;
static uint32_t lastAuthFailCount = 0;
static uint32_t lastAuthFailMs = 0;
static uint32_t prevPacketsReceived = 0, prevPacketsRejected = 0,
                prevMalformed = 0;
static bool lowHeapLatched = false;

// low-heap: one trap per downward crossing; re-arms at limit+1 KiB
static bool checkLowHeapTick() {
  if (freeHeap < heapLimit) {
    if (!lowHeapLatched) { lowHeapLatched = true; return true; }
  } else if (freeHeap > heapLimit + 1024) {
    lowHeapLatched = false;
  }
  return false;
}

// ---- custom callbacks (types their factories don't express) ---------------
// RawOctetCallback: fixed-length raw bytes as OCTET STRING (MAC addresses).
// Their StringCallback is NUL-terminated (breaks binary MACs) and
// OpaqueCallback hard-tags OPAQUE; IF-MIB ifPhysAddress needs a true
// OCTET STRING. Subclassing is the sanctioned extension point (their own
// Dynamic*Callback classes follow the same two-method pattern).
class RawOctetCallback: public ValueCallback {
  public:
    RawOctetCallback(SortableOIDType* oid, const uint8_t* value, size_t len)
        : ValueCallback(oid, STRING), value(value), len(len) {}
    const char* getAccessTag() const noexcept override { return "RO"; }
  protected:
    const uint8_t* const value;
    const size_t len;
    AsnPtr<BER_CONTAINER> buildTypeWithValueRaw() override {
      return AsnPtr<BER_CONTAINER>(asn_new<OctetType>((const char*)value, len));
    }
    SNMP_ERROR_STATUS setTypeWithValue(BER_CONTAINER*) override {
      return READ_ONLY;
    }
};

// ZeroOctetCallback: empty OCTET STRING — serves deprecated ifSpecific.
// Their OIDType rejects non-".1.3." values, so zeroDotZero (the RFC 2863
// value) cannot be encoded as OID here; an empty string keeps the column
// walkable. (ponytail: type-tolerated; ifSpecific is deprecated.)
class ZeroOctetCallback: public ValueCallback {
  public:
    ZeroOctetCallback(SortableOIDType* oid)
        : ValueCallback(oid, STRING) {}
    const char* getAccessTag() const noexcept override { return "RO"; }
  protected:
    AsnPtr<BER_CONTAINER> buildTypeWithValueRaw() override {
      return AsnPtr<BER_CONTAINER>(asn_new<OctetType>());
    }
    SNMP_ERROR_STATUS setTypeWithValue(BER_CONTAINER*) override {
      return READ_ONLY;
    }
};

// IpAddressCallback: espWifiIpAddress as a real IpAddress (application
// tag 0x40). Their factories have no IP handler; NetworkAddress(IPAddress)
// exists at the BER layer (v1 traps use it).
class IpAddressCallback: public ValueCallback {
  public:
    // pointer, not reference: caller keeps a stable IPAddress alive and
    // refreshed (WiFi.localIP() returns a fresh temporary each call —
    // a reference would dangle and encode garbage).
    IpAddressCallback(SortableOIDType* oid, const IPAddress* ip)
        : ValueCallback(oid, NETWORK_ADDRESS), ip(ip) {}
    const char* getAccessTag() const noexcept override { return "RO"; }
  protected:
    const IPAddress* const ip;
    AsnPtr<BER_CONTAINER> buildTypeWithValueRaw() override {
      return AsnPtr<BER_CONTAINER>(asn_new<NetworkAddress>(*ip));
    }
    SNMP_ERROR_STATUS setTypeWithValue(BER_CONTAINER*) override {
      return READ_ONLY;
    }
};

// ---------------- traps ------------------------------------------------------

// Static trap objects: no pool-backed state may survive between sends
// (ASNPool::resetAll() frees transient slots every tick — SNMPTrap.h v3.3.3).
static SNMPTrap bootTrap("public", SNMP_VERSION_2C);
static SNMPTrap lowHeapTrap("public", SNMP_VERSION_2C);
static SNMPTrap authTrap("public", SNMP_VERSION_2C);
static SNMPTrap bootTrapV1("public", SNMP_VERSION_1);
static SNMPTrap lowHeapTrapV1("public", SNMP_VERSION_1);
static SNMPTrap authTrapV1("public", SNMP_VERSION_1);

static void sendTrapToAll(SNMPTrap& v2t, SNMPTrap& v1t) {
  SNMPTrap& t = trapV1 ? v1t : v2t;
  t.setIP(WiFi.localIP());
  t.setUDP(&udp);
  for (size_t i = 0; i < NMS_COUNT; i++) {
    snmp.sendTrapTo(&t, NMS_ADDRS[i], true, 0, 30000);
  }
}


static void setupTrap(SNMPTrap& t, const char* snmpTrapOid, int v1Generic,
                      int v1Specific) {
  t.setTrapOID(new OIDType(snmpTrapOid));
  t.genericTrap = v1Generic;         // v1 generic trap number
  t.specificTrap = v1Specific;       // v1 specific (enterprise-only)
}

// ---------------- refresh: ESP APIs -> backing variables --------------------

static void refresh() {
  const uint32_t ms = millis();
  freeHeap = ESP.getFreeHeap();
  maxFreeBlock = ESP.getMaxFreeBlockSize();
  frag = ESP.getHeapFragmentation();
  chipId = ESP.getChipId();
  cpuFreq = ESP.getCpuFreqMHz();
  flashRealSize = ESP.getFlashChipRealSize();
  sketchSize = ESP.getSketchSize();
  sketchFree = ESP.getFreeSketchSpace();
  flashChipSpeed = ESP.getFlashChipSpeed();
  strlcpy(sdkVersionBuf, ESP.getSdkVersion(), sizeof(sdkVersionBuf));
  sdkVersion = sdkVersionBuf;

  // SNMP-plane counters from engine runtime stats (monotonic since boot):
  // in = datagrams accepted for processing, discards = malformed + rejected.
  SNMP_RuntimeStats st;
  snmp.getRuntimeStats(&st);
  if (st.packets_received > prevPacketsReceived) {
    ifInUcastPkts = st.packets_received;
    ifInDiscards = st.malformed_packets + st.packets_rejected;
    prevPacketsReceived = st.packets_received;
    prevPacketsRejected = st.packets_rejected;
    prevMalformed = st.malformed_packets;
  }
  // Octet totals — LOCAL engine patch: ASNPool::rxBytes/txBytes accumulate
  // exact SNMP-plane datagram bytes (both dispatch paths, both dialects).
  ifInOctets = (uint32_t)ASNPool::rxBytes;
  ifOutOctets = (uint32_t)ASNPool::txBytes;
  // Heap total: SDK has no heap "total"; free-at-first-loop is the defensible
  // snapshot. Utilization derived; voltage via getVcc (divider-biased, mV).
  if (heapTotal == 0 && freeHeap > 0) heapTotal = freeHeap;
  if (heapTotal) {
    heapUtilization = freeHeap >= heapTotal ? 0
        : (uint32_t)(100UL * (heapTotal - freeHeap) / heapTotal);
  }
#ifndef ESP_SNMP_NO_VCC
  supplyVoltage = ESP.getVcc();   // ADC_MODE VCC; NodeMCU A0 divider reads low
#endif

  // WiFi/interface state
  if (WiFi.status() == WL_CONNECTED) {
    const int prevOper = ifOperStatus;
    ifOperStatus = 1;                              // up
    if (prevOper != 1) ifLastChange = ms / 10;
    wifiState = 2;
    strlcpy(ssid, WiFi.SSID().c_str(), sizeof(ssid));
    rssi = WiFi.RSSI();
    channel = WiFi.channel();
    memcpy(bssidRaw, WiFi.BSSID(), 6);
    snprintf(ipStr, sizeof(ipStr), "%s", WiFi.localIP().toString().c_str());
    staIp = WiFi.localIP();
  } else {
    const int prevOper = ifOperStatus;
    const bool associating = (WiFi.status() == WL_IDLE_STATUS ||
                              WiFi.status() == WL_DISCONNECTED);
    ifOperStatus = associating ? 4 : 2;
    if (prevOper != ifOperStatus) ifLastChange = ms / 10;
    wifiState = associating ? 1 : 0;
    ssid[0] = 0; ipStr[0] = 0;
    memset(bssidRaw, 0, sizeof(bssidRaw));
  }

  // espDeviceReset: reboot when a SET wrote 1
  if (resetRequest == 1) {
    Serial.println("[SNMP] espDeviceReset: rebooting");
    delay(50);
    ESP.restart();
  }

  // LED: LED_BUILTIN is active-low on most boards; ledState is logical
  digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);
}

// ---------------- setup ------------------------------------------------------


void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  // boot classification, captured once
  strlcpy(resetReason, ESP.getResetReason().c_str(), sizeof(resetReason));
  resetIsWarm = strcmp(resetReason, "Software/System restart") == 0 ||
                strcmp(resetReason, "Deep-Sleep Wake") == 0;

  WiFi.macAddress(staMac);

  snmp.setUDP(&udp);
  snmp.begin();

  // ---- RFC1213 system group (auto-size form; sysContact skipped) ----
  snmp.addRFC1213SystemGroup(sysDescr, RFC1213_SKIP, sysName, sysLocation,
                             &sysServices);
  // sysObjectID: our enterprise root
  snmp.addOIDHandler(".1.3.6.1.2.1.1.2.0", ".1.3.6.1.4.1.99999");

  // ---- IF-MIB interfaces: ifNumber + ifEntry.1 ----------------------
  snmp.addIntegerHandler(".1.3.6.1.2.1.2.1.0", &ifNumber);
  snmp.addIntegerHandler(".1.3.6.1.2.1.2.2.1.1.1", &ifIndex);
  snmp.addReadOnlyStaticStringHandler(".1.3.6.1.2.1.2.2.1.2.1", ifDescr);
  snmp.addIntegerHandler(".1.3.6.1.2.1.2.2.1.3.1", &ifType);
  snmp.addIntegerHandler(".1.3.6.1.2.1.2.2.1.4.1", &ifMtu);
  snmp.addGaugeHandler(".1.3.6.1.2.1.2.2.1.5.1", &ifSpeed);
  snmp.addPrebuiltHandler(new RawOctetCallback(new SortableOIDType(".1.3.6.1.2.1.2.2.1.6.1"), staMac, 6));
  snmp.addIntegerHandler(".1.3.6.1.2.1.2.2.1.7.1", &ifAdminStatus);
  snmp.addIntegerHandler(".1.3.6.1.2.1.2.2.1.8.1", &ifOperStatus);
  snmp.addTimestampHandler(".1.3.6.1.2.1.2.2.1.9.1", &ifLastChange);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.10.1", &ifInOctets);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.11.1", &ifInUcastPkts);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.12.1", &ifInNUcastPkts);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.13.1", &ifInDiscards);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.14.1", &ifInErrors);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.15.1", &ifInUnknownProtos);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.16.1", &ifOutOctets);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.17.1", &ifOutUcastPkts);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.18.1", &ifOutNUcastPkts);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.19.1", &ifOutDiscards);
  snmp.addCounter32Handler(".1.3.6.1.2.1.2.2.1.20.1", &ifOutErrors);
  snmp.addGaugeHandler(".1.3.6.1.2.1.2.2.1.21.1", &ifOutQLen);
  snmp.addPrebuiltHandler(new ZeroOctetCallback(new SortableOIDType(".1.3.6.1.2.1.2.2.1.22.1")));

  // ---- enterprise device group --------------------------------------
  snmp.addReadOnlyStaticStringHandler(".1.3.6.1.4.1.99999.1.1.0", model);
  snmp.addReadOnlyStaticStringHandler(".1.3.6.1.4.1.99999.1.2.0", firmware);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.3.0", &chipId);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.4.0", &freeHeap);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.5.0", &heapLimit);      // RW gauge
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.6.0", &maxFreeBlock);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.7.0", &frag);
  snmp.addIntegerHandler(".1.3.6.1.4.1.99999.1.8.0", &ledState, true);   // RW
  snmp.addIntegerHandler(".1.3.6.1.4.1.99999.1.9.0", &resetRequest, true);
  {
    static char* reasonPtr = resetReason;
    snmp.addPrebuiltHandler(new StringCallback(
        new SortableOIDType(".1.3.6.1.4.1.99999.1.10.0"),
        &reasonPtr, sizeof(resetReason)));
  }
  snmp.addReadOnlyStaticStringHandler(".1.3.6.1.4.1.99999.1.11.0", hardware);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.12.0", &cpuFreq);
  {
    static char* sdkPtr = sdkVersionBuf;
    snmp.addReadWriteStringHandler(".1.3.6.1.4.1.99999.1.13.0", &sdkPtr, sizeof(sdkVersionBuf), false);
  }
  snmp.addCounter32Handler(".1.3.6.1.4.1.99999.1.14.0", &authFails);
  // CPU/memory extension (espDevice 15-21; see MIB DESCRIPTIONs for the
  // proxy semantics and the NONOS single-process truth)
  snmp.addIntegerHandler(".1.3.6.1.4.1.99999.1.15.0", &cpuCores);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.16.0", &cpuUtilization);
#ifndef ESP_SNMP_NO_VCC
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.17.0", &supplyVoltage);
#endif
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.18.0", &processes);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.19.0", &heapTotal);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.20.0", &heapUtilization);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.1.21.0", &ramTotal);

  // ---- WiFi group ---------------------------------------------------
  snmp.addIntegerHandler(".1.3.6.1.4.1.99999.2.1.0", (int*)&wifiState);
  {
    static char* ssidPtr = ssid;
    snmp.addReadWriteStringHandler(".1.3.6.1.4.1.99999.2.2.0", &ssidPtr, sizeof(ssid), false);
  }
  snmp.addIntegerHandler(".1.3.6.1.4.1.99999.2.3.0", (int*)&channel);
  snmp.addIntegerHandler(".1.3.6.1.4.1.99999.2.4.0", (int*)&rssi);
  snmp.addPrebuiltHandler(new RawOctetCallback(new SortableOIDType(".1.3.6.1.4.1.99999.2.5.0"), bssidRaw, 6));
  // espWifiIpAddress: our own static IPAddress, refreshed from
  // WiFi.localIP() in refresh() (their object is not stable storage).
  snmp.addPrebuiltHandler(new IpAddressCallback(
      new SortableOIDType(".1.3.6.1.4.1.99999.2.6.0"), &staIp));

  // ---- flash group ---------------------------------------------------
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.3.1.0", &flashRealSize);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.3.2.0", &sketchSize);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.3.3.0", &sketchFree);
  snmp.addGaugeHandler(".1.3.6.1.4.1.99999.3.4.0", &flashChipSpeed);

  snmp.sortHandlers();   // mandatory: enables GETNEXT walk order

  // ---- traps ---------------------------------------------------------
  // v2c dialect: snmpTrapOID set per trap; varbinds appended via values.
  setupTrap(bootTrap, ".1.3.6.1.6.3.1.1.5.1", 0, 0);      // coldStart (patched at send)
  setupTrap(bootTrapV1, ".1.3.6.1.4.1.99999", 0, 0);
  setupTrap(lowHeapTrap, ".1.3.6.1.4.1.99999.0.5", 6, 5); // enterpriseSpecific 5
  setupTrap(lowHeapTrapV1, ".1.3.6.1.4.1.99999", 6, 5);
  setupTrap(authTrap, ".1.3.6.1.6.3.1.1.5.5", 4, 0);      // authenticationFailure
  setupTrap(authTrapV1, ".1.3.6.1.6.3.1.1.5.5", 4, 0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ---------------- loop -------------------------------------------------------

void loop() {
  refresh();
  snmp.loop();

  // CPU utilization proxy: count loop() iterations per 1 s window; SYS work
  // (WiFi/MAC/SNMP) runs in the same context and steals iterations, so rate
  // loss tracks all-cause busy. Baseline = best rate ever observed.
  static uint32_t loopCount = 0, windowStart = 0, bestRate = 0;
  loopCount++;
  const uint32_t nowMs = millis();
  if (nowMs - windowStart >= 1000) {
    const uint32_t rate = loopCount * 1000UL / (nowMs - windowStart);
    if (rate > bestRate) bestRate = rate;   // ratchet: idle is the benchmark
    if (bestRate) {
      cpuUtilization = rate >= bestRate ? 0
          : (uint32_t)(100UL * (bestRate - rate) / bestRate);
    }
    loopCount = 0;
    windowStart = nowMs;
  }
  // wifi state transition logging (linkUp/Down traps intentionally omitted)
  if (wifiState != lastWifiState) {
    if (wifiState == 2) {
      Serial.printf("[SNMP] up at %s community=%s\n", ipStr, snmp._readOnlyCommunity);
    }
    lastWifiState = wifiState;
  }

  // boot trap once WiFi can carry UDP. coldStart: power-on, watchdog,
  // exception, external EN; warmStart: deliberate restart / deep-sleep.
  // Reboot cause travels in the espDeviceResetReason varbind (Cisco-style).
  if (!bootTrapSent && wifiState == 2) {
    bootTrapSent = true;
    const char* warmOid = ".1.3.6.1.6.3.1.1.5.2";
    if (!resetIsWarm) {
      // coldStart OID variant; static traps keep one OID each — swap in place
      bootTrap.setTrapOID(new OIDType(".1.3.6.1.6.3.1.1.5.1"));
      bootTrapV1.genericTrap = 0;
    } else {
      bootTrap.setTrapOID(new OIDType(warmOid));
      bootTrapV1.genericTrap = 1;
    }
    // reason varbind (enterprise 99999 espDeviceResetReason)
    static char* reasonPtr = resetReason;
    bootTrap.addOIDPointer(new StringCallback(new SortableOIDType(".1.3.6.1.4.1.99999.1.10.0"), &reasonPtr, sizeof(resetReason)));
    bootTrapV1.addOIDPointer(new StringCallback(new SortableOIDType(".1.3.6.1.4.1.99999.1.10.0"), &reasonPtr, sizeof(resetReason)));
    sendTrapToAll(bootTrap, bootTrapV1);
    Serial.printf("[SNMP] %s (%s) sent to %d NMS\n",
                  resetIsWarm ? "warmStart" : "coldStart", resetReason,
                  (int)NMS_COUNT);
  }

  // low-heap trap
  if (checkLowHeapTick()) {
    lowHeapTrap.addOIDPointer(new Gauge32Callback(new SortableOIDType(".1.3.6.1.4.1.99999.1.4.0"), &freeHeap));
    lowHeapTrap.addOIDPointer(new Gauge32Callback(new SortableOIDType(".1.3.6.1.4.1.99999.1.5.0"), &heapLimit));
    lowHeapTrapV1.addOIDPointer(new Gauge32Callback(new SortableOIDType(".1.3.6.1.4.1.99999.1.4.0"), &freeHeap));
    lowHeapTrapV1.addOIDPointer(new Gauge32Callback(new SortableOIDType(".1.3.6.1.4.1.99999.1.5.0"), &heapLimit));
    sendTrapToAll(lowHeapTrap, lowHeapTrapV1);
    Serial.printf("[SNMP] espTrapLowHeap heap=%u limit=%u\n", freeHeap, heapLimit);
  }

  // authentication-failure trap: fires once per new failure batch,
  // minimum 1 s apart (flood must not spray the NMS).
  if (authFails != lastAuthFailCount && millis() - lastAuthFailMs >= 1000) {
    lastAuthFailMs = millis();
    lastAuthFailCount = authFails;
    // varbind carries the cumulative bad-community count (espDeviceAuthFails)
    authTrap.addOIDPointer(new Counter32Callback(new SortableOIDType(".1.3.6.1.4.1.99999.1.14.0"), &authFails));
    authTrapV1.addOIDPointer(new Counter32Callback(new SortableOIDType(".1.3.6.1.4.1.99999.1.14.0"), &authFails));
    sendTrapToAll(authTrap, authTrapV1);
    Serial.printf("[SNMP] authFail trap, total=%u\n", (unsigned)authFails);
  }

  // serial commands: "loc <s>" sets sysLocation; "trapv1"/"trapv2" dialect
  if (Serial.available()) {
    char line[80];
    const int n = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    if (strncmp(line, "trapv1", 6) == 0) {
      trapV1 = true;
    } else if (strncmp(line, "trapv2", 6) == 0) {
      trapV1 = false;
    } else if (strncmp(line, "loc ", 4) == 0) {
      strlcpy(sysLocation, line + 4, sizeof(sysLocation));
    }
  }

  delay(2);   // yield to WiFi stack; UDP is polled, not interrupt-driven
}
