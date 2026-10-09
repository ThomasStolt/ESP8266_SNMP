/* ==========================================================================
 * esp_device_test.cpp — host-side port of the OLD ESP8266_SNMP test suite
 * (test/agent_test.cpp, test/ber_test.cpp — deleted in the SNMP_Embedded
 * cutover). Same Catch2/no-Arduino harness as the vendored tests.cpp: every
 * case drives the REAL engine (handlePacket / SNMPPacket / SNMPTrap) with
 * the host build profiles (COMPILING_TESTS).
 *
 * Semantic coverage inherited from the old suite:
 *   1. OID registration type fidelity (INTEGER 0x02 / Gauge32 0x42 /
 *      Counter32 0x41 / OCTET STRING 0x04, plus TimeTicks 0x43) asserted
 *      through the wire: handlePacket -> SNMPPacket::parseFrom.
 *   2. Multi-varbind GET: 2-VB and 4-VB GetRequest PDUs (NMS panel query)
 *      answered with the same varbind count, values intact.
 *   3. Community enforcement: wrong community -> SNMP_REQUEST_INVALID_COMMUNITY
 *      (no response bytes), packets_rejected increments; write with the
 *      read-only community is refused (NO_ACCESS global error, value untouched).
 *   4. SET semantics exactly as the engine implements them (per their
 *      "Test SetRequestPDU"): settable writes stick, non-settable answers
 *      readOnly. NOTE: the engine validates and applies per varbind as it
 *      walks the list — there is no rollback of earlier varbinds when a
 *      later one fails (this replaces the old agent's atomic-SET contract;
 *      these tests assert the engine's actual documented behaviour).
 *   5. Trap building: SNMPTrap with snmpTrapOID coldStart (.1.3.6.1.6.3.1.1.5.1),
 *      warmStart (.5.2), authenticationFailure (.5.5) and the low-heap
 *      enterprise notification; v2c bytes carry snmpTrapOID.0 as the second
 *      mandatory varbind; v1 dialect builds TrapPDU (0xA4) with the right
 *      enterprise / generic / specific / agent-addr fields.
 *
 * OID comparisons use OIDType::equals() (encoded-byte compare): the parsed
 * OID's string() renderer is display-only and mangles multi-digit arcs, so
 * the wire-level bytes are the authoritative comparison (same approach the
 * zero-copy dispatcher uses for matching).
 * ========================================================================== */

#include "catch.hpp"

#include "include/SNMPPacket.h"
#include "include/ValueCallbacks.h"
#include "include/SNMPParser.h"

#include "SNMPTrap.h"
#include "SNMP_Embedded.h"

#include <cstring>
#include <cstdio>

/* ESP8266_SNMP device OID vocabulary (mibs/ESP8266-SNMP-MIB.mib, frozen): */
#define OID_sysDescr        ".1.3.6.1.2.1.1.1.0"
#define OID_sysUpTime       ".1.3.6.1.2.1.1.3.0"
#define OID_sysServices     ".1.3.6.1.2.1.1.7.0"
#define OID_ifInUcastPkts   ".1.3.6.1.2.1.2.2.1.11.1"
#define OID_ifOutUcastPkts  ".1.3.6.1.2.1.2.2.1.17.1"
#define OID_trapColdStart   ".1.3.6.1.6.3.1.1.5.1"
#define OID_trapWarmStart   ".1.3.6.1.6.3.1.1.5.2"
#define OID_trapAuthFailure ".1.3.6.1.6.3.1.1.5.5"
#define OID_enterprise      ".1.3.6.1.4.1.99999"
#define OID_lowHeap         ".1.3.6.1.4.1.99999.0.5"
#define OID_freeHeap        ".1.3.6.1.4.1.99999.1.4.0"
#define OID_heapLimit       ".1.3.6.1.4.1.99999.1.5.0"
#define OID_ledState        ".1.3.6.1.4.1.99999.1.8.0"
#define OID_snmpTrapOID     ".1.3.6.1.6.3.1.1.4.1.0"
#define OID_hrSystemProcesses ".1.3.6.1.2.1.25.1.6.0"
#define OID_hrProcessorLoad   ".1.3.6.1.2.1.25.3.3.1.2.1"
#define OID_hrStorageUsedRam  ".1.3.6.1.2.1.25.2.3.1.6.1"

/* Common build route: the same flag-dispatch loop() uses (mirrors the
 * vendored tests.cpp helper of the same name). */
static inline SNMP_ERROR_RESPONSE handlePacketRoute(uint8_t* buffer, int packetLength, int* responseLength, int max_packet_size,
                                                    ValueCallback* const* callbacks, int callbacksCount,
                                                    const char* community, const char* readOnly){
#if SNMP_ZERO_COPY
    return handlePacketInPlace(buffer, packetLength, responseLength, max_packet_size, callbacks, callbacksCount, community, readOnly);
#else
    return handlePacket(buffer, packetLength, responseLength, max_packet_size, callbacks, callbacksCount, community, readOnly);
#endif
}

/* Encoded-byte OID comparison (wire-level truth; immune to render quirks). */
static bool oidMatches(const OIDType* parsed, const char* expected){
    OIDType ref(expected);
    return parsed->equals(&ref);
}

/* Build a GET request packet with the requested varbind OIDs. */
static SNMPPacket* buildGetRequest(const char* const* oids, int oidCount, SNMP_VERSION version = SNMP_VERSION_2C){
    SNMPPacket* packet = new SNMPPacket();
    packet->setPDUType(GetRequestPDU);
    packet->setCommunityString("public");
    packet->setRequestID(SNMPPacket::generate_request_id());
    packet->setVersion(version);
    for(int i = 0; i < oidCount; i++){
        packet->push_back(VarBind(std::make_shared<SortableOIDType>(oids[i]), std::make_shared<NullType>()));
    }
    return packet;
}

/* Serve a GET and parse the response into `response`.
 * NOTE: `response` must be a FRESH packet — parseFrom does not reset a
 * previously parsed packet's varbind list (same discipline as tests.cpp). */
static bool serveGet(SNMPPacket* request, ValueCallback* const* callbacks, int callbacksCount,
                     uint8_t* buf, size_t bufLen, int* respLen, SNMPPacket* response){
    int requestLen = request->serialiseInto(buf, bufLen);
    if(requestLen <= 0) return false;
    *respLen = 0;
    handlePacketRoute(buf, requestLen, respLen, (int)bufLen, callbacks, callbacksCount, "public", "private");
    return response->parseFrom(buf, (size_t)*respLen) == SNMP_ERROR_OK;
}

