#pragma once

#include "VISR/Language/node.hpp"
#include "VISR/Presentation/IPresentationController.hpp"
#include "VISR/Presentation/RpcPresentationController.hpp"

#include <cstdlib>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

namespace VISROS {
inline std::unique_ptr<VISR::Presentation::IPresentationController>
ConnectPresentationController() {
  const char *host = std::getenv("VISR_PRESENTATION_HOST");
  const char *portEnv = std::getenv("VISR_PRESENTATION_PORT");
  if (!host) {
    host = "127.0.0.1";
  }
  const auto port = portEnv
                        ? static_cast<std::uint16_t>(std::stoi(portEnv))
                        : VISR::Presentation::DefaultPresentationPort;
  spdlog::info("Connecting to presentation controller at {}:{}", host, port);
  return std::make_unique<VISR::Presentation::RpcPresentationController>(
      host, port);
}

} // namespace VISROS
