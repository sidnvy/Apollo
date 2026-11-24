/**
 * @file tests/unit/test_mic.cpp
 * @brief Unit tests for Control Channel v1 reverse microphone framing, Opus decoding, FEC, and wraparound.
 */
#include "../tests_common.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opus/opus.h>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
#include <moonlight-common-c/src/Mic.h>
}

#include <src/mic.h>

namespace {

  class mock_mic_sink_t final: public mic::sink_t {
  public:
    bool write_pcm(const int16_t *samples, std::size_t count) override {
      if (closed) {
        return false;
      }
      pcm.insert(pcm.end(), samples, samples + count);
      ++writes;
      return true;
    }

    void close() override {
      closed = true;
    }

    std::vector<int16_t> pcm;
    std::size_t writes {0};
    bool closed {false};
  };

  std::vector<int16_t> make_sine_frame(double freq_hz, std::size_t sample_offset) {
    std::vector<int16_t> pcm(LI_MIC_FRAME_SAMPLES);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
      const double t = static_cast<double>(sample_offset + i) / static_cast<double>(LI_MIC_SAMPLE_RATE);
      pcm[i] = static_cast<int16_t>(std::round(12000.0 * std::sin(2.0 * M_PI * freq_hz * t)));
    }
    return pcm;
  }

  std::string pack_opus_frame(OpusEncoder *enc,
                              const std::vector<int16_t> &pcm,
                              std::uint16_t seq,
                              std::uint32_t timestamp_samples) {
    std::vector<unsigned char> opus(LI_MIC_MAX_OPUS_BYTES);
    const int encoded = opus_encode(enc, pcm.data(), LI_MIC_FRAME_SAMPLES, opus.data(), static_cast<opus_int32>(opus.size()));
    EXPECT_GT(encoded, 0);

    std::string packed(sizeof(SS_MIC_FRAME_HEADER) + static_cast<std::size_t>(encoded), '\0');
    auto *hdr = reinterpret_cast<unsigned char *>(packed.data());
    hdr[0] = static_cast<unsigned char>((seq >> 8) & 0xFF);
    hdr[1] = static_cast<unsigned char>(seq & 0xFF);
    hdr[2] = static_cast<unsigned char>((encoded >> 8) & 0xFF);
    hdr[3] = static_cast<unsigned char>(encoded & 0xFF);
    hdr[4] = static_cast<unsigned char>((timestamp_samples >> 24) & 0xFF);
    hdr[5] = static_cast<unsigned char>((timestamp_samples >> 16) & 0xFF);
    hdr[6] = static_cast<unsigned char>((timestamp_samples >> 8) & 0xFF);
    hdr[7] = static_cast<unsigned char>(timestamp_samples & 0xFF);
    std::memcpy(packed.data() + sizeof(SS_MIC_FRAME_HEADER), opus.data(), static_cast<std::size_t>(encoded));
    return packed;
  }

  double goertzel_magnitude(const std::vector<int16_t> &samples, double target_hz) {
    const double omega = 2.0 * M_PI * target_hz / static_cast<double>(LI_MIC_SAMPLE_RATE);
    const double coeff = 2.0 * std::cos(omega);
    double q0 = 0.0, q1 = 0.0, q2 = 0.0;
    for (int16_t s : samples) {
      q0 = coeff * q1 - q2 + static_cast<double>(s);
      q2 = q1;
      q1 = q0;
    }
    return std::sqrt(q1 * q1 + q2 * q2 - q1 * q2 * coeff);
  }

}  // namespace

TEST(MicProtocolTest, PackAndUnpackHeaderWraparound) {
  int err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(LI_MIC_SAMPLE_RATE, LI_MIC_CHANNELS, OPUS_APPLICATION_VOIP, &err);
  ASSERT_EQ(err, OPUS_OK);
  ASSERT_NE(enc, nullptr);

  auto sink = std::make_unique<mock_mic_sink_t>();
  auto *sink_ptr = sink.get();
  mic::receiver_t receiver(std::move(sink));

  const auto f1 = make_sine_frame(440.0, 0);
  const auto f2 = make_sine_frame(440.0, LI_MIC_FRAME_SAMPLES);
  const std::uint32_t ts1 = 62913600U;
  const std::uint32_t ts2 = 62914560U;

  // seq wraps 65535 -> 0 while timestampSamples advances monotonically past 21.84 min.
  ASSERT_TRUE(receiver.process_packet(pack_opus_frame(enc, f1, 65535U, ts1)));
  ASSERT_TRUE(receiver.process_packet(pack_opus_frame(enc, f2, 0U, ts2)));

  EXPECT_EQ(receiver.stats().frames_decoded, 2U);
  EXPECT_EQ(receiver.stats().duplicates_dropped, 0U);
  EXPECT_EQ(receiver.stats().plc_generated, 0U);
  EXPECT_EQ(receiver.stats().last_timestamp_samples, ts2);
  EXPECT_EQ(sink_ptr->pcm.size(), 2U * LI_MIC_FRAME_SAMPLES);

  opus_encoder_destroy(enc);
}

