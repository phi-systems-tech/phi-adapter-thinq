#include "thinq_instance.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>

#include "phi/adapter/sdk/schema_json.h"
#include "phi/runtime/loop.h"
#include "phi/runtime/str.h"

#include "thinq_dishwasher.h"
#include "thinq_envelope.h"
#include "thinq_frame.h"
#include "thinq_json.h"
#include "thinq_model.h"
#include "thinq_mqtt.h"
#include "thinq_settings.h"
#include "thinq_washer.h"

namespace phicore::thinq::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

using namespace std::chrono_literals;

namespace {

/// An awake washer reports every 1.5 s, an asleep one not at all with the
/// session open (§5.2.4): silence alone is no disconnect. This is the point
/// where it stops being "asleep" and starts being "gone".
constexpr auto kStaleAfter = 30min;
constexpr auto kStaleCheck = 60s;
/// Between `download` and `start` (§5.2.1: "a few seconds later").
constexpr auto kStartAfterDownload = 3s;
/// The meta key the known appliances are kept under, so they are announced
/// again - as disconnected - after a restart.
constexpr const char kMetaDevices[] = "devices";

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<bool> scalarToBool(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<bool>(&value))
        return *v;
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return *v != 0;
    if (const auto *v = std::get_if<double>(&value))
        return *v != 0.0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        const std::string text = str::toLower(str::trimmed(*v));
        if (text == "true" || text == "1" || text == "on")
            return true;
        if (text == "false" || text == "0" || text == "off")
            return false;
    }
    return std::nullopt;
}

std::optional<std::int64_t> scalarToInt(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return *v;
    if (const auto *v = std::get_if<double>(&value))
        return static_cast<std::int64_t>(*v);
    if (const auto *v = std::get_if<bool>(&value))
        return *v ? 1 : 0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        bool ok = false;
        const std::int64_t parsed = str::toInt(str::trimmed(*v), &ok);
        if (ok)
            return parsed;
    }
    return std::nullopt;
}

std::string scalarToString(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<v1::Utf8String>(&value))
        return str::trimmed(*v);
    return {};
}

/// One appliance, by uuid.
struct Appliance {
    std::string uuid;
    std::string kind;
    ModuleInfo module;
    Family family = Family::Unknown;
    std::int64_t lastSeenMs = 0;
    int rssi = 0;
    bool registered = false;
    bool announced = false;
    bool online = false;
    /// Whether the start packet went out for this provisioning round.
    bool started = false;
    /// The last decoded washer status, for the programme a start repeats.
    std::optional<WasherStatus> washer;
    /// What the delayed-end channel was set to, for the next start.
    int delayHours = 0;
    phi::runtime::Timer startTimer;

    [[nodiscard]] const WasherLayout *washerLayout() const
    {
        return family == Family::Washer ? washerLayoutFor(kind) : nullptr;
    }
    [[nodiscard]] const DishwasherLayout *dishwasherLayout() const
    {
        return family == Family::Dishwasher ? dishwasherLayoutFor(kind) : nullptr;
    }
    [[nodiscard]] bool decoderVerified() const
    {
        if (const WasherLayout *l = washerLayout())
            return l->verified;
        if (const DishwasherLayout *l = dishwasherLayout())
            return l->verified;
        return false;
    }
    [[nodiscard]] std::string defaultName() const
    {
        return std::string("LG ") + familyName(family);
    }
};

class ThinqInstance final : public sdk::AdapterInstance
{
public:
    ~ThinqInstance() override = default;

protected:
    bool start() override
    {
        m_loop = phi::runtime::Loop::current();
        if (m_loop == nullptr) {
            log(sdk::LogLevel::Error, sdk::LogCategory::Lifecycle, "started off a loop; no timers are possible");
            return false;
        }
        m_running = true;
        m_mqtt = std::make_unique<MqttClient>(*m_loop);
        m_mqtt->onConnected([this] { onBrokerConnected(); });
        m_mqtt->onDisconnected([this](const std::string &reason) { onBrokerDisconnected(reason); });
        m_mqtt->onMessage([this](const MqttClient::Message &message) { onBrokerMessage(message); });
        m_staleTimer = m_loop->timerEvery(kStaleCheck, [this] { checkStale(); });
        return true;
    }

