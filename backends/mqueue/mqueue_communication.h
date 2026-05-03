#pragma once

#include <mqueue.h>

#include <optional>
#include <string>
#include <vector>

#include "werewolf/client_communication.h"
#include "werewolf/server_communication.h"

namespace werewolf::backends {

static constexpr size_t MQ_MAX_MSG_SIZE = 4096;
static constexpr long MQ_MAX_MSGS = 10;

// Server
class PosixMQServer : public IServerCommunication {
 public:
  explicit PosixMQServer(std::string mq_prefix);

  bool initialize(int num_slots) override;
  void shutdown() override;
  bool send(int slot, const std::string& msg) override;
  std::optional<std::string> recv(int slot) override;
  void broadcast(const std::string& msg,
                 const std::vector<int>& slots) override;

 private:
  std::string mq_prefix_;
  int num_slots_ = 0;
  std::vector<mqd_t> s2p_mqd_;
  std::vector<mqd_t> p2s_mqd_;

  std::string s2p_name(int slot) const;
  std::string p2s_name(int slot) const;
};

// Client
class PosixMQClient : public IClientCommunication {
 public:
  explicit PosixMQClient(std::string mq_prefix);

  bool initialize(int slot_num) override;
  void shutdown() override;
  bool send(const std::string& msg) override;
  std::optional<std::string> recv() override;

 private:
  std::string mq_prefix_;
  int slot_ = -1;
  mqd_t s2p_mqd_ = static_cast<mqd_t>(-1);
  mqd_t p2s_mqd_ = static_cast<mqd_t>(-1);

  std::string s2p_name() const;
  std::string p2s_name() const;
};

}