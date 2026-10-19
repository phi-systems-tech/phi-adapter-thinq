#include "thinq_settings.h"

#include "phi/runtime/str.h"

namespace phicore::thinq::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

ThinqSettings settingsFromAdapter(const v1::Adapter &adapter, const Json &meta)
{
    ThinqSettings out;
    std::string host = str::trimmed(jsonString(meta, "brokerHost"));
    if (host.empty())
        host = str::trimmed(adapter.host);
    if (!host.empty())
        out.brokerHost = host;

    const int fallbackPort = adapter.port != 0 ? adapter.port : kDefaultBrokerPort;
    const int port = jsonInt(meta, "brokerPort", fallbackPort);
    out.brokerPort = static_cast<std::uint16_t>(port > 0 && port <= 0xFFFF ? port : kDefaultBrokerPort);

    out.username = str::trimmed(jsonString(meta, "username"));
    out.password = jsonString(meta, "password");
    const int retry = jsonInt(meta, "retryIntervalMs", kDefaultRetryIntervalMs);
    out.retryIntervalMs = retry >= 1000 ? retry : kDefaultRetryIntervalMs;
    return out;
}

} // namespace phicore::thinq::ipc
