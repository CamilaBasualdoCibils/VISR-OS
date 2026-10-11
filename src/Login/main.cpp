#include "VISR/Core/Debug/AllowDebugger.hpp"
#include "SystemExperience.hpp"

#include <exception>
#include <iostream>

int main() {
  VISR::Debug::AllowConfiguredDebuggerAttach();
  try {
    auto presentation = VISROS::ConnectPresentationController();
    //VISROS::ShowSystemExperience(*presentation, "VISR OS Login");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "visr-login: " << error.what() << '\n';
    return 1;
  }
}
