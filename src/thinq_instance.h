#pragma once

// The cloud the appliances were pointed at, as far as MQTT goes: one
// instance sits on the local broker, answers the provisioning handshake,
// reads every frame the modules forward and sends the washer its commands.
// The class stays private to the .cpp - the factory only ever needs one.

#include <memory>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::thinq::ipc {

std::unique_ptr<phicore::adapter::sdk::AdapterInstance> makeInstance();

} // namespace phicore::thinq::ipc
