#include "thinq_envelope.h"

#include <ctime>

namespace phicore::thinq::ipc {

namespace {

constexpr std::string_view kDevicesSegment = "devices/";

} // namespace

std::string deviceTopic(std::string_view uuid)
{
    return "lime/devices/" + std::string(uuid);
}

std::string messagePublication(std::string_view uuid)
{
    return "$aws/rules/clip_http_action_rule/clip/devices/" + std::string(uuid) + "/message";
}

std::string provisioningPublication(std::string_view uuid)
{
    return "$aws/rules/clip_provisioning_rule/clip/provisioning/devices/" + std::string(uuid);
}

std::string uuidFromTopic(std::string_view topic)
{
    const std::size_t at = topic.rfind(kDevicesSegment);
    if (at == std::string_view::npos)
        return {};
    std::string_view rest = topic.substr(at + kDevicesSegment.size());
    const std::size_t slash = rest.find('/');
    if (slash != std::string_view::npos)
        rest = rest.substr(0, slash);
    return std::string(rest);
}

std::optional<DeviceMessage> parseDeviceMessage(std::string_view payload)
{
    const Json obj = parseObject(payload);
    if (obj.empty() || !obj.contains("cmd") || !obj["cmd"].is_string())
        return std::nullopt;
    DeviceMessage out;
    out.cmd = obj["cmd"].get<std::string>();
    out.did = jsonString(obj, "did");
    out.kind = jsonString(obj, "kind");
    out.fs = jsonString(obj, "fs");
    out.rssi = jsonInt(obj, "rssi", 0);
    out.type = jsonInt(obj, "type", 0);
    if (obj.contains("mid") && obj["mid"].is_number())
        out.mid = obj["mid"].get<std::int64_t>();
    if (obj.contains("data"))
        out.data = obj["data"];
    return out;
}

std::string completeProvisioningMessage(std::string_view uuid, std::string_view provisioningType,
                                        std::int64_t mid)
{
    Json msg = Json::object();
    msg["did"] = std::string(uuid);
    msg["mid"] = mid;
    msg["cmd"] = "completeProvisioning";
    msg["type"] = 0;
    Json publication = Json::object();
    publication["message"] = messagePublication(uuid);
    publication["provisioning"] = provisioningPublication(uuid);
    Json appInfo = Json::object();
    appInfo["host"] = "message";
    appInfo["publication"] = publication;
    Json data = Json::object();
    data["result"] = 0;
    data["host"] = "message";
    data["appInfo"] = appInfo;
    data["provisioningType"] = std::string(provisioningType);
    data["deployInterval"] = 600;
    msg["data"] = data;
    return dump(msg);
}

std::string ackMessage(std::string_view uuid, std::string_view frameHex, std::int64_t mid)
{
    Json msg;
    msg["did"] = std::string(uuid);
    msg["mid"] = mid;
    msg["cmd"] = "ack";
    msg["type"] = 1;
    msg["data"] = std::string(frameHex);
    return dump(msg);
}

Bytes timesyncBytes(std::int64_t utc)
{
    const std::time_t t = static_cast<std::time_t>(utc);
    std::tm tm{};
    gmtime_r(&t, &tm);
    return {static_cast<std::uint8_t>((tm.tm_year + 1900) % 100), static_cast<std::uint8_t>(tm.tm_mon),
            static_cast<std::uint8_t>(tm.tm_mday),                static_cast<std::uint8_t>(tm.tm_hour),
            static_cast<std::uint8_t>(tm.tm_min),                 static_cast<std::uint8_t>(tm.tm_sec),
            static_cast<std::uint8_t>(tm.tm_wday)};
}

namespace {

std::string base64(const Bytes &bytes)
{
    static const char *const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t n = bytes[i] << 16;
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

} // namespace

std::string timesyncMessage(std::string_view uuid, std::int64_t mid, std::int64_t utc)
{
    Json msg;
    msg["did"] = std::string(uuid);
    msg["mid"] = mid;
    msg["cmd"] = "resp_timesync";
    msg["type"] = 1;
    msg["data"] = base64(timesyncBytes(utc));
    return dump(msg);
}

std::string packetMessage(std::string_view uuid, std::string_view frameHex, std::int64_t mid)
{
    Json msg = Json::object();
    msg["did"] = std::string(uuid);
    msg["mid"] = mid;
    msg["cmd"] = "packet";
    msg["type"] = 1;
    msg["data"] = std::string(frameHex);
    return dump(msg);
}

std::string devicePacketHex(const DeviceMessage &message)
{
    return message.data.is_string() ? message.data.get<std::string>() : std::string();
}

ModuleInfo moduleInfoFrom(const DeviceMessage &message)
{
    ModuleInfo out;
    if (!message.data.is_object())
        return out;
    const Json &appInfo = message.data.contains("appInfo") ? message.data["appInfo"] : Json();
    if (!appInfo.is_object())
        return out;
    out.modelName = jsonString(appInfo, "modelName");
    out.softVer = jsonString(appInfo, "softVer");
    out.appVersion = jsonString(appInfo, "appVersion");
    out.modemType = jsonString(appInfo, "modemType");
    out.countryCode = jsonString(appInfo, "countryCode");
    out.deviceType = jsonString(appInfo, "DeviceType");
    return out;
}

} // namespace phicore::thinq::ipc