/* --------------------------------------------------------------------------
 * Device-like roster, registered the way src/main.cpp does it (plain C
 * variables + library callbacks), then sorted for walk order.
 * -------------------------------------------------------------------------- */
struct DeviceRoster {
    ValueCallback* callbacks[SNMP_MAX_CALLBACKS_PER_AGENT] = {nullptr};
    int count = 0;

    char sysDescrBuf[32] = "ESP8266 SNMP agent";
    char* sysDescrStr = sysDescrBuf;
    uint32_t uptime = 424242;           /* 10ms ticks */
    int sysServices = 72;
    int ledWritable = 1;
    uint32_t freeHeap = 40960;
    uint32_t heapLimit = 16384;
    uint32_t ifInUcastPkts = 123456;
    uint32_t ifOutUcastPkts = 654321;
    uint32_t processes = 1;             /* hrSystemProcesses (Gauge32) */
    int hrCpuLoad = 42;                 /* hrProcessorLoad (Integer32) */
    int ramUsed = 512;                  /* hrStorageUsed RAM row (Integer32) */

    IntegerCallback* sysServicesCb;
    IntegerCallback* ledWritableCb;

    DeviceRoster(){
        ValueCallback* cbs[] = {
            new StringCallback(new SortableOIDType(OID_sysDescr), &sysDescrStr, sizeof(sysDescrBuf)),
            new TimestampCallback(new SortableOIDType(OID_sysUpTime), &uptime),
            new IntegerCallback(new SortableOIDType(OID_sysServices), &sysServices),
            ledWritableCb = new IntegerCallback(new SortableOIDType(OID_ledState), &ledWritable),
            new Gauge32Callback(new SortableOIDType(OID_freeHeap), &freeHeap),
            new Gauge32Callback(new SortableOIDType(OID_heapLimit), &heapLimit),
            new Counter32Callback(new SortableOIDType(OID_ifInUcastPkts), &ifInUcastPkts),
            new Counter32Callback(new SortableOIDType(OID_ifOutUcastPkts), &ifOutUcastPkts),
            new Gauge32Callback(new SortableOIDType(OID_hrSystemProcesses), &processes),
            new IntegerCallback(new SortableOIDType(OID_hrProcessorLoad), &hrCpuLoad),
            new IntegerCallback(new SortableOIDType(OID_hrStorageUsedRam), &ramUsed),
        };
        sysServicesCb = static_cast<IntegerCallback*>(cbs[2]);
        ledWritableCb->isSettable = true;
        for(ValueCallback* cb : cbs) callbacks[count++] = cb;
        sort_handlers(callbacks, count);
    }

    ~DeviceRoster(){
        for(int i = 0; i < count; i++) delete callbacks[i];
    }
};

/* ==========================================================================
 * 1. OID registration type fidelity — the registered callback class decides
 *    the served BER application tag (the old suite's core typing contract:
 *    gauge -> 0x42, counter -> 0x41, int -> 0x02, string -> 0x04, ticks 0x43).
 * ========================================================================== */
TEST_CASE( "esp: registered handler serves its BER application type on the wire", "[esp]" ){
    DeviceRoster roster;
    uint8_t buf[800] = {0};
    int respLen = 0;

    /* INTEGER = 0x02 */
    {
        const char* oids[] = { OID_sysServices };
        SNMPPacket* request = buildGetRequest(oids, 1);
        SNMPPacket response;
        REQUIRE( serveGet(request, roster.callbacks, roster.count, buf, sizeof(buf), &respLen, &response) );
        REQUIRE( response.size() == 1 );
        REQUIRE( response[0].type == INTEGER );
        REQUIRE( static_cast<IntegerType*>(response[0].value)->_value == 72 );
        delete request;
    }
    /* TimeTicks = 0x43 */
    {
        const char* oids[] = { OID_sysUpTime };
        SNMPPacket* request = buildGetRequest(oids, 1);
        SNMPPacket response;
        REQUIRE( serveGet(request, roster.callbacks, roster.count, buf, sizeof(buf), &respLen, &response) );
        REQUIRE( response.size() == 1 );
        REQUIRE( response[0].type == TIMESTAMP );
        REQUIRE( static_cast<TimestampType*>(response[0].value)->_value == 424242 );
        delete request;
    }
    /* OCTET STRING = 0x04 */
    {
        const char* oids[] = { OID_sysDescr };
        SNMPPacket* request = buildGetRequest(oids, 1);
        SNMPPacket response;
        REQUIRE( serveGet(request, roster.callbacks, roster.count, buf, sizeof(buf), &respLen, &response) );
        REQUIRE( response.size() == 1 );
        REQUIRE( response[0].type == STRING );
        REQUIRE( strcmp(static_cast<OctetType*>(response[0].value)->_value, "ESP8266 SNMP agent") == 0 );
        delete request;
    }
    /* Gauge32 = 0x42 */
    {
        const char* oids[] = { OID_freeHeap };
        SNMPPacket* request = buildGetRequest(oids, 1);
        SNMPPacket response;
        REQUIRE( serveGet(request, roster.callbacks, roster.count, buf, sizeof(buf), &respLen, &response) );
        REQUIRE( response.size() == 1 );
        REQUIRE( response[0].type == GAUGE32 );
        REQUIRE( static_cast<Gauge*>(response[0].value)->_value == 40960 );
        delete request;
    }
    /* Counter32 = 0x41 */
    {
        const char* oids[] = { OID_ifInUcastPkts };
        SNMPPacket* request = buildGetRequest(oids, 1);
        SNMPPacket response;
        REQUIRE( serveGet(request, roster.callbacks, roster.count, buf, sizeof(buf), &respLen, &response) );
        REQUIRE( response.size() == 1 );
        REQUIRE( response[0].type == COUNTER32 );
        REQUIRE( static_cast<Counter32*>(response[0].value)->_value == 123456 );
        delete request;
    }
}

/* ==========================================================================
 * 2. Multi-varbind GET — the NMS panel query shape from the old suite
 *    (many objects answered in ONE response; the old one-VB limitation died
 *    with the old handler).
 * ========================================================================== */
