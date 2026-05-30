#pragma once

#include <cstdint>
#include <cstring>

struct IsoTpContext {
  enum class State { IDLE, RECEIVING, COMPLETE, ERROR };

  uint8_t* buffer;
  uint16_t buffer_size;
  uint16_t expected_len;
  uint16_t received_len;
  uint8_t next_seq;
  State state;
};

class IsoTp {
 public:
  enum class Result { OK, DATA_READY, NEED_FC, ERROR, NOT_ISOTP };

  static Result handleFrame(IsoTpContext& ctx, const uint8_t* rx_data, uint8_t dlc) {
    if (dlc < 2)
      return Result::NOT_ISOTP;

    uint8_t pci_type = rx_data[0] >> 4;
    uint8_t pci_len = rx_data[0] & 0x0F;

    switch (pci_type) {
      case 0:  // Single Frame
        ctx.expected_len = pci_len;
        if (ctx.expected_len > ctx.buffer_size)
          return Result::ERROR;
        memcpy(ctx.buffer, &rx_data[1], ctx.expected_len);
        ctx.received_len = ctx.expected_len;
        ctx.state = IsoTpContext::State::COMPLETE;
        return Result::DATA_READY;

      case 1:  // First Frame
        ctx.expected_len = ((uint16_t)pci_len << 8) | rx_data[1];
        if (ctx.expected_len > ctx.buffer_size)
          return Result::ERROR;
        memcpy(ctx.buffer, &rx_data[2], 6);
        ctx.received_len = 6;
        ctx.next_seq = 1;
        ctx.state = IsoTpContext::State::RECEIVING;
        return Result::NEED_FC;

      case 2:  // Consecutive Frame
        if (ctx.state != IsoTpContext::State::RECEIVING)
          return Result::ERROR;
        if (pci_len != ctx.next_seq)
          return Result::ERROR;

        uint8_t toCopy = (ctx.expected_len - ctx.received_len < 7) ? (ctx.expected_len - ctx.received_len) : 7;
        memcpy(&ctx.buffer[ctx.received_len], &rx_data[1], toCopy);

        ctx.received_len += toCopy;
        ctx.next_seq = (ctx.next_seq + 1) & 0x0F;

        if (ctx.received_len >= ctx.expected_len) {
          ctx.state = IsoTpContext::State::COMPLETE;
          return Result::DATA_READY;
        }
        return Result::OK;
    }
    return Result::NOT_ISOTP;
  }
};
