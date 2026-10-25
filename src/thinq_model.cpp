#include "thinq_model.h"

#include "thinq_dishwasher.h"
#include "thinq_washer.h"

#include <utility>

namespace phicore::thinq::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

constexpr auto kReadReport = v1::ChannelFlag::Readable | v1::ChannelFlag::Reportable;
constexpr auto kReadWriteReport = v1::ChannelFlag::Readable | v1::ChannelFlag::Writable | v1::ChannelFlag::Reportable;

v1::Channel channel(const char *id, const char *name, v1::ChannelKind kind, v1::ChannelDataType type,
                    v1::ChannelFlags flags = kReadReport)
{
    v1::Channel out;
    out.setExternalId(id).setName(name).setKind(kind).setDataType(type).setFlags(flags);
    return out;
}

v1::Channel boolChannel(const char *id, const char *name)
{
    return channel(id, name, v1::ChannelKind::Unknown, v1::ChannelDataType::Bool);
}

v1::Channel minutesChannel(const char *id, const char *name)
{
    v1::Channel out = channel(id, name, v1::ChannelKind::Duration, v1::ChannelDataType::Int);
    out.setUnit("min").setRange(0, 30 * 60, 1);
    return out;
}

/// A string channel whose choices are a vocabulary table, by value.
template <typename NameFn>
v1::Channel enumChannel(const char *id, const char *name, int first, int last, NameFn nameOf)
{
    v1::Channel out = channel(id, name, v1::ChannelKind::Unknown, v1::ChannelDataType::String);
    for (int value = first; value <= last; ++value) {
        const std::string label = nameOf(value);
        if (label != "UNKNOWN")
            out.addChoice(label, label);
    }
    return out;
}

void appendCommon(v1::ChannelList &channels)
{
    channels.push_back(channel(kChannelConnectivity, "Connectivity", v1::ChannelKind::ConnectivityStatus,
                               v1::ChannelDataType::Enum));
    v1::Channel signal = channel(kChannelSignal, "Wi-Fi signal", v1::ChannelKind::SignalStrength,
                                 v1::ChannelDataType::Int);
    signal.setUnit("dBm").setRange(-100, 0);
    channels.push_back(std::move(signal));
    channels.push_back(channel(kChannelLastFrame, "Last frame", v1::ChannelKind::Unknown,
                               v1::ChannelDataType::String));
}

void appendWasher(v1::ChannelList &channels, bool decoderVerified)
{
    channels.push_back(channel(kChannelPower, "Power", v1::ChannelKind::PowerOnOff, v1::ChannelDataType::Bool,
                               kReadWriteReport));

    // Start needs the course from the last status; without a verified decoder
    // it would send bytes read from a guessed offset. Pause and power are
    // fixed byte strings and stay.
    v1::Channel operation = channel(kChannelOperation, "Operation", v1::ChannelKind::Unknown,
                                    v1::ChannelDataType::String, kReadWriteReport);
    if (decoderVerified)
        operation.addChoice(kOperationStart, "Start");
    operation.addChoice(kOperationPause, "Pause");
    channels.push_back(std::move(operation));

    channels.push_back(enumChannel(kChannelState, "State", 0, 18, washerStateName));
    channels.push_back(channel(kChannelCourse, "Course", v1::ChannelKind::Unknown, v1::ChannelDataType::Int));
    channels.push_back(minutesChannel(kChannelRemaining, "Remaining time"));
    channels.push_back(minutesChannel(kChannelTotal, "Total time"));

    v1::Channel delayEnd = channel(kChannelDelayEnd, "Delayed end", v1::ChannelKind::Unknown,
                                   v1::ChannelDataType::Int, decoderVerified ? kReadWriteReport : kReadReport);
    delayEnd.setUnit("h").setRange(0, 19, 1);
    channels.push_back(std::move(delayEnd));

    channels.push_back(enumChannel(kChannelSpin, "Spin", 0, 255, washerSpinName));
    channels.push_back(enumChannel(kChannelTemperature, "Temperature", 0, 7, washerTempName));
    channels.push_back(enumChannel(kChannelError, "Error", 0, 19, washerErrorName));
    channels.push_back(boolChannel(kChannelRemoteStart, "Remote start armed"));
    channels.push_back(boolChannel(kChannelDoorLocked, "Door locked"));
    channels.push_back(boolChannel(kChannelChildLock, "Child lock"));

    v1::Channel cycles = channel(kChannelCycleCount, "Cycle count", v1::ChannelKind::Unknown,
                                 v1::ChannelDataType::Int);
    cycles.setRange(0, 255);
    channels.push_back(std::move(cycles));

    v1::Channel energy = channel(kChannelEnergy, "Cycle energy", v1::ChannelKind::Unknown,
                                 v1::ChannelDataType::Int);
    energy.setUnit("Wh").setRange(0, 65535);
    channels.push_back(std::move(energy));
}

void appendDishwasher(v1::ChannelList &channels)
{
    channels.push_back(enumChannel(kChannelState, "State", 0, 6, dishwasherStateName));
    channels.push_back(enumChannel(kChannelProcess, "Process", 0, 7, dishwasherProcessName));
    channels.push_back(enumChannel(kChannelCourse, "Course", 0, 13, dishwasherCourseName));
    channels.push_back(minutesChannel(kChannelRemaining, "Remaining time"));
    channels.push_back(minutesChannel(kChannelTotal, "Total time"));
    channels.push_back(boolChannel(kChannelDoorOpen, "Door open"));
    channels.push_back(boolChannel(kChannelChildLock, "Child lock"));
    channels.push_back(boolChannel(kChannelRinseAidRefill, "Rinse aid refill"));
    channels.push_back(boolChannel(kChannelSaltRefill, "Salt refill"));
}

} // namespace

Family familyFor(std::string_view kind)
{
    if (isWasherKind(kind))
        return Family::Washer;
    if (isDishwasherKind(kind))
        return Family::Dishwasher;
    return Family::Unknown;
}

const char *familyName(Family family)
{
    switch (family) {
    case Family::Washer:
        return "washer";
    case Family::Dishwasher:
        return "dishwasher";
    case Family::Unknown:
        break;
    }
    return "appliance";
}

v1::ChannelList buildChannels(Family family, bool decoderVerified)
{
    v1::ChannelList channels;
    appendCommon(channels);
    switch (family) {
    case Family::Washer:
        appendWasher(channels, decoderVerified);
        break;
    case Family::Dishwasher:
        appendDishwasher(channels);
        break;
    case Family::Unknown:
        break;
    }
    return channels;
}

v1::Device buildDevice(const std::string &uuid, const std::string &name, const std::string &model,
                       const std::string &firmware)
{
    v1::Device device;
    device.setExternalId(uuid)
        .setDeviceClass(v1::DeviceClass::Sensor)
        .setName(name)
        .setManufacturer("LG Electronics")
        .setModel(model)
        .setFirmware(firmware);
    return device;
}

} // namespace phicore::thinq::ipc
