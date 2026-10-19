// Process entry point for the LG ThinQ sidecar: the adapter factory and the
// SDK's own main loop. The one instance is the broker side of the cloud we
// stand in for (thinq_instance); the HTTPS side is phi-thinq-routed.

#include <csignal>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include "phi/adapter/sdk/loop_execution_backend.h"
#include "phi/adapter/sdk/sidecar.h"

#include "thinq_instance.h"
#include "thinq_schema.h"

namespace sdk = phicore::adapter::sdk;
namespace v1 = phicore::adapter::v1;
using namespace phicore::thinq::ipc;

namespace {

class ThinqFactory final : public sdk::AdapterFactory
{
protected:
    v1::Utf8String pluginType() const override { return kPluginType; }
    v1::Utf8String displayName() const override { return phicore::thinq::ipc::displayName(); }
    v1::Utf8String description() const override { return phicore::thinq::ipc::description(); }
    v1::Utf8String iconSvg() const override { return phicore::thinq::ipc::iconSvg(); }
    v1::Utf8String apiVersion() const override { return "1.0.0"; }
    int timeoutMs() const override { return 15000; }
    // One broker per box, one instance on it.
    int maxInstances() const override { return 1; }

    v1::AdapterCapabilities capabilities() const override { return phicore::thinq::ipc::capabilities(); }

    std::optional<v1::AdapterConfigSchema> configSchema() const override
    {
        return phicore::thinq::ipc::configSchema();
    }

    std::unique_ptr<sdk::InstanceExecutionBackend> createInstanceExecutionBackend(
        const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        // libmosquitto's socket, its timers and the reconnects all run here.
        return sdk::createLoopExecutionBackend("thinq-instance");
    }

    std::unique_ptr<sdk::AdapterInstance> createInstance(const sdk::ExternalId &externalId) override
    {
        (void)externalId;
        return makeInstance();
    }
};

} // namespace

int main(int argc, char **argv)
{
    std::signal(SIGPIPE, SIG_IGN);
    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = argc > 1
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-thinq-ipc.sock"));
    ThinqFactory factory;
    sdk::SidecarHost host(socketPath, factory);
    return sdk::runSidecarMain(host);
}