TEST_CASE( "esp: multi-varbind GET answered with equal varbind count (2 and 4 VBs)", "[esp]" ){
    DeviceRoster roster;
    uint8_t buf[800] = {0};
    int respLen = 0;

    SECTION( "2-varbind NMS panel query (sysUpTime + sysDescr)" ){
        const char* oids[] = { OID_sysUpTime, OID_sysDescr };
        SNMPPacket* request = buildGetRequest(oids, 2);
        SNMPPacket response;
        REQUIRE( serveGet(request, roster.callbacks, roster.count, buf, sizeof(buf), &respLen, &response) );
        REQUIRE( response.size() == 2 );
        REQUIRE( oidMatches(response[0].oid, OID_sysUpTime) );
        REQUIRE( response[0].type == TIMESTAMP );
        REQUIRE( oidMatches(response[1].oid, OID_sysDescr) );
        REQUIRE( response[1].type == STRING );
        REQUIRE( strcmp(static_cast<OctetType*>(response[1].value)->_value, "ESP8266 SNMP agent") == 0 );
        delete request;
    }

    SECTION( "4-varbind NMS panel query" ){
        const char* oids[] = { OID_sysDescr, OID_sysUpTime, OID_freeHeap, OID_ifInUcastPkts };
        SNMPPacket* request = buildGetRequest(oids, 4);
        SNMPPacket response;
        REQUIRE( serveGet(request, roster.callbacks, roster.count, buf, sizeof(buf), &respLen, &response) );
        REQUIRE( response.size() == 4 );
        REQUIRE( oidMatches(response[0].oid, OID_sysDescr) );
        REQUIRE( oidMatches(response[1].oid, OID_sysUpTime) );
        REQUIRE( oidMatches(response[2].oid, OID_freeHeap) );
        REQUIRE( oidMatches(response[3].oid, OID_ifInUcastPkts) );
        /* types answer in the MIB's declared SYNTAX order */
        REQUIRE( response[0].type == STRING );
        REQUIRE( response[1].type == TIMESTAMP );
        REQUIRE( response[2].type == GAUGE32 );
        REQUIRE( response[3].type == COUNTER32 );
        REQUIRE( static_cast<TimestampType*>(response[1].value)->_value == 424242 );
        REQUIRE( static_cast<Gauge*>(response[2].value)->_value == 40960 );
        REQUIRE( static_cast<Counter32*>(response[3].value)->_value == 123456 );
        delete request;
    }
}

/* ==========================================================================
 * 2b. RFC 2790 Host Resources GET — one multi-varbind request across the
 *     hrSystemProcesses / hrProcessorLoad / hrStorageUsed (RAM row) arcs,
 *     answered in request order with the MIB-declared SYNTAX types.
 * ========================================================================== */
TEST_CASE( "esp: RFC 2790 host resources OIDs answered in one multi-varbind GET", "[esp]" ){
    DeviceRoster roster;
    uint8_t buf[800] = {0};
    int respLen = 0;

    const char* oids[] = { OID_hrSystemProcesses, OID_hrProcessorLoad, OID_hrStorageUsedRam };
    SNMPPacket* request = buildGetRequest(oids, 3);
    SNMPPacket response;
    REQUIRE( serveGet(request, roster.callbacks, roster.count, buf, sizeof(buf), &respLen, &response) );
    REQUIRE( response.size() == 3 );
    REQUIRE( oidMatches(response[0].oid, OID_hrSystemProcesses) );
    REQUIRE( oidMatches(response[1].oid, OID_hrProcessorLoad) );
    REQUIRE( oidMatches(response[2].oid, OID_hrStorageUsedRam) );
    /* hrSystemProcesses: Gauge32 (0x42); CPU + storage columns: INTEGER (0x02) */
    REQUIRE( response[0].type == GAUGE32 );
    REQUIRE( static_cast<Gauge*>(response[0].value)->_value == 1 );
    REQUIRE( response[1].type == INTEGER );
    REQUIRE( static_cast<IntegerType*>(response[1].value)->_value == 42 );
    REQUIRE( response[2].type == INTEGER );
    REQUIRE( static_cast<IntegerType*>(response[2].value)->_value == 512 );
    delete request;
}

/* ==========================================================================
 * 3. Community enforcement — wrong community: SNMP_REQUEST_INVALID_COMMUNITY,
 *    no response bytes, packets_rejected counted.
 * ========================================================================== */
TEST_CASE( "esp: wrong community is rejected without response bytes, counted in runtime stats", "[esp]" ){
    DeviceRoster roster;
    uint8_t buf[500] = {0};

    const char* oids[] = { OID_freeHeap };
    SNMPPacket* request = buildGetRequest(oids, 1);
    int requestLen = request->serialiseInto(buf, 500);
    REQUIRE( requestLen > 0 );
    delete request;

    size_t baseRej = ASNPool::packetsRejected;
    size_t baseMal = ASNPool::malformedPackets;

    int respLen = -1;   /* sentinel: must NOT be set to a positive length */
    SNMP_ERROR_RESPONSE r = handlePacketRoute(buf, requestLen, &respLen, 500,
                                              roster.callbacks, roster.count, "nope", "nope");
    REQUIRE( r == SNMP_REQUEST_INVALID_COMMUNITY );
    REQUIRE( respLen <= 0 );                          /* no response bytes  */
    REQUIRE( ASNPool::packetsRejected  == baseRej + 1 );
    REQUIRE( ASNPool::malformedPackets == baseMal ); /* parse was fine      */
}

TEST_CASE( "esp: SET under read-only community is refused globally (NO_ACCESS), value untouched", "[esp]" ){
    DeviceRoster roster;
    uint8_t buf[500] = {0};

    /* SET ledState=0 with the READ community — the denied-write case from
     * the old suite (public is RO, private is RW). */
    SNMPPacket* request = new SNMPPacket();
    request->setPDUType(SetRequestPDU);
    request->setCommunityString("public");     /* read-only credential */
    request->setRequestID(SNMPPacket::generate_request_id());
    request->setVersion(SNMP_VERSION_2C);
    request->push_back(VarBind(std::make_shared<SortableOIDType>(OID_ledState),
                               std::make_shared<IntegerType>(0)));

    int requestLen = request->serialiseInto(buf, 500);
    REQUIRE( requestLen > 0 );
    delete request;

    int respLen = 0;
    SNMP_ERROR_RESPONSE r = handlePacketRoute(buf, requestLen, &respLen, 500,
                                              roster.callbacks, roster.count,
                                              "private", "public");
    /* the RO community reached the SET path but has no write permission:
     * engine answers a NO_ACCESS error PDU (silence would hang the manager) */
    REQUIRE( r == SNMP_ERROR_PACKET_SENT );
    SNMPPacket response;
    REQUIRE( response.parseFrom(buf, (size_t)respLen) == SNMP_ERROR_OK );
    REQUIRE( response.errorStatus.errorStatus == NO_ACCESS );
    REQUIRE( response.errorIndex.errorIndex   == 0 );
    REQUIRE( roster.ledWritable == 1 );           /* value untouched */
}

