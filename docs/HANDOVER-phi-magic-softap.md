# Handover: the phone's SoftAP step is written, the box has to be ready for it

Written 2026-10-20 from the phi-magic-app side, for the session on taifun
that builds and deploys this adapter. Everything below about the phone is
done and tested with fakes; nothing has met a real module yet. Everything
about the box is what the phone will expect to find.

## 1. What the phone does now

phi-magic-app (`src/client/softap.*`, `thinqsetup.*`, `wifiprovisioner.*`,
`src/app/hotspot.mm`, `qml/WifiDevicePage.qml`; on the `main` working tree,
commit pending) has the §2 sequence of `phi-core/docs/thinq-protocol.md`:

1. Join the access point `LGE_XXX_yyyy` with password `yyyyyyyy`
   (NEHotspotConfiguration, `joinOnce`). iOS cannot scan, so the name comes
   one of three ways: typed in full; only `yyyy`, and the phone joins any
   `LGE_*` network and then checks the name it landed on ends so; or a
   `WIFI:S:...;P:...;;` QR code on the appliance, which the same camera as
   the Matter code reads and routes to this page. **Whether LG prints such
   a code on ours, and whether the display shows `yyyy`, is to be looked at
   on the appliance.**
2. TLS to `192.168.120.254:5500`, certificate not verified, TLS 1.2 or
   later as Qt's SecureTransport backend offers it. **Which suites that
   stack offers an RTL8711/RTL8720 module is the first open question**; the
   handshake failing is the most likely first failure, and it is reported.
3. Over that one connection, every message with `"constantConnect":"Y"`:

   | step | what we send | what we need back | without an answer |
   |------|--------------|-------------------|-------------------|
   | `setDeviceInit` | `{"set":"true"}` | nothing | go on after 4 s |
   | `getDeviceInfo` | `publicKey` (fixed throwaway RSA-2048 PEM, lines at column 0), and when the box name is known `mqttServer":"ssl://<host>:8883"`, `"apiServer":"https://<host>:443"` | `data.uuid`, `modelName`, `deviceType` | **fail** after 4 s |
   | `setCertInfo` | `otp` 48 random `[A-Za-z0-9]`, `svccode":"SVC202"`, `svcphase":"OP"` | nothing | go on |
   | `setApInfo` | `format":"B64"`, `ssid`/`password` base64, `WPA2_PSK`/`AES` | nothing | go on |
   | `releaseDev` | no `data` | nothing | done |

4. Leave the access point. The phone then says "the appliance is joining
   your network; if it does not show up, switch it off and on once".

Every byte both ways is kept in a transcript the page shows (home
password blanked). The first runs are captures: the person will read it off
the phone or screenshot it. A "share transcript" button is not there yet.

The `otp` is not recorded anywhere; `routed` does not check it, so nothing
has to change there. The public key is a throwaway whose private half was
discarded: `encrypt_val` is never decrypted (§2.3).

The `boxHost` the phone puts into `mqttServer`/`apiServer` is, by default,
the host of the WebSocket address the app itself uses (`pi5.local` or
whatever the person typed), editable on the page. Whether those two fields
make the module skip `/route` is §7's open question; the phone sends them
either way.

## 1a. First capture, dishwasher D0211, 2026-10-20 10:33 [C]

Run from the iPhone with "only ask who it is"; nothing was changed on the
appliance. Verbatim from the app's transcript:

- Access point `LG_Smart_DishWasher2_open`, no password. **Not** the
  `LGE_XXX_yyyy` scheme of §2.1; the `[LG_...]` one the notes mention as
  seen before the setup sequence. How it was put into this mode: the
  appliance's own menu (to be written down).
- The module hands the phone `192.168.120.1/24`; the module is
  `192.168.120.254`, port **5500** open; 443, 80 and 8080 on it closed.
- TLS on 5500: **TLSv1.2, ECDHE-RSA-AES128-GCM-SHA256** against Apple's
  stack, no CBC needed. Certificate: host name mismatch, untrusted root,
  as expected; ignored.
- `setDeviceInit` answers:
  `{"type":"response","cmd":"setDeviceInit","data":{"protocolVer":"4.9","subErrorCode":"W008","result":"000"}}`
