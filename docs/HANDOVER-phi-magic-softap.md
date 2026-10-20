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