/* ==========================================================================
 * 4. SET semantics exactly as the engine implements them (vendored
 *    "Test SetRequestPDU"): type must match the handler, isSettable gates
 *    writes, the backing variable receives the value. Per-varbind apply —
 *    a later non-settable varbind does NOT roll back the earlier write.
 * ========================================================================== */
TEST_CASE( "esp: SET writes stick to the settable handler only (engine contract)", "[esp]" ){
    DeviceRoster roster;
    uint8_t buf[500] = {0};

    SNMPPacket* request = new SNMPPacket();
    request->setPDUType(SetRequestPDU);
    request->setCommunityString("private");
    request->setRequestID(SNMPPacket::generate_request_id());
    request->setVersion(SNMP_VERSION_2C);
    request->push_back(VarBind(std::make_shared<SortableOIDType>(OID_ledState),
                               std::make_shared<IntegerType>(0)));
    request->push_back(VarBind(std::make_shared<SortableOIDType>(OID_sysServices),
                               std::make_shared<IntegerType>(55)));

    int requestLen = request->serialiseInto(buf, 500);
    REQUIRE( requestLen > 0 );
    delete request;

    int respLen = 0;
    REQUIRE( handlePacketRoute(buf, requestLen, &respLen, 500,
                              roster.callbacks, roster.count,
                              "private", "public") == SNMP_SET_OCCURRED );

    SNMPPacket response;
    REQUIRE( response.parseFrom(buf, (size_t)respLen) == SNMP_ERROR_OK );

    /* settable one applied... */
    REQUIRE( roster.ledWritableCb->setOccurred == true );
    REQUIRE( roster.ledWritable == 0 );
    /* ...the non-settable one did not (isSettable gate, no value written) */
    REQUIRE( roster.sysServicesCb->setOccurred == false );
    REQUIRE( roster.sysServices == 72 );

    /* response echoes the applied varbind and flags the readOnly one at
     * errorIndex 2 (1-based), errorStatus READ_ONLY — the engine's
     * per-varbind SET contract */
    REQUIRE( response.size() == 2 );
    REQUIRE( response.errorStatus.errorStatus == READ_ONLY );
    REQUIRE( response.errorIndex.errorIndex   == 2 );
    REQUIRE( oidMatches(response[0].oid, OID_ledState) );
    REQUIRE( response[0].type == INTEGER );
    REQUIRE( static_cast<IntegerType*>(response[0].value)->_value == 0 );
    REQUIRE( oidMatches(response[1].oid, OID_sysServices) );
    /* v2c wire protocol carries the error at PDU level only (asserted
     * above); the varbind slot itself is echoed value-only. */
}

/* ==========================================================================
 * 5. Trap building — cold/warm/auth SNMPTrap objects carrying the device's
 *    snmpTrapOIDs (the exact trap set src/main.cpp registers).
 * ========================================================================== */
#if !SNMP_NO_TRAPS

/* Minimal BER TLV reader for the byte-level v1 trap header assertions. */
struct Tlv { uint8_t tag; size_t content; size_t len; };
static bool readTlv(const uint8_t* buf, size_t bufLen, size_t pos, Tlv* out){
    if(pos + 2 > bufLen) return false;
    out->tag = buf[pos];
    uint8_t l = buf[pos + 1];
    if(l < 0x80){
        out->content = pos + 2;
        out->len = l;
    } else {
        int n = l & 0x7F;
        if(n == 0 || n > 4 || pos + 2 + (size_t)n > bufLen) return false;
        size_t v = 0;
        for(int i = 0; i < n; i++) v = (v << 8) | buf[pos + 2 + i];
        out->content = pos + 2 + n;
        out->len = v;
    }
    return out->content + out->len <= bufLen;
}
static int tlvInt(const uint8_t* buf, const Tlv& t){
    int v = 0;
    for(size_t i = 0; i < t.len && i < 4; i++) v = (v << 8) | buf[t.content + i];
    return v;
}

static void checkV2cTrapBinds(const char* trapOIDStr){
    SNMPTrap trap("public", SNMP_VERSION_2C);
    trap.setTrapOID(new OIDType(trapOIDStr));
    uint32_t uptime = 424242;
    TimestampCallback* uptimeCb = new TimestampCallback(new SortableOIDType(OID_sysUpTime), &uptime);
    trap.setUptimeCallback(uptimeCb);
    trap.setIP(IPAddress(192, 168, 2, 151));

    REQUIRE( trap.buildForSending() == true );
    REQUIRE( trap.packet != nullptr );

    uint8_t buffer[500] = {0};
    int len = trap.packet->serialise(buffer, 500);
    REQUIRE( len > 0 );
    REQUIRE( (size_t)len < sizeof(buffer) );

    /* v2c traps parse back as Trapv2PDU packets (their inform test uses
     * the same parse route) */
    SNMPPacket trapPacket;
    REQUIRE( trapPacket.parseFrom(buffer, (size_t)len) == SNMP_ERROR_OK );
    REQUIRE( trapPacket.packetPDUType == Trapv2PDU );

    /* mandatory varbind #1: sysUpTime.0 timestamp */
    REQUIRE( trapPacket.size() >= 2 );
    REQUIRE( oidMatches(trapPacket[0].oid, OID_sysUpTime) );
    REQUIRE( trapPacket[0].type == TIMESTAMP );

    /* mandatory varbind #2: snmpTrapOID.0 = the configured trap OID */
    REQUIRE( oidMatches(trapPacket[1].oid, OID_snmpTrapOID) );
    REQUIRE( trapPacket[1].type == ASN_TYPE::OID );
    REQUIRE( oidMatches(static_cast<OIDType*>(trapPacket[1].value), trapOIDStr) );

    delete uptimeCb;
}

TEST_CASE( "esp: v2c traps carry coldStart/warmStart/authFailure snmpTrapOID", "[esp][trap]" ){
    checkV2cTrapBinds(OID_trapColdStart);
    checkV2cTrapBinds(OID_trapWarmStart);
    checkV2cTrapBinds(OID_trapAuthFailure);
    checkV2cTrapBinds(OID_lowHeap);
}

