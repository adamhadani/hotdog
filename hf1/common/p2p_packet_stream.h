// A buffered stream of P2P packets.
// 
// The Each pair of 

#include "p2p_packet_protocol.h"
#include "p2p_byte_stream_interface.h"
#include "priority_ring_buffer.h"
#include "status_or.h"
#ifdef ARDUINO
#include <DebugLog.h>
#define assert ASSERT
#else
#include <assert.h>
#endif

class P2PPriority {
public:
  enum Level { kLow = 0, kMedium, kHigh, kNumLevels };

  P2PPriority(Level level) : level_(level) {}  

  P2PPriority(int numeric_priority) : level_(static_cast<Level>(numeric_priority)) {
    assert(level_ < kNumLevels);
  }
  virtual operator int() const { return static_cast<int>(level_); }

  bool operator==(const P2PPriority &other) { return static_cast<int>(*this) == static_cast<int>(other); }
  bool operator!=(const P2PPriority &other) { return !(*this == other); }
  bool operator<(const P2PPriority &other) { return static_cast<int>(*this) > static_cast<int>(other); }
  bool operator<=(const P2PPriority &other) { return *this < other || *this == other; }
  bool operator>(const P2PPriority &other) { return !(*this <= other); }
  bool operator>=(const P2PPriority &other) { return !(*this < other); }

private:
  Level level_;
};

class P2PPacket {
public:
  P2PPacket() {
    data_.header.start_token = kP2PStartToken;
  }

  P2PHeader *header() { return &data_.header; }
  const P2PHeader *header() const { return &data_.header; }

  uint8_t length() const { return data_.header.length; }
  uint8_t &length() { return data_.header.length; }

  const uint8_t *content() const { return data_.content_and_footer; }
  uint8_t *content() { return data_.content_and_footer; }

  P2PSequenceNumberType sequence_number() const { return data_.header.sequence_number; }
  P2PSequenceNumberType &sequence_number() { return data_.header.sequence_number; }

  // Decodes the content in place and updates the length accordingly. 
  // Returns true if success, or false if error.
  // An error will occur if the encoded content is malformed.
  bool PrepareToRead();

  // Encodes the content in place and updates the length and checksum accordingly.
  // Returns true if success, or false if error.
  // An error will occur if the encoded content surpasses the maximum content length. 
  bool PrepareToSend();

  P2PChecksumType checksum() const { return *reinterpret_cast<const P2PChecksumType *>(&data_.content_and_footer[length() + offsetof(P2PFooter, checksum)]); }
  P2PChecksumType &checksum() { return *reinterpret_cast<P2PChecksumType *>(&data_.content_and_footer[length() + offsetof(P2PFooter, checksum)]); }

protected:
  P2PChecksumType CalculateChecksum() const; 

private:
#pragma pack(push, 1)
  struct {
    P2PHeader header;
    uint8_t content_and_footer[kP2PMaxContentLength + sizeof(P2PFooter)];
  } data_;
  //__attribute__ ((aligned (4)));  // Aligning the memory ensures that we can access it as a byte array.
#pragma pack(pop)
};

class P2PPacketView {
public:
  // Does not take ownership of the packet, which must outlive this object.
  P2PPacketView(const P2PPacket *packet) : packet_(packet) {}
  P2PPacketView() : P2PPacketView(NULL) {}

  uint8_t length() const { 
    assert(packet_ != NULL);
    return packet_->length();
  }
  const uint8_t *content() const {
    assert(packet_ != NULL);
    return packet_->content();
  }
  P2PPriority priority() const {
    assert(packet_ != NULL);
    return P2PPriority(packet_->header()->priority);
  }

private:
  const P2PPacket *packet_;
};

class P2PMutablePriorityView : public P2PPriority {
public:
  P2PMutablePriorityView(P2PHeader *header) : P2PPriority(header->priority), header_(header) {}
  P2PMutablePriorityView &operator=(P2PPriority priority) {
    header_->priority = static_cast<int>(priority);
    return *this;
  }
  virtual operator int() const { return static_cast<int>(header_->priority); }

private:
  P2PHeader *header_;
};

class P2PMutablePacketView {
public:
  // Does not take ownership of the packet, which must outlive this object.
  P2PMutablePacketView(P2PPacket *packet) : packet_(packet) {}
  P2PMutablePacketView() : P2PMutablePacketView(NULL) {}

  uint8_t length() const { 
    assert(packet_ != NULL);
    return packet_->length();
  }
  uint8_t &length() { 
    assert(packet_ != NULL);
    return packet_->length();
  }

  const uint8_t *content() const { 
    assert(packet_ != NULL);
    return packet_->content();
  }
  uint8_t *content() { 
    assert(packet_ != NULL);
    return packet_->content();
  }

  P2PPriority priority() const {
    assert(packet_ != NULL);
    return P2PPriority(packet_->header()->priority);
  }
  P2PMutablePriorityView priority() {
    assert(packet_ != NULL);
    return P2PMutablePriorityView(packet_->header());
  }

private:
  P2PPacket *packet_;
};

