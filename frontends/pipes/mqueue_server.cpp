#include <memory>
#include "mqueue_communication.h"
#include "werewolf/frontends/pipes/server_types.h"
#include "werewolf/server_communication.h"

namespace {
werewolf::frontends::ServerCommunicationFactory makeMQueueServerFactory() {
  return [](const werewolf::frontends::ServerOptions& options)
             -> std::unique_ptr<werewolf::IServerCommunication> {
    return std::make_unique<werewolf::backends::PosixMQServer>("ww");
  };
}
}

int main(int argc, char* argv[]) {
  auto options = werewolf::frontends::ParseServerArgs(argc, argv);
  if (options.show_help) {
    werewolf::frontends::PrintServerUsage(argv[0]);
    return 0;
  }
  return werewolf::frontends::RunServer(options, makeMQueueServerFactory());
}