- `getDeviceInfo` answers (`encrypt_val` shortened):

  ```
  {"type":"response","cmd":"getDeviceInfo","data":{"protocolVer":"4.9",
  "mac":"14:7f:67:cf:d3:24","uuid":"c5c8f38b-15c6-19ff-bc00-147f67cfd324",
  "deviceType":"204","modelName":"D0211","modemVer":"clip_hna_v1.9.230",
  "demandType":"QCOM_QCA4010","isReg":"Y","supportsWpa3":"N","jitpCert":"N",
  "bootId":"6657459293354554224","cctld":"","cctld_result":-79,"mdFota":"Y",
  "softwareVer":"1","countryCode":"WW","encrypt_val":"DXeBdlzaf2r...NSLXQ==",
  "eepromChecksum":"0","errorcodeDisplay":"0","remainingTime":"420",
  "agentVer":"0","staticip":"Y","errSSID":"Villa Sabina","subErrorCode":"NONE",
  "factoryResetCode":"0","ch":"10","extra":"POWER_ON|49401|encryptRes:0",
  "regStep":0}}
  ```

What that settles for the box side:

- **Module variant: `QCOM_QCA4010`, firmware `clip_hna_v1.9.230`,
  protocol 4.9.** The TLS profile `routed` and mosquitto must offer is the
  QCA4010 one, not the RTL one; the suite it chose on 5500 says it does
  GCM. Whether it also accepts the `@SECLEVEL=0` legacy profile is not
  contradicted, but it may not need it.
- The uuid is `c5c8f38b-15c6-19ff-bc00-147f67cfd324` = `externalId` of the
  device the adapter will announce; the MAC matches the DNS override entry.
- `isReg: Y`, `regStep: 0`: registered with LG. `jitpCert: N`.
- `encryptRes:0` and a 256-byte `encrypt_val`: the RSA public key with
  lines at column 0 works on this firmware too.
- `errSSID: "Villa Sabina"`: the network the module last used. The phone
  was on `VSP`. **The QCA4010 is 2.4 GHz only**; if `VSP` is the 5 GHz
  name, `setApInfo` has to carry the 2.4 GHz SSID, or the module never
  comes home. To check in the router before the full run.
- `staticip: Y`, `ch: 10`, `cctld_result: -79` (RSSI of the last link?):
  recorded, meaning open.
- Still open: the answers to `setCertInfo`, `setApInfo`, `releaseDev`, and
  whether `apiServer`/`mqttServer` in `getDeviceInfo` skip `/route`. Those
  need the full run, i.e. the box ready (§2).

## 2. What the box needs before the first run

In order of "nothing works without it":

1. **Port 443.** The module speaks to `apiServer` on 443 (`/route`,
   `/route/certificate`, `POST /device/<uuid>/certificate`). On the box
   nginx owns 443 for the UI. README says the two are not reconciled. Our
   proposal: keep `routed` off 443, put it on 8443, and let nginx's stream
   module route by SNI (`ssl_preread`): `common.lgthinq.com`,
   `comon.lgthinq.com` and `PHI_THINQ_HOST` go to `routed`, everything else
   to the UI. That keeps `@SECLEVEL=0` and TLS 1.2 inside `routed`, where
   it belongs. If the phone's `apiServer` lets the module skip `/route`,
   the SNI the module sends is `PHI_THINQ_HOST`, so that name has to be in
   the list too. `routed --api-port` must then still *advertise* 443.
2. **Build and deploy this repo as it is** (`phi-adapter-thinq` from GitHub
   `phi-systems-tech`, current `main`): the backend `thinq@main` with
   `PHI_THINQ_HOST=<name the home network resolves to the box>` in
   `/etc/default/phi-adapter-thinq`, restarted, its server certificate under
   `/var/lib/phi/backends/thinq@main`; the 8883 listener's certificate
   carrying the same name (`phi-mosquitto-tls <name>` if it differs); the
   adapter instance created by the intent and running.
3. **DNS override** for the two appliance MACs (`examples/dnsmasq-thinq.conf`)
   as the fallback for a module that asks `/route` anyway. Both spellings,
   `common.` and `comon.`.
4. **Tell the phone it is ready.** Today the app has no way to know whether
   the box can take an appliance; it offers the page regardless. The same
   pattern as phi-adapter-matter's `thread` flag: put into the instance meta
   `"provisioning": {"host": "<PHI_THINQ_HOST>", "apiPort": 443,
   "mqttPort": 8883, "ready": true}`. The adapter can read the host from the
   backend's environment or from the backend record; the app will then
   prefill the box field from it and hide the page when `ready` is false.
   Not needed for the first capture; needed before anyone else uses it.
