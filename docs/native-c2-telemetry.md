# Native XBLink C2 telemetry in ABR

Branch: `feat/implement_telemetry`.

The Python prototype in `tools/xblink_telemetry` has been replaced by C++.
The service does not require Python, pymavlink, tcpdump, libpcap, an Internet
connection, or a runtime message-definition JSON file. Qt Core/Network and a
C++17 compiler are required, as for the existing service.

## Data flow

```text
eth1 UDP/3345 (passive AF_PACKET socket)
  -> XbTelemetryHandler
  -> C2TelemetryDecoder / SrtTelemetrySession / MavlinkStream
  -> QVector<C2TelemetryStat>
  -> ABRFactory::handleNativeC2Data
  -> SRTAdaptiveBitrateStreaming::handleNativeC2Telemetry
  -> C2TelemetryPolicy
  -> network partition ceiling + final bitrate writer
```

The capture never opens `/dev/ttyS0`, binds the UDP service port, sends a
command to the FC, or competes with XBFirm for its serial device. The Linux
socket binds to one interface. A kernel BPF filter removes unrelated untagged
traffic; VLAN frames are validated in the decoder. The capture descriptor is
nonblocking and each event callback has a bounded packet budget.

Camera statistics (`9990` on UDP/12345) still drive camera congestion and
bandwidth estimation. SIYI still supplies RF ceilings and its pause-on-loss
guard. Native C2 data constrains those decisions; it does not replace either
source. Legacy `9991` is ignored by the engine while native C2 is enabled, so
it cannot overwrite FC freshness or inject cumulative retransmits as samples.

## What is measured

- Complete, unfragmented Ethernet/VLAN IPv4/UDP packets for XBLink port 3345.
  The peer UDP port is discovered dynamically and may change on reconnect.
- SRT sequence reordering (up to 250 ms / 256 pending packets), deduplication,
  31-bit sequence wrap, and message fragmentation/reassembly.
- The observed XBLink x74 envelope: outgoing `FD + counter32 + 63 + MAVLink`.
  The reverse envelope has an additional source byte. Status and internal XB
  messages do not refresh the FC signal.
- MAVLink v1/v2 framing with CRC_EXTRA for 301 ArduPilotMega message IDs.
  Only frames from configured FC **1/1** refresh FC/heartbeat timestamps.
  Frames spanning XB chunks are reassembled. Signed frames are CRC checked;
  cryptographic signatures are **not** verified. Flight/sensor fields are not
  decoded because the ABR policy uses freshness and network measurements.
- Fresh peer ACKs, reported RTT in microseconds converted to milliseconds,
  time-based RTT EWMA (`tau=1s`), rolling 60-second p10 baseline, inflation,
  ratio, and trend. Baseline requires 5 reports spanning at least 3 seconds.
  Light ACKs refresh ACK age but cannot create RTT measurements.
- Window data transmissions, retransmission-flagged transmissions, NAK report
  rate, CRC-rejected candidates, unobserved sequences, and kernel capture
  drops. Every snapshot advances window counters, including retired sessions.

`C2TelemetryStat` deliberately has no estimated available bandwidth, unacked
count, final receiver-drop count, or end-to-end loss percentage. Capture gaps
and retransmission share are not substitutes for those measurements. A clean
capture is not proof of loss-free end-to-end delivery.

The terminal shows a short row per sample, or `C2 NO SIGNAL ...` when FC frames
expire. `RX_APP` is only the comparison model discovered in PilotApp 10.9.4:
`(2 * MAVLink_stream_bytes + XB_chunks) / elapsed_seconds / 1024`. It is not
physical throughput and is not used to calculate bitrate or link quality.

Sessions are keyed by endpoints and destination SRT socket ID. Old remote
ports are excluded when the same XBLink source/peer host has fresh sessions.
Simultaneously fresh ports are preserved. If all expire, the latest session
remains to report the outage. New outgoing socket IDs reset matching reverse
RTT/ACK state, even if the peer reuses its socket ID. Delayed packets belonging
to already-known old sessions cannot reset the current session.

## Bitrate behavior

Constants live in `ABRConfigs.h`; the following are initial policy settings,
not flight-tested optimum thresholds.

