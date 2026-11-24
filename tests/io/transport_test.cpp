#include "io/transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rme::io {
namespace {

using consensus::AppendRequest;
using consensus::Entry;
using consensus::EntryType;
using consensus::Index;
using consensus::Message;
using consensus::NodeId;
using consensus::Term;

class Inbox {
 public:
  void push(Message&& message) {
    const std::scoped_lock lock(mutex_);
    messages_.push_back(std::move(message));
    ready_.notify_all();
  }

  bool wait_for(std::size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, timeout, [&] { return messages_.size() >= count; });
  }

  std::vector<Message> take() {
    const std::scoped_lock lock(mutex_);
    return std::exchange(messages_, {});
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<Message> messages_;
};

Message append(std::uint32_t from, std::uint32_t to, std::uint64_t index) {
  return Message{NodeId(from), NodeId(to), Term(1),
                 AppendRequest{Index(index - 1),
                               Term(1),
                               {Entry{Term(1), Index(index), EntryType::kCommand,
                                      std::vector<std::byte>(100, std::byte{3})}},
                               Index(0)}};
}

struct Node {
  Inbox inbox;
  std::unique_ptr<Transport> transport;
};

TEST(Transport, EveryNodeDeliversToEveryPeerInOrder) {
  constexpr std::uint32_t kNodes = 3;
  constexpr std::uint64_t kMessages = 2000;
  std::vector<Node> nodes(kNodes);
  for (std::uint32_t i = 0; i < kNodes; ++i) {
    nodes[i].transport = std::make_unique<Transport>(
        NodeId(i + 1), "127.0.0.1", 0,
        [inbox = &nodes[i].inbox](Message&& message) { inbox->push(std::move(message)); });
  }
  for (std::uint32_t i = 0; i < kNodes; ++i) {
    for (std::uint32_t j = 0; j < kNodes; ++j) {
      if (i != j) {
        nodes[i].transport->add_peer({NodeId(j + 1), "127.0.0.1", nodes[j].transport->port()});
      }
    }
    nodes[i].transport->start();
  }
  // Probe until every stream is up, since messages to a peer still connecting are dropped.
  for (std::uint32_t round = 0; round < 200; ++round) {
    for (std::uint32_t i = 0; i < kNodes; ++i) {
      for (std::uint32_t j = 0; j < kNodes; ++j) {
        if (i != j) {
          nodes[i].transport->send(append(i + 1, j + 1, 1));
        }
      }
      nodes[i].transport->flush();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    bool all = true;
    for (Node& node : nodes) {
      std::vector<Message> got = node.inbox.take();
      std::vector<bool> from(kNodes + 1, false);
      for (const Message& message : got) {
        from[message.from.value()] = true;
      }
      all = all && std::count(from.begin(), from.end(), true) == kNodes - 1;
    }
    if (all) {
      break;
    }
  }
  for (std::uint64_t k = 2; k <= kMessages + 1; ++k) {
    nodes[0].transport->send(append(1, 2, k));
    nodes[0].transport->send(append(1, 3, k));
    if (k % 16 == 0) {
      nodes[0].transport->flush();
    }
  }
  nodes[0].transport->flush();
  for (std::uint32_t j = 1; j < kNodes; ++j) {
    ASSERT_TRUE(nodes[j].inbox.wait_for(kMessages));
    std::uint64_t next = 2;
    for (const Message& message : nodes[j].inbox.take()) {
      if (std::get<AppendRequest>(message.body).entries.front().index == Index(1)) {
        continue;
      }
      ASSERT_EQ(message, append(1, j + 1, next));
      ++next;
    }
    EXPECT_EQ(next, kMessages + 2);
  }
}

TEST(Transport, ReconnectsAfterThePeerRestarts) {
  Inbox inbox;
  auto receiver = std::make_unique<Transport>(
      NodeId(2), "127.0.0.1", 0, [&inbox](Message&& message) { inbox.push(std::move(message)); });
  const std::uint16_t port = receiver->port();
  receiver->start();
  Transport sender(NodeId(1), "127.0.0.1", 0, [](Message&&) {});
  sender.add_peer({NodeId(2), "127.0.0.1", port});
  sender.start();
  const auto deliver = [&] {
    for (int attempt = 0; attempt < 500; ++attempt) {
      sender.send(append(1, 2, 1));
      sender.flush();
      if (inbox.wait_for(1, std::chrono::milliseconds(10))) {
        inbox.take();
        return true;
      }
    }
    return false;
  };
  ASSERT_TRUE(deliver());
  receiver.reset();
  receiver = std::make_unique<Transport>(NodeId(2), "127.0.0.1", port, [&inbox](Message&& message) {
    inbox.push(std::move(message));
  });
  receiver->start();
  EXPECT_TRUE(deliver());
}

TEST(Transport, DropsMessagesAddressedToAnotherNode) {
  Inbox inbox;
  Transport receiver(NodeId(2), "127.0.0.1", 0,
                     [&inbox](Message&& message) { inbox.push(std::move(message)); });
  receiver.start();
  Transport sender(NodeId(1), "127.0.0.1", 0, [](Message&&) {});
  sender.add_peer({NodeId(2), "127.0.0.1", receiver.port()});
  sender.add_peer({NodeId(3), "127.0.0.1", receiver.port()});
  sender.start();
  for (int attempt = 0; attempt < 300 && !inbox.wait_for(1, std::chrono::milliseconds(10));
       ++attempt) {
    sender.send(append(1, 3, 1));
    sender.send(append(1, 2, 1));
    sender.flush();
  }
  for (const Message& message : inbox.take()) {
    EXPECT_EQ(message.to, NodeId(2));
  }
}

}  // namespace
}  // namespace rme::io