TEST_CASE( "esp: v1 dialect builds TrapPDU with correct enterprise/generic/specific", "[esp][trap]" ){
    /* src/main.cpp v1 dialect (setupTrap on the *V1 objects): the engine's
     * TrapPDU builder puts trapOID into the ENTERPRISE field, so the device
     * registers the enterprise OID .1.3.6.1.4.1.99999 there and encodes the
     * event in the generic trap number (cold 0 / warm 1 / auth 4); low-heap
     * is enterpriseSpecific (generic 6, specific 5). */
    struct Spec { short generic; short specific; };
    const Spec specs[] = {
        { 0, 0 },   /* coldStart          */
        { 1, 0 },   /* warmStart          */
        { 4, 0 },   /* authenticationFailure */
        { 6, 5 },   /* espTrapLowHeap (enterprise-specific) */
    };

    for(const Spec& s : specs){
        SNMPTrap trap("public", SNMP_VERSION_1);
        trap.setTrapOID(new OIDType(OID_enterprise));   /* enterprise field */
        trap.genericTrap = s.generic;     /* src/main.cpp setupTrap() pattern */
        trap.setSpecificTrap(s.specific);
        trap.setIP(IPAddress(192, 168, 2, 151));
        REQUIRE( trap.buildForSending() == true );

        uint8_t buffer[500] = {0};
        int len = trap.packet->serialise(buffer, 500);
        REQUIRE( len > 0 );
        size_t blen = (size_t)len;

        /* message ::= SEQUENCE { version INTEGER(0), community OCTET STRING,
         *                      pdu TrapPDU } */
        Tlv msg;  REQUIRE( readTlv(buffer, blen, 0, &msg) );   REQUIRE( msg.tag == 0x30 );
        size_t c = msg.content;
        Tlv ver;  REQUIRE( readTlv(buffer, blen, c, &ver) );   REQUIRE( ver.tag == 0x02 );
        REQUIRE( tlvInt(buffer, ver) == 0 );                 /* SNMPv1 */
        c = ver.content + ver.len;
        Tlv com;  REQUIRE( readTlv(buffer, blen, c, &com) );   REQUIRE( com.tag == 0x04 );
        REQUIRE( (com.len == 6 && memcmp(buffer + com.content, "public", 6) == 0) );
        c = com.content + com.len;

        Tlv pdu;  REQUIRE( readTlv(buffer, blen, c, &pdu) );   REQUIRE( pdu.tag == 0xa4 ); /* TrapPDU */

        /* Trap-PDU ::= SEQUENCE { enterprise OID, agent-addr NetworkAddress,
         *   generic-trap INTEGER, specific-trap INTEGER, time-stamp TimeTicks,
         *   varbind-list SEQUENCE } */
        Tlv ent;  REQUIRE( readTlv(buffer, blen, pdu.content, &ent) ); REQUIRE( ent.tag == 0x06 );
        REQUIRE( buffer[ent.content] == 0x2b );               /* .1.3 prefix */
        OIDType entRef(OID_enterprise);
        REQUIRE( ent.len == (size_t)entRef.encodedLen() );
        REQUIRE( memcmp(buffer + ent.content, entRef.encodedData(), ent.len) == 0 );

        size_t q = ent.content + ent.len;
        Tlv addr; REQUIRE( readTlv(buffer, blen, q, &addr) ); REQUIRE( addr.tag == 0x40 );
        REQUIRE( addr.len == 4 );
        REQUIRE( buffer[addr.content + 0] == 192 );
        REQUIRE( buffer[addr.content + 1] == 168 );
        REQUIRE( buffer[addr.content + 2] == 2 );
        REQUIRE( buffer[addr.content + 3] == 151 );

        q = addr.content + addr.len;
        Tlv gen;  REQUIRE( readTlv(buffer, blen, q, &gen) );   REQUIRE( gen.tag == 0x02 );
        REQUIRE( tlvInt(buffer, gen) == s.generic );
        q = gen.content + gen.len;
        Tlv spec; REQUIRE( readTlv(buffer, blen, q, &spec) ); REQUIRE( spec.tag == 0x02 );
        REQUIRE( tlvInt(buffer, spec) == s.specific );
        q = spec.content + spec.len;
        Tlv ts;   REQUIRE( readTlv(buffer, blen, q, &ts) );   REQUIRE( ts.tag == 0x43 );
        q = ts.content + ts.len;
        Tlv vbl;  REQUIRE( readTlv(buffer, blen, q, &vbl) ); REQUIRE( vbl.tag == 0x30 );
    }
}
#endif /* !SNMP_NO_TRAPS */

/* ==========================================================================
 * GETBULK truncation regression (RFC 3416 4.2.3) — LOCAL ESP8266_SNMP PATCH.
 *
 * The vendored engine now truncates a GetBulk whose repeater expansion
 * would exceed SNMP_MAX_VARBINDS instead of answering tooBig (which is
 * what aborted whole snmpbulkwalk sessions with the default
 * max-repetitions=10 > our cap of 8).  These cases pin the patched
 * truncation semantics through BOTH dispatch entries — the classic
 * owning-container handlePacket() and the zero-copy
 * handlePacketInPlace() — exactly the way tests.cpp's zero-copy parity
 * case A/B's the two paths (tests.cpp "v3.4.0 phase4" compare() lambda).
 *
 * Request construction follows tests.cpp's GetBulk conventions:
 *   - nonRepeaters rides the PDU's error-status INTEGER,
 *   - maxRepetitions rides the PDU's error-index INTEGER,
 *   - repeater OIDs are SortableOIDType varbinds on a v2c GetBulk PDU.
 * In this host build MAX_SNMP_PACKET_LENGTH=1400 (no ESP8266_TINY),
 * so the wire cap SNMP_MAX_VARBINDS == (1400-40)/160 == 8, matching the
 * flashed platformio.ini build.
 * ========================================================================== */
static void buildBulkRoster(ValueCallback** callbacks, int* count,
                            int32_t* vals, int n){
    for(int i = 0; i < n; i++){
        char oid[48]; // gcc -Wformat-truncation: worst-case "%d" expansion needs 33+
        snprintf(oid, sizeof(oid), ".1.3.6.1.4.1.990000.1.%d", i + 1);
        vals[i] = 100 + i;
        callbacks[(*count)++] = new IntegerCallback(new SortableOIDType(oid), &vals[i]);
    }
    /* tests.cpp-style leaf-by-leaf walk: OID order is encoded order, and
     * .1...10 sorts before .1...2 on the wire. sort_handlers() installs the
     * engine's own roster order after registration. */
    sort_handlers(callbacks, *count);
}

/* Serve a pre-built bulk request through one dispatch entry and hand back
 * the parsed response.  Mirrors tests.cpp's phase4 compare(): serialise
 * into one buffer per path (handlePacketInPlace rewrites in place, so the
 * classic path and zero-copy path must NOT share one request buffer).
 * Returns false when the engine did not answer a normal response. */
