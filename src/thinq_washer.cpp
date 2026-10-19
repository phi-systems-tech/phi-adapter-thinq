#include "thinq_washer.h"

#include <map>

namespace phicore::thinq::ipc {

namespace {

constexpr std::uint8_t kWasherClass = 0x20;

std::string nameFrom(const std::map<int, const char *> &table, int value)
{
    const auto it = table.find(value);
    return it == table.end() ? std::string("UNKNOWN") : std::string(it->second);
}

int byteAt(const Bytes &frame, int offset)
{
    return offset >= 0 && static_cast<std::size_t>(offset) < frame.size() ? frame[static_cast<std::size_t>(offset)]
                                                                         : 0;
}

std::optional<bool> bitAt(const Bytes &frame, int offset, std::uint8_t mask)
{
    if (offset < 0 || mask == 0)
        return std::nullopt;
    return (byteAt(frame, offset) & mask) != 0;
}

} // namespace

bool isWasherKind(std::string_view kind)
{
    // `F_V8_F___W.B_2QEUK`, `Y_V8_Y___W.B32QEUK`: a letter, `_V`, a digit.
    return kind.size() > 4 && kind[1] == '_' && kind[2] == 'V' && kind[3] >= '0' && kind[3] <= '9';
}

std::string washerStateName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "POWEROFF"}, {1, "INITIAL"}, {2, "PAUSE"}, {3, "RESERVED"}, {4, "DETECTING"},
        {6, "RUNNING"}, {7, "RINSING"}, {8, "SPINNING"}, {9, "DRYING"}, {10, "END"},
        {11, "COOLDOWN"}, {12, "RINSEHOLD"}, {14, "WASH_REFRESHING"}, {15, "STEAMSOFTENING"},
        {16, "DEMO"}, {18, "ERROR"},
    };
    return nameFrom(kNames, value);
}

std::string washerSpinName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "NOT_SELECTED"}, {1, "NO_SPIN"}, {2, "400"}, {3, "600"}, {4, "700"}, {5, "800"},
        {6, "900"}, {7, "1000"}, {8, "1100"}, {9, "1200"}, {10, "1400"}, {11, "1600"}, {255, "MAX"},
    };
    return nameFrom(kNames, value);
}

std::string washerTempName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "NO_TEMP"}, {1, "COLD"}, {2, "20"}, {3, "30"}, {4, "40"}, {5, "50"}, {6, "60"}, {7, "95"},
    };
    return nameFrom(kNames, value);
}

std::string washerSoilName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "NONE"}, {1, "TURBO_WASH"}, {2, "TIMESAVE"}, {3, "NORMAL"}, {4, "INTENSIVE"},
    };
    return nameFrom(kNames, value);
}

std::string washerRinseName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "NONE"}, {1, "NORMAL"}, {2, "PLUS"}, {3, "PLUSPLUS"}, {4, "NORMAL_HOLD"}, {5, "PLUS_HOLD"},
    };
    return nameFrom(kNames, value);
}

std::string washerDryLevelName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "NOT_SELECTED"}, {1, "NO_DRYLEVEL"}, {2, "NORMAL"}, {3, "30"}, {4, "60"}, {5, "90"},
        {6, "120"}, {7, "150"}, {8, "ECO"}, {9, "VERY"}, {10, "IRON"}, {11, "LOW"}, {12, "ENERGY"},
        {13, "SPEED"}, {14, "COOLING"},
    };
    return nameFrom(kNames, value);
}

std::string washerErrorName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "NO"}, {1, "DE2"}, {2, "DE1"}, {3, "IE"}, {4, "OE"}, {5, "UE"}, {6, "FE"}, {7, "PE"},
        {8, "TE"}, {9, "LE"}, {11, "DHE"}, {12, "PF"}, {13, "FF"}, {14, "DCE"}, {15, "AE"},
        {16, "EE"}, {17, "PS"}, {18, "DE4"}, {19, "VS"},
    };
    return nameFrom(kNames, value);
}

