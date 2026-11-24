#include "gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <array>
#include <asio/connect.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <cstdint>
#include <optional>
#include <vector>

#include "io/frame.hpp"
#include "testing/command_stream.hpp"
#include "tests/node/cluster.hpp"

namespace rme::gateway {
namespace {

using node::LocalCluster;

// A blocking test client speaking the frame protocol to one gateway.
class Wire {
 public:
  explicit Wire(std::uint16_t port) : socket_(io_) {
    socket_.connect({asio::ip::make_address("127.0.0.1"), port});
  }

  void submit(const codec::ClientRecordBytes& record) {
    std::vector<std::byte> out;
    encode_submit(record, out);
    asio::write(socket_, asio::buffer(out));
  }

  Ack next_ack() {
    for (;;) {
      if (const std::optional<io::Frame> frame = decoder_.next()) {
        EXPECT_EQ(frame->type, static_cast<std::uint16_t>(FrameType::kAck));
        return decode_ack(frame->body).value_or(Ack{});
      }
      const std::span<std::byte> space = decoder_.writable();
      decoder_.commit(socket_.read_some(asio::buffer(space.data(), space.size())));
    }
  }

 private:
  asio::io_context io_;
  asio::ip::tcp::socket socket_;
  io::FrameDecoder decoder_;
};

TEST(Gateway, AcksEveryCommandAfterApplyAndReplaysDuplicates) {
  LocalCluster cluster(3);
  ASSERT_TRUE(LocalCluster::eventually([&] { return cluster.leader().has_value(); }));
  Wire wire(cluster.client_port(cluster.leader().value_or(1)));
  wire.submit(codec::encode_register(42));
  const Ack registered = wire.next_ack();
  ASSERT_EQ(registered.result, engine::SessionResult::kRegistered);
  ASSERT_EQ(registered.seq, 42U);
  const std::uint64_t session = registered.client;

  testing::CommandStream stream(3);
  std::vector<codec::ClientRecordBytes> sent;
  for (std::uint64_t seq = 1; seq <= 300; ++seq) {
    sent.push_back(codec::encode(
        engine::ClientCommand{.client = session, .seq = seq, .command = stream.next()}));
    wire.submit(sent.back());
  }
  std::vector<Ack> acks;
  for (std::uint64_t seq = 1; seq <= 300; ++seq) {
    acks.push_back(wire.next_ack());
    EXPECT_EQ(acks.back().seq, seq);
    EXPECT_EQ(acks.back().result, engine::SessionResult::kApplied);
    EXPECT_GT(acks.back().index, registered.index);
  }
  wire.submit(sent[299]);
  const Ack duplicate = wire.next_ack();
  EXPECT_EQ(duplicate.result, engine::SessionResult::kDuplicate);
  EXPECT_EQ(duplicate.events, acks.back().events);
  for (std::uint32_t id = 1; id <= 3; ++id) {
    EXPECT_TRUE(LocalCluster::eventually(
        [&] { return cluster.node(id).status().commit >= acks.back().index; }));
  }
}

TEST(Protocol, AckRoundTripsAndRejectsMalformedBodies) {
  const Ack ack{.index = 9,
                .client = 3,
                .seq = 4,
                .result = engine::SessionResult::kApplied,
                .events = {engine::Accepted{OrderId(5)}}};
  std::vector<std::byte> frame;
  encode_ack(ack, frame);
  const std::vector<std::byte> body(frame.begin() + sizeof(io::FrameHeader), frame.end());
  EXPECT_EQ(decode_ack(body), ack);
  for (std::size_t size = 0; size < body.size(); ++size) {
    EXPECT_FALSE(decode_ack(std::span(body).first(size)).has_value()) << size;
  }
  std::vector<std::byte> bad_result = body;
  bad_result[24] = std::byte{99};
  EXPECT_FALSE(decode_ack(bad_result).has_value());
}

}  // namespace
}  // namespace rme::gateway
