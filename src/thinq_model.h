#pragma once

// An appliance as phi-core sees it. The SDK has no household-appliance class
// yet (ROADMAP M4), so each one is a Sensor with the channels of
// docs/thinq-protocol.md §5.5, and the washer's four verbs are a power
// channel plus an `operation` selector.

#include "phi/adapter/v1/channel.h"
#include "phi/adapter/v1/device.h"

#include <string>
#include <string_view>

namespace phicore::thinq::ipc {

enum class Family { Unknown, Washer, Dishwasher };

Family familyFor(std::string_view kind);
const char *familyName(Family family);

// Every appliance.
inline constexpr const char kChannelConnectivity[] = "connectivity";
inline constexpr const char kChannelSignal[] = "signal";
/// The last `device_packet` as hex: what a capture reads offsets from.
inline constexpr const char kChannelLastFrame[] = "lastFrame";
inline constexpr const char kChannelState[] = "state";
inline constexpr const char kChannelCourse[] = "course";
inline constexpr const char kChannelRemaining[] = "remainingTime";
inline constexpr const char kChannelTotal[] = "totalTime";
inline constexpr const char kChannelError[] = "error";
inline constexpr const char kChannelChildLock[] = "childLock";

// Washer.
inline constexpr const char kChannelPower[] = "power";
inline constexpr const char kChannelOperation[] = "operation";
inline constexpr const char kChannelDelayEnd[] = "delayEnd";
inline constexpr const char kChannelSpin[] = "spin";
inline constexpr const char kChannelTemperature[] = "temperature";
inline constexpr const char kChannelRemoteStart[] = "remoteStart";
inline constexpr const char kChannelDoorLocked[] = "doorLocked";
inline constexpr const char kChannelCycleCount[] = "cycleCount";
inline constexpr const char kChannelEnergy[] = "energy";

// Dishwasher.
inline constexpr const char kChannelProcess[] = "process";
inline constexpr const char kChannelDoorOpen[] = "doorOpen";
inline constexpr const char kChannelRinseAidRefill[] = "rinseAidRefill";
inline constexpr const char kChannelSaltRefill[] = "saltRefill";

/// The `operation` choices: LG's `washerOperationMode` minus the two that are
/// the power channel.
inline constexpr const char kOperationStart[] = "START";
inline constexpr const char kOperationPause[] = "PAUSE";

/// Channels for a family. With no verified decoder the status channels are
/// still announced - they show what the hypothesis reads, labelled as such -
/// but nothing is writable except what needs no decoder (power).
phicore::adapter::v1::ChannelList buildChannels(Family family, bool decoderVerified);

phicore::adapter::v1::Device buildDevice(const std::string &uuid, const std::string &name,
                                         const std::string &model, const std::string &firmware);

} // namespace phicore::thinq::ipc
