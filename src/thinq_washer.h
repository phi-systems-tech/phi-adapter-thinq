#pragma once

// The washer: its commands (docs/thinq-protocol.md §5.2.1, from the protocol
// notes), its vocabulary (§5.4b, from LG's own modelJson, authoritative for
// values) and a status decoder driven by an offset table (§5.2.3).
//
// The table for our `F_V8_*` washer is a hypothesis. Nothing a user acts on
// may depend on it until a capture has confirmed it, so WasherLayout carries
// a `verified` flag and the instance says so in the device's channels.

#include "thinq_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace phicore::thinq::ipc {

/// `F_V8_F___W.B_2QEUK` and its siblings: second segment `V<n>` is the
/// platform generation.
bool isWasherKind(std::string_view kind);

// ---- vocabulary [L] ------------------------------------------------------

/// LG's `state` enum: POWEROFF 0 … ERROR 18. Unknown values read "UNKNOWN".
std::string washerStateName(int value);
std::string washerSpinName(int value);
std::string washerTempName(int value);
std::string washerSoilName(int value);
std::string washerRinseName(int value);
std::string washerDryLevelName(int value);
std::string washerErrorName(int value);

/// The programme-selecting states: a start is only accepted in one of them.
bool washerAcceptsStart(int state);

// ---- commands [W] --------------------------------------------------------

/// The 16-byte body of `download` and `start`. Defaults are the notes' "AI
/// wash, auto spin, default temperature" example minus the course, which has
/// no default - the caller must set one.
struct Programme {
    std::uint8_t course = 0;
    std::uint8_t spin = 0xFF;
    std::uint8_t temp = 0x03;
    std::uint8_t rinse = 0x01;
    std::uint8_t dry = 0x00;
    std::uint8_t delayHours = 0x00;
    std::uint8_t options = 0x03;
    std::array<std::uint8_t, 5> tail{0, 0, 0, 0, 0};

    [[nodiscard]] Bytes body() const;
};

/// Frame contents (between `len` and `ck`), not whole frames.
Bytes washerWake();
Bytes washerPowerOff();
Bytes washerPause();
Bytes washerStartDefault();
Bytes washerDownload(const Programme &programme);
Bytes washerStart(const Programme &programme);

// ---- status ----------------------------------------------------------------

/// Where each field lives in a status frame. Offsets count from the frame's
/// first byte (`AA` = 0), which is how the notes index their hex dumps. -1
/// means "not in this layout".
struct WasherLayout {
    const char *name = "";
    bool verified = false;
    /// The status frame's total length; other lengths are not status.
    std::size_t frameLength = 0;
    int state = -1;
    int remainHour = -1;
    int remainMinute = -1;
    int initialHour = -1;
    int initialMinute = -1;
    int course = -1;
    int error = -1;
    int soil = -1;
    int spin = -1;
    int temp = -1;
    int rinse = -1;
    int dry = -1;
    int delayHour = -1;
    int options = -1;
    /// The lock byte and what its bits mean in this layout.
    int lock = -1;
    std::uint8_t remoteStartMask = 0;
    std::uint8_t doorUnlockedMask = 0;
    std::uint8_t childLockOffMask = 0;
    int cycleCount = -1;
    /// Big-endian Wh of the running cycle.
    int energyHigh = -1;
    int energyLow = -1;
};

/// §5.2.3: the 80-byte frame of the `F_V8_*` washers, offsets as reported
/// from a third-party table. Unverified.
const WasherLayout &washerLayoutFV8();

/// The layout for a model code, or nullptr when none is known.
const WasherLayout *washerLayoutFor(std::string_view kind);

struct WasherStatus {
    int state = 0;
    int remainMinutes = 0;
    int initialMinutes = 0;
    int course = 0;
    int error = 0;
    int soil = 0;
    int spin = 0;
    int temp = 0;
    int rinse = 0;
    int dry = 0;
    int delayHours = 0;
    std::optional<bool> remoteStart;
    std::optional<bool> doorLocked;
    std::optional<bool> childLock;
    int cycleCount = 0;
    int energyWh = 0;
};

/**
 * @brief Reads a status frame by `layout`.
 *
 * `frame` is the whole frame, `AA` first. nullopt when the length is not the
 * layout's or the class byte is not a washer's (`20`). A zeroed frame
 * (state 0, course 0) is the sleep marker of §5.2.4 and decodes like any
 * other.
 */
std::optional<WasherStatus> decodeWasherStatus(const Bytes &frame, const WasherLayout &layout);

} // namespace phicore::thinq::ipc
