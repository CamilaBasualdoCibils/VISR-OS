#include <VISROS/VirtualDisplay/RfbClient.hpp>
#include <GL/gl.h>
#include <GL/glx.h>
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_GLX
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <mutex>
#include <span>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
void Check(XrResult result, const char *operation) {
  if (XR_FAILED(result))
    throw std::runtime_error(std::string(operation) +
                             " failed (OpenXR result " +
                             std::to_string(result) + ")");
}
bool HasExtension(const char *wanted) {
  uint32_t n = 0;
  Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr),
        "enumerate extensions");
  std::vector<XrExtensionProperties> p(n, {XR_TYPE_EXTENSION_PROPERTIES});
  Check(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, p.data()),
        "enumerate extensions");
  return std::ranges::any_of(p, [&](const auto &e) {
    return std::strcmp(e.extensionName, wanted) == 0;
  });
}

struct StereoFrame { std::uint32_t width{}, height{}; std::vector<std::uint8_t> rgba; std::uint64_t generation{}; };
class StereoStreamClient {
public:
  StereoStreamClient() : worker_([this](std::stop_token stop) { Run(stop); }) {}
  ~StereoStreamClient() { worker_.request_stop(); const int fd = socket_.exchange(-1); if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); } }
  bool Poll(StereoFrame &frame) { std::scoped_lock lock(mutex_); if (generation_ == consumed_) return false; frame = latest_; consumed_ = generation_; return true; }
private:
  static bool ReceiveAll(int fd, std::span<std::uint8_t> bytes) { std::size_t received = 0; while (received < bytes.size()) { const auto n = recv(fd, bytes.data() + received, bytes.size() - received, 0); if (n <= 0) return false; received += static_cast<std::size_t>(n); } return true; }
  static std::uint32_t ReadU32(const std::uint8_t *bytes) { std::uint32_t value{}; std::memcpy(&value, bytes, sizeof(value)); return ntohl(value); }
  void Run(std::stop_token stop) {
    using namespace std::chrono_literals;
    while (!stop.stop_requested()) {
      const int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) { std::this_thread::sleep_for(500ms); continue; }
      socket_.store(fd);
      sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(4245); inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
      if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) { close(fd); socket_.store(-1); std::this_thread::sleep_for(500ms); continue; }
      while (!stop.stop_requested()) {
        std::array<std::uint8_t, 20> header{};
        if (!ReceiveAll(fd, header) || std::memcmp(header.data(), "ARXR", 4) != 0) break;
        StereoFrame frame; const auto eye = header[4]; frame.width = ReadU32(header.data() + 8); frame.height = ReadU32(header.data() + 12); const auto size = ReadU32(header.data() + 16);
        if (eye > 1 || !frame.width || !frame.height || size != std::uint64_t(frame.width) * frame.height * 4U) break;
        frame.rgba.resize(size); if (!ReceiveAll(fd, frame.rgba)) break;
        if (eye == 0) { std::scoped_lock lock(mutex_); frame.generation = ++generation_; latest_ = std::move(frame); }
      }
      if (socket_.exchange(-1) == fd) close(fd);
    }
  }
  std::jthread worker_; std::atomic_int socket_{-1}; std::mutex mutex_; StereoFrame latest_; std::uint64_t generation_{}, consumed_{};
};