bool washerAcceptsStart(int state)
{
    // INITIAL is "Ready" on the panel; PAUSE and RESERVED take a start as
    // resume. Everything else is running, finished, asleep or broken.
    return state == 1 || state == 2 || state == 3;
}

Bytes Programme::body() const
{
    Bytes out = {course, 0x03, spin, temp, rinse, dry, delayHours, 0x00, 0x00, 0x00, options};
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

Bytes washerWake()
{
    return {0xF0, 0x2A, 0x01, 0x00};
}

Bytes washerPowerOff()
{
    return {0xF0, 0x24, 0x01, 0x01, 0x00};
}

Bytes washerPause()
{
    return {0xF0, 0x24, 0x04, 0x01, 0x00};
}

Bytes washerStartDefault()
{
    return {0xF0, 0x24, 0x05, 0x01, 0x00};
}

Bytes washerDownload(const Programme &programme)
{
    Bytes out = {0xF0, 0x25, 0x03, 0x15};
    const Bytes body = programme.body();
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes washerStart(const Programme &programme)
{
    Bytes out = {0xF0, 0x26};
    const Bytes body = programme.body();
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

const WasherLayout &washerLayoutFV8()
{
    static const WasherLayout kLayout = [] {
        WasherLayout l;
        l.name = "F_V8 (hypothesis)";
        l.verified = false;
        l.frameLength = 80;
        l.state = 43;
        l.remainHour = 44;
        l.remainMinute = 45;
        l.initialHour = 46;
        l.initialMinute = 47;
        l.course = 48;
        l.error = 49;
        l.soil = 50;
        l.spin = 51;
        l.temp = 52;
        l.rinse = 53;
        l.dry = 54;
        l.delayHour = 55;
        l.options = 57;
        l.lock = 58;
        l.remoteStartMask = 0x02;
        l.doorUnlockedMask = 0x40;
        l.childLockOffMask = 0x80;
        l.cycleCount = 64;
        l.energyHigh = 71;
        l.energyLow = 72;
        return l;
    }();
    return kLayout;
}

const WasherLayout *washerLayoutFor(std::string_view kind)
{
    if (kind.rfind("F_V8_", 0) == 0)
        return &washerLayoutFV8();
    return nullptr;
}

std::optional<WasherStatus> decodeWasherStatus(const Bytes &frame, const WasherLayout &layout)
{
    if (layout.frameLength == 0 || frame.size() != layout.frameLength)
        return std::nullopt;
    if (frame.size() < 3 || frame[2] != kWasherClass)
        return std::nullopt;

    WasherStatus s;
    s.state = byteAt(frame, layout.state);
    s.remainMinutes = byteAt(frame, layout.remainHour) * 60 + byteAt(frame, layout.remainMinute);
    s.initialMinutes = byteAt(frame, layout.initialHour) * 60 + byteAt(frame, layout.initialMinute);
    s.course = byteAt(frame, layout.course);
    s.error = byteAt(frame, layout.error);
    s.soil = byteAt(frame, layout.soil);
    s.spin = byteAt(frame, layout.spin);
    s.temp = byteAt(frame, layout.temp);
    s.rinse = byteAt(frame, layout.rinse);
    s.dry = byteAt(frame, layout.dry);
    s.delayHours = byteAt(frame, layout.delayHour);
    s.remoteStart = bitAt(frame, layout.lock, layout.remoteStartMask);
    if (const std::optional<bool> unlocked = bitAt(frame, layout.lock, layout.doorUnlockedMask))
        s.doorLocked = !*unlocked;
    if (const std::optional<bool> off = bitAt(frame, layout.lock, layout.childLockOffMask))
        s.childLock = !*off;
    s.cycleCount = byteAt(frame, layout.cycleCount);
    s.energyWh = (byteAt(frame, layout.energyHigh) << 8) | byteAt(frame, layout.energyLow);
    return s;
}

} // namespace phicore::thinq::ipc
