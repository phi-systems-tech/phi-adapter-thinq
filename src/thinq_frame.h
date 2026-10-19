#pragma once

// The appliance frame ("AABB"): what travels as hex in `device_packet.data`
// and `packet.data`, see docs/thinq-protocol.md §5.1 in phi-core.
//
//   AA | len | contents … | ck | BB
//
// `len` is the whole frame, AA and BB included; `ck` is the byte sum of
// everything before it, xor 0x55. A long frame says `FF` for the length and
// ends in two checksum bytes; what they are is a hypothesis (CRC-16/XMODEM
// over everything before them) until a capture confirms it, and decodeFrame
// says so in `longChecksumVerified`.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phicore::thinq::ipc {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint8_t kFrameStart = 0xAA;
inline constexpr std::uint8_t kFrameEnd = 0xBB;
inline constexpr std::uint8_t kLongFrameLength = 0xFF;

struct Frame {
    Bytes contents;
    bool longFrame = false;
    /// False on a long frame whose two checksum bytes do not match the
    /// CRC hypothesis; the contents are still handed out, the caller decides.
    bool longChecksumVerified = true;
};

/// The one-byte checksum of §5.1 over `bytes` (the frame up to, not
/// including, the checksum itself).
std::uint8_t checksum8(const Bytes &bytes);

/// CRC-16/XMODEM (poly 0x1021, init 0, no reflection, no xorout).
std::uint16_t crc16Xmodem(const Bytes &bytes);

/// Wraps `contents` into a short frame. Contents longer than 251 bytes need
/// the long form, which no command we send does; such contents give nullopt.
std::optional<Bytes> encodeFrame(const Bytes &contents);

/// Unwraps a frame; nullopt when the markers, the length or the short
/// checksum are wrong.
std::optional<Frame> decodeFrame(const Bytes &frame);

/// `"AABB"` → `{0xAA, 0xBB}`; whitespace between bytes is skipped; nullopt on
/// an odd count or a non-hex character.
std::optional<Bytes> hexToBytes(std::string_view hex);
/// Upper-case, no separators - the way the module writes it.
std::string bytesToHex(const Bytes &bytes);

} // namespace phicore::thinq::ipc