class Bridge {
public:
  Bridge() : rfb_("127.0.0.1", 5901, "XR Display Bridge") { Initialize(); }
  ~Bridge() {
    DestroySwapchain();
    if (space_)
      xrDestroySpace(space_);
    if (session_)
      xrDestroySession(session_);
    if (instance_)
      xrDestroyInstance(instance_);
    if (window_)
      glfwDestroyWindow(window_);
    glfwTerminate();
  }
  int Run() {
    std::cout << "XR Display Bridge: waiting for VISR OS framebuffer at "
              << rfb_.Endpoint() << "\n";
    while (!exit_) {
      glfwPollEvents();
      PollEvents();
      VISROS::VirtualDisplay::Frame incoming;
      if (rfb_.Poll(incoming)) {
        AcceptFrame(std::move(incoming));
      }
      StereoFrame stereo;
      if (stereo_.Poll(stereo)) {
        AcceptStereoFrame(std::move(stereo));
      }
      if (running_)
        RenderFrame();
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
  }

private:
  void AcceptFrame(VISROS::VirtualDisplay::Frame incoming) {
    if (frame_.width != incoming.width || frame_.height != incoming.height)
      CreateSwapchain(incoming.width, incoming.height);
    frame_.width = incoming.width;
    frame_.height = incoming.height;
    frame_.generation = incoming.generation;
    const std::size_t rowBytes = std::size_t(incoming.width) * 4U;
    frame_.rgba.resize(incoming.rgba.size());
    for (std::uint32_t y = 0; y < incoming.height; ++y)
      std::copy_n(incoming.rgba.data() +
                      std::size_t(incoming.height - 1U - y) * rowBytes,
                  rowBytes, frame_.rgba.data() + std::size_t(y) * rowBytes);
  }
  void AcceptStereoFrame(StereoFrame incoming) {
    if (frame_.width != incoming.width || frame_.height != incoming.height)
      CreateSwapchain(incoming.width, incoming.height);
    frame_.width = incoming.width;
    frame_.height = incoming.height;
    frame_.generation = incoming.generation;
    frame_.rgba = std::move(incoming.rgba);
    std::cout << "XR Display Bridge: receiving VISR OpenXR frames on 4245\\n";
  }
  void Initialize() {
    if (!HasExtension(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME))
      throw std::runtime_error(
          "active OpenXR runtime does not support XR_KHR_opengl_enable");
          glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    if (!glfwInit())
      throw std::runtime_error("GLFW could not connect to the host display; an "
                               "X11 OpenGL session is required");
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    window_ =
        glfwCreateWindow(16, 16, "VISR OS XR Display Bridge", nullptr, nullptr);
    if (!window_)
      throw std::runtime_error("could not create the hidden OpenGL context");
    glfwMakeContextCurrent(window_);
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ci.applicationInfo.applicationName, "VISR OS XR Display Bridge",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strncpy(ci.applicationInfo.engineName, "VISR OS",
                 XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    const char *extensions[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = extensions;
    Check(xrCreateInstance(&ci, &instance_), "create instance");
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult systemResult = xrGetSystem(instance_, &si, &system_);
    if (systemResult == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
      throw std::runtime_error("OpenXR runtime is active, but no headset is "
                               "connected (start WiVRn and connect Quest 2)");
    Check(systemResult, "get HMD system");
    PFN_xrGetOpenGLGraphicsRequirementsKHR requirementsFn{};
    Check(xrGetInstanceProcAddr(
              instance_, "xrGetOpenGLGraphicsRequirementsKHR",
              reinterpret_cast<PFN_xrVoidFunction *>(&requirementsFn)),
          "load OpenGL requirements");
    XrGraphicsRequirementsOpenGLKHR requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    Check(requirementsFn(instance_, system_, &requirements),
          "get OpenGL requirements");
    Display *display = glfwGetX11Display();
    const Window drawable = glfwGetGLXWindow(window_);
    const Window xWindow = glfwGetX11Window(window_);
    XWindowAttributes wa{};
    XGetWindowAttributes(display, xWindow, &wa);
    int count = 0;
    GLXFBConfig *configs =
        glXGetFBConfigs(display, DefaultScreen(display), &count);
    GLXFBConfig selected{};
    for (int i = 0; i < count; ++i) {
      XVisualInfo *vi = glXGetVisualFromFBConfig(display, configs[i]);
      if (vi && vi->visualid == XVisualIDFromVisual(wa.visual)) {
        selected = configs[i];
        XFree(vi);
        break;
      }
      if (vi)
        XFree(vi);
    }
    XFree(configs);
    if (!selected)
      throw std::runtime_error(
          "could not identify GLFW's GLX framebuffer configuration");
    XrGraphicsBindingOpenGLXlibKHR binding{
        XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
    binding.xDisplay = display;
    binding.visualid = XVisualIDFromVisual(wa.visual);
    binding.glxFBConfig = selected;
    binding.glxDrawable = drawable;
    binding.glxContext = glfwGetGLXContext(window_);
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system_;
    Check(xrCreateSession(instance_, &sci, &session_), "create session");
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rs.poseInReferenceSpace.orientation.w = 1;
    Check(xrCreateReferenceSpace(session_, &rs, &space_),
          "create VIEW reference space");
    std::cout << "XR Display Bridge: OpenXR session created; waiting for "
                 "runtime readiness\n";
  }
  void PollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
      if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto &changed =
            *reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
        state_ = changed.state;
        if (state_ == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
          begin.primaryViewConfigurationType =
              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          Check(xrBeginSession(session_, &begin), "begin session");
          running_ = true;
        } else if (state_ == XR_SESSION_STATE_STOPPING) {
          running_ = false;
          Check(xrEndSession(session_), "end session");
        } else if (state_ == XR_SESSION_STATE_EXITING ||
                   state_ == XR_SESSION_STATE_LOSS_PENDING)
          exit_ = true;
      } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        exit_ = true;
      event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
  }
  void DestroySwapchain() {
    if (swapchain_) {
      xrDestroySwapchain(swapchain_);
      swapchain_ = XR_NULL_HANDLE;
    }
    images_.clear();
    swapWidth_ = swapHeight_ = 0;
  }
  void CreateSwapchain(uint32_t width, uint32_t height) {
    if (!width || !height)
      return;
    DestroySwapchain();
    uint32_t n = 0;
    Check(xrEnumerateSwapchainFormats(session_, 0, &n, nullptr),
          "enumerate swapchain formats");
    std::vector<int64_t> formats(n);
    Check(xrEnumerateSwapchainFormats(session_, n, &n, formats.data()),
          "enumerate swapchain formats");
    auto choose = [&](int64_t f) {
      return std::find(formats.begin(), formats.end(), f) != formats.end();
    };
    int64_t format = choose(GL_SRGB8_ALPHA8) ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    if (!choose(format))
      throw std::runtime_error(
          "OpenXR runtime offers no RGBA8 OpenGL swapchain format");
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags =
        XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    Check(xrCreateSwapchain(session_, &ci, &swapchain_),
          "create display swapchain");
    Check(xrEnumerateSwapchainImages(swapchain_, 0, &n, nullptr),
          "count swapchain images");
    images_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    Check(xrEnumerateSwapchainImages(
              swapchain_, n, &n,
              reinterpret_cast<XrSwapchainImageBaseHeader *>(images_.data())),
          "enumerate swapchain images");
    swapWidth_ = width;
    swapHeight_ = height;
    std::cout << "XR Display Bridge: virtual monitor is " << width << 'x'
              << height << "\n";
  }
  void RenderFrame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    Check(xrWaitFrame(session_, &wi, &fs), "wait frame");
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    Check(xrBeginFrame(session_, &bi), "begin frame");
    std::array<const XrCompositionLayerBaseHeader *, 1> layers{};
    uint32_t count = 0;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (fs.shouldRender && swapchain_ && !frame_.rgba.empty()) {
      uint32_t index = 0;
      XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      Check(xrAcquireSwapchainImage(swapchain_, &ai, &index), "acquire image");
      XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      swi.timeout = XR_INFINITE_DURATION;
      Check(xrWaitSwapchainImage(swapchain_, &swi), "wait image");
      glBindTexture(GL_TEXTURE_2D, images_[index].image);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, swapWidth_, swapHeight_, GL_RGBA,
                      GL_UNSIGNED_BYTE, frame_.rgba.data());
      glBindTexture(GL_TEXTURE_2D, 0);
      XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      Check(xrReleaseSwapchainImage(swapchain_, &ri), "release image");
      quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      quad.space = space_;
      quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      quad.subImage.swapchain = swapchain_;
      quad.subImage.imageRect.extent = {int32_t(swapWidth_),
                                        int32_t(swapHeight_)};
      quad.pose.orientation.w = 1;
      quad.pose.position.z = -1.5F;
      quad.size.width = 1.6F;
      quad.size.height = 1.6F * float(swapHeight_) / float(swapWidth_);
      layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad);
      count = 1;
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = count;
    ei.layers = count ? layers.data() : nullptr;
    Check(xrEndFrame(session_, &ei), "end frame");
  }
  VISROS::VirtualDisplay::RfbClient rfb_;
  VISROS::VirtualDisplay::Frame frame_;
  GLFWwindow *window_{};
  XrInstance instance_{};
  XrSystemId system_{};
  XrSession session_{};
  XrSpace space_{};
  XrSwapchain swapchain_{};
  std::vector<XrSwapchainImageOpenGLKHR> images_;
  uint32_t swapWidth_{}, swapHeight_{};
  XrSessionState state_{XR_SESSION_STATE_UNKNOWN};
  bool running_{}, exit_{};
  StereoStreamClient stereo_;
};
} // namespace
int main() {
  try {
    return Bridge{}.Run();
  } catch (const std::exception &e) {
    std::cerr << "XR Display Bridge: " << e.what()
              << "\nCheck that XR_RUNTIME_JSON selects WiVRn, the WiVRn server "
                 "is running, and Quest 2 is connected.\n";
    return 1;
  }
}
