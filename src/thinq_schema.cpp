#include "thinq_schema.h"

#include "thinq_settings.h"

#include <utility>

namespace phicore::thinq::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

// A front-loader: the drum as a ring, the door as a dot.
constexpr const char kIconSvg[] =
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
    "xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Washer icon\">"
    "<rect x=\"3.5\" y=\"2.5\" width=\"17\" height=\"19\" rx=\"2\" "
    "stroke=\"#2E3A4F\" stroke-width=\"1.6\" fill=\"#121A26\"/>"
    "<circle cx=\"12\" cy=\"13.5\" r=\"5\" stroke=\"#7A8AA4\" stroke-width=\"1.6\"/>"
    "<circle cx=\"12\" cy=\"13.5\" r=\"2\" fill=\"#A50034\"/>"
    "<circle cx=\"7\" cy=\"5.5\" r=\"1\" fill=\"#7A8AA4\"/>"
    "<circle cx=\"10\" cy=\"5.5\" r=\"1\" fill=\"#7A8AA4\"/>"
    "</svg>";

v1::AdapterConfigField field(const char *key, v1::AdapterConfigFieldType type, const char *label,
                             const char *description, v1::ScalarValue defaultValue = {})
{
    v1::AdapterConfigField out;
    out.key = key;
    out.type = type;
    out.label = label;
    out.description = description;
    out.defaultValue = std::move(defaultValue);
    return out;
}

/// The broker beside its port, the account below it.
v1::AdapterConfigSection section()
{
    using Type = v1::AdapterConfigFieldType;
    using Flag = v1::AdapterConfigFieldFlag;
    v1::AdapterConfigSection out;
    out.title = "LG ThinQ appliances";
    out.description = "Appliances provisioned onto the box's own broker announce themselves here; "
                      "nothing is entered per appliance.";
    out.layout.columns = 2;

    v1::AdapterConfigField host = field("brokerHost", Type::Hostname, "Broker",
                                        "The MQTT broker the appliances were provisioned onto; the box's own.",
                                        v1::Utf8String(kDefaultBrokerHost));
    v1::AdapterConfigField port = field("brokerPort", Type::Port, "Port", "The plain listener, local only.",
                                        std::int64_t{kDefaultBrokerPort});
    port.layout.controlWidth = v1::AdapterConfigSize::Narrow;
    v1::AdapterConfigField username = field("username", Type::String, "Username",
                                            "The adapter's account on the broker.");
    v1::AdapterConfigField password = field("password", Type::Password, "Password", "");
    password.flags = Flag::Secret;
    v1::AdapterConfigField retry = field("retryIntervalMs", Type::Integer, "Retry interval",
                                         "Reconnect interval while the broker is unreachable.",
                                         std::int64_t{kDefaultRetryIntervalMs});
    retry.layout.controlWidth = v1::AdapterConfigSize::Narrow;

    out.fields = {host, port, username, password, retry};
    return out;
}

} // namespace

v1::Utf8String displayName()
{
    return "LG ThinQ appliances";
}

v1::Utf8String description()
{
    return "LG ThinQ washers and dishwashers, over the box's own broker instead of LG's cloud";
}

v1::Utf8String iconSvg()
{
    return kIconSvg;
}

v1::AdapterCapabilities capabilities()
{
    v1::AdapterCapabilities caps;
    caps.required = v1::AdapterRequirement::ManualConfirm | v1::AdapterRequirement::UsesRetryInterval;
    caps.optional = v1::AdapterRequirement::Host | v1::AdapterRequirement::Port;
    return caps;
}

v1::AdapterConfigSchema configSchema()
{
    v1::AdapterConfigSchema schema;
    schema.factory = section();
    schema.instance = section();
    return schema;
}

} // namespace phicore::thinq::ipc
