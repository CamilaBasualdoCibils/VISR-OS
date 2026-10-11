#pragma once

#include <VISROS/DeviceEmulator/IDeviceTransport.hpp>
#include <VISROS/DeviceEmulator/EmulatedXRDevice.hpp>
#include <VISROS/DeviceEmulator/RfbTransport.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace VISROS::DeviceEmulator {

class SandboxTransport final : public IDeviceTransport {
public:
    SandboxTransport();
    ~SandboxTransport() override;
    SandboxTransport(const SandboxTransport&) = delete;
    SandboxTransport& operator=(const SandboxTransport&) = delete;

    void Poll(EmulatedXRDevice& device) override;
    void SetHeadState(const Pose& pose, float interPupillaryDistance) override;
    [[nodiscard]] bool Connected() const noexcept override;
    [[nodiscard]] std::string_view Name() const noexcept override;

private:
    struct Frame {
        std::uint8_t eye {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::vector<std::uint8_t> pixels;
        std::uint64_t generation {};
    };
    void Run(std::stop_token stopToken);
    void TrackingRun(std::stop_token stopToken);

    RfbTransport rfb_;
    std::jthread worker_;
    std::jthread trackingWorker_;
    std::atomic_bool trackingConnected_ {false};
    std::atomic_int trackingSocket_ {-1};
    std::mutex trackingMutex_;
    Pose trackingPose_ {{0.0F, 1.6F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}};
    float interPupillaryDistance_ {0.064F};
    std::atomic_bool xrConnected_ {false};
    std::atomic_int socket_ {-1};
    std::mutex mutex_;
    std::array<Frame, 2> pending_;
    std::uint64_t nextGeneration_ {1};
    bool openXRTookOver_ {false};
    std::array<std::uint64_t, 2> consumedGeneration_ {};
};

} // namespace VISROS::DeviceEmulator