template<int kCapacity, Endianness LocalEndianness> class P2PPacketInputStream {
public:
  // Does not take ownership of the byte stream, which must outlive this object.
  // Only one packet stream can be associated to each byte stream at a time.
  P2PPacketInputStream(P2PByteStreamInterface<LocalEndianness> *byte_stream) : byte_stream_(*byte_stream), state_(kWaitingForPacket) {}

  // Returns the number of times that Consume() can be called without OldestPacket() returning NULL.
  int NumAvailablePackets(P2PPriority priority) const { return packet_buffer_[priority].Size(); }

  // Returns a view to the oldest packet in the stream, or kUnavailableError if empty.
  StatusOr<P2PPacketView> OldestPacket() const { 
    const P2PPacket *packet = packet_buffer_.OldestValue();
    if (packet == NULL) {
      return Status::kUnavailableError;
    }
    return P2PPacketView(packet);
  }

  // Consumes the oldest packet with highest priority in the stream. Afterwards, OldestPacket() returns a new
  // value. Returns false, if there is no packet to consume.
  bool Consume() { return packet_buffer_.Consume(); }

  void Run();

private:
  PriorityRingBuffer<P2PPacket, kCapacity, P2PPriority> packet_buffer_;
  P2PByteStreamInterface<LocalEndianness> &byte_stream_;
  unsigned int current_field_read_bytes_;
  enum State { kWaitingForPacket, kReadingHeader, kReadingContent, kDisambiguatingStartTokenInContent, kReadingFooter } state_;
  P2PHeader incoming_header_;
};

template<int kCapacity, Endianness LocalEndianness> class P2PPacketOutputStream {
public:
  // Does not take ownership of the byte stream, which must outlive this object.
  // Only one packet stream can be associated to each byte stream at a time.
  P2PPacketOutputStream(P2PByteStreamInterface<LocalEndianness> *byte_stream)
    : byte_stream_(*byte_stream), current_sequence_number_(0), state_(kGettingNextPacket) {}

  // Returns the number of packet slots available for writing in the stream.
  int NumAvailableSlots(P2PPriority priority) const {
    return packet_buffer_.Capacity(priority) - packet_buffer_.Size(priority);
  }

  // Returns a view to a new packet with `priority` in the stream, or kUnavailableError if no space is available
  // in the stream for the given priority. Commit() must be called for the packet to be finalized.
  StatusOr<P2PMutablePacketView> NewPacket(P2PPriority priority) { 
    if (NumAvailableSlots(priority) == 0) {
      return Status::kUnavailableError;
    }
    return P2PMutablePacketView(&packet_buffer_.NewValue(priority));
  }

  // Commits changes to the new packet. Must be called for the packet to be sent.
  // Afterwards, NewPacket() returns a new value.
  bool Commit(P2PPriority priority) {
    P2PPacket &packet = packet_buffer_.NewValue(priority);
    packet.header()->priority = priority;
    packet.header()->is_continuation = 0;
    packet.sequence_number() = current_sequence_number_;
    if (!packet.PrepareToSend()) { return false; }
    // Fix endianness.
    packet.checksum() = LocalToNetwork<LocalEndianness>(packet.checksum());
    packet.length() = LocalToNetwork<LocalEndianness>(packet.length());
    packet.sequence_number() = LocalToNetwork<LocalEndianness>(packet.sequence_number());

    // Increment the sequence with every byte module kP2PLowestToken, so that no byte
    // equals a token.
    // WARNING: this assumes the network order is little-endian.
    // We should protect that pre-condition with a test.
    for (unsigned int i = 0; i < sizeof(current_sequence_number_); ++i) {
      ++reinterpret_cast<uint8_t *>(&current_sequence_number_)[i];
      reinterpret_cast<uint8_t *>(&current_sequence_number_)[i] %= kP2PLowestToken;
      if (reinterpret_cast<uint8_t *>(&current_sequence_number_)[i] > 0) {
        break;
      }
    }
    packet_buffer_.Commit(priority);
    return true;
  }

  // Runs the stream and returns the minimum number of microseconds the caller may wait
  // until calling Run() again. Multi-threaded platforms can use this value to yield time
  // to other threads.
  uint64_t Run(uint64_t timestamp_ns);

private:
  PriorityRingBuffer<P2PPacket, kCapacity, P2PPriority> packet_buffer_;
  P2PByteStreamInterface<LocalEndianness> &byte_stream_;
  int total_packet_length_;
  const P2PPacket *current_packet_;
  int pending_packet_bytes_;
  int pending_burst_bytes_;
  uint64_t burst_end_timestamp_ns_;
  P2PSequenceNumberType current_sequence_number_; 
  enum State { kGettingNextPacket, kSendingBurst, kWaitingForBurstIngestion } state_;  
};

// template<int kCapacity, Endianness LocalEndianness> class P2PPacketStream {

#include "p2p_packet_stream.hh"
