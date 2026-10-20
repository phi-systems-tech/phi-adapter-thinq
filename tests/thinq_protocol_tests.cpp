// Frames, envelope, decoders and the model, without a broker.

#include <phi/adapter/testing/check.h>

#include "thinq_dishwasher.h"
#include "thinq_envelope.h"
#include "thinq_frame.h"
#include "thinq_model.h"
#include "thinq_settings.h"
#include "thinq_washer.h"

#include <algorithm>
#include <optional>
#include <string>

using namespace phicore::thinq::ipc;
namespace v1 = phicore::adapter::v1;

namespace {

// The §5.1 test vector, whole frame, checksum 0x4F.
const char *const kVectorHex = "AA 16 F0 26 3A 03 FF 04 01 00 00 00 00 00 03 00 00 00 00 00 4F BB";

Bytes bytesOf(const char *hex)
{
    const std::optional<Bytes> out = hexToBytes(hex);
    return out ? *out : Bytes();
}

const v1::Channel *findChannel(const v1::ChannelList &channels, const char *id)
{
    for (const v1::Channel &c : channels) {
        if (c.externalId == id)
            return &c;
    }
    return nullptr;
}

bool hasChoice(const v1::Channel &channel, const char *value)
{
    return std::any_of(channel.choices.begin(), channel.choices.end(),
                       [&](const v1::AdapterConfigOption &o) { return o.value == value; });
}

bool writable(const v1::Channel &channel)
{
    return v1::hasFlag(channel.flags, v1::ChannelFlag::Writable);
}

/// A whole washer status frame of `length` bytes with a valid short checksum;
/// the caller fills the bytes between the header and the checksum first.
Bytes washerFrame(std::size_t length, std::uint8_t classByte)
{
    Bytes frame(length, 0x00);
    frame[0] = kFrameStart;
    frame[1] = static_cast<std::uint8_t>(length);
    frame[2] = classByte;
    return frame;
}

void seal(Bytes &frame)
{
    const Bytes covered(frame.begin(), frame.end() - 2);
    frame[frame.size() - 2] = checksum8(covered);
    frame.back() = kFrameEnd;
}

// ---- frame -----------------------------------------------------------------

void testChecksumOfSpecVector()
{
    // §5.1: the byte sum before the checksum is 0x11A, which reduces to 0x1A
    // and gives 0x4F after the xor with 0x55.
    const Bytes frame = bytesOf(kVectorHex);
    PHI_CHECK(frame.size() == 22);
    const Bytes covered(frame.begin(), frame.end() - 2);
    PHI_CHECK(checksum8(covered) == 0x4F);
    PHI_CHECK(checksum8({}) == 0x55);
}

void testCrc16Xmodem()
{
    // The standard check value of CRC-16/XMODEM, which pins poly, init and
    // the lack of reflection and final xor.
    const std::string text = "123456789";
    PHI_CHECK(crc16Xmodem(Bytes(text.begin(), text.end())) == 0x31C3);
    PHI_CHECK(crc16Xmodem({}) == 0x0000);
}

// The cloud's delivery ack: by type, by sequence for envelopes, never for
// status, heartbeat and the appliance's own acks.
void testCloudAck()
{
    // The D0211 identification record: class 32, type 31.
    const Bytes ident = bytesOf("32 31 02 01 53 41 41 34");
    const std::optional<Bytes> ack = cloudAckFor(ident);
    PHI_CHECK(ack.has_value());
    PHI_CHECK(*ack == bytesOf("F0 00 31 04"));
    const std::optional<Bytes> framed = encodeFrame(*ack);
    PHI_CHECK(framed.has_value());
    PHI_CHECK(bytesToHex(*framed) == "AA08F000310482BB");

    // An envelope carries its sequence number at 5..6.
    const Bytes envelope = bytesOf("20 0A 00 39 00 03 90 00 01 00 EB 00 27 00");
    const std::optional<Bytes> seqAck = cloudAckFor(envelope);
    PHI_CHECK(seqAck.has_value());
    PHI_CHECK(*seqAck == bytesOf("F0 00 0A 04 03 90"));
    // Too short an envelope is left alone, as the cloud leaves it.
    PHI_CHECK(!cloudAckFor(bytesOf("20 0A 00 39 00 03 90")).has_value());
    // A long frame's checksum byte does not count towards the header.
    PHI_CHECK(!cloudAckFor(bytesOf("20 0A 00 39 00 03 90 00 01 00 EB 00 27"), true).has_value());
    PHI_CHECK(cloudAckFor(bytesOf("20 0A 00 39 00 03 90 00 01 00 EB 00 27 00"), true).has_value());

    PHI_CHECK(!cloudAckFor(bytesOf("32 EB 01 02")).has_value());
    PHI_CHECK(!cloudAckFor(bytesOf("32 EC 01 02")).has_value());
    PHI_CHECK(!cloudAckFor(bytesOf("20 C3")).has_value());
    PHI_CHECK(!cloudAckFor(bytesOf("20 00 26")).has_value());
    PHI_CHECK(!cloudAckFor(bytesOf("20")).has_value());
}

void testAckAndTimesyncMessages()
{
    const std::string ack = ackMessage("u-1", "AA08F000310482BB", 7);
    PHI_CHECK(ack.find("\"cmd\":\"ack\"") != std::string::npos);
    PHI_CHECK(ack.find("\"data\":\"AA08F000310482BB\"") != std::string::npos);
    PHI_CHECK(ack.find("\"type\":1") != std::string::npos);
    PHI_CHECK(ack.find("\"did\":\"u-1\"") != std::string::npos);

    // 2026-10-20 13:05:07 UTC, a Tuesday: 26, 9 (October, 0-based), 20, 13, 5, 7, 2.
    const Bytes seven = timesyncBytes(1792501507);
    PHI_CHECK(seven == bytesOf("1A 09 14 0D 05 07 02"));
    const std::string sync = timesyncMessage("u-1", 8, 1792501507);
    PHI_CHECK(sync.find("\"cmd\":\"resp_timesync\"") != std::string::npos);
    PHI_CHECK(sync.find("\"data\":\"GgkUDQUHAg==\"") != std::string::npos);
}

void testEncodeSpecVector()
{
    // Encoding the contents of the §5.1 vector must give the vector back.
    const Bytes frame = bytesOf(kVectorHex);
    const Bytes contents(frame.begin() + 2, frame.end() - 2);
    const std::optional<Bytes> encoded = encodeFrame(contents);
    PHI_CHECK(encoded.has_value());
    PHI_CHECK(encoded && *encoded == frame);
    PHI_CHECK(encoded && (*encoded)[1] == 0x16);
}

void testEncodeTooLong()
{
    // 251 bytes of contents would make the length byte 0xFF, which is the
    // long-frame marker; the short form must refuse it, 250 is the last fit.
    PHI_CHECK(!encodeFrame(Bytes(251, 0x01)).has_value());
    const std::optional<Bytes> fits = encodeFrame(Bytes(250, 0x01));
    PHI_CHECK(fits.has_value());
    PHI_CHECK(fits && (*fits)[1] == 0xFE);
}

void testDecodeSpecVector()
{
    const std::optional<Frame> f = decodeFrame(bytesOf(kVectorHex));
    PHI_CHECK(f.has_value());
    PHI_CHECK(f && !f->longFrame);
    PHI_CHECK(f && f->longChecksumVerified);
    PHI_CHECK(f && f->contents.size() == 18);
    PHI_CHECK(f && f->contents.front() == 0xF0 && f->contents[1] == 0x26);
}

void testDecodeRejectsWrongChecksum()
{
    // One flipped bit in the checksum byte must not pass as a frame.
    Bytes frame = bytesOf(kVectorHex);
    frame[frame.size() - 2] ^= 0x01;
    PHI_CHECK(!decodeFrame(frame).has_value());

    // Nor must a corrupted payload byte with the old checksum.
    Bytes payload = bytesOf(kVectorHex);
    payload[5] ^= 0x10;
    PHI_CHECK(!decodeFrame(payload).has_value());
}

void testDecodeRejectsWrongLength()
{
    // The length byte counts the whole frame; a wrong one is not a frame
    // even when the checksum was computed over it.
    Bytes frame = bytesOf(kVectorHex);
    frame[1] = 0x15;
    frame[frame.size() - 2] = checksum8(Bytes(frame.begin(), frame.end() - 2));
    PHI_CHECK(!decodeFrame(frame).has_value());
}

void testDecodeRejectsMissingMarkers()
{
    Bytes noStart = bytesOf(kVectorHex);
    noStart[0] = 0xAB;
    PHI_CHECK(!decodeFrame(noStart).has_value());

    Bytes noEnd = bytesOf(kVectorHex);
    noEnd.back() = 0xBC;
    PHI_CHECK(!decodeFrame(noEnd).has_value());

    PHI_CHECK(!decodeFrame({}).has_value());
    PHI_CHECK(!decodeFrame({0xAA, 0xBB}).has_value());
}

void testLongFrameRoundTrip()
{
    // AA FF contents crc16 BB: the CRC covers everything before it.
    Bytes frame = {0xAA, 0xFF, 0x30, 0x0A, 0x01, 0x02, 0x03};
    const std::uint16_t crc = crc16Xmodem(frame);
    frame.push_back(static_cast<std::uint8_t>(crc >> 8));
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    frame.push_back(0xBB);

    const std::optional<Frame> f = decodeFrame(frame);
    PHI_CHECK(f.has_value());
    PHI_CHECK(f && f->longFrame);
    PHI_CHECK(f && f->longChecksumVerified);
    PHI_CHECK(f && f->contents == Bytes({0x30, 0x0A, 0x01, 0x02, 0x03}));
}

void testLongFrameWrongCrcIsReported()
{
    // The long-frame algorithm is a hypothesis, so a mismatch is not an
    // error: the contents come out and the flag says the CRC did not match.
    Bytes frame = {0xAA, 0xFF, 0x30, 0x0A, 0x01, 0x02, 0x03};
    const std::uint16_t crc = static_cast<std::uint16_t>(crc16Xmodem(frame) ^ 0x0001);
    frame.push_back(static_cast<std::uint8_t>(crc >> 8));
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    frame.push_back(0xBB);

    const std::optional<Frame> f = decodeFrame(frame);
    PHI_CHECK(f.has_value());
    PHI_CHECK(f && f->longFrame);
    PHI_CHECK(f && !f->longChecksumVerified);
    PHI_CHECK(f && f->contents == Bytes({0x30, 0x0A, 0x01, 0x02, 0x03}));
}

void testHexToBytes()
{
    PHI_CHECK(hexToBytes("AABB") == Bytes({0xAA, 0xBB}));
    PHI_CHECK(hexToBytes("aa bb 0c") == Bytes({0xAA, 0xBB, 0x0C}));
    PHI_CHECK(hexToBytes(" AA\tBB\n") == Bytes({0xAA, 0xBB}));
    PHI_CHECK(hexToBytes("").has_value() && hexToBytes("")->empty());

    // An odd digit count and anything that is not hex are rejected whole.
    PHI_CHECK(!hexToBytes("AAB").has_value());
    PHI_CHECK(!hexToBytes("AAZZ").has_value());
    PHI_CHECK(!hexToBytes("0x10").has_value());
    // A space inside a byte would silently re-pair the digits otherwise.
    PHI_CHECK(!hexToBytes("A A").has_value());
}

void testBytesToHexIsUpperCase()
{
    PHI_CHECK(bytesToHex({0xAA, 0x0b, 0xff, 0x00}) == "AA0BFF00");
    PHI_CHECK(bytesToHex({}).empty());
    const Bytes frame = bytesOf(kVectorHex);
    PHI_CHECK(hexToBytes(bytesToHex(frame)) == frame);
}

// ---- envelope --------------------------------------------------------------

void testUuidFromTopic()
{
    const std::string uuid = "6b0e3a52-1111-2222-3333-444455556666";
    PHI_CHECK(uuidFromTopic("clip/provisioning/devices/" + uuid) == uuid);
    PHI_CHECK(uuidFromTopic("$aws/rules/clip_http_action_rule/clip/devices/" + uuid + "/message") == uuid);
    PHI_CHECK(uuidFromTopic("$aws/rules/clip_provisioning_rule/clip/provisioning/devices/" + uuid) == uuid);
    PHI_CHECK(uuidFromTopic(messagePublication(uuid)) == uuid);
    PHI_CHECK(uuidFromTopic(provisioningPublication(uuid)) == uuid);
    PHI_CHECK(uuidFromTopic("clip/provisioning/devices/" + uuid + "/undeploy") == uuid);
    PHI_CHECK(uuidFromTopic("clip/other").empty());
    PHI_CHECK(uuidFromTopic("").empty());
}

void testUuidFromTopicIsNotDirectional()
{
    // The function only looks for the last "devices/" segment; it does not
    // check the prefix. The cloud-to-device topic therefore yields its uuid
    // too, which is harmless because the adapter never subscribes to it.
    PHI_CHECK(uuidFromTopic("lime/devices/x") == "x");
    PHI_CHECK(uuidFromTopic(deviceTopic("abc")) == "abc");
}

void testParsePreDeploy()
{
    const std::string payload =
        R"({"mid":945658,"did":"u-1","kind":"F_V8_F___W.B_2QEUK","cmd":"preDeploy","rssi":-56,"fs":"idle","type":0,)"
        R"("data":{"appInfo":{"modelName":"F_V8_F___W.B_2QEUK","softVer":"690409","appVersion":"clip_hna_v1.9.183",)"
        R"("modemType":"RTK_RTL8711am","countryCode":"PL","DeviceType":""},)"
        R"("platformInfo":{"provisioningKey":"F_V8_F___W.B_2QEUK"}}})";
    const std::optional<DeviceMessage> m = parseDeviceMessage(payload);
    PHI_CHECK(m.has_value());
    if (!m)
        return;
    PHI_CHECK(m->mid == 945658);
    PHI_CHECK(m->did == "u-1");
    PHI_CHECK(m->kind == "F_V8_F___W.B_2QEUK");
    PHI_CHECK(m->cmd == "preDeploy");
    PHI_CHECK(m->rssi == -56);
    PHI_CHECK(m->fs == "idle");
    PHI_CHECK(m->type == 0);
    PHI_CHECK(m->data.is_object() && m->data.contains("appInfo"));

    const ModuleInfo info = moduleInfoFrom(*m);
    PHI_CHECK(info.modelName == "F_V8_F___W.B_2QEUK");
    PHI_CHECK(info.softVer == "690409");
    PHI_CHECK(info.modemType == "RTK_RTL8711am");
    PHI_CHECK(info.appVersion == "clip_hna_v1.9.183");
    PHI_CHECK(info.countryCode == "PL");
    PHI_CHECK(info.deviceType.empty());
}

void testParseDevicePacket()
{
    const std::string payload =
        R"({"mid":7,"did":"u-1","kind":"D0211","cmd":"device_packet","rssi":-48,"fs":"idle","type":1,"data":"AABB"})";
    const std::optional<DeviceMessage> m = parseDeviceMessage(payload);
    PHI_CHECK(m.has_value());
    PHI_CHECK(m && m->cmd == "device_packet");
    PHI_CHECK(m && m->type == 1);
    PHI_CHECK(m && devicePacketHex(*m) == "AABB");
}

void testParseRejectsNonMessages()
{
    // A payload needs to be an object with a string cmd; everything else is
    // not ours and must not be half-parsed.
    PHI_CHECK(!parseDeviceMessage("").has_value());
    PHI_CHECK(!parseDeviceMessage("not json").has_value());
    PHI_CHECK(!parseDeviceMessage("[1,2]").has_value());
    PHI_CHECK(!parseDeviceMessage(R"({"did":"x"})").has_value());
    PHI_CHECK(!parseDeviceMessage(R"({"cmd":5})").has_value());
}

void testNullDataGivesNoInfoAndNoHex()
{
    // req_timesync and completeProvisioning_ack carry "data":null.
    const std::optional<DeviceMessage> m =
        parseDeviceMessage(R"({"mid":1,"did":"u","cmd":"req_timesync","rssi":-100,"fs":"idle","data":null,"type":1})");
    PHI_CHECK(m.has_value());
    PHI_CHECK(m && m->data.is_null());
    PHI_CHECK(m && m->rssi == -100);
    PHI_CHECK(m && devicePacketHex(*m).empty());
    PHI_CHECK(m && moduleInfoFrom(*m).modelName.empty());

    DeviceMessage number;
    number.data = 5;
    PHI_CHECK(devicePacketHex(number).empty());
    DeviceMessage object;
    object.data = Json::object();
    PHI_CHECK(devicePacketHex(object).empty());
}

void testCompleteProvisioningMessage()
{
    const std::string uuid = "6b0e3a52-aaaa";
    const Json m = parseObject(completeProvisioningMessage(uuid, "preDeploy", 1700263891876LL));
    PHI_CHECK(m["did"] == uuid);
    PHI_CHECK(m["mid"] == 1700263891876LL);
    PHI_CHECK(m["cmd"] == "completeProvisioning");
    PHI_CHECK(m["type"] == 0);
    PHI_CHECK(m["data"]["result"] == 0);
    PHI_CHECK(m["data"]["host"] == "message");
    PHI_CHECK(m["data"]["provisioningType"] == "preDeploy");
    PHI_CHECK(m["data"]["deployInterval"] == 600);
    PHI_CHECK(m["data"]["appInfo"]["host"] == "message");
    // §4.2 keeps LG's own $aws/rules topics so that a module can go back.
    PHI_CHECK(m["data"]["appInfo"]["publication"]["message"]
              == "$aws/rules/clip_http_action_rule/clip/devices/6b0e3a52-aaaa/message");
    PHI_CHECK(m["data"]["appInfo"]["publication"]["provisioning"]
              == "$aws/rules/clip_provisioning_rule/clip/provisioning/devices/6b0e3a52-aaaa");

    // Later connections repeat the handshake as "deploy" and say so.
    const Json d = parseObject(completeProvisioningMessage(uuid, "deploy", 2));
    PHI_CHECK(d["data"]["provisioningType"] == "deploy");
}

void testTopics()
{
    PHI_CHECK(deviceTopic("abc") == "lime/devices/abc");
    PHI_CHECK(messagePublication("abc") == "$aws/rules/clip_http_action_rule/clip/devices/abc/message");
    PHI_CHECK(provisioningPublication("abc")
              == "$aws/rules/clip_provisioning_rule/clip/provisioning/devices/abc");
}

void testPacketMessage()
{
    const Json m = parseObject(packetMessage("u-1", "AABB", 42));
    PHI_CHECK(m["did"] == "u-1");
    PHI_CHECK(m["mid"] == 42);
    PHI_CHECK(m["cmd"] == "packet");
    PHI_CHECK(m["type"] == 1);
    PHI_CHECK(m["data"] == "AABB");
}

// ---- washer ----------------------------------------------------------------

void testProgrammeBody()
{
    // 16 bytes with the options at index 10 and the tail at 11-15.
    Programme p;
    p.course = 0x05;
    p.spin = 0x07;
    p.temp = 0x01;
    p.rinse = 0x01;
    p.dry = 0;
    p.delayHours = 0;
    p.options = 0x03;
    const Bytes body = p.body();
    PHI_CHECK(body.size() == 16);
    PHI_CHECK(body == bytesOf("05 03 07 01 01 00 00 00 00 00 03 00 00 00 00 00"));

    p.delayHours = 4;
    p.dry = 2;
    p.tail = {0x20, 0x00, 0x01, 0x01, 0x00};
    const Bytes later = p.body();
    PHI_CHECK(later[5] == 0x02);
    PHI_CHECK(later[6] == 0x04);
    PHI_CHECK(later[10] == 0x03);
    PHI_CHECK(later[11] == 0x20 && later[13] == 0x01 && later[14] == 0x01);
}

void testWasherStartMatchesSpecVector()
{
    // The §5.1 test vector is itself a start of the AI wash (course 0x3A,
    // auto spin, 40 degrees); building it from a Programme must reproduce
    // its contents byte for byte, and framing it must give the whole vector.
    Programme p;
    p.course = 0x3A;
    p.spin = 0xFF;
    p.temp = 0x04;
    p.rinse = 0x01;
    p.options = 0x03;
    const Bytes start = washerStart(p);
    PHI_CHECK(start.size() == 18);
    PHI_CHECK(start == bytesOf("F0 26 3A 03 FF 04 01 00 00 00 00 00 03 00 00 00 00 00"));
    const std::optional<Bytes> frame = encodeFrame(start);
    PHI_CHECK(frame && *frame == bytesOf(kVectorHex));
}

void testWasherStartDuvet()
{
    // The duvet example of §5.2.1. The spec prints it with two more zero
    // bytes after the rinse than the 16-byte body table has; the body table
    // and the §5.1 vector agree with the header, so this follows them.
    Programme p;
    p.course = 0x05;
    p.spin = 0x07;
    p.temp = 0x01;
    p.rinse = 0x01;
    p.dry = 0;
    p.delayHours = 0;
    p.options = 0x03;
    PHI_CHECK(washerStart(p) == bytesOf("F0 26 05 03 07 01 01 00 00 00 00 00 03 00 00 00 00 00"));
}

void testWasherFixedCommands()
{
    PHI_CHECK(washerWake() == bytesOf("F0 2A 01 00"));
    PHI_CHECK(washerPowerOff() == bytesOf("F0 24 01 01 00"));
    PHI_CHECK(washerPause() == bytesOf("F0 24 04 01 00"));
    PHI_CHECK(washerStartDefault() == bytesOf("F0 24 05 01 00"));
}

void testWasherDownload()
{
    Programme p;
    p.course = 0x05;
    const Bytes download = washerDownload(p);
    PHI_CHECK(download.size() == 4 + 16);
    PHI_CHECK(download[0] == 0xF0 && download[1] == 0x25 && download[2] == 0x03 && download[3] == 0x15);
    PHI_CHECK(Bytes(download.begin() + 4, download.end()) == p.body());
    // Download and start carry the same body, which the start-from-standby
    // sequence of §5.2.1 relies on.
    const Bytes start = washerStart(p);
    PHI_CHECK(Bytes(start.begin() + 2, start.end()) == p.body());
}

void testWasherVocabulary()
{
    PHI_CHECK(washerStateName(0) == "POWEROFF");
    PHI_CHECK(washerStateName(6) == "RUNNING");
    PHI_CHECK(washerStateName(18) == "ERROR");
    // 5 is a hole in LG's enum and must not invent a name.
    PHI_CHECK(washerStateName(5) == "UNKNOWN");
    PHI_CHECK(washerStateName(-1) == "UNKNOWN");
    PHI_CHECK(washerSpinName(9) == "1200");
    PHI_CHECK(washerSpinName(255) == "MAX");
    PHI_CHECK(washerSpinName(100) == "UNKNOWN");
    PHI_CHECK(washerTempName(3) == "30");
    PHI_CHECK(washerTempName(1) == "COLD");
    PHI_CHECK(washerErrorName(0) == "NO");
    PHI_CHECK(washerSoilName(3) == "NORMAL");
    PHI_CHECK(washerRinseName(1) == "NORMAL");
    PHI_CHECK(washerDryLevelName(8) == "ECO");
}

void testWasherAcceptsStart()
{
    for (int state = 0; state <= 20; ++state)
        PHI_CHECK_MSG(washerAcceptsStart(state) == (state >= 1 && state <= 3), "state %d", state);
}

void testIsWasherKind()
{
    PHI_CHECK(isWasherKind("F_V8_F___W.B_2QEUK"));
    PHI_CHECK(isWasherKind("Y_V8_Y___W.B32QEUK"));
    PHI_CHECK(!isWasherKind("D0211"));
    PHI_CHECK(!isWasherKind(""));
    PHI_CHECK(!isWasherKind("RAC_056905_WW"));
}

void testWasherLayouts()
{
    PHI_CHECK(!washerLayoutFV8().verified);
    PHI_CHECK(washerLayoutFV8().frameLength == 80);
    PHI_CHECK(washerLayoutFor("F_V8_F___W.B_2QEUK") != nullptr);
    PHI_CHECK(washerLayoutFor("F_V8_F___W.B_2QEUK") == &washerLayoutFV8());
    // The Y column of the notes is not a layout we have a table for.
    PHI_CHECK(washerLayoutFor("Y_V8_Y___W.B32QEUK") == nullptr);
    PHI_CHECK(washerLayoutFor("D0211") == nullptr);
}

void testDecodeWasherStatus()
{
    Bytes frame = washerFrame(80, 0x20);
    frame[43] = 6;
    frame[44] = 1;
    frame[45] = 30;
    frame[46] = 2;
    frame[47] = 5;
    frame[48] = 5;
    frame[49] = 0;
    frame[51] = 9;
    frame[52] = 3;
    frame[53] = 1;
    frame[55] = 4;
    frame[58] = 0x02 | 0x40;
    frame[64] = 44;
    frame[71] = 0x01;
    frame[72] = 0x42;
    seal(frame);
    PHI_CHECK(decodeFrame(frame).has_value());

    const std::optional<WasherStatus> s = decodeWasherStatus(frame, washerLayoutFV8());
    PHI_CHECK(s.has_value());
    if (!s)
        return;
    PHI_CHECK(s->state == 6);
    PHI_CHECK(s->remainMinutes == 90);
    PHI_CHECK(s->initialMinutes == 125);
    PHI_CHECK(s->course == 5);
    PHI_CHECK(s->error == 0);
    PHI_CHECK(s->spin == 9);
    PHI_CHECK(s->temp == 3);
    PHI_CHECK(s->rinse == 1);
    PHI_CHECK(s->delayHours == 4);
    PHI_CHECK(s->cycleCount == 44);
    PHI_CHECK(s->energyWh == 322);
    // 0x02 arms remote start; 0x40 means the door is unlocked, hence not
    // locked; the child-lock-off bit 0x80 is clear, hence the lock is on.
    PHI_CHECK(s->remoteStart.has_value() && *s->remoteStart);
    PHI_CHECK(s->doorLocked.has_value() && !*s->doorLocked);
    PHI_CHECK(s->childLock.has_value() && *s->childLock);
}

void testDecodeWasherLockBitsInverted()
{
    // The opposite lock byte: no remote start, door not unlocked (so locked),
    // child lock switched off (so not locked).
    Bytes frame = washerFrame(80, 0x20);
    frame[58] = 0x80;
    seal(frame);
    const std::optional<WasherStatus> s = decodeWasherStatus(frame, washerLayoutFV8());
    PHI_CHECK(s.has_value());
    PHI_CHECK(s && s->remoteStart && !*s->remoteStart);
    PHI_CHECK(s && s->doorLocked && *s->doorLocked);
    PHI_CHECK(s && s->childLock && !*s->childLock);
}

void testDecodeWasherSleepMarker()
{
    // A zeroed frame is the sleep marker of §5.2.4 and decodes like any other.
    Bytes frame = washerFrame(80, 0x20);
    seal(frame);
    const std::optional<WasherStatus> s = decodeWasherStatus(frame, washerLayoutFV8());
    PHI_CHECK(s.has_value());
    PHI_CHECK(s && s->state == 0 && s->course == 0 && s->remainMinutes == 0);
}

void testDecodeWasherRejects()
{
    Bytes shortFrame = washerFrame(79, 0x20);
    seal(shortFrame);
    PHI_CHECK(!decodeWasherStatus(shortFrame, washerLayoutFV8()).has_value());

    Bytes longFrame = washerFrame(81, 0x20);
    seal(longFrame);
    PHI_CHECK(!decodeWasherStatus(longFrame, washerLayoutFV8()).has_value());

    // Class 30 is a dryer's; its frame must not be read as a washer's.
    Bytes dryer = washerFrame(80, 0x30);
    seal(dryer);
    PHI_CHECK(!decodeWasherStatus(dryer, washerLayoutFV8()).has_value());

    PHI_CHECK(!decodeWasherStatus({}, washerLayoutFV8()).has_value());

    // A layout without a frame length decodes nothing.
    WasherLayout empty;
    PHI_CHECK(!decodeWasherStatus(washerFrame(80, 0x20), empty).has_value());
}

// ---- dishwasher ------------------------------------------------------------

Bytes dishwasherBlock()
{
    Bytes block(26, 0x00);
    block[0] = 2;
    block[1] = 2;
    block[3] = 3;
    block[4] = 53;
    block[5] = 5;
    block[7] = 1;
    block[8] = 5;
    block[11] = 0x0E;
    block[12] = 0x21;
    return block;
}

void testDecodeDishwasherState()
{
    Bytes contents = {0x32, 0xEB};
    const Bytes block = dishwasherBlock();
    contents.insert(contents.end(), block.begin(), block.end());

    const std::optional<DishwasherStatus> s = decodeDishwasherStatus(contents, dishwasherLayoutD0211());
    PHI_CHECK(s.has_value());
    if (!s)
        return;
    PHI_CHECK(s->state == 2);
    PHI_CHECK(s->process == 2);
    PHI_CHECK(s->initialMinutes == 233);
    PHI_CHECK(s->remainMinutes == 65);
    PHI_CHECK(s->course == 5);
    PHI_CHECK(s->options == 0x21);
    // Flags 0x0E: door open, rinse aid and salt refill, but not the child lock.
    PHI_CHECK(s->doorOpen);
    PHI_CHECK(s->rinseRefill);
    PHI_CHECK(s->saltRefill);
    PHI_CHECK(!s->childLock);
    PHI_CHECK(!s->changed);
}

void testDecodeDishwasherChange()
{
    // `EC` carries the previous and the current block; for this layout the
    // first one is the current one, and the result says it was a change.
    Bytes current = dishwasherBlock();
    Bytes previous(26, 0x00);
    previous[0] = 1;
    previous[5] = 1;
    Bytes contents = {0x32, 0xEC};
    contents.insert(contents.end(), current.begin(), current.end());
    contents.insert(contents.end(), previous.begin(), previous.end());

    const std::optional<DishwasherStatus> s = decodeDishwasherStatus(contents, dishwasherLayoutD0211());
    PHI_CHECK(s.has_value());
    PHI_CHECK(s && s->changed);
    PHI_CHECK(s && s->state == 2 && s->course == 5);

    // A layout whose current block comes last reads the other one.
    DishwasherLayout last = dishwasherLayoutD0211();
    last.currentBlockFirst = false;
    const std::optional<DishwasherStatus> t = decodeDishwasherStatus(contents, last);
    PHI_CHECK(t.has_value());
    PHI_CHECK(t && t->state == 1 && t->course == 1);

    // One block is not enough for a change record.
    Bytes half = {0x32, 0xEC};
    half.insert(half.end(), current.begin(), current.end());
    PHI_CHECK(!decodeDishwasherStatus(half, dishwasherLayoutD0211()).has_value());
}

void testDecodeDishwasherRejects()
{
    Bytes wrongClass = {0x30, 0xEB};
    const Bytes block = dishwasherBlock();
    wrongClass.insert(wrongClass.end(), block.begin(), block.end());
    PHI_CHECK(!decodeDishwasherStatus(wrongClass, dishwasherLayoutD0211()).has_value());

    Bytes shortBlock = {0x32, 0xEB};
    shortBlock.insert(shortBlock.end(), block.begin(), block.begin() + 25);
    PHI_CHECK(!decodeDishwasherStatus(shortBlock, dishwasherLayoutD0211()).has_value());

    Bytes otherRecord = {0x32, 0xD8};
    otherRecord.insert(otherRecord.end(), block.begin(), block.end());
    PHI_CHECK(!decodeDishwasherStatus(otherRecord, dishwasherLayoutD0211()).has_value());

    PHI_CHECK(!decodeDishwasherStatus({}, dishwasherLayoutD0211()).has_value());
    PHI_CHECK(!decodeDishwasherStatus({0x32}, dishwasherLayoutD0211()).has_value());
}

// The D0211's own frames, 2026-10-20: a state record right after the start
// packet, and change records while it was switched off and on.
void testDishwasherRealFrames()
{
    const DishwasherLayout &layout = dishwasherLayoutD0211();
    const std::optional<Frame> state = decodeFrame(
        bytesOf("AA2032EB001801000003350500033500001A000202010000000000000000C1BB"));
    PHI_CHECK(state.has_value());
    const std::optional<DishwasherStatus> s = decodeDishwasherStatus(state->contents, layout);
    PHI_CHECK(s.has_value());
    PHI_CHECK(s->state == 0x01);
    PHI_CHECK(s->process == 0x00);
    PHI_CHECK(s->course == 0x05);
    PHI_CHECK(s->initialMinutes == 3 * 60 + 0x35);
    PHI_CHECK(s->remainMinutes == 3 * 60 + 0x35);
    PHI_CHECK(!s->changed);

    // Switched off: previous block first (state 01), current last (04).
    const std::optional<Frame> change = decodeFrame(bytesOf(
        "AA3A32EC001801000003350500033500001A000202010000000000000000001804000003350000033500001A0002020100000000000000000FBB"));
    PHI_CHECK(change.has_value());
    const std::optional<DishwasherStatus> c = decodeDishwasherStatus(change->contents, layout);
    PHI_CHECK(c.has_value());
    PHI_CHECK(c->changed);
    PHI_CHECK(c->state == 0x04);
    PHI_CHECK(c->course == 0x00);

    // A change record with only one block is not a change.
    PHI_CHECK(!decodeDishwasherStatus(bytesOf("32 EC 00 18 01 00 00 03 35 05 00 03 35 00 00 1A 00 02 02 01 00 00 00 00 00 00 00 00"), layout).has_value());
    // A truncated block is refused.
    PHI_CHECK(!decodeDishwasherStatus(bytesOf("32 EB 00 18 01 00 00"), layout).has_value());
}

void testDishwasherVocabulary()
{
    PHI_CHECK(dishwasherCourseName(5) == "NORMAL");
    PHI_CHECK(dishwasherCourseName(1) == "AUTO");
    PHI_CHECK(dishwasherCourseName(13) == "WASHING_ONLY");
    PHI_CHECK(dishwasherCourseName(99) == "UNKNOWN");
    PHI_CHECK(dishwasherStateName(0) == "POWEROFF");
    PHI_CHECK(dishwasherStateName(2) == "RUNNING");
    PHI_CHECK(dishwasherProcessName(4) == "DRYING");
    PHI_CHECK(dishwasherErrorName(0) == "NO");
    PHI_CHECK(dishwasherStateName(50) == "UNKNOWN");
}

void testIsDishwasherKind()
{
    PHI_CHECK(isDishwasherKind("D0211"));
    PHI_CHECK(isDishwasherKind("D0211S"));
    PHI_CHECK(!isDishwasherKind("F_V8_F___W.B_2QEUK"));
    PHI_CHECK(!isDishwasherKind("D"));
    PHI_CHECK(!isDishwasherKind("DX"));
    PHI_CHECK(dishwasherLayoutFor("D0211") == &dishwasherLayoutD0211());
    PHI_CHECK(dishwasherLayoutFor("F_V8_F___W.B_2QEUK") == nullptr);
    PHI_CHECK(!dishwasherLayoutD0211().verified);
}

// ---- model -----------------------------------------------------------------

void testFamilyFor()
{
    PHI_CHECK(familyFor("F_V8_F___W.B_2QEUK") == Family::Washer);
    PHI_CHECK(familyFor("D0211") == Family::Dishwasher);
    PHI_CHECK(familyFor("RAC_056905_WW") == Family::Unknown);
    PHI_CHECK(std::string(familyName(Family::Washer)) == "washer");
    PHI_CHECK(std::string(familyName(Family::Dishwasher)) == "dishwasher");
    PHI_CHECK(std::string(familyName(Family::Unknown)) == "appliance");
}

void testWasherChannelsWithoutDecoder()
{
    // With an unverified decoder a start would send bytes read from a guessed
    // offset, so the operation selector must not offer it; the delayed end is
    // part of that programme and is read-only as well.
    const v1::ChannelList channels = buildChannels(Family::Washer, false);
    for (const char *id : {kChannelConnectivity, kChannelLastFrame, kChannelState, kChannelSignal, kChannelPower})
        PHI_CHECK_MSG(findChannel(channels, id) != nullptr, "missing %s", id);

    const v1::Channel *operation = findChannel(channels, kChannelOperation);
    PHI_CHECK(operation != nullptr);
    PHI_CHECK(operation && !hasChoice(*operation, kOperationStart));
    PHI_CHECK(operation && hasChoice(*operation, kOperationPause));

    const v1::Channel *delay = findChannel(channels, kChannelDelayEnd);
    PHI_CHECK(delay != nullptr);
    PHI_CHECK(delay && !writable(*delay));

    const v1::Channel *power = findChannel(channels, kChannelPower);
    PHI_CHECK(power && writable(*power));
}

void testWasherChannelsWithDecoder()
{
    const v1::ChannelList channels = buildChannels(Family::Washer, true);
    for (const char *id : {kChannelConnectivity, kChannelLastFrame, kChannelState})
        PHI_CHECK_MSG(findChannel(channels, id) != nullptr, "missing %s", id);
    const v1::Channel *operation = findChannel(channels, kChannelOperation);
    PHI_CHECK(operation && hasChoice(*operation, kOperationStart));
    PHI_CHECK(operation && hasChoice(*operation, kOperationPause));
    const v1::Channel *delay = findChannel(channels, kChannelDelayEnd);
    PHI_CHECK(delay && writable(*delay));

    // The state channel offers LG's vocabulary and leaves out the holes.
    const v1::Channel *state = findChannel(channels, kChannelState);
    PHI_CHECK(state && hasChoice(*state, "POWEROFF") && hasChoice(*state, "ERROR"));
    PHI_CHECK(state && !hasChoice(*state, "UNKNOWN"));
}

void testDishwasherChannels()
{
    const v1::ChannelList channels = buildChannels(Family::Dishwasher, false);
    for (const char *id : {kChannelConnectivity, kChannelLastFrame, kChannelState, kChannelProcess, kChannelDoorOpen,
                           kChannelSaltRefill, kChannelRinseAidRefill, kChannelChildLock})
        PHI_CHECK_MSG(findChannel(channels, id) != nullptr, "missing %s", id);
    // LG offers no power for this model, so the appliance stays read-only.
    PHI_CHECK(findChannel(channels, kChannelPower) == nullptr);
    PHI_CHECK(findChannel(channels, kChannelOperation) == nullptr);
    for (const v1::Channel &c : channels)
        PHI_CHECK_MSG(!writable(c), "%s is writable", c.externalId.c_str());
}

void testUnknownFamilyHasOnlyCommonChannels()
{
    const v1::ChannelList channels = buildChannels(Family::Unknown, false);
    PHI_CHECK(findChannel(channels, kChannelConnectivity) != nullptr);
    PHI_CHECK(findChannel(channels, kChannelLastFrame) != nullptr);
    PHI_CHECK(findChannel(channels, kChannelState) == nullptr);
}

void testBuildDevice()
{
    const v1::Device d = buildDevice("u-1", "Washer", "F_V8_F___W.B_2QEUK", "690409");
    PHI_CHECK(d.externalId == "u-1");
    PHI_CHECK(d.deviceClass == v1::DeviceClass::Sensor);
    PHI_CHECK(d.manufacturer == "LG Electronics");
    PHI_CHECK(d.name == "Washer");
    PHI_CHECK(d.model == "F_V8_F___W.B_2QEUK");
    PHI_CHECK(d.firmware == "690409");
}

// ---- settings --------------------------------------------------------------

void testSettingsFromMeta()
{
    v1::Adapter adapter;
    const Json meta = {{"brokerHost", "10.0.0.2"}, {"brokerPort", "1884"}, {"username", "u"}, {"password", "p"}};
    const ThinqSettings s = settingsFromAdapter(adapter, meta);
    PHI_CHECK(s.brokerHost == "10.0.0.2");
    // A form saves the port as a string; it must not read as 0.
    PHI_CHECK(s.brokerPort == 1884);
    PHI_CHECK(s.username == "u");
    PHI_CHECK(s.password == "p");
    PHI_CHECK(s.retryIntervalMs == kDefaultRetryIntervalMs);
}

void testSettingsDefaults()
{
    v1::Adapter adapter;
    const ThinqSettings s = settingsFromAdapter(adapter, Json::object());
    PHI_CHECK(s.brokerHost == kDefaultBrokerHost);
    PHI_CHECK(s.brokerPort == kDefaultBrokerPort);
    PHI_CHECK(s.username.empty() && s.password.empty());
    PHI_CHECK(s.retryIntervalMs == kDefaultRetryIntervalMs);
}

void testSettingsAdapterFieldsAndBadValues()
{
    // The adapter's own fields are the fallback; meta wins when it has a
    // value, and an out-of-range port or a too short retry falls back.
    v1::Adapter adapter;
    adapter.host = " broker.lan ";
    adapter.port = 1885;
    const ThinqSettings a = settingsFromAdapter(adapter, Json::object());
    PHI_CHECK(a.brokerHost == "broker.lan");
    PHI_CHECK(a.brokerPort == 1885);

    const Json bad = {{"brokerPort", 70000}, {"retryIntervalMs", 10}};
    const ThinqSettings b = settingsFromAdapter(adapter, bad);
    PHI_CHECK(b.brokerPort == kDefaultBrokerPort);
    PHI_CHECK(b.retryIntervalMs == kDefaultRetryIntervalMs);

    ThinqSettings c = a;
    PHI_CHECK(a.sameBroker(c));
    c.password = "x";
    PHI_CHECK(!a.sameBroker(c));
}

} // namespace

