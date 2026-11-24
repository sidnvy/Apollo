/**
 * @file src/mic.h
 * @brief Reverse microphone Opus receiver and sink abstraction.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

struct OpusDecoder;

namespace mic {

  class sink_t {
  public:
    virtual ~sink_t() = default;
    virtual bool write_pcm(const int16_t *samples, std::size_t count) = 0;
    virtual void close() = 0;
  };

  struct stats_t {
    std::uint64_t frames_received {0};
    std::uint64_t frames_decoded {0};
    std::uint64_t fec_recovered {0};
    std::uint64_t plc_generated {0};
    std::uint64_t duplicates_dropped {0};
    std::uint64_t malformed_dropped {0};
    std::uint32_t last_timestamp_samples {0};
  };

  class receiver_t {
  public:
    explicit receiver_t(std::unique_ptr<sink_t> sink);
    ~receiver_t();

    receiver_t(const receiver_t &) = delete;
    receiver_t &operator=(const receiver_t &) = delete;

    bool process_packet(std::string_view payload);
    void close();
    const stats_t &stats() const;

  private:
    std::unique_ptr<sink_t> sink_;
    OpusDecoder *decoder_ {nullptr};
    std::vector<int16_t> pcm_buf_;
    stats_t stats_;
    bool has_seq_ {false};
    std::uint16_t last_seq_ {0};
  };

}  // namespace mic