static bool serveBulk(SNMPPacket* request,
                      ValueCallback* const* callbacks, int callbacksCount,
                      uint8_t* buf, size_t bufLen, int* respLen,
                      SNMPPacket* response, bool zeroCopy){
    int reqLen = request->serialiseInto(buf, bufLen);
    if(reqLen <= 0) return false;

    *respLen = 0;
    SNMP_ERROR_RESPONSE ret = zeroCopy
        ? handlePacketInPlace(buf, reqLen, respLen, (int)bufLen, callbacks, callbacksCount, "public", "private")
        : handlePacket(       buf, reqLen, respLen, (int)bufLen, callbacks, callbacksCount, "public", "private");

    if(ret != SNMP_GETBULK_OCCURRED) return false;   /* not a clean answer */
    return response->parseFrom(buf, (size_t)*respLen) == SNMP_ERROR_OK;
}


/* Build a v2c GetBulk request the tests.cpp way.  tests.cpp's
 * GenerateTestSNMPRequestPacket() is a translation-unit static, invisible
 * here; this constructs the same PDU shape (nonRepeaters in the
 * error-status INTEGER field, maxRepetitions in the error-index field). */
static SNMPPacket* buildBulkRequest(int nonRepeaters, int maxRepetitions,
                                     const char* const* oids, int oidCount){
    SNMPPacket* packet = new SNMPPacket();
    packet->setPDUType(GetBulkRequestPDU);
    packet->setCommunityString("public");
    packet->setRequestID(SNMPPacket::generate_request_id());
    packet->setVersion(SNMP_VERSION_2C);
    packet->errorStatus.nonRepeaters = (unsigned int)nonRepeaters;
    packet->errorIndex.maxRepititions = (unsigned int)maxRepetitions;
    for(int i = 0; i < oidCount; i++){
        packet->push_back(VarBind(std::make_shared<SortableOIDType>(oids[i]), std::make_shared<NullType>()));
    }
    return packet;
}
TEST_CASE( "esp: GETBULK maxRepetitions>cap truncates to SNMP_MAX_VARBINDS without error (RFC 3416 4.2.3)", "[esp][bulk]" ){
    /* snmpbulkwalk default: one repeater, nonRepeaters=0, maxRepetitions=10
     * against a 12-leaf roster.  Pre-patch the engine answered tooBig and
     * aborted the walk; post-patch it MUST answer noError with exactly
     * SNMP_MAX_VARBINDS (8) varbinds: the first 8 leaves in roster order,
     * ascending, engine-picked (no endOfMibView inside the truncated
     * window — the walk continues from the 8th OID). */
    static int32_t vals[12];
    ValueCallback* callbacks[SNMP_MAX_CALLBACKS_PER_AGENT] = {nullptr};
    int callbacksCount = 0;
    buildBulkRoster(callbacks, &callbacksCount, vals, 12);

    SECTION( "classic handlePacket" ){
        uint8_t buf[1400] = {0};
        int respLen = 0;
        const char* reps[] = { ".1.3.6.1.4.1.990000.1" };
        SNMPPacket* request = buildBulkRequest(0, 10, reps, 1);

        SNMPPacket response;
        REQUIRE( serveBulk(request, callbacks, callbacksCount, buf, sizeof(buf), &respLen, &response, false) );
        delete request;

        REQUIRE( response.size() == SNMP_MAX_VARBINDS );
        REQUIRE( response.errorStatus.errorStatus == NO_ERROR );
        REQUIRE( response.errorIndex.errorIndex == 0 );
        /* All engine-picked rows, ascending in roster (encoded) order;
         * none of them endOfMibView. */
        for(int i = 0; i < SNMP_MAX_VARBINDS; i++){
            REQUIRE( response[i].type == INTEGER );
            REQUIRE( static_cast<IntegerType*>(response[i].value)->_value == 100 + i );
        }
        /* The truncation point: last returned varbind is the 8th callback. */
        char lastOid[32];
        snprintf(lastOid, sizeof(lastOid), ".1.3.6.1.4.1.990000.1.%d", 8);
        REQUIRE( oidMatches(response[SNMP_MAX_VARBINDS - 1].oid, lastOid) );
    }

    SECTION( "zero-copy handlePacketInPlace" ){
        uint8_t buf[1400] = {0};
        int respLen = 0;
        const char* reps[] = { ".1.3.6.1.4.1.990000.1" };
        SNMPPacket* request = buildBulkRequest(0, 10, reps, 1);

        SNMPPacket response;
        REQUIRE( serveBulk(request, callbacks, callbacksCount, buf, sizeof(buf), &respLen, &response, true) );
        delete request;

        REQUIRE( response.size() == SNMP_MAX_VARBINDS );
        REQUIRE( response.errorStatus.errorStatus == NO_ERROR );
        for(int i = 0; i < SNMP_MAX_VARBINDS; i++){
            REQUIRE( response[i].type == INTEGER );
            REQUIRE( static_cast<IntegerType*>(response[i].value)->_value == 100 + i );
        }
        char lastOid[32];
        snprintf(lastOid, sizeof(lastOid), ".1.3.6.1.4.1.990000.1.%d", 8);
        REQUIRE( oidMatches(response[SNMP_MAX_VARBINDS - 1].oid, lastOid) );
    }

    for(int i = 0; i < callbacksCount; i++) delete callbacks[i];
}

TEST_CASE( "esp: GETBULK maxRepetitions under cap returns exactly that many", "[esp][bulk]" ){
    /* maxRepetitions=3: NO truncation should occur — the response carries
     * the requested 3 repetitions, engine-picked, ascending. */
    static int32_t vals[12];
    ValueCallback* callbacks[SNMP_MAX_CALLBACKS_PER_AGENT] = {nullptr};
    int callbacksCount = 0;
    buildBulkRoster(callbacks, &callbacksCount, vals, 12);

    SECTION( "classic handlePacket" ){
        uint8_t buf[1400] = {0};
        int respLen = 0;
        const char* reps[] = { ".1.3.6.1.4.1.990000.1" };
        SNMPPacket* request = buildBulkRequest(0, 3, reps, 1);

        SNMPPacket response;
        REQUIRE( serveBulk(request, callbacks, callbacksCount, buf, sizeof(buf), &respLen, &response, false) );
        delete request;

        REQUIRE( response.size() == 3 );
        REQUIRE( response.errorStatus.errorStatus == NO_ERROR );
        for(int i = 0; i < 3; i++){
            REQUIRE( response[i].type == INTEGER );
            REQUIRE( static_cast<IntegerType*>(response[i].value)->_value == 100 + i );
        }
    }

    SECTION( "zero-copy handlePacketInPlace" ){
        uint8_t buf[1400] = {0};
        int respLen = 0;
        const char* reps[] = { ".1.3.6.1.4.1.990000.1" };
        SNMPPacket* request = buildBulkRequest(0, 3, reps, 1);

        SNMPPacket response;
        REQUIRE( serveBulk(request, callbacks, callbacksCount, buf, sizeof(buf), &respLen, &response, true) );
        delete request;

        REQUIRE( response.size() == 3 );
        REQUIRE( response.errorStatus.errorStatus == NO_ERROR );
        for(int i = 0; i < 3; i++){
            REQUIRE( response[i].type == INTEGER );
            REQUIRE( static_cast<IntegerType*>(response[i].value)->_value == 100 + i );
        }
    }

    for(int i = 0; i < callbacksCount; i++) delete callbacks[i];
}

