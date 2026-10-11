#include <VISROS/DeviceEmulator/SandboxTransport.hpp>
#include <VISROS/DeviceEmulator/EmulatedXRDevice.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <span>
#include <thread>

namespace VISROS::DeviceEmulator {
namespace {
using namespace std::chrono_literals;

bool ReceiveAll(int socket, std::span<std::uint8_t> bytes) {
    std::size_t received = 0;
    while (received < bytes.size()) {
        const auto count = recv(socket, bytes.data() + received,
                                bytes.size() - received, 0);
        if (count <= 0) return false;
        received += static_cast<std::size_t>(count);
    }
    return true;
}

std::uint32_t ReadU32(const std::uint8_t* bytes) {
    std::uint32_t value {};
    std::memcpy(&value, bytes, sizeof(value));
    return ntohl(value);
}

bool SendAll(const int socket, std::span<const std::uint8_t> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto count = send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

struct RemotePose { std::array<float, 4> orientation; std::array<float, 3> position; };
struct RemoteFov { float left, right, up, down; };
struct RemoteView { RemoteFov fov; RemotePose pose; std::uint32_t pad; };
struct RemoteHead { std::array<RemoteView, 2> views; RemotePose center; bool perViewDataValid; std::array<bool, 3> pad {}; };
struct RemoteController {
    RemotePose pose {}; std::array<float, 3> linearVelocity {}, angularVelocity {};
    std::array<float, 5> handCurl {}; std::array<float, 1> trigger {}, squeeze {}, squeezeForce {};
    std::array<float, 2> thumbstick {}; std::array<float, 1> trackpadForce {}; std::array<float, 2> trackpad {};
    std::array<bool, 16> flags {};
};
struct RemoteData { std::uint64_t header; RemoteHead head; RemoteController left, right; };
static_assert(sizeof(RemoteData) == 376, "Monado remote-driver ABI changed");

RemoteData MakeRemoteData(const Pose& pose, const float ipd) {
    RemoteData data {};
    std::memcpy(&data.header, "mndrmt3", 8);
    data.head.center = {pose.orientation, pose.position};
    data.head.perViewDataValid = true;
    for (std::size_t eye = 0; eye < data.head.views.size(); ++eye) {
        auto& view = data.head.views[eye];
        view.fov = {-0.8F, 0.8F, 0.8F, -0.8F};
        view.pose.orientation = {0.0F, 0.0F, 0.0F, 1.0F};
        view.pose.position = {eye == 0 ? -ipd * 0.5F : ipd * 0.5F, 0.0F, 0.0F};
    }
    return data;
}
} // namespace

SandboxTransport::SandboxTransport()
    : rfb_(),
      worker_([this](std::stop_token token) { Run(token); }),
      trackingWorker_([this](std::stop_token token) { TrackingRun(token); }) {}

SandboxTransport::~SandboxTransport() {
    worker_.request_stop();
    trackingWorker_.request_stop();
    const int socket = socket_.exchange(-1);
    if (socket >= 0) {
        shutdown(socket, SHUT_RDWR);
        close(socket);
    }
    const int trackingSocket = trackingSocket_.exchange(-1);
    if (trackingSocket >= 0) {
        shutdown(trackingSocket, SHUT_RDWR);
        close(trackingSocket);
    }
}

void SandboxTransport::Poll(EmulatedXRDevice& device) {
    if (!openXRTookOver_) rfb_.Poll(device);
    std::array<Frame, 2> frames;
    std::array<bool, 2> available {};
    {
        std::scoped_lock lock(mutex_);
        if (!openXRTookOver_) {
            if (!pending_[0].generation || !pending_[1].generation) return;
            for (std::size_t eye = 0; eye < pending_.size(); ++eye) {
                frames[eye] = pending_[eye];
                consumedGeneration_[eye] = pending_[eye].generation;
                available[eye] = true;
            }
            openXRTookOver_ = true;
        } else {
            for (std::size_t eye = 0; eye < pending_.size(); ++eye) {
                if (pending_[eye].generation == consumedGeneration_[eye]) continue;
                frames[eye] = pending_[eye];
                consumedGeneration_[eye] = pending_[eye].generation;
                available[eye] = true;
            }
        }
    }
    if (!available[0] && !available[1]) return;
    device.SetConnected(true);
    device.SetOpenXRSessionActive(true);
    for (std::size_t eye = 0; eye < frames.size(); ++eye)
        if (available[eye])
            device.SubmitEyeImage(eye == 0 ? Eye::Left : Eye::Right,
                                  frames[eye].width, frames[eye].height,
                                  frames[eye].pixels);
}

void SandboxTransport::SetHeadState(const Pose& pose, const float interPupillaryDistance) {
    std::scoped_lock lock(trackingMutex_);
    trackingPose_ = pose;
    interPupillaryDistance_ = interPupillaryDistance;
}

bool SandboxTransport::Connected() const noexcept {
    return xrConnected_.load() || rfb_.Connected();
}

std::string_view SandboxTransport::Name() const noexcept {
    return xrConnected_.load() ? "VISR OS stereo XR stream" : rfb_.Name();
}

void SandboxTransport::Run(const std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        const int connection = ::socket(AF_INET, SOCK_STREAM, 0);
        if (connection < 0) {
            std::this_thread::sleep_for(500ms);
            continue;
        }
        socket_.store(connection);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(4245);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (connect(connection, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) != 0) {
            close(connection);
            socket_.store(-1);
            std::this_thread::sleep_for(500ms);
            continue;
        }
        xrConnected_.store(true);
        while (!stopToken.stop_requested()) {
            std::array<std::uint8_t, 20> header {};
            if (!ReceiveAll(connection, header) ||
                std::memcmp(header.data(), "ARXR", 4) != 0) break;
            Frame frame;
            frame.eye = header[4];
            frame.width = ReadU32(header.data() + 8);
            frame.height = ReadU32(header.data() + 12);
            const auto size = ReadU32(header.data() + 16);
            if (!frame.width || !frame.height ||
                size != static_cast<std::uint64_t>(frame.width) *
                            frame.height * 4U) break;
            frame.pixels.resize(size);
            if (!ReceiveAll(connection, frame.pixels)) break;
            std::scoped_lock lock(mutex_);
            frame.generation = nextGeneration_++;
            pending_[frame.eye == 0 ? 0 : 1] = std::move(frame);
        }
        xrConnected_.store(false);
        if (socket_.exchange(-1) == connection) close(connection);
    }
}

void SandboxTransport::TrackingRun(const std::stop_token stopToken) {
    using namespace std::chrono_literals;
    while (!stopToken.stop_requested()) {
        const int connection = ::socket(AF_INET, SOCK_STREAM, 0);
        if (connection < 0) { std::this_thread::sleep_for(500ms); continue; }
        trackingSocket_.store(connection);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(4244);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(connection); trackingSocket_.store(-1); std::this_thread::sleep_for(500ms); continue;
        }
        std::array<std::uint8_t, sizeof(RemoteData)> initial {};
        if (!ReceiveAll(connection, initial) || !ReceiveAll(connection, initial)) {
            close(connection); trackingSocket_.store(-1); continue;
        }
        trackingConnected_.store(true);
        while (!stopToken.stop_requested()) {
            Pose pose; float ipd;
            { std::scoped_lock lock(trackingMutex_); pose = trackingPose_; ipd = interPupillaryDistance_; }
            const RemoteData data = MakeRemoteData(pose, ipd);
            if (!SendAll(connection, std::span {reinterpret_cast<const std::uint8_t*>(&data), sizeof(data)})) break;
            std::this_thread::sleep_for(16ms);
        }
        trackingConnected_.store(false);
        if (trackingSocket_.exchange(-1) == connection) close(connection);
    }
}

} // namespace VISROS::DeviceEmulator
