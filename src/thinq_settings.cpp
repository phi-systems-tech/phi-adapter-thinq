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

    // The account core keeps on the adapter record itself - which is where an
    // intent's user and password land - unless the form overrode it.
    out.username = str::trimmed(jsonString(meta, "username"));
    if (out.username.empty())
        out.username = str::trimmed(adapter.user);
    out.password = jsonString(meta, "password");
    if (out.password.empty())
        out.password = adapter.password;
    const int retry = jsonInt(meta, "retryIntervalMs", kDefaultRetryIntervalMs);
    out.retryIntervalMs = retry >= 1000 ? retry : kDefaultRetryIntervalMs;
    return out;
}

} // namespace phicore::thinq::ipc