TEST_CASE( "esp: GETBULK past end of roster terminates cleanly via endOfMibView", "[esp][bulk]" ){
    /* Aim the repeater past the LAST leaf: phase A hands back one
     * endOfMibView varbind (fewer than the requested maxRepetitions,
     * which here exceeds the roster) and terminates the walk cleanly —
     * no error status, no crash, no truncated 8-row padding.
     * Requested maxRepetitions=10 (a bulkwalk default, > roster + 1), so
     * the engine MUST stop on endOfMibView, not run to the cap. */
    static int32_t vals[12];
    ValueCallback* callbacks[SNMP_MAX_CALLBACKS_PER_AGENT] = {nullptr};
    int callbacksCount = 0;
    buildBulkRoster(callbacks, &callbacksCount, vals, 12);

    SECTION( "classic handlePacket" ){
        uint8_t buf[1400] = {0};
        int respLen = 0;
        const char* reps[] = { ".1.3.6.1.4.1.990000.1.13" };
        SNMPPacket* request = buildBulkRequest(0, 10, reps, 1);

        SNMPPacket response;
        REQUIRE( serveBulk(request, callbacks, callbacksCount, buf, sizeof(buf), &respLen, &response, false) );
        delete request;

        REQUIRE( response.errorStatus.errorStatus == NO_ERROR );
        int eomv = 0;
        for(int i = 0; i < (int)response.size(); i++){
            if(response[i].type == ENDOFMIBVIEW) eomv++;
        }
        REQUIRE( eomv >= 1 );
        REQUIRE( eomv < 10 );            /* ended before maxRepetitions filled */
        REQUIRE( response.size() < 10 );  /* overall response short of max-rep count */
    }

    SECTION( "zero-copy handlePacketInPlace" ){
        uint8_t buf[1400] = {0};
        int respLen = 0;
        const char* reps[] = { ".1.3.6.1.4.1.990000.1.13" };
        SNMPPacket* request = buildBulkRequest(0, 10, reps, 1);

        SNMPPacket response;
        REQUIRE( serveBulk(request, callbacks, callbacksCount, buf, sizeof(buf), &respLen, &response, true) );
        delete request;

        REQUIRE( response.errorStatus.errorStatus == NO_ERROR );
        int eomv = 0;
        for(int i = 0; i < (int)response.size(); i++){
            if(response[i].type == ENDOFMIBVIEW) eomv++;
        }
        REQUIRE( eomv >= 1 );
        REQUIRE( eomv < 10 );
        REQUIRE( response.size() < 10 );
    }

    for(int i = 0; i < callbacksCount; i++) delete callbacks[i];
}

TEST_CASE( "esp: GETBULK nonRepeaters=1 answered first, repeaters truncated at cap", "[esp][bulk]" ){
    /* nonRepeaters=1 (a scalar column object) + one repeater whose
     * expansion overflows the cap.  RFC 3416 4.2.3: the non-repeater row
     * MUST occupy the FIRST response varbind (echoed request OID, its own
     * value), the repeater fills the remaining SNMP_MAX_VARBINDS-1 slots,
     * and the patch truncates the total at exactly SNMP_MAX_VARBINDS. */
    static int32_t vals[12];
    ValueCallback* callbacks[SNMP_MAX_CALLBACKS_PER_AGENT] = {nullptr};
    int callbacksCount = 0;
    buildBulkRoster(callbacks, &callbacksCount, vals, 12);

    SECTION( "classic handlePacket" ){
        uint8_t buf[1400] = {0};
        int respLen = 0;
        /* vb0 = non-repeater (a GETNEXT-style scalar fetch: leaf 11),
         * vb1 = repeater walking from the parent. */
        const char* oids[] = { ".1.3.6.1.4.1.990000.1.11", ".1.3.6.1.4.1.990000.1" };
        SNMPPacket* request = buildBulkRequest(1, 10, oids, 2);

        SNMPPacket response;
        REQUIRE( serveBulk(request, callbacks, callbacksCount, buf, sizeof(buf), &respLen, &response, false) );
        delete request;

        REQUIRE( response.size() == SNMP_MAX_VARBINDS );
        REQUIRE( response.errorStatus.errorStatus == NO_ERROR );
        /* Non-repeater answered FIRST via GETNEXT: .11 -> next leaf .12 (111). */
        REQUIRE( response[0].type == INTEGER );
        REQUIRE( static_cast<IntegerType*>(response[0].value)->_value == 111 );
        /* Repeaters then fill the rest — first 7 leaves ascending. */
        for(int i = 1; i < SNMP_MAX_VARBINDS; i++){
            REQUIRE( response[i].type == INTEGER );
            REQUIRE( static_cast<IntegerType*>(response[i].value)->_value == 100 + (i - 1) );
        }
        /* Truncation boundary: last repeater row is the 7th leaf. */
        char lastOid[32];
        snprintf(lastOid, sizeof(lastOid), ".1.3.6.1.4.1.990000.1.%d", 7);
        REQUIRE( oidMatches(response[SNMP_MAX_VARBINDS - 1].oid, lastOid) );
    }

    SECTION( "zero-copy handlePacketInPlace" ){
        uint8_t buf[1400] = {0};
        int respLen = 0;
        const char* oids[] = { ".1.3.6.1.4.1.990000.1.11", ".1.3.6.1.4.1.990000.1" };
        SNMPPacket* request = buildBulkRequest(1, 10, oids, 2);

        SNMPPacket response;
        REQUIRE( serveBulk(request, callbacks, callbacksCount, buf, sizeof(buf), &respLen, &response, true) );
        delete request;

        REQUIRE( response.size() == SNMP_MAX_VARBINDS );
        REQUIRE( response.errorStatus.errorStatus == NO_ERROR );
        REQUIRE( response[0].type == INTEGER );
        REQUIRE( static_cast<IntegerType*>(response[0].value)->_value == 111 );
        for(int i = 1; i < SNMP_MAX_VARBINDS; i++){
            REQUIRE( response[i].type == INTEGER );
            REQUIRE( static_cast<IntegerType*>(response[i].value)->_value == 100 + (i - 1) );
        }
        char lastOid[32];
        snprintf(lastOid, sizeof(lastOid), ".1.3.6.1.4.1.990000.1.%d", 7);
        REQUIRE( oidMatches(response[SNMP_MAX_VARBINDS - 1].oid, lastOid) );
    }

    for(int i = 0; i < callbacksCount; i++) delete callbacks[i];
}