    void stop() override
    {
        m_running = false;
        m_retry.reset();
        m_staleTimer.reset();
        for (auto &[uuid, appliance] : m_appliances)
            appliance.startTimer.reset();
        if (m_mqtt)
            m_mqtt->disconnect();
        m_mqtt.reset();
    }

    void onDisconnected() override
    {
        // phi-core went away; it comes back with the configuration.
        m_retry.reset();
        if (m_mqtt)
            m_mqtt->disconnect();
        setConnected(false);
    }

    void onConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        if (!m_running)
            return;
        m_info = request.adapter;
        const Json meta = parseObject(request.adapter.metaJson);
        const ThinqSettings next = settingsFromAdapter(request.adapter, meta);
        const bool brokerChanged = !m_configured || !next.sameBroker(m_settings);
        m_settings = next;

        if (!m_configured) {
            restoreAppliances(meta);
            m_configured = true;
        }
        for (auto &[uuid, appliance] : m_appliances)
            announce(appliance);

        // Every meta patch this instance sends comes back as a config change;
        // only a broker change is a reason to dial again.
        if (brokerChanged)
            dial();
    }

    void onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        const auto it = m_appliances.find(request.deviceExternalId);
        if (it == m_appliances.end()) {
            answer(request.cmdId, v1::CmdStatus::NotSupported, "Unknown appliance");
            return;
        }
        Appliance &appliance = it->second;
        if (appliance.family != Family::Washer) {
            answer(request.cmdId, v1::CmdStatus::NotSupported, "This appliance takes no commands");
            return;
        }
        const std::string &channel = request.channelExternalId;
        if (channel == kChannelDelayEnd) {
            const std::optional<std::int64_t> hours = scalarToInt(request.value);
            if (!hours || *hours < 0 || *hours > 19) {
                answer(request.cmdId, v1::CmdStatus::InvalidArgument, "Delayed end takes 0 to 19 hours");
                return;
            }
            appliance.delayHours = static_cast<int>(*hours);
            emitChannel(appliance, kChannelDelayEnd, static_cast<std::int64_t>(appliance.delayHours));
            answer(request.cmdId, v1::CmdStatus::Success, {});
            return;
        }
        if (!m_mqtt || !m_mqtt->connected() || !appliance.online) {
            answer(request.cmdId, v1::CmdStatus::TemporarilyOffline, "The appliance is not reporting");
            return;
        }
        if (channel == kChannelPower) {
            const std::optional<bool> on = scalarToBool(request.value);
            if (!on) {
                answer(request.cmdId, v1::CmdStatus::InvalidArgument, "Power takes on or off");
                return;
            }
            setPower(appliance, request.cmdId, *on);
            return;
        }
        if (channel == kChannelOperation) {
            const std::string op = str::toUpper(scalarToString(request.value));
            if (op == kOperationPause) {
                sendCommand(appliance, request.cmdId, washerPause(), "pause");
                return;
            }
            if (op == kOperationStart) {
                startProgramme(appliance, request.cmdId);
                return;
            }
            answer(request.cmdId, v1::CmdStatus::InvalidArgument, "Operation takes START or PAUSE");
            return;
        }
        answer(request.cmdId, v1::CmdStatus::NotSupported, "Channel is read-only");
    }