5. **Tolerate `preDeploy` twice** (RTL8720cm re-registers after
   `releaseDev`, §2.3). Worth a look at `thinq_instance.cpp` line ~319:
   answering twice with `completeProvisioning` must be harmless.
6. **Log the TLS version and suite** the module negotiates on 443 and 8883;
   `routed` does already (`journalctl -u phi-backend-thinq@main`).

## 3. The day of the first run

1. Box prepared as in §2, `journalctl -f` on `phi-backend-thinq@main` and the
   adapter instance in a terminal; `tcpdump -i <lan> port 53 or port 443 or
   port 8883` if the DNS question matters that day.
2. The dishwasher (`D0211`, `14:7f:67:cf:d3:24`) into setup mode. **How is
   not written anywhere we have; it is in the appliance's manual.** The
   washer is the second candidate; it has the unverified frame layout, the
   dishwasher is read-only and the safer first.
3. iPhone: Mirroring to the Mac off. Phi Magic, +, "Set up a WiFi appliance
   instead". Name of the appliance's WiFi as the display shows it, home
   network (prefilled when the location permission was given), home
   password, box name. "Set up the appliance". iOS asks once to join.
4. Read the transcript. What it settles, in the spec's terms (§7):
   - TLS on 5500: version and whether the handshake succeeds at all.
   - `getDeviceInfo`: the whole answer, especially `demandType`,
     `modemVer`, `protocolVer` (module variant, decides the TLS profile on
     the box side), `deviceType`, `modelName`, `isReg`.
   - Whether `setCertInfo`, `setApInfo`, `releaseDev` answer, and what.
