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
