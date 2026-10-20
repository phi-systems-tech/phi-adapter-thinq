#include "thinq_dishwasher.h"

#include <map>

namespace phicore::thinq::ipc {

namespace {

constexpr std::uint8_t kRecordState = 0xEB;
constexpr std::uint8_t kRecordChange = 0xEC;

std::string nameFrom(const std::map<int, const char *> &table, int value)
{
    const auto it = table.find(value);
    return it == table.end() ? std::string("UNKNOWN") : std::string(it->second);
}

int blockByte(const Bytes &block, int offset)
{
    return offset >= 0 && static_cast<std::size_t>(offset) < block.size() ? block[static_cast<std::size_t>(offset)]
                                                                         : 0;
}

} // namespace

bool isDishwasherKind(std::string_view kind)
{
    // LG's dishwasher model codes start with `D` and a digit (D0211, D0211S).
    return kind.size() >= 2 && kind[0] == 'D' && kind[1] >= '0' && kind[1] <= '9';
}

std::string dishwasherStateName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "POWEROFF"}, {1, "INITIAL"}, {2, "RUNNING"}, {3, "PAUSE"}, {4, "STANDBY"}, {5, "END"},
        {6, "POWERFAIL"},
    };
    return nameFrom(kNames, value);
}

std::string dishwasherProcessName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "NONE"}, {1, "RESERVED"}, {2, "RUNNING"}, {3, "RINSING"}, {4, "DRYING"}, {5, "END"},
        {6, "NIGHTDRY"}, {7, "CANCEL"},
    };
    return nameFrom(kNames, value);
}

std::string dishwasherCourseName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "NONE"}, {1, "AUTO"}, {2, "HEAVY"}, {3, "DELICATE"}, {4, "TURBO"}, {5, "NORMAL"},
        {6, "RINSE"}, {7, "REFRESH"}, {8, "EXPRESS"}, {9, "MACHINE_CLEAN"}, {10, "SHORT_MODE"},
        {11, "DOWNLOAD_CYCLE"}, {12, "ACCELERATED_MODE"}, {13, "WASHING_ONLY"},
    };
    return nameFrom(kNames, value);
}

std::string dishwasherErrorName(int value)
{
    static const std::map<int, const char *> kNames = {
        {0, "NO"}, {1, "HE"}, {2, "IE"}, {3, "OE"}, {4, "FE"}, {5, "TE"}, {6, "AE"}, {7, "EE"},
        {8, "LE"}, {9, "NE"}, {10, "BE"},
    };
    return nameFrom(kNames, value);
}

const DishwasherLayout &dishwasherLayoutD0211()
{
    static const DishwasherLayout kLayout = [] {
        DishwasherLayout l;
        l.name = "D0211 (hypothesis)";
        return l;
    }();
    return kLayout;
}

const DishwasherLayout *dishwasherLayoutFor(std::string_view kind)
{
    return isDishwasherKind(kind) ? &dishwasherLayoutD0211() : nullptr;
}

std::optional<DishwasherStatus> decodeDishwasherStatus(const Bytes &contents, const DishwasherLayout &layout)
{
    if (contents.size() < 2 || contents[0] != layout.classByte)
        return std::nullopt;
    const std::uint8_t record = contents[1];
    const Bytes payload(contents.begin() + 2, contents.end());

    Bytes block;
    DishwasherStatus s;
    if (record != kRecordState && record != kRecordChange)
        return std::nullopt;

    // The blocks of the record, bodies only. With headed blocks the length
    // byte says where each body ends; without, they are laid end to end.
    std::vector<Bytes> blocks;
    if (layout.headedBlocks) {
        std::size_t at = 0;
        while (at + 2 <= payload.size()) {
            const std::size_t len = payload[at + 1];
            if (len == 0 || at + 2 + len > payload.size())
                break;
            blocks.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(at + 2),
                                payload.begin() + static_cast<std::ptrdiff_t>(at + 2 + len));
            at += 2 + len;
        }
    } else {
        for (std::size_t at = 0; at + layout.blockLength <= payload.size(); at += layout.blockLength)
            blocks.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(at),
                                payload.begin() + static_cast<std::ptrdiff_t>(at + layout.blockLength));
    }
    if (blocks.empty())
        return std::nullopt;
    if (record == kRecordChange) {
        if (blocks.size() < 2)
            return std::nullopt;
        block = layout.currentBlockFirst ? blocks.front() : blocks.back();
        s.changed = true;
    } else {
        block = blocks.front();
    }
    if (block.size() < layout.blockLength)
        return std::nullopt;

    s.state = blockByte(block, layout.state);
    s.process = blockByte(block, layout.process);
    s.initialMinutes = blockByte(block, layout.initialHour) * 60 + blockByte(block, layout.initialMinute);
    s.remainMinutes = blockByte(block, layout.remainHour) * 60 + blockByte(block, layout.remainMinute);
    s.course = blockByte(block, layout.course);
    const int flags = blockByte(block, layout.flags);
    s.childLock = (flags & layout.childLockMask) != 0;
    s.doorOpen = (flags & layout.doorOpenMask) != 0;
    s.rinseRefill = (flags & layout.rinseRefillMask) != 0;
    s.saltRefill = (flags & layout.saltRefillMask) != 0;
    s.options = blockByte(block, layout.options);
    return s;
}

} // namespace phicore::thinq::ipc