TEST(MicReceiverTest, OpusToneRoundTrip) {
  int err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(LI_MIC_SAMPLE_RATE, LI_MIC_CHANNELS, OPUS_APPLICATION_VOIP, &err);
  ASSERT_EQ(err, OPUS_OK);
  ASSERT_EQ(opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000)), OPUS_OK);
  ASSERT_EQ(opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1)), OPUS_OK);

  auto sink = std::make_unique<mock_mic_sink_t>();
  auto *sink_ptr = sink.get();
  mic::receiver_t receiver(std::move(sink));

  for (std::uint16_t i = 0; i < 25; ++i) {
    const auto frame = make_sine_frame(440.0, static_cast<std::size_t>(i) * LI_MIC_FRAME_SAMPLES);
    ASSERT_TRUE(receiver.process_packet(pack_opus_frame(enc, frame, i, static_cast<std::uint32_t>(i) * LI_MIC_FRAME_SAMPLES)));
  }

  ASSERT_EQ(sink_ptr->pcm.size(), 25U * LI_MIC_FRAME_SAMPLES);
  // Skip first 5 frames for Opus encoder warm-up, then verify dominant 440 Hz tone.
  std::vector<int16_t> steady(sink_ptr->pcm.begin() + 5 * LI_MIC_FRAME_SAMPLES, sink_ptr->pcm.end());
  const double mag_440 = goertzel_magnitude(steady, 440.0);
  const double mag_1000 = goertzel_magnitude(steady, 1000.0);
  EXPECT_GT(mag_440, mag_1000 * 5.0);

  opus_encoder_destroy(enc);
}

TEST(MicReceiverTest, SinglePacketLossUsesFec) {
  int err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(LI_MIC_SAMPLE_RATE, LI_MIC_CHANNELS, OPUS_APPLICATION_VOIP, &err);
  ASSERT_EQ(err, OPUS_OK);
  ASSERT_EQ(opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1)), OPUS_OK);
  ASSERT_EQ(opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(15)), OPUS_OK);

  auto sink = std::make_unique<mock_mic_sink_t>();
  auto *sink_ptr = sink.get();
  mic::receiver_t receiver(std::move(sink));

  for (std::uint16_t i = 0; i < 6; ++i) {
    const auto frame = make_sine_frame(440.0, static_cast<std::size_t>(i) * LI_MIC_FRAME_SAMPLES);
    const std::string pkt = pack_opus_frame(enc, frame, i, static_cast<std::uint32_t>(i) * LI_MIC_FRAME_SAMPLES);
    if (i == 3) {
      continue;  // drop seq 3
    }
    ASSERT_TRUE(receiver.process_packet(pkt));
  }

  EXPECT_EQ(receiver.stats().fec_recovered, 1U);
  EXPECT_EQ(receiver.stats().plc_generated, 0U);
  EXPECT_EQ(sink_ptr->pcm.size(), 6U * LI_MIC_FRAME_SAMPLES);

  opus_encoder_destroy(enc);
}

TEST(MicReceiverTest, DtxGapResetsCleanly) {
  int err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(LI_MIC_SAMPLE_RATE, LI_MIC_CHANNELS, OPUS_APPLICATION_VOIP, &err);
  ASSERT_EQ(err, OPUS_OK);

  auto sink = std::make_unique<mock_mic_sink_t>();
  auto *sink_ptr = sink.get();
  mic::receiver_t receiver(std::move(sink));

  ASSERT_TRUE(receiver.process_packet(pack_opus_frame(enc, make_sine_frame(440.0, 0), 10, 9600U)));
  ASSERT_TRUE(receiver.process_packet(pack_opus_frame(enc, make_sine_frame(440.0, 960), 35, 33600U)));

  EXPECT_EQ(receiver.stats().plc_generated, 0U);
  EXPECT_EQ(receiver.stats().fec_recovered, 0U);
  EXPECT_EQ(receiver.stats().frames_decoded, 2U);
  EXPECT_EQ(sink_ptr->pcm.size(), 2U * LI_MIC_FRAME_SAMPLES);

  opus_encoder_destroy(enc);
}

TEST(MicReceiverTest, RejectsMalformedAndDuplicates) {
  int err = OPUS_OK;
  OpusEncoder *enc = opus_encoder_create(LI_MIC_SAMPLE_RATE, LI_MIC_CHANNELS, OPUS_APPLICATION_VOIP, &err);
  ASSERT_EQ(err, OPUS_OK);

  auto sink = std::make_unique<mock_mic_sink_t>();
  mic::receiver_t receiver(std::move(sink));

  EXPECT_FALSE(receiver.process_packet(std::string(4, '\0')));
  EXPECT_EQ(receiver.stats().malformed_dropped, 1U);

  const std::string p5 = pack_opus_frame(enc, make_sine_frame(440.0, 0), 5, 4800U);
  const std::string p4 = pack_opus_frame(enc, make_sine_frame(440.0, 0), 4, 3840U);

  std::string mismatched = p5;
  mismatched.push_back('\x01');
  EXPECT_FALSE(receiver.process_packet(mismatched));
  EXPECT_EQ(receiver.stats().malformed_dropped, 2U);

  ASSERT_TRUE(receiver.process_packet(p5));
  ASSERT_TRUE(receiver.process_packet(p5));  // duplicate
  ASSERT_TRUE(receiver.process_packet(p4));  // out-of-order older frame
  EXPECT_EQ(receiver.stats().frames_decoded, 1U);
  EXPECT_EQ(receiver.stats().duplicates_dropped, 2U);

  opus_encoder_destroy(enc);
}
