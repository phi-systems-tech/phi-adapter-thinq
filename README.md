# phi-adapter-thinq

LG ThinQ washers and dishwashers for phi, without LG's cloud. The box
becomes the cloud the appliance's WiFi module is pointed at; the module
never knows the difference, and can be pointed back.

The protocol and every assumption behind this code is written down in
`phi-core/docs/thinq-protocol.md`. That document is the only input here
(clean-room rule, §0); nothing in it is confirmed against our own appliances
yet, and the code says so where it matters.

## What it does (0.1)

Two programs in one package:

- **`phi-thinq-routed`** (`/usr/libexec/phi/`) - the HTTPS half (§3), run
  by the host layer as the backend `thinq@main` (kind descriptor
  `share/backend-thinq.json`, unit `phi-backend-thinq@main`). On first start
  it mints the box CA and a server certificate for the box's DNS name into
  `/var/lib/phi/backends/thinq@main`. It then answers `GET /route` with that
  name as `apiServer` and `mqttServer`, hands the CA out as both trust
  anchors under `/route/certificate`, and signs each module's CSR
  (`POST /device/<uuid>/certificate`), recording the result under
  `devices/`. TLS 1.2 only, legacy suites, `@SECLEVEL=0` - the profile of
  the module families (§6). Port 443 through `CAP_NET_BIND_SERVICE`, as the
  `phi` user. The backend is created by the intent the package ships
  (`share/intent-backend-thinq.json`); it exits until `PHI_THINQ_HOST` is set
  in `/etc/default/phi-adapter-thinq`.
- **`phi_adapter_thinq_ipc`** - the MQTT half (§4). One instance on the
  box's own broker (the local 1883 listener), created by the adapter intent
  rule `share/intent-adapter-thinq.json` with the account the host layer
  made for the backend - its ACL (`clip/#`, `$aws/#`, `lime/#`, and the
  per-device patterns for the appliances' own certificates) comes from the
  kind descriptor. Subscribed to
  `clip/#` and `$aws/#`. It answers `preDeploy`/`deploy` with
  `completeProvisioning` (LG's publication topics verbatim, so the module
  can go back), reads every `device_packet`, and reports one phi device per
  uuid. The appliances it has seen are kept in the instance meta and come
  back after a restart, as disconnected.

What a device shows:

- always: `connectivity`, `signal` (RSSI), `lastFrame` - the last frame as
  hex. The last one is the capture instrument: the offsets in §5.2.3 and
  §5.3 are hypotheses, and a known value turning up at an offset is how they
  become facts.
- washer: `state`, `course`, `remainingTime`, `totalTime`, `spin`,
  `temperature`, `error`, `remoteStart`, `doorLocked`, `childLock`,
  `cycleCount`, `energy` from the `F_V8` table; `power` (off = `F0 24 01`,
  on = `wake`, refused while the status says awake, since wake toggles);
  `operation` with `PAUSE`. `START` and the writable `delayEnd` appear only
  once the model's layout is marked verified: a start repeats the course the
  status shows, and an unverified status is a guessed course.
- dishwasher: `state`, `process`, `course`, times, `doorOpen`, `childLock`,
  `rinseAidRefill`, `saltRefill`. Read-only; LG itself offers no command
  for this model (§5.4a).

The SDK has no household-appliance class yet, so the device is a `Sensor`
with these channels (ROADMAP M4).

## What is not there yet

- Captures. Everything in §7 of the spec is open; until then the status
  channels read what the hypothesis reads, labelled as such.
- The 8883 listener and the DNS override are example drop-ins under
  `/usr/share/doc/phi-adapter-thinq/examples/`, not applied by the package.
- The SoftAP step (§2) is the phi-magic app's (M8). Until it exists, a
  module is re-pointed with LG's own app and the DNS override in place.
- `req_timesync` goes unanswered; the module works without (§4.4).

## Enrolling an appliance, by hand

1. `PHI_THINQ_HOST=<box dns name>` in `/etc/default/phi-adapter-thinq`,
   `phi-cli backend restart thinq@main`. The CA and the server certificate
   appear in `/var/lib/phi/backends/thinq@main`.
2. The 8883 listener: `examples/mosquitto-thinq.conf` to
   `/etc/mosquitto/conf.d/`, mosquitto can read `server.key`; reload. The
   ACL is already written by the host layer.
3. DNS: `examples/dnsmasq-thinq.conf` with the box address and the
   appliance MACs; the appliance must get the box as its resolver.
4. Reset the appliance's WiFi and set it up again with the LG app; the
   module asks `common.lgthinq.com/route`, gets the box, fetches the CA,
   has its CSR signed, connects to 8883 and publishes `preDeploy`. The
   adapter answers and the device appears.

`journalctl -u phi-backend-thinq@main` shows each request with the TLS version
and suite the module negotiated - §7.5 of the spec, for free.

## Layout

| File | What |
|------|------|
| `src/thinq_frame.*` | AABB framing, checksums, hex |
| `src/thinq_envelope.*` | MQTT topics and the JSON envelope |
| `src/thinq_washer.*` | washer commands, vocabulary, status table |
| `src/thinq_dishwasher.*` | dishwasher vocabulary, status table |
| `src/thinq_model.*` | device and channels per family |
| `src/thinq_mqtt.*` | libmosquitto on the SDK loop (from phi-adapter-z2m) |
| `src/thinq_instance.*` | the broker side: provisioning, frames, commands |
| `src/thinq_ca.*` | CA, server leaf, CSR signing (OpenSSL) |
| `src/thinq_route.*` | HTTP parsing and the four endpoints |
| `src/routed_main.cpp` | the TLS listener around them |
| `share/*.json` | backend kind, backend intent, adapter intent |
| `src/thinq_schema.*` | name, icon, settings form |

## Building

```
cmake -S . -B build -G Ninja -DPHI_ADAPTER_THINQ_USE_LOCAL_ADAPTER_SDK=OFF
ninja -C build && (cd build && ctest --output-on-failure)
```

Needs `phi-adapter-sdk-dev`, `libmosquitto-dev`, `libssl-dev`,
`nlohmann-json3-dev`.