5. Read the box: whether `/route` was asked (answers "do `mqttServer`/
   `apiServer` skip it"), the CSR arriving and being signed, the MQTT
   connect, `preDeploy`, the device appearing with `externalId` = uuid.
6. If the module loops or never shows: power-cycle the appliance once
   (§2.3) before changing anything.

## 4. What is deliberately not done on the phone

- No scanning for networks (iOS has none). No prefix join: the LG password
  is derived from the suffix, so the name has to be typed whole anyway.
- No Android join yet; the interface (`Hotspot` in `wifiprovisioner.h`) is
  the seam, `WifiNetworkSpecifier` goes behind it.
- No `Network.framework` transport bound to the WiFi interface. The
  module's subnet is on the WiFi interface, so routing goes there without
  help; if a run shows the connection going elsewhere, that is the next
  thing to add (`Link` is the seam).
- The home password is not persisted.

## 5. Who to ask

The phone side is in phi-magic-app; the spec is the only shared input
(clean-room rule). Anything the transcript shows that contradicts §2 goes
into `phi-core/docs/thinq-protocol.md` with a `[C]` mark, and the phone
code follows the spec, not the other way round.

## 6. What was done on the box, 2026-10-20 10:51 (pi5, 192.168.18.2)

Built on taifun with sbuild (pipeline id `thinq-stack-1020`, extra packages
sdk 0.23.0, libphi-runtime 0.2.1, transport-api 2.2.0, phi-chip-dev) and
installed with `apt-get install --no-install-recommends ./*.deb`:

- phi-adapter-sdk 0.23.0-1 (was 0.22.0; sdk-dev and sdk-qt-dev removed,
  sdk-qt went to 0.6.0 from apt)
- phi-core and phi-hostd 0.0~git202610191509.12ceee0-1 (was 96f04b1 of
  10-15; phi-hostd is new on this box)
- phi-adapter-thinq 0.1.2-1, phi-adapter-matter 0.0.18-1

State after that: hostd up on `/run/phi/hostd.sock`, backend `thinq@main`
installed and expected, adapter instance 5 "LG ThinQ main" running and on
the broker at 127.0.0.1:1883 (anonymously, see below), hue/tuya/z2m as
before. `/etc/default/phi-adapter-thinq` has `PHI_THINQ_HOST=pi5.lan`.

**Not installed: phi-mosquitto.** Its 1.0-10 (built on taifun, `mosq1`)
turns `allow_anonymous` off on 1883 and expects accounts from hostd, which
rewrites passwd and acl from its own credentials on every change. This
box's zigbee2mqtt (`/opt/zigbee2mqtt`, its own unit) and the z2m adapter log
in with no account and are not under the host layer, so they would lose the
broker. Instead, a script written for the box (the user runs it) mints the
CA and the broker leaf exactly as `phi-mosquitto-tls` does, for `pi5`,
`pi5.lan` and 192.168.18.2, adds `/etc/mosquitto/conf.d/phi-thinq.conf`
(1883 explicit and anonymous, 8883 with `require_certificate`, phi's cipher
list) and `/etc/dnsmasq.d/thinq.conf` (`dhcp-mac=set:thinq,14:7f:67:cf:d3:24`,
`dhcp-option=tag:thinq,6,192.168.18.2`, `address=/pi5.lan/…`,
`address=/common.lgthinq.com/…`, `comon.` too). The box is this LAN's DHCP
server, so the per-MAC resolver is clean here. Bringing z2m under hostd
(phi-z2m-runtime) is the way to phi-mosquitto proper; separate step.

**Seen on the upgrade, not ours but yours:** phi-core 12ceee0 on the
10-15 history database logs every 10 s
`ChannelHistoryStore prepare history insert failed: ON CONFLICT clause does
not match any PRIMARY KEY or UNIQUE constraint` - the insert in
`channelhistorystore.cpp:293` wants `UNIQUE(channel_id, ts_ms)` and the old
table has none; there is no migration. Channel history is not being written
on the box until that is fixed. Also `Ignoring legacy global config
/etc/phi/phi-core.ini`, harmless.

## 7. Full run, dishwasher D0211, 2026-10-20 11:21 [C]

Box prepared as in §6 (hand-made 8883 listener, routed on 443 for
`pi5.lan`, per-MAC resolver). Phone: `VSP` (2.4 GHz) as the home network,
`pi5.lan` as the box, "only ask" off. Every step answered `result 000`:

- `setCertInfo` → `{"protocolVer":"4.9","subErrorCode":"NONE","result":"000"}`
- `setApInfo` → `{"result":"000","rssi":-68,"protocolVer":"4.9","subErrorCode":"NONE",
  "homeNetwork":{"oui":"3c64cf","freq":2462,"security":"WPA2_PSK","encryption":"AES","ssid":"VSP"},
  "modemInfo":{"macReturnValue":{"flag":-1,"reason":-1,"sigscore":0,"rssi":0,"per":0}},
  "sysInfo":{"bootId":"…","resetReason":"POWER_ON","mid":47468}}` - the module
  scans for the SSID and reports the AP it found, channel 11 (2462 MHz).
- `releaseDev` → `{"protocolVer":"4.9","subErrorCode":"NONE","result":"000"}`
- Second `getDeviceInfo` of the day: `errSSID` had become `VSP`, and an
  `errorCode":"nS"` appeared (first run had none). Meaning open.

Then, on the box (dnsmasq log, tcpdump):

- 11:21:59 DHCPDISCOVER/ACK from `14:7f:67:cf:d3:24` with tags
  `known, thinq`: the module is on the home network, 192.168.18.11, and got
  the box as DNS. Immediately `query[A] common.lgthinq.com` → 192.168.18.2.
  **It never asks for `pi5.lan`: the `apiServer`/`mqttServer` given in
  `getDeviceInfo` do not replace `/route` on this firmware (§7's question,
  answered for QCA4010 / clip_hna_v1.9.230).** Only the `common.` spelling
  was asked.
- After the DNS answer: **no packet at all** to 443 or anywhere, then a
  fresh DHCPDISCOVER 27 s later and the same again, with growing gaps
  (11:22:26, 11:29:10, 11:29:52, 11:31:19, 11:34:54). The backend saw no
  request.
- Suspected cause, from §6: dnsmasq answers `address=` records with TTL 0,
  and "the module ignores a TTL of 0 [I]". `local-ttl=60` was added to
  `/etc/dnsmasq.d/thinq.conf` at 11:37. Whether that frees the loop is the
  next thing the log shows; if not, a power-cycle of the appliance (§2.3)
  is the next step, then a capture of what it does after the DNS answer.

### 7a. After a power-cycle, 11:55 [C]

The loop ended with the power-cycle (§2.3 was right; whether `local-ttl=60`
mattered is not separable, keep it anyway). Then, in order:

- DHCP with tag `thinq`, `query[A] common.lgthinq.com` → box, **then
  `query[A] pi5.lan`**: the module read `/route`'s answer and used it.
- `GET /route -> 200 (TLSv1.2, ECDHE-RSA-AES256-SHA)` in routed's journal:
  the HTTPS side with this module is a CBC suite, the legacy profile is
  needed there after all.
- **No `GET /route/certificate`, no `POST /device/<uuid>/certificate`.**
- Every 30 s a TLS connection to 8883: ClientHello 131 B, our flight
  2163 B, the module's flight 691 B carrying a client certificate (682 B,
  sha256WithRSAEncryption, not ours), our alert `unknown_ca` (0x30), FIN.
  mosquitto: `certificate verify failed`. `tcpdump -X` of the module's
  flight starts `16 03 03 02 ae 0b 00 02 aa …`.

