#pragma once

// What the one instance is configured with: the local broker the appliances
// are provisioned onto, and the account the adapter uses on it. The
// appliances themselves are not configured; they announce themselves.

#include "thinq_json.h"

#include "phi/adapter/v1/schema.h"

#include <cstdint>
#include <string>

namespace phicore::thinq::ipc {

inline constexpr const char kDefaultBrokerHost[] = "127.0.0.1";
inline constexpr std::uint16_t kDefaultBrokerPort = 1883;
inline constexpr int kDefaultRetryIntervalMs = 10000;

struct ThinqSettings {
    std::string brokerHost = kDefaultBrokerHost;
    std::uint16_t brokerPort = kDefaultBrokerPort;
    std::string username;
    std::string password;
    int retryIntervalMs = kDefaultRetryIntervalMs;

    [[nodiscard]] bool sameBroker(const ThinqSettings &other) const
    {
        return brokerHost == other.brokerHost && brokerPort == other.brokerPort && username == other.username
               && password == other.password;
    }
};

/// Meta keys win over the adapter's own fields, which is what a form saves to.
ThinqSettings settingsFromAdapter(const phicore::adapter::v1::Adapter &adapter, const Json &meta);

} // namespace phicore::thinq::ipc