int main()
{
    testChecksumOfSpecVector();
    testCrc16Xmodem();
    testCloudAck();
    testAckAndTimesyncMessages();
    testEncodeSpecVector();
    testEncodeTooLong();
    testDecodeSpecVector();
    testDecodeRejectsWrongChecksum();
    testDecodeRejectsWrongLength();
    testDecodeRejectsMissingMarkers();
    testLongFrameRoundTrip();
    testLongFrameWrongCrcIsReported();
    testHexToBytes();
    testBytesToHexIsUpperCase();

    testUuidFromTopic();
    testUuidFromTopicIsNotDirectional();
    testParsePreDeploy();
    testParseDevicePacket();
    testParseRejectsNonMessages();
    testNullDataGivesNoInfoAndNoHex();
    testCompleteProvisioningMessage();
    testTopics();
    testPacketMessage();

    testProgrammeBody();
    testWasherStartMatchesSpecVector();
    testWasherStartDuvet();
    testWasherFixedCommands();
    testWasherDownload();
    testWasherVocabulary();
    testWasherAcceptsStart();
    testIsWasherKind();
    testWasherLayouts();
    testDecodeWasherStatus();
    testDecodeWasherLockBitsInverted();
    testDecodeWasherSleepMarker();
    testDecodeWasherRejects();

    testDecodeDishwasherState();
    testDecodeDishwasherChange();
    testDecodeDishwasherRejects();
    testDishwasherRealFrames();
    testDishwasherVocabulary();
    testIsDishwasherKind();

    testFamilyFor();
    testWasherChannelsWithoutDecoder();
    testWasherChannelsWithDecoder();
    testDishwasherChannels();
    testUnknownFamilyHasOnlyCommonChannels();
    testBuildDevice();

    testSettingsFromMeta();
    testSettingsDefaults();
    testSettingsAdapterFieldsAndBadValues();

    return phi::testing::report("thinq_protocol_tests");
}
