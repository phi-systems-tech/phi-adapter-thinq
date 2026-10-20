#pragma once

// The dishwasher D0211: vocabulary from LG's modelJson (§5.4b, authoritative
// for names and the course bytes) and a status decoder on the §5.3
// hypothesis. LG itself offers no start, stop or power for this model, so
// there are no commands here; the appliance stays read-only.

#include "thinq_frame.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace phicore::thinq::ipc {

bool isDishwasherKind(std::string_view kind);

/// State and process are listed by LG in order and without numbers;
/// "value = position" is the working assumption (§5.4b).
std::string dishwasherStateName(int value);
std::string dishwasherProcessName(int value);
/// The course bytes LG's ConvertingRule gives: 0 NONE … 13 WASHING_ONLY.
std::string dishwasherCourseName(int value);
std::string dishwasherErrorName(int value);

/// Offsets within one status block; the frame is `<class> EB <block>` or
/// `<class> EC <block><block>`.
struct DishwasherLayout {
    const char *name = "";
    bool verified = false;
    /// Each block comes as `[flag][len]` + `len` bytes; the status offsets
    /// below count from the first body byte. Observed on the D0211: `len`
    /// is 0x18, and an `EC` record carries two such blocks, the previous
    /// first and the current last.
    bool headedBlocks = true;
    std::uint8_t classByte = 0x32;
    std::size_t blockLength = 24;
    /// Which of the two `EC` blocks is the current one.
    bool currentBlockFirst = false;
    int state = 0;
    int process = 1;
    int initialHour = 3;
    int initialMinute = 4;
    int course = 5;
    int remainHour = 7;
    int remainMinute = 8;
    int flags = 11;
    std::uint8_t childLockMask = 0x01;
    std::uint8_t doorOpenMask = 0x02;
    std::uint8_t rinseRefillMask = 0x04;
    std::uint8_t saltRefillMask = 0x08;
    int options = 12;
};

/// §5.3 as written. Unverified.
const DishwasherLayout &dishwasherLayoutD0211();
const DishwasherLayout *dishwasherLayoutFor(std::string_view kind);

struct DishwasherStatus {
    int state = 0;
    int process = 0;
    int initialMinutes = 0;
    int remainMinutes = 0;
    int course = 0;
    bool childLock = false;
    bool doorOpen = false;
    bool rinseRefill = false;
    bool saltRefill = false;
    int options = 0;
    /// True when the frame was a change notification (`EC`), which carries
    /// the previous block too.
    bool changed = false;
};

/// `contents` is the frame between `len` and `ck`. nullopt when the class
/// byte, the record type or the length do not fit the layout.
std::optional<DishwasherStatus> decodeDishwasherStatus(const Bytes &contents, const DishwasherLayout &layout);

} // namespace phicore::thinq::ipc