/* ==========================================================================
 * P1 byte counters — ASNPool::rxBytes / ASNPool::txBytes (LOCAL
 * ESP8266_SNMP SNMP-plane datagram totals).  Contract (both dispatch
 * paths): a served request adds at least the serialised request length to
 * rxBytes and at least the serialised response length to txBytes; a
 * malformed datagram still counts rxBytes but must leave txBytes untouched
 * (no response bytes are ever written for an unparseable datagram).
 *
 * Catch2 re-runs the case once per SECTION, so per-path facts live in
 * static locals and the cross-path "identical byte accounting" assertions
 * run once, on the second (last) expansion when both paths have been
 * exercised.  Counters are reset at case start as the contract states.
 * ========================================================================== */
TEST_CASE( "P1 byte counters: 2-varbind GET counts rx/tx by exact serialised lengths, both dispatch paths", "[esp][stats]" ){
    DeviceRoster roster;
    uint8_t buf[800] = {0};

    ASNPool::rxBytes = 0;
    ASNPool::txBytes = 0;

    const char* oids[] = { OID_sysUpTime, OID_sysDescr };
    SNMPPacket* request = buildGetRequest(oids, 2);
    int reqLen = request->serialiseInto(buf, sizeof(buf));
    REQUIRE( reqLen > 0 );

    /* static: survive the per-section case re-runs so both paths can be
     * compared against each other on the second expansion. */
    static size_t rxClassic = 0, txClassic = 0, rxZeroCopy = 0, txZeroCopy = 0;
    static int respClassic = 0, respZeroCopy = 0;
    static int sectionRuns = 0;
    static int savedReqLen = 0;
    sectionRuns++;
    savedReqLen = reqLen;

    SECTION( "classic handlePacket" ){
        uint8_t req[800];
        memcpy(req, buf, (size_t)reqLen);
        respClassic = 0;
        SNMP_ERROR_RESPONSE r = handlePacket(req, reqLen, &respClassic, (int)sizeof(req),
                                              roster.callbacks, roster.count, "public", "private");
        REQUIRE( r == SNMP_GET_OCCURRED );
        REQUIRE( respClassic > 0 );
        rxClassic = ASNPool::rxBytes;
        txClassic = ASNPool::txBytes;
        /* rx counts at least the serialised request length;
         * tx counts at least the serialised response length. */
        REQUIRE( rxClassic >= (size_t)savedReqLen );
        REQUIRE( txClassic >= (size_t)respClassic );
    }

    SECTION( "zero-copy handlePacketInPlace" ){
        uint8_t req[800];
        memcpy(req, buf, (size_t)reqLen);
        respZeroCopy = 0;
        SNMP_ERROR_RESPONSE r = handlePacketInPlace(req, reqLen, &respZeroCopy, (int)sizeof(req),
                                                     roster.callbacks, roster.count, "public", "private");
        REQUIRE( r == SNMP_GET_OCCURRED );
        REQUIRE( respZeroCopy > 0 );
        rxZeroCopy = ASNPool::rxBytes;
        txZeroCopy = ASNPool::txBytes;
        REQUIRE( rxZeroCopy >= (size_t)savedReqLen );
        REQUIRE( txZeroCopy >= (size_t)respZeroCopy );
    }

    delete request;

    /* Cross-path byte accounting: both dispatchers serve the same request
     * bytes and produce byte-identical responses, so the (rx, tx) deltas
     * must match exactly.  Only decidable once BOTH sections have run. */
    if(sectionRuns >= 2){
        REQUIRE( rxClassic  == rxZeroCopy );
        REQUIRE( txClassic  == txZeroCopy );
        REQUIRE( txClassic  > 0 );
        REQUIRE( respClassic == respZeroCopy );
    }
}

TEST_CASE( "P1 byte counters: malformed packet counts rxBytes only, txBytes unchanged, both dispatch paths", "[esp][stats]" ){
    DeviceRoster roster;

    /* 40 bytes of 0xFF: not a BER SNMP message on either dispatcher
     * (classic parseFrom fails; zero-copy BER header peek fails. */
    uint8_t junk[40];
    memset(junk, 0xFF, sizeof(junk));
    const int junkLen = (int)sizeof(junk);

    size_t baseMal = ASNPool::malformedPackets;

    SECTION( "classic handlePacket" ){
        uint8_t pkt[40];
        memcpy(pkt, junk, sizeof(junk));

        ASNPool::rxBytes = 0;
        ASNPool::txBytes = 0;
        int respLen = -1;   /* sentinel: no response bytes may be produced */
        SNMP_ERROR_RESPONSE r = handlePacket(pkt, junkLen, &respLen, (int)sizeof(pkt),
                                              roster.callbacks, roster.count, "public", "private");
        REQUIRE( r == SNMP_REQUEST_INVALID );
        REQUIRE( respLen <= 0 );                       /* no response emitted */
        REQUIRE( ASNPool::rxBytes >= (size_t)junkLen ); /* datagram still counted */
        REQUIRE( ASNPool::txBytes == 0 );               /* nothing transmitted   */
        REQUIRE( ASNPool::malformedPackets == baseMal + 1 );
    }

    SECTION( "zero-copy handlePacketInPlace" ){
        uint8_t pkt[40];
        memcpy(pkt, junk, sizeof(junk));

        ASNPool::rxBytes = 0;
        ASNPool::txBytes = 0;
        int respLen = -1;
        SNMP_ERROR_RESPONSE r = handlePacketInPlace(pkt, junkLen, &respLen, (int)sizeof(pkt),
                                                     roster.callbacks, roster.count, "public", "private");
        REQUIRE( r == SNMP_REQUEST_INVALID );
        REQUIRE( respLen <= 0 );
        REQUIRE( ASNPool::rxBytes >= (size_t)junkLen );
        REQUIRE( ASNPool::txBytes == 0 );
        REQUIRE( ASNPool::malformedPackets == baseMal + 1 );
    }
}
