#include "thinq_frame.h"

#include <cctype>

namespace phicore::thinq::ipc {

namespace {

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

} // namespace

std::uint8_t checksum8(const Bytes &bytes)
{
    unsigned sum = 0;
    for (const std::uint8_t b : bytes)
        sum += b;
    return static_cast<std::uint8_t>((sum & 0xFF) ^ 0x55);
}

std::uint16_t crc16Xmodem(const Bytes &bytes)
{
    std::uint16_t crc = 0;
    for (const std::uint8_t b : bytes) {
        crc ^= static_cast<std::uint16_t>(b) << 8;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
    }
    return crc;
}

std::optional<Bytes> encodeFrame(const Bytes &contents)
{
    // AA, len, contents, ck, BB: four bytes around the contents, and the
    // length byte has to stay below the long-frame marker.
    if (contents.size() + 4 >= kLongFrameLength)
        return std::nullopt;
    Bytes out;
    out.reserve(contents.size() + 4);
    out.push_back(kFrameStart);
    out.push_back(static_cast<std::uint8_t>(contents.size() + 4));
    out.insert(out.end(), contents.begin(), contents.end());
    out.push_back(checksum8(out));
    out.push_back(kFrameEnd);
    return out;
}

std::optional<Frame> decodeFrame(const Bytes &frame)
{
    if (frame.size() < 4 || frame.front() != kFrameStart || frame.back() != kFrameEnd)
        return std::nullopt;

    Frame out;
    const std::uint8_t len = frame[1];
    if (len == kLongFrameLength) {
        // AA FF contents ck ck BB
        if (frame.size() < 5)
            return std::nullopt;
        out.longFrame = true;
        out.contents.assign(frame.begin() + 2, frame.end() - 3);
        const Bytes covered(frame.begin(), frame.end() - 3);
        const std::uint16_t expected = crc16Xmodem(covered);
        const std::uint16_t given = static_cast<std::uint16_t>((frame[frame.size() - 3] << 8) | frame[frame.size() - 2]);
        out.longChecksumVerified = expected == given;
        return out;
    }

    if (len != frame.size())
        return std::nullopt;
    const Bytes covered(frame.begin(), frame.end() - 2);
    if (checksum8(covered) != frame[frame.size() - 2])
        return std::nullopt;
    out.contents.assign(frame.begin() + 2, frame.end() - 2);
    return out;
}

std::optional<Bytes> hexToBytes(std::string_view hex)
{
    Bytes out;
    out.reserve(hex.size() / 2);
    int high = -1;
    for (const char c : hex) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (high >= 0)
                return std::nullopt;
            continue;
        }
        const int digit = hexDigit(c);
        if (digit < 0)
            return std::nullopt;
        if (high < 0) {
            high = digit;
        } else {
            out.push_back(static_cast<std::uint8_t>((high << 4) | digit));
            high = -1;
        }
    }
    if (high >= 0)
        return std::nullopt;
    return out;
}

std::string bytesToHex(const Bytes &bytes)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

} // namespace phicore::thinq::ipc