| Condition | Effect |
|---|---|
| FC, heartbeat, ACK or RTT older than 2 seconds; missing capture; incomplete observation; baseline warmup | Block bitrate increases; permit camera-driven decreases |
| Startup without ready C2 | Permit the existing 50 kbps survival bootstrap only |
| RTT EWMA >60 ms, baseline inflation >=30 ms, retransmission share >=2.5%, or NAK reports >=3/s | High C2 priority; video ceiling 1800 kbps |
| RTT EWMA >300 ms, baseline inflation >=90 ms, retransmission share >=8%, or NAK reports >=15/s | Critical C2 priority; video ceiling 200 kbps |
| Stable, ready recovery | Release the C2 restriction after 3 continuous seconds; normal camera/RF ABR controls increases |

Retransmission thresholds require at least 20 observed data transmissions in
the window. NAK thresholds apply to report rate, not unique lost packets.
Absolute RTT and retransmission/NAK protection can act before baseline warmup
finishes, provided FC, heartbeat, ACK, RTT and the observation are valid.

The effective bitrate is constrained by the camera decision, estimated camera
bandwidth, RF environment, maximum configured bitrate, and C2 ceiling. C2 caps
are enforced at the final writer as well as during partition calculation.
Native C2 does not request video-off or manufacture an explicit C2-only flag.
If the cable is pulled, the last FC frame expires even when RTT/ACKs continue.
This holds increases; it does not interpret a disconnected FC cable as proof
of network congestion. SIYI loss still pauses all bitrate writes as before.

Freshness and the C2 recovery gate use monotonic clocks. The policy also ages
samples between callbacks, so a stopped collector cannot keep C2 healthy.

## Build and run on XBLink

Build **on the Linux target or with its existing cross toolchain**; the Windows
build artifact cannot run on XBLink. From the checkout root:

```sh
mkdir -p build/native-c2
cd build/native-c2
qmake ../../aiCompressor.pro CONFIG+=release CONFIG-=debug
make -j2
sudo env XB_C2_INTERFACE=eth1 ./aiCompressor
```

Use `qmake6` if that is the Qt installation's command. No additional packages
are required by this change when the existing Qt/C++ build environment is
already available. Live capture requires root or `CAP_NET_RAW`; an existing
service unit must grant that capability and must not prohibit AF_PACKET.

Native capture is enabled by default. `XB_C2_INTERFACE` overrides `eth1`.
Other defaults (port, FC IDs, timers, bitrate ceilings) are in `ABRConfigs.h`.
If the interface or permissions are wrong, the service logs the capture error,
reports no signal, and holds increases; fix the deployment and restart it.

To explicitly retain the previous legacy `9991` behavior for a deployment:

```sh
XB_C2_TELEMETRY=0 ./aiCompressor
```

Do not leave another ABR process running against the same camera while testing
this service. Pull/reconnect the FC cable and verify that the row changes to
`NO SIGNAL` within about 2–2.5 seconds of the last valid frame, then recovers.
Check camera/RF telemetry alongside C2 before evaluating bitrate behavior.

## Tests

The standalone test target uses Qt Core/Network and C++; no Python dependency.
The fixture JSON files are test inputs produced independently by pymavlink
2.4.50, not runtime telemetry output. Tests cover all 301 CRC definitions,
MAVLink1/signed framing, CRC resync, XB envelope filtering, SRT reorder/duplicate/
fragment/wrap, stale FC with live ACKs, recovery, RTT calculations, capture
gaps/drops, changing ports/socket IDs, policy gates/caps, and factory/engine wiring.

```sh
mkdir -p build/native-c2-tests
cd build/native-c2-tests
qmake ../../tests/telemetry/telemetry-tests.pro CONFIG+=release CONFIG-=debug
make -j2
./test-c2-telemetry ../../tests/telemetry
```

On Windows, use the matching Qt MinGW `qmake` and `mingw32-make`; the executable
is `release/test-c2-telemetry.exe`. The native Linux capture adapter has an
explicit unavailable implementation on Windows; offline decoder/policy tests
still run. AF_PACKET capture and camera behavior must also be checked on XBLink.

Protocol references: [SRT specification](https://github.com/Haivision/srt-rfc/blob/main/draft-sharabayko-srt.md),
[Linux socket filtering](https://docs.kernel.org/networking/filter.html),
[MAVLink ArduPilotMega](https://mavlink.io/en/messages/ardupilotmega.html).
