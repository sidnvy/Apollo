/**
 * @file src/mic.cpp
 * @brief Platform-agnostic reverse microphone Opus decoding and FEC/PLC handling.
 */
#include "mic.h"

#include <cstring>

#include <opus/opus.h>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
#include <moonlight-common-c/src/Mic.h>
}

#include "logging.h"
#include "platform/common.h"

namespace {
  std::uint16_t read_be16(const std::uint8_t *src) {
    return (static_cast<std::uint16_t>(src[0]) << 8) | static_cast<std::uint16_t>(src[1]);
  }

  std::uint32_t read_be32(const std::uint8_t *src) {
    return (static_cast<std::uint32_t>(src[0]) << 24) |
           (static_cast<std::uint32_t>(src[1]) << 16) |
           (static_cast<std::uint32_t>(src[2]) << 8) |
           static_cast<std::uint32_t>(src[3]);
  }
}  // namespace

namespace mic {

  receiver_t::receiver_t(std::unique_ptr<sink_t> sink):
      sink_(std::move(sink)),
      pcm_buf_(LI_MIC_FRAME_SAMPLES) {
    int err = OPUS_OK;
    decoder_ = opus_decoder_create(LI_MIC_SAMPLE_RATE, LI_MIC_CHANNELS, &err);
    if (err != OPUS_OK || decoder_ == nullptr) {
      BOOST_LOG(error) << "Failed to create reverse mic Opus decoder: " << opus_strerror(err);
      decoder_ = nullptr;
    }
  }

  receiver_t::~receiver_t() {
    close();
    if (decoder_ != nullptr) {
      opus_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
  }

  void receiver_t::close() {
    if (sink_) {
      sink_->close();
    }
  }

  const stats_t &receiver_t::stats() const {
    return stats_;
  }

  bool receiver_t::process_packet(std::string_view payload) {
    if (!decoder_ || !sink_) {
      return false;
    }

    if (payload.size() <= sizeof(SS_MIC_FRAME_HEADER)) {
      ++stats_.malformed_dropped;
      return false;
    }

    const auto *raw = reinterpret_cast<const std::uint8_t *>(payload.data());
    const std::uint16_t seq = read_be16(raw + 0);
    const std::uint16_t opus_len = read_be16(raw + 2);
    const std::uint32_t timestamp_samples = read_be32(raw + 4);

    if (opus_len == 0 || opus_len > LI_MIC_MAX_OPUS_BYTES ||
        payload.size() != sizeof(SS_MIC_FRAME_HEADER) + opus_len) {
      ++stats_.malformed_dropped;
      return false;
    }

    ++stats_.frames_received;

    const auto *opus_data = reinterpret_cast<const unsigned char *>(raw + sizeof(SS_MIC_FRAME_HEADER));

    if (has_seq_) {
      const std::uint16_t delta = static_cast<std::uint16_t>(seq - last_seq_);
      if (delta == 0 || delta >= 0x8000U) {
        ++stats_.duplicates_dropped;
        return true;
      }

      if (delta == 2) {
        // Recover the single missing packet using in-band FEC from the current frame.
        const int fec_samples = opus_decode(decoder_, opus_data, opus_len, pcm_buf_.data(), LI_MIC_FRAME_SAMPLES, 1);
        if (fec_samples > 0) {
          sink_->write_pcm(pcm_buf_.data(), static_cast<std::size_t>(fec_samples));
          ++stats_.fec_recovered;
        }
      } else if (delta >= 3 && delta <= 5) {
        for (std::uint16_t i = 1; i < delta; ++i) {
          const int plc_samples = opus_decode(decoder_, nullptr, 0, pcm_buf_.data(), LI_MIC_FRAME_SAMPLES, 0);
          if (plc_samples > 0) {
            sink_->write_pcm(pcm_buf_.data(), static_cast<std::size_t>(plc_samples));
            ++stats_.plc_generated;
          }
        }
      }
      // delta > 5 is treated as a DTX silence gap or stall; reset sequence baseline cleanly.
    }

    const int decoded_samples = opus_decode(decoder_, opus_data, opus_len, pcm_buf_.data(), LI_MIC_FRAME_SAMPLES, 0);
    if (decoded_samples < 0) {
      BOOST_LOG(warning) << "Reverse mic Opus decode failed: " << opus_strerror(decoded_samples);
      ++stats_.malformed_dropped;
      return false;
    }

    has_seq_ = true;
    last_seq_ = seq;
    stats_.last_timestamp_samples = timestamp_samples;

    if (decoded_samples > 0) {
      if (!sink_->write_pcm(pcm_buf_.data(), static_cast<std::size_t>(decoded_samples))) {
        return false;
      }
      ++stats_.frames_decoded;
    }

    return true;
  }

}  // namespace mic

#if !defined(__linux__)
namespace platf {
  bool mic_available() {
    return false;
  }

  std::unique_ptr<mic::sink_t> create_mic_sink() {
    return nullptr;
  }
}  // namespace platf
#endif