private:
    // ---- broker --------------------------------------------------------------

    void dial()
    {
        m_retry.reset();
        if (!m_running || !m_mqtt)
            return;
        m_mqtt->disconnect();
        setConnected(false);

        MqttSettings mqtt;
        mqtt.clientId = "phi-thinq-" + std::string(m_info.externalId.empty() ? "1" : m_info.externalId);
        mqtt.host = m_settings.brokerHost;
        mqtt.port = m_settings.brokerPort;
        mqtt.username = m_settings.username;
        mqtt.password = m_settings.password;
        std::string error;
        if (!m_mqtt->connect(mqtt, &error)) {
            log(sdk::LogLevel::Warn, sdk::LogCategory::Network, "cannot connect to %1:%2: %3",
                {m_settings.brokerHost, std::to_string(m_settings.brokerPort), error});
            scheduleRetry();
        }
    }

    void scheduleRetry()
    {
        if (!m_running)
            return;
        m_retry = m_loop->timerAfter(std::chrono::milliseconds(m_settings.retryIntervalMs), [this] { dial(); });
    }

    void onBrokerConnected()
    {
        std::string error;
        for (const char *filter : {kProvisioningFilter, kRulesFilter}) {
            if (!m_mqtt->subscribe(filter, 0, &error))
                log(sdk::LogLevel::Error, sdk::LogCategory::Network, "subscribe %1 failed: %2", {filter, error});
        }
        setConnected(true);
        log(sdk::LogLevel::Info, sdk::LogCategory::Network, "on the broker at %1:%2",
            {m_settings.brokerHost, std::to_string(m_settings.brokerPort)});
    }

    void onBrokerDisconnected(const std::string &reason)
    {
        log(sdk::LogLevel::Warn, sdk::LogCategory::Network, "broker connection lost: %1", {reason});
        setConnected(false);
        scheduleRetry();
    }

    void onBrokerMessage(const MqttClient::Message &message)
    {
        const std::optional<DeviceMessage> parsed = parseDeviceMessage(message.payload);
        if (!parsed) {
            log(sdk::LogLevel::Debug, sdk::LogCategory::Protocol, "unreadable message on %1", {message.topic});
            return;
        }
        std::string uuid = parsed->did.empty() ? uuidFromTopic(message.topic) : parsed->did;
        if (uuid.empty()) {
            log(sdk::LogLevel::Debug, sdk::LogCategory::Protocol, "no device id on %1", {message.topic});
            return;
        }
        Appliance &appliance = applianceFor(uuid, parsed->kind);
        appliance.lastSeenMs = nowMs();
        appliance.rssi = parsed->rssi;
        setOnline(appliance, true);
        emitChannel(appliance, kChannelSignal, static_cast<std::int64_t>(appliance.rssi));

        const std::string &cmd = parsed->cmd;
        if (cmd == "preDeploy" || cmd == "deploy") {
            onProvisioning(appliance, *parsed);
        } else if (cmd == "completeProvisioning_ack") {
            appliance.registered = true;
            log(sdk::LogLevel::Info, sdk::LogCategory::Protocol, "%1 acknowledged provisioning", {uuid});
        } else if (cmd == "req_timesync") {
            // Seven bytes of UTC, base64, as `resp_timesync` (§4.4). rssi
            // -100 marks a reboot.
            std::string error;
            if (!m_mqtt->publish(deviceTopic(uuid), timesyncMessage(uuid, nowMs(), nowMs() / 1000), 0, false, &error))
                log(sdk::LogLevel::Warn, sdk::LogCategory::Network, "%1: resp_timesync failed: %2", {uuid, error});
            else
                log(sdk::LogLevel::Debug, sdk::LogCategory::Protocol, "%1 asked for the time (rssi %2); answered",
                    {uuid, std::to_string(parsed->rssi)});
        } else if (cmd == "undeploy") {
            // The module is about to drop its registration and start over
            // (§4.2); the next preDeploy is a new round.
            appliance.registered = false;
            appliance.started = false;
            log(sdk::LogLevel::Info, sdk::LogCategory::Protocol, "%1 undeployed", {uuid});
        } else if (cmd == "device_packet") {
            onDevicePacket(appliance, *parsed);
        } else {
            log(sdk::LogLevel::Debug, sdk::LogCategory::Protocol, "%1 sent %2 on %3", {uuid, cmd, message.topic});
        }
    }

    void onProvisioning(Appliance &appliance, const DeviceMessage &message)
    {
        const ModuleInfo info = moduleInfoFrom(message);
        const bool changed = info.modelName != appliance.module.modelName || info.softVer != appliance.module.softVer;
        if (!info.modelName.empty())
            appliance.module = info;
        if (appliance.kind.empty() && !info.modelName.empty())
            appliance.kind = info.modelName;
        appliance.family = familyFor(appliance.kind);

        const std::string reply = completeProvisioningMessage(appliance.uuid, message.cmd, nowMs());
        std::string error;
        if (!m_mqtt->publish(deviceTopic(appliance.uuid), reply, 0, false, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Network, "completeProvisioning to %1 failed: %2",
                {appliance.uuid, error});
        else
            log(sdk::LogLevel::Info, sdk::LogCategory::Protocol, "%1 (%2) sent %3; answered completeProvisioning",
                {appliance.uuid, appliance.kind, message.cmd});

        if (changed || !appliance.announced)
            announce(appliance, true);
        saveAppliances();
        start(appliance);
    }

    /// What the cloud sends a dishwasher right after provisioning, once per
    /// round: `F0 ED 11 21 01 00 00 00 18 00`, observed on the D0211. Its
    /// meaning is not documented; without it the appliance never streams
    /// its status. Washers get nothing here: an awake one streams unasked.
    void start(Appliance &appliance)
    {
        if (appliance.started || appliance.family != Family::Dishwasher)
            return;
        appliance.started = true;
        std::string error;
        if (!publishFrame(appliance, {0xF0, 0xED, 0x11, 0x21, 0x01, 0x00, 0x00, 0x00, 0x18, 0x00}, &error))
            log(sdk::LogLevel::Warn, sdk::LogCategory::Network, "%1: start packet failed: %2", {appliance.uuid, error});
        else
            log(sdk::LogLevel::Info, sdk::LogCategory::Protocol, "%1: sent the dishwasher start packet", {appliance.uuid});
    }

    void onDevicePacket(Appliance &appliance, const DeviceMessage &message)
    {
        const std::string hex = devicePacketHex(message);
        if (hex.empty())
            return;
        emitChannel(appliance, kChannelLastFrame, v1::Utf8String(hex));

        const std::optional<Bytes> bytes = hexToBytes(hex);
        if (!bytes) {
            log(sdk::LogLevel::Debug, sdk::LogCategory::Protocol, "%1: packet is not hex", {appliance.uuid});
            return;
        }
        const std::optional<Frame> frame = decodeFrame(*bytes);
        if (!frame) {
            log(sdk::LogLevel::Debug, sdk::LogCategory::Protocol, "%1: not an AABB frame: %2", {appliance.uuid, hex});
            return;
        }
        if (frame->longFrame && !frame->longChecksumVerified)
            log(sdk::LogLevel::Debug, sdk::LogCategory::Protocol, "%1: long frame, checksum hypothesis misses: %2",
                {appliance.uuid, hex});
        acknowledge(appliance, *frame);

        if (const WasherLayout *layout = appliance.washerLayout()) {
            if (const std::optional<WasherStatus> status = decodeWasherStatus(*bytes, *layout)) {
                appliance.washer = status;
                emitWasher(appliance, *status);
            }
            return;
        }
        if (const DishwasherLayout *layout = appliance.dishwasherLayout()) {
            if (const std::optional<DishwasherStatus> status = decodeDishwasherStatus(frame->contents, *layout))
                emitDishwasher(appliance, *status);
        }
    }

    // ---- commands ----------------------------------------------------------

    void setPower(Appliance &appliance, v1::CmdId cmdId, bool on)
    {
        if (!on) {
            sendCommand(appliance, cmdId, washerPowerOff(), "power off");
            return;
        }
        // Wake is a toggle on an awake machine (§5.2.1): it would switch it
        // off. Only a status that says POWEROFF, or no status at all, gets one.
        if (appliance.washer && appliance.washer->state != 0) {
            answer(cmdId, v1::CmdStatus::InvalidArgument, "The washer is already awake");
            return;
        }
        sendCommand(appliance, cmdId, washerWake(), "wake");
    }

    void startProgramme(Appliance &appliance, v1::CmdId cmdId)
    {
        const WasherLayout *layout = appliance.washerLayout();
        if (!layout || !layout->verified) {
            answer(cmdId, v1::CmdStatus::NotSupported,
                   "Starting needs the course from the status, and this model's status layout is not verified yet");
            return;
        }
        const std::optional<WasherStatus> &status = appliance.washer;
        if (!status) {
            answer(cmdId, v1::CmdStatus::TemporarilyOffline, "No status from the washer yet");
            return;
        }
        if (status->remoteStart.has_value() && !*status->remoteStart) {
            answer(cmdId, v1::CmdStatus::InvalidArgument, "Remote start is not armed; press it on the panel first");
            return;
        }
        if (!washerAcceptsStart(status->state)) {
            answer(cmdId, v1::CmdStatus::InvalidArgument,
                   "The washer is " + washerStateName(status->state) + "; wake it and select a course first");
            return;
        }
        if (status->course == 0) {
            answer(cmdId, v1::CmdStatus::InvalidArgument, "No course is selected on the washer");
            return;
        }

        Programme programme;
        programme.course = static_cast<std::uint8_t>(status->course);
        programme.spin = static_cast<std::uint8_t>(status->spin);
        programme.temp = static_cast<std::uint8_t>(status->temp);
        programme.rinse = static_cast<std::uint8_t>(status->rinse == 0 ? 1 : status->rinse);
        programme.dry = static_cast<std::uint8_t>(status->dry);
        programme.delayHours = static_cast<std::uint8_t>(appliance.delayHours);

        std::string error;
        if (!publishFrame(appliance, washerDownload(programme), &error)) {
            answer(cmdId, v1::CmdStatus::Failure, error);
            return;
        }
        const std::string uuid = appliance.uuid;
        appliance.startTimer = m_loop->timerAfter(kStartAfterDownload, [this, uuid, programme, cmdId] {
            const auto it = m_appliances.find(uuid);
            if (it == m_appliances.end()) {
                answer(cmdId, v1::CmdStatus::Failure, "The appliance went away");
                return;
            }
            sendCommand(it->second, cmdId, washerStart(programme), "start");
        });
    }

    void sendCommand(Appliance &appliance, v1::CmdId cmdId, const Bytes &contents, const char *what)
    {
        std::string error;
        if (!publishFrame(appliance, contents, &error)) {
            answer(cmdId, v1::CmdStatus::Failure, error);
            return;
        }
        log(sdk::LogLevel::Info, sdk::LogCategory::Protocol, "%1: sent %2", {appliance.uuid, what});
        // The washer's acknowledgement frame is not documented (§5.1); the
        // status that follows is the confirmation a user sees.
        answer(cmdId, v1::CmdStatus::Success, {});
    }

    /// The delivery ack the cloud gives every frame but status and
    /// heartbeat (thinq_frame.h). Under the `ack` command, not `packet`.
    void acknowledge(const Appliance &appliance, const Frame &frame)
    {
        const std::optional<Bytes> ack = cloudAckFor(frame.contents, frame.longFrame);
        if (!ack)
            return;
        const std::optional<Bytes> wrapped = encodeFrame(*ack);
        if (!wrapped)
            return;
        std::string error;
        if (!m_mqtt->publish(deviceTopic(appliance.uuid), ackMessage(appliance.uuid, bytesToHex(*wrapped), nowMs()),
                             0, false, &error))
            log(sdk::LogLevel::Warn, sdk::LogCategory::Network, "%1: ack failed: %2", {appliance.uuid, error});
    }

    bool publishFrame(const Appliance &appliance, const Bytes &contents, std::string *error)
    {
        const std::optional<Bytes> frame = encodeFrame(contents);
        if (!frame) {
            if (error)
                *error = "Command too long for a frame";
            return false;
        }
        return m_mqtt->publish(deviceTopic(appliance.uuid), packetMessage(appliance.uuid, bytesToHex(*frame), nowMs()),
                               0, false, error);
    }

    // ---- appliances ----------------------------------------------------------

    Appliance &applianceFor(const std::string &uuid, const std::string &kind)
    {
        auto it = m_appliances.find(uuid);
        if (it == m_appliances.end()) {
            Appliance appliance;
            appliance.uuid = uuid;
            it = m_appliances.emplace(uuid, std::move(appliance)).first;
            log(sdk::LogLevel::Info, sdk::LogCategory::Protocol, "new appliance %1 (%2)", {uuid, kind});
        }
        Appliance &appliance = it->second;
        if (!kind.empty() && appliance.kind != kind) {
            appliance.kind = kind;
            appliance.family = familyFor(kind);
            announce(appliance, true);
            saveAppliances();
        } else if (!appliance.announced) {
            announce(appliance);
            saveAppliances();
        }
        return appliance;
    }

    void announce(Appliance &appliance, bool force = false)
    {
        if (!m_configured || (appliance.announced && !force))
            return;
        const std::string model = appliance.kind.empty() ? std::string("ThinQ") : appliance.kind;
        v1::Utf8String error;
        if (!sendDeviceUpdated(buildDevice(appliance.uuid, appliance.defaultName(), model, appliance.module.softVer),
                               buildChannels(appliance.family, appliance.decoderVerified()), &error)) {
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send the device %1: %2",
                {appliance.uuid, error});
            return;
        }
        const bool first = !appliance.announced;
        appliance.announced = true;
        if (first) {
            emitChannel(appliance, kChannelConnectivity, connectivityValue(appliance));
            if (appliance.family == Family::Washer)
                emitChannel(appliance, kChannelDelayEnd, static_cast<std::int64_t>(appliance.delayHours));
        }
    }

    void setOnline(Appliance &appliance, bool online)
    {
        if (appliance.online == online)
            return;
        appliance.online = online;
        emitChannel(appliance, kChannelConnectivity, connectivityValue(appliance));
    }

    void checkStale()
    {
        const std::int64_t limit = nowMs() - std::chrono::duration_cast<std::chrono::milliseconds>(kStaleAfter).count();
        for (auto &[uuid, appliance] : m_appliances) {
            if (appliance.online && appliance.lastSeenMs < limit)
                setOnline(appliance, false);
        }
    }

    v1::ScalarValue connectivityValue(const Appliance &appliance) const
    {
        const bool reachable = m_connected && appliance.online;
        return static_cast<std::int64_t>(reachable ? v1::ConnectivityStatus::Connected
                                                   : v1::ConnectivityStatus::Disconnected);
    }

    void emitWasher(Appliance &appliance, const WasherStatus &s)
    {
        emitChannel(appliance, kChannelState, v1::Utf8String(washerStateName(s.state)));
        emitChannel(appliance, kChannelPower, s.state != 0);
        emitChannel(appliance, kChannelCourse, static_cast<std::int64_t>(s.course));
        emitChannel(appliance, kChannelRemaining, static_cast<std::int64_t>(s.remainMinutes));
        emitChannel(appliance, kChannelTotal, static_cast<std::int64_t>(s.initialMinutes));
        emitChannel(appliance, kChannelSpin, v1::Utf8String(washerSpinName(s.spin)));
        emitChannel(appliance, kChannelTemperature, v1::Utf8String(washerTempName(s.temp)));
        emitChannel(appliance, kChannelError, v1::Utf8String(washerErrorName(s.error)));
        if (s.remoteStart)
            emitChannel(appliance, kChannelRemoteStart, *s.remoteStart);
        if (s.doorLocked)
            emitChannel(appliance, kChannelDoorLocked, *s.doorLocked);
        if (s.childLock)
            emitChannel(appliance, kChannelChildLock, *s.childLock);
        emitChannel(appliance, kChannelCycleCount, static_cast<std::int64_t>(s.cycleCount));
        emitChannel(appliance, kChannelEnergy, static_cast<std::int64_t>(s.energyWh));
    }

    void emitDishwasher(Appliance &appliance, const DishwasherStatus &s)
    {
        emitChannel(appliance, kChannelState, v1::Utf8String(dishwasherStateName(s.state)));
        emitChannel(appliance, kChannelProcess, v1::Utf8String(dishwasherProcessName(s.process)));
        emitChannel(appliance, kChannelCourse, v1::Utf8String(dishwasherCourseName(s.course)));
        emitChannel(appliance, kChannelRemaining, static_cast<std::int64_t>(s.remainMinutes));
        emitChannel(appliance, kChannelTotal, static_cast<std::int64_t>(s.initialMinutes));
        emitChannel(appliance, kChannelDoorOpen, s.doorOpen);
        emitChannel(appliance, kChannelChildLock, s.childLock);
        emitChannel(appliance, kChannelRinseAidRefill, s.rinseRefill);
        emitChannel(appliance, kChannelSaltRefill, s.saltRefill);
    }

    void emitChannel(const Appliance &appliance, const char *channelId, const v1::ScalarValue &value)
    {
        if (!appliance.announced)
            return;
        v1::Utf8String error;
        if (!sendChannelStateUpdated(appliance.uuid, channelId, value, nowMs(), &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send the state of %1/%2: %3",
                {appliance.uuid, v1::Utf8String(channelId), error});
    }

    // ---- persistence -----------------------------------------------------------

    void restoreAppliances(const Json &meta)
    {
        if (!meta.contains(kMetaDevices))
            return;
        Json devices = meta[kMetaDevices];
        if (devices.is_string())
            devices = parseObject(devices.get<std::string>());
        if (!devices.is_object())
            return;
        for (const auto &[uuid, entry] : devices.items()) {
            if (uuid.empty() || !entry.is_object())
                continue;
            Appliance appliance;
            appliance.uuid = uuid;
            appliance.kind = jsonString(entry, "kind");
            appliance.family = familyFor(appliance.kind);
            appliance.module.modelName = jsonString(entry, "modelName");
            appliance.module.softVer = jsonString(entry, "softVer");
            appliance.module.modemType = jsonString(entry, "modemType");
            m_appliances.emplace(uuid, std::move(appliance));
        }
    }

    void saveAppliances()
    {
        Json devices = Json::object();
        for (const auto &[uuid, appliance] : m_appliances) {
            Json entry = Json::object();
            entry["kind"] = appliance.kind;
            entry["modelName"] = appliance.module.modelName;
            entry["softVer"] = appliance.module.softVer;
            entry["modemType"] = appliance.module.modemType;
            devices[uuid] = entry;
        }
        Json patch = Json::object();
        patch[kMetaDevices] = devices;
        v1::Utf8String error;
        if (!sendAdapterMetaUpdated(sdk::formValuesFromJson(dump(patch)), &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send adapterMetaUpdated(devices): %1",
                {error});
    }

    // ---- plumbing --------------------------------------------------------------

    void setConnected(bool connected)
    {
        if (m_connected == connected)
            return;
        m_connected = connected;
        v1::Utf8String error;
        if (!sendConnectionStateChanged(m_connected, &error))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send connectionStateChanged: %1",
                {error});
        for (auto &[uuid, appliance] : m_appliances)
            emitChannel(appliance, kChannelConnectivity, connectivityValue(appliance));
    }

    void answer(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        v1::CmdResponse response;
        response.id = cmdId;
        response.status = status;
        response.error = error;
        response.tsMs = nowMs();
        v1::Utf8String sendError;
        if (!sendResult(response, &sendError))
            log(sdk::LogLevel::Error, sdk::LogCategory::Internal, "failed to send a command result: %1",
                {sendError});
    }

    phi::runtime::Loop *m_loop = nullptr;
    bool m_running = false;
    bool m_configured = false;
    bool m_connected = false;
    v1::Adapter m_info;
    ThinqSettings m_settings;
    std::unique_ptr<MqttClient> m_mqtt;
    phi::runtime::Timer m_retry;
    phi::runtime::Timer m_staleTimer;
    std::map<std::string, Appliance> m_appliances;
};

} // namespace

std::unique_ptr<sdk::AdapterInstance> makeInstance()
{
    return std::make_unique<ThinqInstance>();
}

} // namespace phicore::thinq::ipc
