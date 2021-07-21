// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quic/core/http/http_encoder.h"
#include <cstdint>
#include <memory>

#include "quic/core/crypto/quic_random.h"
#include "quic/core/quic_data_writer.h"
#include "quic/core/quic_types.h"
#include "quic/platform/api/quic_bug_tracker.h"
#include "quic/platform/api/quic_flag_utils.h"
#include "quic/platform/api/quic_flags.h"
#include "quic/platform/api/quic_logging.h"

namespace quic {

namespace {

bool WriteFrameHeader(QuicByteCount length,
                      HttpFrameType type,
                      QuicDataWriter* writer) {
  return writer->WriteVarInt62(static_cast<uint64_t>(type)) &&
         writer->WriteVarInt62(length);
}

QuicByteCount GetTotalLength(QuicByteCount payload_length, HttpFrameType type) {
  return QuicDataWriter::GetVarInt62Len(payload_length) +
         QuicDataWriter::GetVarInt62Len(static_cast<uint64_t>(type)) +
         payload_length;
}

}  // namespace

// static
QuicByteCount HttpEncoder::GetDataFrameHeaderLength(
    QuicByteCount payload_length) {
  QUICHE_DCHECK_NE(0u, payload_length);
  return QuicDataWriter::GetVarInt62Len(payload_length) +
         QuicDataWriter::GetVarInt62Len(
             static_cast<uint64_t>(HttpFrameType::DATA));
}

// static
QuicBuffer HttpEncoder::SerializeDataFrameHeader(
    QuicByteCount payload_length,
    QuicBufferAllocator* allocator) {
  QUICHE_DCHECK_NE(0u, payload_length);
  QuicByteCount header_length = GetDataFrameHeaderLength(payload_length);

  QuicBuffer header(allocator, header_length);
  QuicDataWriter writer(header.size(), header.data());

  if (WriteFrameHeader(payload_length, HttpFrameType::DATA, &writer)) {
    return header;
  }
  QUIC_DLOG(ERROR)
      << "Http encoder failed when attempting to serialize data frame header.";
  return QuicBuffer();
}

// static
QuicByteCount HttpEncoder::SerializeHeadersFrameHeader(
    QuicByteCount payload_length,
    std::unique_ptr<char[]>* output) {
  QUICHE_DCHECK_NE(0u, payload_length);
  QuicByteCount header_length =
      QuicDataWriter::GetVarInt62Len(payload_length) +
      QuicDataWriter::GetVarInt62Len(
          static_cast<uint64_t>(HttpFrameType::HEADERS));

  output->reset(new char[header_length]);
  QuicDataWriter writer(header_length, output->get());

  if (WriteFrameHeader(payload_length, HttpFrameType::HEADERS, &writer)) {
    return header_length;
  }
  QUIC_DLOG(ERROR)
      << "Http encoder failed when attempting to serialize headers "
         "frame header.";
  return 0;
}

// static
QuicByteCount HttpEncoder::SerializeSettingsFrame(
    const SettingsFrame& settings,
    std::unique_ptr<char[]>* output) {
  QuicByteCount payload_length = 0;
  std::vector<std::pair<uint64_t, uint64_t>> ordered_settings{
      settings.values.begin(), settings.values.end()};
  std::sort(ordered_settings.begin(), ordered_settings.end());
  // Calculate the payload length.
  for (const auto& p : ordered_settings) {
    payload_length += QuicDataWriter::GetVarInt62Len(p.first);
    payload_length += QuicDataWriter::GetVarInt62Len(p.second);
  }

  QuicByteCount total_length =
      GetTotalLength(payload_length, HttpFrameType::SETTINGS);

  output->reset(new char[total_length]);
  QuicDataWriter writer(total_length, output->get());

  if (!WriteFrameHeader(payload_length, HttpFrameType::SETTINGS, &writer)) {
    QUIC_DLOG(ERROR) << "Http encoder failed when attempting to serialize "
                        "settings frame header.";
    return 0;
  }

  for (const auto& p : ordered_settings) {
    if (!writer.WriteVarInt62(p.first) || !writer.WriteVarInt62(p.second)) {
      QUIC_DLOG(ERROR) << "Http encoder failed when attempting to serialize "
                          "settings frame payload.";
      return 0;
    }
  }

  return total_length;
}

// static
QuicByteCount HttpEncoder::SerializeGoAwayFrame(
    const GoAwayFrame& goaway,
    std::unique_ptr<char[]>* output) {
  QuicByteCount payload_length = QuicDataWriter::GetVarInt62Len(goaway.id);
  QuicByteCount total_length =
      GetTotalLength(payload_length, HttpFrameType::GOAWAY);

  output->reset(new char[total_length]);
  QuicDataWriter writer(total_length, output->get());

  if (WriteFrameHeader(payload_length, HttpFrameType::GOAWAY, &writer) &&
      writer.WriteVarInt62(goaway.id)) {
    return total_length;
  }
  QUIC_DLOG(ERROR)
      << "Http encoder failed when attempting to serialize goaway frame.";
  return 0;
}

// static
QuicByteCount HttpEncoder::SerializePriorityUpdateFrame(
    const PriorityUpdateFrame& priority_update,
    std::unique_ptr<char[]>* output) {
  if (priority_update.prioritized_element_type != REQUEST_STREAM) {
    QUIC_BUG(quic_bug_10402_1)
        << "PRIORITY_UPDATE for push streams not implemented";
    return 0;
  }

  QuicByteCount payload_length =
      QuicDataWriter::GetVarInt62Len(priority_update.prioritized_element_id) +
      priority_update.priority_field_value.size();
  QuicByteCount total_length = GetTotalLength(
      payload_length, HttpFrameType::PRIORITY_UPDATE_REQUEST_STREAM);

  output->reset(new char[total_length]);
  QuicDataWriter writer(total_length, output->get());

  if (WriteFrameHeader(payload_length,
                       HttpFrameType::PRIORITY_UPDATE_REQUEST_STREAM,
                       &writer) &&
      writer.WriteVarInt62(priority_update.prioritized_element_id) &&
      writer.WriteBytes(priority_update.priority_field_value.data(),
                        priority_update.priority_field_value.size())) {
    return total_length;
  }

  QUIC_DLOG(ERROR) << "Http encoder failed when attempting to serialize "
                      "PRIORITY_UPDATE frame.";
  return 0;
}

// static
QuicByteCount HttpEncoder::SerializeAcceptChFrame(
    const AcceptChFrame& accept_ch,
    std::unique_ptr<char[]>* output) {
  QuicByteCount payload_length = 0;
  for (const auto& entry : accept_ch.entries) {
    payload_length += QuicDataWriter::GetVarInt62Len(entry.origin.size());
    payload_length += entry.origin.size();
    payload_length += QuicDataWriter::GetVarInt62Len(entry.value.size());
    payload_length += entry.value.size();
  }

  QuicByteCount total_length =
      GetTotalLength(payload_length, HttpFrameType::ACCEPT_CH);

  output->reset(new char[total_length]);
  QuicDataWriter writer(total_length, output->get());

  if (!WriteFrameHeader(payload_length, HttpFrameType::ACCEPT_CH, &writer)) {
    QUIC_DLOG(ERROR)
        << "Http encoder failed to serialize ACCEPT_CH frame header.";
    return 0;
  }

  for (const auto& entry : accept_ch.entries) {
    if (!writer.WriteStringPieceVarInt62(entry.origin) ||
        !writer.WriteStringPieceVarInt62(entry.value)) {
      QUIC_DLOG(ERROR)
          << "Http encoder failed to serialize ACCEPT_CH frame payload.";
      return 0;
    }
  }

  return total_length;
}

// static
QuicByteCount HttpEncoder::SerializeGreasingFrame(
    std::unique_ptr<char[]>* output) {
  uint64_t frame_type;
  QuicByteCount payload_length;
  std::string payload;
  if (!GetQuicFlag(FLAGS_quic_enable_http3_grease_randomness)) {
    frame_type = 0x40;
    payload_length = 1;
    payload = "a";
  } else {
    uint32_t result;
    QuicRandom::GetInstance()->RandBytes(&result, sizeof(result));
    frame_type = 0x1fULL * static_cast<uint64_t>(result) + 0x21ULL;

    // The payload length is random but within [0, 3];
    payload_length = result % 4;

    if (payload_length > 0) {
      std::unique_ptr<char[]> buffer(new char[payload_length]);
      QuicRandom::GetInstance()->RandBytes(buffer.get(), payload_length);
      payload = std::string(buffer.get(), payload_length);
    }
  }
  QuicByteCount total_length = QuicDataWriter::GetVarInt62Len(frame_type) +
                               QuicDataWriter::GetVarInt62Len(payload_length) +
                               payload_length;

  output->reset(new char[total_length]);
  QuicDataWriter writer(total_length, output->get());

  bool success =
      writer.WriteVarInt62(frame_type) && writer.WriteVarInt62(payload_length);

  if (payload_length > 0) {
    success &= writer.WriteBytes(payload.data(), payload_length);
  }

  if (success) {
    return total_length;
  }

  QUIC_DLOG(ERROR) << "Http encoder failed when attempting to serialize "
                      "greasing frame.";
  return 0;
}

QuicByteCount HttpEncoder::SerializeWebTransportStreamFrameHeader(
    WebTransportSessionId session_id,
    std::unique_ptr<char[]>* output) {
  uint64_t stream_type =
      static_cast<uint64_t>(HttpFrameType::WEBTRANSPORT_STREAM);
  QuicByteCount header_length = QuicDataWriter::GetVarInt62Len(stream_type) +
                                QuicDataWriter::GetVarInt62Len(session_id);

  *output = std::make_unique<char[]>(header_length);
  QuicDataWriter writer(header_length, output->get());
  bool success =
      writer.WriteVarInt62(stream_type) && writer.WriteVarInt62(session_id);
  if (success && writer.remaining() == 0) {
    return header_length;
  }

  QUIC_DLOG(ERROR) << "Http encoder failed when attempting to serialize "
                      "WEBTRANSPORT_STREAM frame header.";
  return 0;
}

// static
QuicByteCount HttpEncoder::SerializeCapsuleFrame(
    const CapsuleFrame& capsule_frame, std::unique_ptr<char[]>* output) {
  QuicByteCount capsule_type_length = QuicDataWriter::GetVarInt62Len(
      static_cast<uint64_t>(capsule_frame.capsule_type));
  QuicByteCount capsule_data_length;
  switch (capsule_frame.capsule_type) {
    case CapsuleType::REGISTER_DATAGRAM_CONTEXT:
      capsule_data_length =
          QuicDataWriter::GetVarInt62Len(
              capsule_frame.register_datagram_context_capsule.context_id) +
          capsule_frame.register_datagram_context_capsule.context_extensions
              .length();
      break;
    case CapsuleType::CLOSE_DATAGRAM_CONTEXT:
      capsule_data_length =
          QuicDataWriter::GetVarInt62Len(
              capsule_frame.close_datagram_context_capsule.context_id) +
          capsule_frame.close_datagram_context_capsule.context_extensions
              .length();
      break;
    case CapsuleType::DATAGRAM:
      capsule_data_length =
          capsule_frame.datagram_capsule.http_datagram_payload.length();
      if (capsule_frame.datagram_capsule.context_id.has_value()) {
        capsule_data_length += QuicDataWriter::GetVarInt62Len(
            capsule_frame.datagram_capsule.context_id.value());
      }
      break;
    case CapsuleType::REGISTER_DATAGRAM_NO_CONTEXT:
      capsule_data_length = capsule_frame.register_datagram_no_context_capsule
                                .context_extensions.length();
      break;
    default:
      capsule_data_length = capsule_frame.unknown_capsule_data.length();
      break;
  }
  QuicByteCount frame_length_field_value =
      capsule_type_length + capsule_data_length;
  QuicByteCount total_frame_length =
      QuicDataWriter::GetVarInt62Len(
          static_cast<uint64_t>(HttpFrameType::CAPSULE)) +
      QuicDataWriter::GetVarInt62Len(frame_length_field_value) +
      capsule_type_length + capsule_data_length;
  *output = std::make_unique<char[]>(total_frame_length);
  QuicDataWriter writer(total_frame_length, output->get());
  if (!writer.WriteVarInt62(static_cast<uint64_t>(HttpFrameType::CAPSULE))) {
    QUIC_BUG(capsule frame type write fail)
        << "Failed to write CAPSULE frame type";
    return 0;
  }
  if (!writer.WriteVarInt62(frame_length_field_value)) {
    QUIC_BUG(capsule frame length write fail)
        << "Failed to write CAPSULE frame length";
    return 0;
  }
  if (!writer.WriteVarInt62(
          static_cast<uint64_t>(capsule_frame.capsule_type))) {
    QUIC_BUG(capsule type write fail) << "Failed to write CAPSULE type";
    return 0;
  }
  switch (capsule_frame.capsule_type) {
    case CapsuleType::REGISTER_DATAGRAM_CONTEXT:
      if (!writer.WriteVarInt62(
              capsule_frame.register_datagram_context_capsule.context_id)) {
        QUIC_BUG(register context capsule context ID write fail)
            << "Failed to write REGISTER_DATAGRAM_CONTEXT CAPSULE context ID";
        return 0;
      }
      if (!writer.WriteBytes(capsule_frame.register_datagram_context_capsule
                                 .context_extensions.data(),
                             capsule_frame.register_datagram_context_capsule
                                 .context_extensions.length())) {
        QUIC_BUG(register context capsule extensions write fail)
            << "Failed to write REGISTER_DATAGRAM_CONTEXT CAPSULE extensions";
        return 0;
      }
      break;
    case CapsuleType::CLOSE_DATAGRAM_CONTEXT:
      if (!writer.WriteVarInt62(
              capsule_frame.close_datagram_context_capsule.context_id)) {
        QUIC_BUG(close context capsule context ID write fail)
            << "Failed to write CLOSE_DATAGRAM_CONTEXT CAPSULE context ID";
        return 0;
      }
      if (!writer.WriteBytes(capsule_frame.close_datagram_context_capsule
                                 .context_extensions.data(),
                             capsule_frame.close_datagram_context_capsule
                                 .context_extensions.length())) {
        QUIC_BUG(close context capsule extensions write fail)
            << "Failed to write CLOSE_DATAGRAM_CONTEXT CAPSULE extensions";
        return 0;
      }
      break;
    case CapsuleType::DATAGRAM:
      if (capsule_frame.datagram_capsule.context_id.has_value()) {
        if (!writer.WriteVarInt62(
                capsule_frame.datagram_capsule.context_id.value())) {
          QUIC_BUG(datagram capsule context ID write fail)
              << "Failed to write DATAGRAM CAPSULE context ID";
          return 0;
        }
      }
      if (!writer.WriteBytes(
              capsule_frame.datagram_capsule.http_datagram_payload.data(),
              capsule_frame.datagram_capsule.http_datagram_payload.length())) {
        QUIC_BUG(datagram capsule payload write fail)
            << "Failed to write DATAGRAM CAPSULE payload";
        return 0;
      }
      break;
    case CapsuleType::REGISTER_DATAGRAM_NO_CONTEXT:
      if (!writer.WriteBytes(capsule_frame.register_datagram_no_context_capsule
                                 .context_extensions.data(),
                             capsule_frame.register_datagram_no_context_capsule
                                 .context_extensions.length())) {
        QUIC_BUG(register no context capsule extensions write fail)
            << "Failed to write REGISTER_DATAGRAM_NO_CONTEXT CAPSULE "
               "extensions";
        return 0;
      }
      break;
    default:
      if (!writer.WriteBytes(capsule_frame.unknown_capsule_data.data(),
                             capsule_frame.unknown_capsule_data.length())) {
        QUIC_BUG(capsule data write fail) << "Failed to write CAPSULE data";
        return 0;
      }
      break;
  }
  if (writer.remaining() != 0) {
    QUIC_BUG(capsule write length mismatch)
        << "CAPSULE serialization wrote " << writer.length() << " instead of "
        << writer.capacity();
    return 0;
  }
  return total_frame_length;
}

}  // namespace quic