Reading: the module still holds the certificate from its LG registration
(`isReg: Y`, `jitpCert: N`) and presents it; it only goes through §3.2/§3.3
when it has none. `setDeviceInit` (answered `W008`) did not make it drop
that. So the next step is on the appliance itself: its own reset of the
WiFi/ThinQ registration (manual), after which `getDeviceInfo` should say
`isReg: N`, and the run repeated. Not an adapter change.

Also seen: `errorCode: "nS"` on the second `getDeviceInfo`; `homeNetwork`
in the `setApInfo` answer confirms the module scans and reports channel
and security of the SSID it was given.

## 8. Enrolled, 2026-10-20 12:52 [C]

What freed the module: deleting the dishwasher in LG's own app (manual p.56,
"Supprimer le produit"), with the DNS override off for a moment so the
module could reach LG's cloud for that. Afterwards `getDeviceInfo` said
`isReg: N`, `errSSID: ""`, same `modemVer clip_hna_v1.9.230` (the app
also ran a firmware update first; the module version did not change). The
sales model is **DB365TXS** (sticker QR `http://thinq.link/QR1?s=<serial>&m=DB365TXS.AASQEES`,
which the app now recognises as an LG sticker).

Then the full run from the phone, override back on, box `pi5.lan`:

```
12:51:35 DHCPACK 192.168.18.11, tags known,thinq
12:51:37 query[A] common.lgthinq.com -> 192.168.18.2
12:51:38 GET /route -> 200                      (TLSv1.2, ECDHE-RSA-AES256-SHA)
12:51:40 query[A] pi5.lan -> 192.168.18.2
12:51:41 TLS handshake failed: tlsv1 alert unknown ca   (the module, before it had our anchor)
12:51:43 GET /route/certificate -> 200
12:51:44 GET /route/certificate?name=common-server -> 200
12:51:58 no HTTP request (0 bytes)              (one idle connection)
12:52:05 issued a certificate to c5c8f38b-… (/CN=*.clip.com/O=LGE/C=KR)
12:52:05 POST /device/c5c8f38b-…/certificate -> 200
12:52:07 8883: client alert unknown ca           (still before the aws-iot anchor)
12:52:12 GET /route/certificate -> 200
12:52:18 GET /route/certificate?name=aws-iot -> 200
12:52:18 8883: connection (no error logged)
12:52:35 DHCP again, 12:52:40 query pi5.lan, 12:52:43 GET /route -> 200
12:52:44 8883: New client connected … as c5c8f38b-15c6-19ff-bc00-147f67cfd324 (p1, c1, k60, u'*.clip.com')
         adapter: new appliance c5c8f38b-… (D0211); sent preDeploy; answered completeProvisioning; acknowledged provisioning
```

Settled by this:
- The module asks `/route` first, always, then the two anchors, then
  posts its CSR (`CN=*.clip.com, O=LGE, C=KR`) - §3.2/§3.3 as the notes had
  them, in that order, with `common-server` before the CSR and `aws-iot`
  after. HTTPS to routed is TLS 1.2 `ECDHE-RSA-AES256-SHA` throughout.
- MQTT: protocol level 3.1 (`p1`), clean session, keep-alive 60, username
  from the certificate CN `*.clip.com`, client id = uuid. The `%u` ACL
  patterns of the kind descriptor would therefore match `*.clip.com`, not
  the uuid - worth a look before phi-mosquitto's ACL goes live.
- `preDeploy` → `completeProvisioning` → ack works with the 0.1.2 adapter
  as built. The device is known to the adapter; whether it is visible in
  the UI with channels is the next check (the box had no `device added`
  line at info level).
- The module re-DHCPs once more about 30 s after `releaseDev`'s join; harmless.

## 9. After enrolment: what the appliance does and does not do [C]

