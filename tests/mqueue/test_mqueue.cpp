#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "mqueue_communication.h"

// Initialize & Shutdown
TEST(MQueueCommunicationTest, InitializeShutdown) {
  auto server =
      std::make_unique<werewolf::backends::PosixMQServer>("ww_t1");
  ASSERT_TRUE(server->initialize(2));
  server->shutdown();
}

// Client sends, Server receives (player → server)
TEST(MQueueCommunicationTest, ClientToServer) {
  auto server = std::make_unique<werewolf::backends::PosixMQServer>("ww_t2");
  auto client = std::make_unique<werewolf::backends::PosixMQClient>("ww_t2");

  ASSERT_TRUE(server->initialize(1));
  ASSERT_TRUE(client->initialize(0));

  std::thread player([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client->send("hello");
  });

  std::optional<std::string> msg;
  for (int i = 0; i < 20; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    msg = server->recv(0);
    if (msg) break;
  }

  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg.value(), "hello");

  player.join();
  client->shutdown();
  server->shutdown();
}

// Server sends, Client receives (server → player)
TEST(MQueueCommunicationTest, ServerToClient) {
  auto server = std::make_unique<werewolf::backends::PosixMQServer>("ww_t3");
  auto client = std::make_unique<werewolf::backends::PosixMQClient>("ww_t3");

  ASSERT_TRUE(server->initialize(1));
  ASSERT_TRUE(client->initialize(0));

  std::thread sender([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server->send(0, "night starts");
  });

  std::optional<std::string> msg;
  for (int i = 0; i < 20; i++) {
    msg = client->recv();
    if (msg) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg.value(), "night starts");

  sender.join();
  client->shutdown();
  server->shutdown();
}

// FIFO ordering: multiple messages arrive in send order
TEST(MQueueCommunicationTest, FIFOOrdering) {
  auto server = std::make_unique<werewolf::backends::PosixMQServer>("ww_t4");
  auto client = std::make_unique<werewolf::backends::PosixMQClient>("ww_t4");

  ASSERT_TRUE(server->initialize(1));
  ASSERT_TRUE(client->initialize(0));

  std::thread sender([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server->send(0, "msg1");
    server->send(0, "msg2");
    server->send(0, "msg3");
  });

  // wait for all messages to arrive
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  auto m1 = client->recv();
  auto m2 = client->recv();
  auto m3 = client->recv();

  ASSERT_TRUE(m1.has_value());
  EXPECT_EQ(m1.value(), "msg1");
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m2.value(), "msg2");
  ASSERT_TRUE(m3.has_value());
  EXPECT_EQ(m3.value(), "msg3");

  sender.join();
  client->shutdown();
  server->shutdown();
}

// Broadcast: all slots receive the same message
TEST(MQueueCommunicationTest, BroadcastSendsToAllSlots) {
  auto server = std::make_unique<werewolf::backends::PosixMQServer>("ww_t5");
  auto client0 = std::make_unique<werewolf::backends::PosixMQClient>("ww_t5");
  auto client1 = std::make_unique<werewolf::backends::PosixMQClient>("ww_t5");

  ASSERT_TRUE(server->initialize(2));
  ASSERT_TRUE(client0->initialize(0));
  ASSERT_TRUE(client1->initialize(1));

  server->broadcast("day starts", {0, 1});

  std::optional<std::string> msg0, msg1;
  for (int i = 0; i < 20; i++) {
    if (!msg0) msg0 = client0->recv();
    if (!msg1) msg1 = client1->recv();
    if (msg0 && msg1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ASSERT_TRUE(msg0.has_value());
  EXPECT_EQ(msg0.value(), "day starts");
  ASSERT_TRUE(msg1.has_value());
  EXPECT_EQ(msg1.value(), "day starts");

  client0->shutdown();
  client1->shutdown();
  server->shutdown();
}

// Multicast: only specified slots receive the message
TEST(MQueueCommunicationTest, MulticastOnlyTargetSlots) {
  auto server = std::make_unique<werewolf::backends::PosixMQServer>("ww_t6");
  auto wolf0  = std::make_unique<werewolf::backends::PosixMQClient>("ww_t6");
  auto wolf1  = std::make_unique<werewolf::backends::PosixMQClient>("ww_t6");
  auto villager = std::make_unique<werewolf::backends::PosixMQClient>("ww_t6");

  ASSERT_TRUE(server->initialize(3));
  ASSERT_TRUE(wolf0->initialize(0));
  ASSERT_TRUE(wolf1->initialize(1));
  ASSERT_TRUE(villager->initialize(2));

  // send only to wolves (slot 0 and 1), not to villager (slot 2)
  server->broadcast("wolves discuss", {0, 1});

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  auto m0 = wolf0->recv();
  auto m1 = wolf1->recv();
  auto m2 = villager->recv();  // should be empty

  ASSERT_TRUE(m0.has_value());
  EXPECT_EQ(m0.value(), "wolves discuss");
  ASSERT_TRUE(m1.has_value());
  EXPECT_EQ(m1.value(), "wolves discuss");
  EXPECT_FALSE(m2.has_value());  // villager did NOT receive it

  wolf0->shutdown();
  wolf1->shutdown();
  villager->shutdown();
  server->shutdown();
}

// Non-blocking recv returns nullopt when queue is empty
TEST(MQueueCommunicationTest, NonBlockingRecvEmptyQueue) {
  auto server = std::make_unique<werewolf::backends::PosixMQServer>("ww_t7");
  auto client = std::make_unique<werewolf::backends::PosixMQClient>("ww_t7");

  ASSERT_TRUE(server->initialize(1));
  ASSERT_TRUE(client->initialize(0));

  // nothing sent — recv must return immediately with nullopt
  auto msg = client->recv();
  EXPECT_FALSE(msg.has_value());

  client->shutdown();
  server->shutdown();
}
