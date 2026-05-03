#include "mqueue_communication.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <string.h>
#include <sys/stat.h>

#include <iostream>

namespace werewolf::backends {

// POSIX MQ names must start with '/'.
std::string PosixMQServer::s2p_name(int slot) const {
  return "/" + mq_prefix_ + "_s2p_" + std::to_string(slot);
}
std::string PosixMQServer::p2s_name(int slot) const {
  return "/" + mq_prefix_ + "_p2s_" + std::to_string(slot);
}
std::string PosixMQClient::s2p_name() const {
  return "/" + mq_prefix_ + "_s2p_" + std::to_string(slot_);
}
std::string PosixMQClient::p2s_name() const {
  return "/" + mq_prefix_ + "_p2s_" + std::to_string(slot_);
}

// PosixMQServer
PosixMQServer::PosixMQServer(std::string mq_prefix)
    : mq_prefix_(std::move(mq_prefix)) {}

// initialize():
bool PosixMQServer::initialize(int num_slots) {
  num_slots_ = num_slots;
  s2p_mqd_.assign(num_slots, static_cast<mqd_t>(-1));
  p2s_mqd_.assign(num_slots, static_cast<mqd_t>(-1));

  struct mq_attr attr{};
  attr.mq_maxmsg = MQ_MAX_MSGS;
  attr.mq_msgsize = static_cast<long>(MQ_MAX_MSG_SIZE);

  for (int slot = 0; slot < num_slots; ++slot) {
    mq_unlink(s2p_name(slot).c_str());
    mq_unlink(p2s_name(slot).c_str());

    int flags = O_CREAT | O_RDWR | O_NONBLOCK;

    s2p_mqd_[slot] = mq_open(s2p_name(slot).c_str(), flags, 0666, &attr);
    if (s2p_mqd_[slot] == static_cast<mqd_t>(-1)) {
      std::cerr << "[MQ] mq_open s2p slot=" << slot << " : " << strerror(errno)
                << "\n";
      return false;
    }

    p2s_mqd_[slot] = mq_open(p2s_name(slot).c_str(), flags, 0666, &attr);
    if (p2s_mqd_[slot] == static_cast<mqd_t>(-1)) {
      std::cerr << "[MQ] mq_open p2s slot=" << slot << " : " << strerror(errno)
                << "\n";
      return false;
    }
  }
  return true;
}

// shutdown():
void PosixMQServer::shutdown() {
  for (int slot = 0; slot < num_slots_; ++slot) {
    if (s2p_mqd_[slot] != static_cast<mqd_t>(-1)) {
      mq_close(s2p_mqd_[slot]);
      mq_unlink(s2p_name(slot).c_str());
      s2p_mqd_[slot] = static_cast<mqd_t>(-1);
    }
    if (p2s_mqd_[slot] != static_cast<mqd_t>(-1)) {
      mq_close(p2s_mqd_[slot]);
      mq_unlink(p2s_name(slot).c_str());
      p2s_mqd_[slot] = static_cast<mqd_t>(-1);
    }
  }
}

// send():
// mq_send() enqueues msg as ONE atomic message.
bool PosixMQServer::send(int slot, const std::string& msg) {
  if (slot < 0 || slot >= num_slots_) return false;
  if (msg.size() > MQ_MAX_MSG_SIZE) {
    std::cerr << "[MQ] message too large: " << msg.size() << "\n";
    return false;
  }
  int ret = mq_send(s2p_mqd_[slot], msg.data(), msg.size(), 0);
  if (ret < 0 && errno != EAGAIN) {
    std::cerr << "[MQ] mq_send slot=" << slot << " : " << strerror(errno)
              << "\n";
  }
  return ret == 0;
}

// recv():
std::optional<std::string> PosixMQServer::recv(int slot) {
  if (slot < 0 || slot >= num_slots_) return std::nullopt;
  char buf[MQ_MAX_MSG_SIZE];
  ssize_t n = mq_receive(p2s_mqd_[slot], buf, MQ_MAX_MSG_SIZE, nullptr);
  if (n < 0) return std::nullopt;
  return std::string(buf, static_cast<size_t>(n));
}

// broadcast():
// POSIX MQ has no native multicast — loop over send().
void PosixMQServer::broadcast(const std::string& msg,
                              const std::vector<int>& slots) {
  for (int slot : slots) send(slot, msg);
}

//  PosixMQClient
PosixMQClient::PosixMQClient(std::string mq_prefix)
    : mq_prefix_(std::move(mq_prefix)) {}

// initialize():
bool PosixMQClient::initialize(int slot_num) {
  slot_ = slot_num;
  int flags = O_RDWR | O_NONBLOCK;

  s2p_mqd_ = mq_open(s2p_name().c_str(), flags);
  if (s2p_mqd_ == static_cast<mqd_t>(-1)) {
    std::cerr << "[MQ] client mq_open s2p slot=" << slot_ << " : "
              << strerror(errno) << "\n";
    return false;
  }

  p2s_mqd_ = mq_open(p2s_name().c_str(), flags);
  if (p2s_mqd_ == static_cast<mqd_t>(-1)) {
    std::cerr << "[MQ] client mq_open p2s slot=" << slot_ << " : "
              << strerror(errno) << "\n";
    return false;
  }
  return true;
}

// shutdown():
void PosixMQClient::shutdown() {
  if (s2p_mqd_ != static_cast<mqd_t>(-1)) {
    mq_close(s2p_mqd_);
    s2p_mqd_ = static_cast<mqd_t>(-1);
  }
  if (p2s_mqd_ != static_cast<mqd_t>(-1)) {
    mq_close(p2s_mqd_);
    p2s_mqd_ = static_cast<mqd_t>(-1);
  }
}

// send(): player → server
bool PosixMQClient::send(const std::string& msg) {
  if (msg.size() > MQ_MAX_MSG_SIZE) return false;
  return mq_send(p2s_mqd_, msg.data(), msg.size(), 0) == 0;
}

// recv(): server → player (non-blocking)
std::optional<std::string> PosixMQClient::recv() {
  char buf[MQ_MAX_MSG_SIZE];
  ssize_t n = mq_receive(s2p_mqd_, buf, MQ_MAX_MSG_SIZE, nullptr);
  if (n < 0) return std::nullopt;
  return std::string(buf, static_cast<size_t>(n));
}

}