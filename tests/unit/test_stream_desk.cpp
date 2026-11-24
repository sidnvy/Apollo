/**
 * @file tests/unit/test_stream_desk.cpp
 * @brief Unit tests for Control Channel v1 COMMIT_TEXT, CLIPBOARD_EVENT, and fcitx5 Unix socket protocol.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
#include <moonlight-common-c/src/StreamDesk.h>
}

#include "src/stream_desk.h"

TEST(StreamDeskProtocolTest, CommitTextReassemblyMultiChunk) {
  stream_desk::text_reassembler_t reassembler;

  std::string part1 = "Hello, ";
  std::string part2 = "Stream-Desk 中文测试 🚀";

  std::vector<std::uint8_t> pkt1(SS_COMMIT_TEXT_HEADER_SIZE + part1.size());
  int len1 = LiPackCommitTextFrame(42, 0, part1.data(), static_cast<std::uint16_t>(part1.size()), pkt1.data(), static_cast<int>(pkt1.size()));
  ASSERT_GT(len1, 0);

  std::vector<std::uint8_t> pkt2(SS_COMMIT_TEXT_HEADER_SIZE + part2.size());
  int len2 = LiPackCommitTextFrame(42, SS_COMMIT_TEXT_FLAG_FINAL, part2.data(), static_cast<std::uint16_t>(part2.size()), pkt2.data(), static_cast<int>(pkt2.size()));
  ASSERT_GT(len2, 0);

  EXPECT_FALSE(reassembler.feed(std::string_view(reinterpret_cast<const char *>(pkt1.data()), static_cast<std::size_t>(len1))).has_value());
  auto done = reassembler.feed(std::string_view(reinterpret_cast<const char *>(pkt2.data()), static_cast<std::size_t>(len2)));
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(*done, part1 + part2);
}

TEST(StreamDeskProtocolTest, CommitTextRejectsOversizeAndStaleId) {
  stream_desk::text_reassembler_t reassembler;

  std::string chunk16k(SS_STREAM_DESK_MAX_CHUNK_BYTES, 'A');
  std::vector<std::uint8_t> pkt(SS_COMMIT_TEXT_HEADER_SIZE + chunk16k.size());
  int len = LiPackCommitTextFrame(100, 0, chunk16k.data(), static_cast<std::uint16_t>(chunk16k.size()), pkt.data(), static_cast<int>(pkt.size()));
  ASSERT_GT(len, 0);

  // Feed 4 * 16 KiB = 64 KiB without final, then 1 more byte should trigger overflow reset
  for (int i = 0; i < 4; ++i) {
    EXPECT_FALSE(reassembler.feed(std::string_view(reinterpret_cast<const char *>(pkt.data()), static_cast<std::size_t>(len))).has_value());
  }

  std::string extra = "X";
  std::vector<std::uint8_t> overflow_pkt(SS_COMMIT_TEXT_HEADER_SIZE + extra.size());
  int overflow_len = LiPackCommitTextFrame(100, SS_COMMIT_TEXT_FLAG_FINAL, extra.data(), 1, overflow_pkt.data(), static_cast<int>(overflow_pkt.size()));
  EXPECT_FALSE(reassembler.feed(std::string_view(reinterpret_cast<const char *>(overflow_pkt.data()), static_cast<std::size_t>(overflow_len))).has_value());

  // Stale message ID replacement cleanly abandons incomplete prior message
  std::string msg_b = "fresh";
  std::vector<std::uint8_t> pkt_a(SS_COMMIT_TEXT_HEADER_SIZE + 3);
  LiPackCommitTextFrame(200, 0, "old", 3, pkt_a.data(), static_cast<int>(pkt_a.size()));
  reassembler.feed(std::string_view(reinterpret_cast<const char *>(pkt_a.data()), pkt_a.size()));

  std::vector<std::uint8_t> pkt_b(SS_COMMIT_TEXT_HEADER_SIZE + msg_b.size());
  LiPackCommitTextFrame(201, SS_COMMIT_TEXT_FLAG_FINAL, msg_b.data(), static_cast<std::uint16_t>(msg_b.size()), pkt_b.data(), static_cast<int>(pkt_b.size()));
  auto res = reassembler.feed(std::string_view(reinterpret_cast<const char *>(pkt_b.data()), pkt_b.size()));
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(*res, "fresh");
}

TEST(StreamDeskProtocolTest, ClipboardHashNormalizesCrLf) {
  const char *lf = "line1\nline2\n你好";
  const char *crlf = "line1\r\nline2\r\n你好";
  std::uint64_t h1 = LiStreamDeskComputeClipboardHash(lf, static_cast<std::uint32_t>(std::strlen(lf)));
  std::uint64_t h2 = LiStreamDeskComputeClipboardHash(crlf, static_cast<std::uint32_t>(std::strlen(crlf)));
  EXPECT_EQ(h1, h2);
  EXPECT_NE(h1, 0ULL);
}

TEST(StreamDeskProtocolTest, ClipboardDedupSuppressesEchoLoop) {
  stream_desk::clipboard_dedup_t dedup;

  // Client pushed hash H1 -> host clipboard change with H1 must NOT echo back to client
  dedup.record_client_push(0x1122334455667788ULL);
  EXPECT_FALSE(dedup.should_forward_host_event(0x1122334455667788ULL));
  EXPECT_FALSE(dedup.should_apply_client_push(0x1122334455667788ULL));

  // Host clipboard changes to H2 -> should forward once, ignore duplicate H2, allow client re-push of H1 later
  EXPECT_TRUE(dedup.should_forward_host_event(0xAABBCCDDEEFF0011ULL));
  EXPECT_FALSE(dedup.should_forward_host_event(0xAABBCCDDEEFF0011ULL));
  EXPECT_TRUE(dedup.should_apply_client_push(0x1122334455667788ULL));
}

#ifdef __linux__
TEST(StreamDeskFcitxTest, FcitxSocketRoundTripAndProbe) {
  std::string sock_path = "/tmp/apollo-test-fcitx-" + std::to_string(::getpid()) + ".sock";
  ::unlink(sock_path.c_str());

  int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(srv, 0);
  sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  ASSERT_LT(sock_path.size(), sizeof(addr.sun_path));
  std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size() + 1);
  ASSERT_EQ(::bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
  ASSERT_EQ(::listen(srv, 4), 0);

  std::string received_commit;
  std::thread server([&]() {
    for (int i = 0; i < 2; ++i) {
      int cfd = ::accept(srv, nullptr, nullptr);
      if (cfd < 0) {
        break;
      }
      std::uint8_t hdr[12] {};
      ASSERT_EQ(::recv(cfd, hdr, 12, MSG_WAITALL), 12);
      std::uint32_t plen = (static_cast<std::uint32_t>(hdr[8]) << 24) |
                           (static_cast<std::uint32_t>(hdr[9]) << 16) |
                           (static_cast<std::uint32_t>(hdr[10]) << 8) |
                           static_cast<std::uint32_t>(hdr[11]);
      std::string body(plen, '\0');
      if (plen > 0) {
        ASSERT_EQ(::recv(cfd, body.data(), plen, MSG_WAITALL), static_cast<ssize_t>(plen));
      }
      if (hdr[4] == 0x02) {
        received_commit = body;
      }
      std::uint8_t status = 0;
      ASSERT_EQ(::send(cfd, &status, 1, 0), 1);
      ::close(cfd);
    }
  });

  setenv("STREAM_DESK_FCITX5_SOCKET", sock_path.c_str(), 1);
  EXPECT_TRUE(platf::commit_text_available());
  EXPECT_TRUE(platf::commit_text("你好 Stream-Desk 🚀"));
  server.join();
  EXPECT_EQ(received_commit, "你好 Stream-Desk 🚀");

  ::close(srv);
  ::unlink(sock_path.c_str());

  setenv("STREAM_DESK_FCITX5_SOCKET", "/tmp/nonexistent-fcitx-probe.sock", 1);
  EXPECT_FALSE(platf::commit_text_available());
  unsetenv("STREAM_DESK_FCITX5_SOCKET");
}
#endif
