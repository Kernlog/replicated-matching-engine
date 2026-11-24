#include "io/raft_codec.hpp"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <variant>

namespace rme::io {

namespace {

using consensus::AppendRequest;
using consensus::AppendResponse;
using consensus::Entry;
using consensus::EntryType;
using consensus::Index;
using consensus::InstallSnapshot;
using consensus::Message;
using consensus::NodeId;
using consensus::Term;
using consensus::VoteRequest;
using consensus::VoteResponse;

class Writer {
 public:
  explicit Writer(std::vector<std::byte>& out) : out_(&out) {}

  template <typename T>
    requires std::is_trivially_copyable_v<T>
  void put(T value) {
    const std::size_t at = out_->size();
    out_->resize(at + sizeof value);
    std::memcpy(&(*out_)[at], &value, sizeof value);
  }

  void put_bytes(std::span<const std::byte> bytes) {
    put(static_cast<std::uint32_t>(bytes.size()));
    out_->insert(out_->end(), bytes.begin(), bytes.end());
  }

 private:
  std::vector<std::byte>* out_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  template <typename T>
    requires std::is_trivially_copyable_v<T>
  bool get(T& value) noexcept {
    if (bytes_.size() < sizeof value) {
      return false;
    }
    std::memcpy(&value, bytes_.data(), sizeof value);
    bytes_ = bytes_.subspan(sizeof value);
    return true;
  }

  bool get_flag(bool& value) noexcept {
    std::uint8_t raw = 0;
    if (!get(raw) || raw > 1) {
      return false;
    }
    value = raw == 1;
    return true;
  }

  template <typename S>
  bool get_strong(S& value) noexcept {
    typename S::ValueType raw{};
    if (!get(raw)) {
      return false;
    }
    value = S(raw);
    return true;
  }

  bool get_bytes(std::vector<std::byte>& out) {
    std::uint32_t size = 0;
    if (!get(size) || bytes_.size() < size) {
      return false;
    }
    const std::span<const std::byte> taken = bytes_.first(size);
    out.assign(taken.begin(), taken.end());
    bytes_ = bytes_.subspan(size);
    return true;
  }

  [[nodiscard]] std::size_t left() const noexcept { return bytes_.size(); }

 private:
  std::span<const std::byte> bytes_;
};

// Smallest encoded entry: term, index, type and the data length.
constexpr std::size_t kMinEntryBytes = 8 + 8 + 1 + 4;

void put_flag(Writer& writer, bool value) {
  writer.put(static_cast<std::uint8_t>(value ? 1 : 0));
}

void encode_body(Writer& writer, const VoteRequest& body) {
  writer.put(body.last_index.value());
  writer.put(body.last_term.value());
  put_flag(writer, body.pre_vote);
}

void encode_body(Writer& writer, const VoteResponse& body) {
  put_flag(writer, body.granted);
  put_flag(writer, body.pre_vote);
}

void encode_body(Writer& writer, const AppendRequest& body) {
  writer.put(body.prev_index.value());
  writer.put(body.prev_term.value());
  writer.put(body.commit.value());
  writer.put(static_cast<std::uint32_t>(body.entries.size()));
  for (const Entry& entry : body.entries) {
    writer.put(entry.term.value());
    writer.put(entry.index.value());
    writer.put(static_cast<std::uint8_t>(entry.type));
    writer.put_bytes(entry.data);
  }
}

void encode_body(Writer& writer, const AppendResponse& body) {
  put_flag(writer, body.success);
  writer.put(body.match.value());
  writer.put(body.conflict_index.value());
  writer.put(body.conflict_term.value());
}

void encode_body(Writer& writer, const InstallSnapshot& body) {
  writer.put(body.snapshot.index.value());
  writer.put(body.snapshot.term.value());
  writer.put_bytes(body.snapshot.data);
}

bool decode_body(Reader& reader, VoteRequest& body) {
  return reader.get_strong(body.last_index) && reader.get_strong(body.last_term) &&
         reader.get_flag(body.pre_vote);
}

bool decode_body(Reader& reader, VoteResponse& body) {
  return reader.get_flag(body.granted) && reader.get_flag(body.pre_vote);
}

bool decode_body(Reader& reader, AppendRequest& body) {
  std::uint32_t count = 0;
  if (!reader.get_strong(body.prev_index) || !reader.get_strong(body.prev_term) ||
      !reader.get_strong(body.commit) || !reader.get(count) ||
      reader.left() / kMinEntryBytes < count) {
    return false;
  }
  body.entries.resize(count);
  for (Entry& entry : body.entries) {
    std::uint8_t type = 0;
    if (!reader.get_strong(entry.term) || !reader.get_strong(entry.index) || !reader.get(type) ||
        type > static_cast<std::uint8_t>(EntryType::kCommand) || !reader.get_bytes(entry.data)) {
      return false;
    }
    entry.type = static_cast<EntryType>(type);
  }
  return true;
}

bool decode_body(Reader& reader, AppendResponse& body) {
  return reader.get_flag(body.success) && reader.get_strong(body.match) &&
         reader.get_strong(body.conflict_index) && reader.get_strong(body.conflict_term);
}

bool decode_body(Reader& reader, InstallSnapshot& body) {
  return reader.get_strong(body.snapshot.index) && reader.get_strong(body.snapshot.term) &&
         reader.get_bytes(body.snapshot.data);
}

template <std::size_t I = 0>
bool decode_variant(Reader& reader, std::uint8_t kind, consensus::Body& body) {
  if constexpr (I < std::variant_size_v<consensus::Body>) {
    if (kind == I) {
      return decode_body(reader, body.emplace<I>());
    }
    return decode_variant<I + 1>(reader, kind, body);
  } else {
    return false;
  }
}

}  // namespace

void encode_message(const Message& message, std::vector<std::byte>& out) {
  Writer writer(out);
  writer.put(message.from.value());
  writer.put(message.to.value());
  writer.put(message.term.value());
  writer.put(static_cast<std::uint8_t>(message.body.index()));
  std::visit([&writer](const auto& body) { encode_body(writer, body); }, message.body);
}

std::optional<Message> decode_message(std::span<const std::byte> bytes) {
  Reader reader(bytes);
  Message message;
  std::uint8_t kind = 0;
  if (!reader.get_strong(message.from) || !reader.get_strong(message.to) ||
      !reader.get_strong(message.term) || !reader.get(kind) ||
      !decode_variant(reader, kind, message.body) || reader.left() != 0) {
    return std::nullopt;
  }
  return message;
}

}  // namespace rme::io
