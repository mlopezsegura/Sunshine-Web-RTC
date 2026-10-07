/**
 * @file tests/unit/webrtc/test_webrtc_tv_auth.cpp
 * @brief Tests for pairing and authenticating Moonlight WebRTC TVs.
 */
// standard includes
#include <filesystem>
#include <fstream>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/webrtc/tv_auth.h"

using namespace webrtc_stream::tv_auth;

namespace {
  std::filesystem::path temporary_store(std::string_view name) {
    auto path = std::filesystem::temp_directory_path() / ("sunshine-webrtc-" + std::string(name) + ".json");
    std::filesystem::remove(path);
    return path;
  }
}  // namespace

TEST(WebrtcTvAuthTest, ComputesTheProofTheTvComputes) {
  // The same vector is checked against the TV's JavaScript implementation.
  EXPECT_EQ(authentication_proof("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"), "a8daa311fff071e004bcdf679a27d61367025038036ed95f4ff96691000053d5");
}

TEST(WebrtcTvAuthTest, ComparesInConstantTimeAndChecksHex) {
  EXPECT_TRUE(constant_time_equals("abc", "abc"));
  EXPECT_FALSE(constant_time_equals("abc", "abd"));
  EXPECT_FALSE(constant_time_equals("abc", "abcd"));
  EXPECT_TRUE(is_hex(random_hex(NONCE_BYTES), NONCE_BYTES));
  EXPECT_FALSE(is_hex("ABCD", 2));
}

TEST(WebrtcTvAuthTest, SanitizesTvNames) {
  EXPECT_EQ(sanitize_client_name("  Living room\n TV "), "Living room TV");
  EXPECT_EQ(sanitize_client_name("\x01\x02"), "Samsung TV");
  EXPECT_EQ(sanitize_client_name(std::string(100, 'a')).size(), MAXIMUM_CLIENT_NAME_LENGTH);
}

TEST(WebrtcTvAuthTest, PersistsAndForgetsPairedTvs) {
  const auto path = temporary_store("persist");
  tv_client_t client;
  {
    tv_client_store_t store(path);
    client = store.add("Bedroom");
    EXPECT_TRUE(is_hex(client.id, CLIENT_ID_BYTES));
    EXPECT_TRUE(is_hex(client.secret, CLIENT_SECRET_BYTES));
  }
  {
    tv_client_store_t store(path);
    const auto found = store.find(client.id);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->secret, client.secret);
    EXPECT_EQ(found->name, "Bedroom");
    EXPECT_TRUE(store.list().front().secret.empty());
    EXPECT_EQ(store.remove_all(), 1);
  }
  EXPECT_EQ(tv_client_store_t(path).count(), 0);
  std::filesystem::remove(path);
}

TEST(WebrtcTvAuthTest, KeepsTheNewestTvsAtCapacity) {
  const auto path = temporary_store("capacity");
  tv_client_store_t store(path);
  const auto first = store.add("first");
  for (std::size_t index = 0; index < MAXIMUM_CLIENTS; ++index) {
    store.add("tv");
  }
  EXPECT_EQ(store.count(), MAXIMUM_CLIENTS);
  EXPECT_FALSE(store.find(first.id));
  std::filesystem::remove(path);
}

TEST(WebrtcTvAuthTest, TrustsNobodyFromAnUnreadableStore) {
  const auto path = temporary_store("corrupt");
  std::ofstream(path) << "{not json";
  EXPECT_EQ(tv_client_store_t(path).count(), 0);
  std::filesystem::remove(path);
}

TEST(WebrtcTvAuthTest, AcceptsTheShownPinOnce) {
  tv_pairing_window_t window;
  const auto now = tv_pairing_window_t::clock::now();
  EXPECT_EQ(window.attempt("0000", now), pairing_attempt_e::not_open);
  const auto pin = window.open(now);
  EXPECT_EQ(window.state(now), pairing_state_e::waiting);
  EXPECT_EQ(window.attempt(pin, now), pairing_attempt_e::accepted);
  EXPECT_EQ(window.state(now), pairing_state_e::paired);
  EXPECT_EQ(window.attempt(pin, now), pairing_attempt_e::not_open);
}

TEST(WebrtcTvAuthTest, LocksAfterThreeWrongPins) {
  tv_pairing_window_t window;
  const auto now = tv_pairing_window_t::clock::now();
  const auto pin = window.open(now);
  const auto *wrong = pin == "0000" ? "1111" : "0000";
  EXPECT_EQ(window.attempt(wrong, now), pairing_attempt_e::incorrect_pin);
  EXPECT_EQ(window.attempt(wrong, now), pairing_attempt_e::incorrect_pin);
  EXPECT_EQ(window.attempt(wrong, now), pairing_attempt_e::too_many_attempts);
  EXPECT_EQ(window.state(now), pairing_state_e::failed);
  EXPECT_EQ(window.attempt(pin, now), pairing_attempt_e::not_open);
}

TEST(WebrtcTvAuthTest, ExpiresAfterTwoMinutes) {
  tv_pairing_window_t window;
  const auto now = tv_pairing_window_t::clock::now();
  const auto pin = window.open(now);
  EXPECT_EQ(window.attempt(pin, now + PAIRING_WINDOW_LIFETIME), pairing_attempt_e::not_open);
  EXPECT_EQ(window.state(now + PAIRING_WINDOW_LIFETIME), pairing_state_e::expired);
}