- The module stays on the broker (keep-alive 60) but **publishes nothing
  on its own**, switched on or off, door or not. The appliance's Wi-Fi LED
  stays dark and, switched on, it does not "connect by itself" as far as
  the panel shows - the module is connected all the while. So the main
  board does not treat the module as registered. Something LG's cloud does
  after `completeProvisioning_ack` is missing on our side; the spec's §7
  items 6 (`req_timesync` answer) and the `deploy` cycle are the
  candidates. A capture of LG's side (bridge) is what settles it.
- After a power-cycle at 13:09 the module came back by itself with the
  saved network, asked `/route`, **posted a new CSR again** (a certificate
  per boot, apparently), connected, sent `preDeploy` again (not `deploy`),
  got `completeProvisioning`, acked.
- One frame did arrive, when the appliance was switched on at ~13:11:

  ```
  $aws/rules/clip_http_action_rule/clip/devices/<uuid>/message
  {"mid":272002,"did":"<uuid>","kind":"D0211","cmd":"device_packet","rssi":-71,
   "fs":"idle","br":4800,"per":5,"data":"AA37323102015341413431323633393132000011C4
   00008000000000000253414134313236313031300000FFA7FFFC00000000000041BB","type":1}
  ```

  55 bytes, `AA 37` length ok, checksum `41` verifies with §5.1's rule
  (sum mod 256 xor 0x55). Class byte **`32`** as the §5.3 hypothesis
  says. Record type `31` (not `EB`/`EC`): `02 01` then the ASCII
  `SAA41263912`, then `00 00 11 C4 00 00 80 00 00 00 00 00 02`, the ASCII
  `SAA41261010`, `00 00 FF A7 FF FC 00 00 00 00 00 00`. Reads as an
  identification record (two firmware ids) sent at power-on, not a status
  block. No `32 EB` status followed. `br: 4800` is the UART rate, `per: 5`
  unknown.
- The topic the module publishes on is the `publication.message` we gave
  it in `completeProvisioning` (`$aws/rules/clip_http_action_rule/clip/devices/<uuid>/message`).

### 9a. `undeploy` after ten minutes [C]

Broker, second session: connected 13:09:36, `preDeploy` →
`completeProvisioning` (our `deployInterval: 600`) → ack. The same
identification frame once more when the appliance was switched on (`per`
went 5 → 12). Then, inside a ten-minute listen window, on the provisioning
topic `clip/provisioning/devices/<uuid>`:

```
{"did":"<uuid>","kind":"D0211","cmd":"undeploy"}
```

and at 13:20:18 (+642 s from connect, i.e. `deployInterval` plus the
keep-alive) mosquitto drops the client for timeout; no DHCP, no WiFi since.
The first session went the same way sooner (connected 12:52:46, timeout
12:57:21). The panel's Wi-Fi LED never lit.

