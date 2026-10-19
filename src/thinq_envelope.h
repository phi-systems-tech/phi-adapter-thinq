#pragma once

// The MQTT side of the module: topics and the JSON envelope of
// docs/thinq-protocol.md §4. Device → cloud messages are parsed, cloud →
// device messages are built. Nothing here touches a socket.

#include "thinq_json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace phicore::thinq::ipc {

/// Topic filters the adapter subscribes to on the local broker.
inline constexpr const char kProvisioningFilter[] = "clip/#";
inline constexpr const char kRulesFilter[] = "$aws/#";

/// `lime/devices/<uuid>`: everything the cloud says to one module.
std::string deviceTopic(std::string_view uuid);

/// The topics the real cloud hands out in `completeProvisioning`, kept
/// verbatim so a module that was with us can go back to LG (§4.2).
std::string messagePublication(std::string_view uuid);
std::string provisioningPublication(std::string_view uuid);

/**
 * @brief The device uuid in a topic the module publishes on, or empty.
 *
 * Covers the factory default `clip/provisioning/devices/<uuid>` and both
 * `$aws/rules/<rule>/clip/.../<uuid>[/message]` forms; `undeploy` topics give
 * the uuid too. The uuid is the segment after `devices/`.
 */
std::string uuidFromTopic(std::string_view topic);

/// What one message from a module says. `data` is whatever JSON was there,
/// null included.
struct DeviceMessage {
    std::int64_t mid = 0;
    std::string did;
    std::string kind;
    std::string cmd;
    int rssi = 0;
    std::string fs;
    Json data;
    int type = 0;
};

/// nullopt unless the payload is an object with a string `cmd`.
std::optional<DeviceMessage> parseDeviceMessage(std::string_view payload);

/// §4.2 step 2, for both `preDeploy` and `deploy`. `provisioningType` is the
/// `cmd` the module sent.
std::string completeProvisioningMessage(std::string_view uuid, std::string_view provisioningType,
                                        std::int64_t mid);

/// §4.4 `packet`: `frameHex` is a whole AABB frame.
std::string packetMessage(std::string_view uuid, std::string_view frameHex, std::int64_t mid);

/// The hex of a `device_packet`, or empty when `data` is not a string.
std::string devicePacketHex(const DeviceMessage &message);

/// The module's `appInfo` fields worth keeping: model, firmware, module type.
struct ModuleInfo {
    std::string modelName;
    std::string softVer;
    std::string appVersion;
    std::string modemType;
    std::string countryCode;
    std::string deviceType;
};
ModuleInfo moduleInfoFrom(const DeviceMessage &message);

} // namespace phicore::thinq::ipc
