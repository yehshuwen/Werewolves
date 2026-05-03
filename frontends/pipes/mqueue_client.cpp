#include <memory>
#include <string>

#include "mqueue_communication.h"
#include "werewolf/client_communication.h"
#include "werewolf/frontends/pipes/client_types.h"

namespace {
werewolf::frontends::ClientCommunicationFactory makeMQueueClientFactory() {
  return [](const werewolf::frontends::ClientOptions& options)
             -> std::unique_ptr<werewolf::IClientCommunication> {
    return std::make_unique<werewolf::backends::PosixMQClient>("ww");
  };
}
}

int main(int argc, char* argv[]) {
  auto options = werewolf::frontends::ParseClientArgs(argc, argv);
  if (options.show_help) {
    werewolf::frontends::PrintClientUsage(argv[0]);
    return 0;
  }
  return werewolf::frontends::RunClient(options, makeMQueueClientFactory());
}