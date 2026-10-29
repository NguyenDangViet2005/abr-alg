# GPS Telemetry — end-to-end

Status: **consumer shipped, senders pending**. Designed 2026-10-19.

Concern: how a GPS fix (lat/lon) travels from the router box → XBQoSService →
qos-dashboard, and what is left to build.

## TL;DR

| Hop | State today |
|-----|-------------|
| Router **produces** 604 (`formatGps`) | **STUB** — zero caller, no GPS source |
| Router **sends** 604 over UDP 12346 | **NOT SENT** |
| XBQoS **receives** 604 → `onGpsFix` | ✅ shipped (`dev/RouterNetworkHandler.cpp:237-253`) |
| XBQoS **uses** fix for ceiling-escape | ✅ shipped (`NetworkReference::setGps`) |
| XBQoS **forwards** fix to dashboard | ❌ **the gap** — no `onGpsFix` tap on the QS channel |
| Dashboard **displays** fix | ❌ not implemented (out of scope, dashboard untouched) |

So "XBQoSService already has GPS" is true only for the **consume** path. The fix
is used internally for the ceiling-escape gate; it is never published. And a
real fix cannot exist yet, because the router does not emit 604.

## 1. The existing (consumer) chain

```
router telemetry id 604            "60" + "4" + "<lat>#<lon>"     (22-field family exception)
  └─ RouterNetworkHandler::parseDatagram  ConnType::GPS branch
       └─ emit onGpsFix(double lat, double lon)          dev/RouterNetworkHandler.cpp:252
            └─ connect (BROADCAST — GPS is global, no iface)
                 XBQoSService.cpp:208  onGpsFix → ABRFactory::handleGpsFix
                      └─ ABRFactory.cpp:164  fan out to every instance
                           └─ XBAdaptiveBitrateStreaming::handleGpsFix   (.cpp:500)
                                └─ NetworkReference::setGps(lat, lon)   (OWNER of meaning)
```

`NetworkReference::setGps` (`NetworkReference.cpp:51`):
- rejects out-of-range lat/lon (NaN fails too) → coerced to "no fix", never seeds a bad anchor;
- stores latest fix, refreshes the 5 s staleness timer (`ABR_STRENGTH_STALE_MS`);
- first fix after a learn seeds the move ANCHOR; a later fix ≥ **`ABR_GPS_MOVE_METERS` = 500 m**
  (haversine, `ABRGpsMove.h`) since the anchor → `environmentChanged()` true → ceiling may lift.

Purpose: a **second, independent ceiling-escape gate** beside radio-strength 603.
See router plan `plans/261013-1124-gps-604-ceiling-escape/`.

## 2. The gap — forward the fix to the dashboard

The debug channel (`QoSDebugSender`, magic `'Q''S'` = `0x51 0x53`, UDP 9999 +
`.qsdb` file) already taps every relevant signal (`onRouterRow`, `onRouterTick`,
ABR decision, ceiling). GPS is simply not one of them. Add it symmetrically:

### 2.1 New QS record type `QS_GPS = 4`

`dev/QoSDebugProtocol.h`:

```cpp
enum QsRecordType : quint8 {
    QS_SAMPLE = 1,
    QS_EVENT  = 2,
    QS_CONFIG = 3,
    QS_GPS    = 4     // GLOBAL fix; no iface
};
```

Payload layout (encode in `QoSDebugProtocol.cpp`, LE):

```
i64 timestampMs
i32 latE6      // degrees × 1e6, signed
i32 lonE6      // degrees × 1e6, signed
```

**Do NOT bump `QS_VERSION`.** Adding a record type does not change any existing
payload's byte size; the strict `offset !== length` rule is *per record type*.
Existing v3 consumers stay valid.

**Why `i32 ×1e6`, not `float`/`double`:** `configureQsStream()` sets
`SinglePrecision`, so a `QDataStream` write of `double` is **truncated to 4-byte
float** by Qt (≈11 m error). `i32 ×1e6` is integer-exact, LE, and matches the
router's existing `%.6f` (~0.11 m). Unit == the router wire — no new convention.

### 2.2 Emit

`dev/QoSDebugSender.h` — new slot (mirrors `handleRouterRow`):
```cpp
void handleGpsFix(double lat, double lon);
```

`.cpp`:
```cpp
void QoSDebugSender::handleGpsFix(double lat, double lon)
{
    QsGps g;
    g.timestampMs = QDateTime::currentMSecsSinceEpoch();
    g.latE6 = qRound(lat * 1e6);
    g.lonE6 = qRound(lon * 1e6);
    qDebug().nospace() << "[QS][GPS] lat=" << lat << " lon=" << lon;   // job 1: always
    if (!m_enabled) return;                                            // job 2: gated
    writeRecord(QS_GPS, buildQsDatagram(QS_GPS, encodeQsGps(g)));
}
```

`writeRecord` already fans out to **both** UDP 9999 and the `.qsdb` file, and
logs `[QSDBG] UDP sent <n> bytes … type=4`. That log line is the send-boundary
proof that separates "never sent" from "sent but not received".

### 2.3 Wire

`XBQoSService.cpp`, inside the `if (m_qosDebug)` block (next to the `onRouterRow`
tap):
```cpp
connect(m_routerNetworkHandler, &RouterNetworkHandler::onGpsFix,
        m_qosDebug, &QoSDebugSender::handleGpsFix);
```

## 3. Fake-send (verify before any hardware exists)

The router does not emit 604, so there is no real fix to forward. Two fixtures
prove the path now:

- **A — unit test** `tests/tst_qs_gps.cpp`: `encodeQsGps → decodeQsGps`
  round-trip. Pure, no socket. Catches the LE / `SinglePrecision` trap at test
  time. (Add `dev/QoSDebugProtocol.cpp` to the test build like `tst_qs_encoder.cpp`.)
- **B — one-shot CLI** `--fake-gps <lat>,<lon>` in `main.cpp`: builds one
  `QS_GPS` datagram and sends it to `--qos-debug-host:--qos-debug-port`, then
  exits. Mirrors the existing one-shot `--upload` block. Lets you fire a fix at
  the dashboard / `.qsdb` on demand.

Both are additive and independent of the ABR path.

## 4. Receiver side (dashboard NOT modified — deliberate)

Per requirement, the dashboard is not touched.

| Channel | Behaviour with a `QS_GPS` record |
|---------|----------------------------------|
| UDP 9999 | `server.js` logs `[UDP] Received … hex: 51 53 01 04 …`; `parseQS` hits `default: return null` → `[UDP] Parse failed`. **No crash, just noise.** |
| `.qsdb` file | Record stored verbatim → offline-decodable with no dashboard. |

To silence the UDP noise later, one line in `qos-dashboard/public/js/parser.js`
`switch (recordType)` adds `case 4`. Deferred on purpose.

## 5. Remaining / unresolved

1. **Real GPS source** — router box has no serial/gpsd/NMEA reader. `formatGps`
   is RESERVED, zero caller. Until a source lands (router modem / external /
   camera), only fake-send produces a fix. *(router plan open-question #1)*
2. **Router sender** — when a source exists, call `formatGps` in
   `TelemetryModule::sendLoop()` after 603 (router plan: currently nothing sends).
3. **`i32 ×1e6` vs `f64`** payload — design picks `i32 ×1e6`; confirm.
4. **Dashboard display** — out of scope now; lat/lon map or text cell is a
   follow-up, requires a parser.js `case 4`.