Reading: the module gives us `deployInterval` worth of time to finish
what LG's cloud finishes after the ack, then undeploys and drops the link
(the notes' "undeploy/re-register loop", §2.3/§4.2). What that step is,
the notes do not say. Candidates, in order: (1) `req_timesync` sent right
after connect goes unanswered (§4.4, answer format [?]; both listen windows
today started after the connect, so the request itself was not even seen -
the next listen covers a power-on), (2) a `deploy` round the cloud is
expected to drive, (3) something over HTTPS after the CSR. The adapter
is where this lands; the phone's part is done.

### 9b. The first seconds after connect, captured [C]

Third enrolment, 13:34, listener running before the module came up
(`mosquitto_sub -v` on 1883, timestamps are the box's):

```
13:34:48  DHCP (tag thinq)                      → /route, CSR signed again, connect
13:34:56  $aws/rules/clip_provisioning_rule/clip/provisioning/devices/<uuid>
          {"mid":445937,"did":"<uuid>","kind":"D0211","cmd":"preDeploy","rssi":-72,
           "fs":"idle","br":4800,"per":16,"data":{"appInfo":{"rollbackInfo":{"area_b":
           "clip_hna_v1.9.230","active":"area_b","area_a":"clip_hna_v1.9.226"},
           "modelName":"D0211","modelLanguage":"02","softVer":"1","ruleVer":"1",
           "countryCode":"WW","subCountryCode":"WW","appVersion":"clip_hna_v1.9.230", …
13:34:56  lime/devices/<uuid>   our completeProvisioning (provisioningType preDeploy, deployInterval 600)
13:34:56  …/message              {"cmd":"completeProvisioning_ack","rssi":-72,"fs":"idle","br":4800,"per":16,"data":null,"type":1}
13:36:26  …/message              device_packet, the same AA 37 32 31 … identification frame, rssi -67, per 1
13:37:03  DHCP again (the module restarted its link)
13:37:11  clip/provisioning/devices/<uuid>   {"did":"<uuid>","kind":"D0211","cmd":"undeploy"}
13:37:11  preDeploy → completeProvisioning → ack, as above (per 43)
13:40:33  device_packet, identification frame again
```

- **No `req_timesync` at all**, at any point. Strike §7 item 6 as the
  cause; leave it as a question.
- The module publishes `preDeploy` on the `$aws/rules/clip_provisioning_rule/…`
  topic we handed it (so the flash write from §4.2 step 2 happened), and
  `undeploy` on the bare `clip/provisioning/devices/<uuid>` topic.
- The module's `preDeploy` carries more than the notes' example:
  `rollbackInfo` with two firmware areas (`clip_hna_v1.9.226` and `.230`,
  `.230` active), `modelLanguage "02"`, `softVer "1"`, `ruleVer "1"`. The
  rest of the payload was cut by the listener; capture it whole next time
  (`mosquitto_sub` without a line cut).
- The cycle: connect, `preDeploy`/ack, 1.5–2.5 min later the
  identification frame, then at some point `undeploy`, a fresh DHCP, a
  fresh CSR, `preDeploy` again. The gap before `undeploy` was 10 min in
  the second session and 2 min in the third; the module stays
  `provisioningType preDeploy` throughout - it never moves to `deploy`.
  So the step LG's cloud takes between `completeProvisioning_ack` and the
  first `deploy` is the missing one, and it is not something the module
  asks for on MQTT. Candidates now: a message from the cloud on
  `lime/devices/<uuid>` after the ack (unknown shape), or an HTTPS call
  the module expects to succeed (routed logged only `/route`, the anchors
  and the CSR - nothing was refused). The panel's Wi-Fi LED stays dark the
  whole time, which says the main board is told "not registered" by the
  module.

## 10. What LG's cloud does that we did not - observed behaviour, 2026-10-20

These are facts about how the cloud behaves towards the module, established
today and to be carried into `phi-core/docs/thinq-protocol.md` as observed
behaviour; they are not documented protocol.

1. **Delivery acks.** The cloud acknowledges every frame the appliance
   sends except types `00` (the appliance's own acks), `C3` (heartbeat),
   `EB`/`EC` (bare status records). The ack is a frame with contents
   `F0 00 <type> 04`, and for an envelope (`<class> 0A …`) additionally the
   envelope's 16-bit sequence number (contents bytes 5..6); envelopes
   shorter than 13 bytes are ignored. It is published under its **own MQTT
   command `ack`** (`{"did","mid","cmd":"ack","type":1,"data":"<hex>"}`),
   not `packet`. Unacknowledged, an appliance repeats a frame up to ten
   times and its content sync never completes; `00` in place of `04`, or
   `packet` in place of `ack`, stops the retransmits but leaves the content
   sync re-running. **That is our symptom**: the D0211's identification
   record (type `31`) repeated, then `undeploy`.
2. **`req_timesync` is answered**: `{"cmd":"resp_timesync","type":1,"did","mid",
   "data":"<base64 of 7 bytes>"}` with UTC year mod 100, month (0-based),
   day, hours, minutes, seconds, weekday (0 = Sunday). Our module has not
   sent `req_timesync` yet, but the answer is cheap.
3. **Dishwasher start packet.** The D0211 is sent, right after
   `preDeploy`, one command: contents `F0 ED 11 21 01 00 00 00 18 00` as a
   `packet`. Meaning undocumented. The H11 dishwasher gets nothing at start.
4. `completeProvisioning` as we send it is right (did, mid, cmd, type 0,
   data with result 0, host "message", appInfo.publication,
   provisioningType echoing the request, deployInterval 600). The
   `$aws/rules/...` topic prefixes are optional: the bare
   `clip/message/devices/<did>` and `clip/provisioning/devices/<did>` work
   as well. Ours keeps LG's prefixes; the module publishes to them fine.
5. Nothing follows `completeProvisioning_ack` from the cloud side; the
   ack is a no-op. No HTTPS endpoint beyond `/route`, `/route/certificate`
   and `POST /device/:id/certificate` is involved.
6. The loop we saw (device sits in "connecting…", streams for a few
   minutes, undeploys, re-registers) is known with newer module firmware;
   a hard power-cycle has resolved it elsewhere.
7. D0211 status, for later: `EB`/`EC` records of `[flag][0x18]` + 24-byte
   body, class byte `32`, `32 D8 n` transition frame, about one update per
   minute. To compare with §5.3 once frames flow.

Adapter 0.1.3-1 (commit f7a0eb6) does 1, 2, 3 and resets the round on
`undeploy`. Built on taifun as pipeline `thinq-0.1.3`; being tried on the
box next.

## 11. With 0.1.3 on the box: the dishwasher streams [C]

Fresh enrolment 13:58:48 with the acks, the time answer and the start
packet in place:

```
13:58:48 preDeploy → completeProvisioning → completeProvisioning_ack
13:58:48 packet  AA 0E F0 ED 11 21 01 00 00 00 18 00 B5 BB          (our start packet)
13:58:48 device_packet AA 20 32 EB 00 18 01 00 00 03 35 05 00 03 35 00 00 1A 00 02 02 01 00 00 00 00 00 00 00 00 C1 BB
13:58:48 device_log  {"device_log_key":"registration_time","device_log_data":{"list":[{"st":0,"n":"PERF_softap_start",...
…        device_packet AA 37 32 31 … (identification)  → our ack AA 08 F0 00 31 04 82 BB
…        device_packet AA 3A 32 EC [00 18 <24 B>] [00 18 <24 B>] …   on every change
```

The module stayed connected (no undeploy in the window) and sent six
frames in four minutes while the appliance was switched off and on.

**D0211 status layout, now observed:** `32 EB` carries one block, `32 EC`
two; each block is `[flag 00][len 0x18]` + 24 bytes; in `EC` the previous
block comes first, the current last. Counting from the first body byte,
the §5.3 offsets fit: [0] state (01 while on, 04 after switching off),
[1] process 00, [3]/[4] initial time `03 35`, [5] course 05 (Eco, as
LG's vocabulary says), [7]/[8] remaining `03 35`, [11] flags 0x1A/0x12
(bit 0x08 toggled on one of the user's actions - to pin down), [12]
options 00, then `02 02 01` at [13..15]. Whether `35` is 53 minutes or
BCD 35 is to be read off the display. Adapter 0.1.4-1 decodes this.

Also new: `device_log` (`registration_time` with PERF_* timings) right
after provisioning - logged, not acked, no harm seen.

## 12. With 0.1.4: values in the UI, and the end of the day

Enrolment 14:51:22 (the previous try failed on `vsp` typed in lower case -
SSIDs are case-sensitive, the module reported `errSSID: "vsp"`). The UI
then showed `state`, `course`, `remainingTime 233 min`, `doorOpen` from
the first frame: `03 35` is 3 h 53 min, read as hour and minute bytes.
The course byte 05 shows as "Normal" from LG's vocabulary (§5.4b), the
§5.3 guess "Eco" was wrong for that value.

Flags at [11], observed: door open → `1A`, door closed → `12`: the door is
bit 0x08, not 0x02. Bits 0x02 and 0x10 were set in both states and are
not named yet. 0.1.5 (commit pending build) moves the door to 0x08.

Frames seen on closing the door: `EC` with state 04 / course 00 at
14:55:29, then `EC` with state 01 / course 05 / flags 12 at 14:55:37 -
the appliance seems to pass through 04 briefly.

Ten-minute mark: see the next entry. Open for tomorrow: whether the
undeploy at `deployInterval` still happens with acks in place (and if so,
try the power-cycle after registration), the names of flag bits 0x02 and
0x10, the identification record's meaning, and `device_log`. On the
phone: remember the home network name between runs.

## 13. Ten-minute mark passed, 15:05 [C]

Provisioned 14:51:22 with 0.1.4 (acks, time answer, start packet), the
module was still on the broker at 15:02:25 and 15:05:41, fourteen minutes
on, no `undeploy`, frames on every change. The round before (13:58, same
adapter behaviour) had undeployed at ten minutes, so one survival is not
yet a rule; watch the next enrolments. 0.1.5 (door bit 0x08) went on the
box at 15:01 during this session; the module's MQTT session is to
mosquitto and was not touched by the adapter reload.

State at the end of 2026-10-20: dishwasher DB365TXS enrolled with the box,
values in the UI, adapter 0.1.5-1 on the box and in
`build-artifacts/thinq-0.1.5` on taifun (not published to aptly), phone
app with saved network names on the iPhone (phi-magic-app changes after
f18d67c not yet committed).
