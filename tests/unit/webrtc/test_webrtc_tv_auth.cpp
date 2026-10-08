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

TEST(WebrtcTvAuthTest, DisablesAndForgetsOneTvLikeAMoonlightClient) {
  const auto path = temporary_store("single");
  tv_client_t kept;
  tv_client_t removed;
  {
    tv_client_store_t store(path);
    kept = store.add("Living room");
    removed = store.add("Bedroom");
    EXPECT_TRUE(kept.enabled);
    EXPECT_TRUE(store.set_enabled(kept.id, false));
    EXPECT_FALSE(store.set_enabled("00000000000000000000000000000000", false));
    EXPECT_TRUE(store.remove(removed.id));
    EXPECT_FALSE(store.remove(removed.id));
  }
  // Both changes survive a restart; the disabled TV stays paired.
  const tv_client_store_t reloaded(path);
  EXPECT_EQ(reloaded.count(), 1);
  ASSERT_TRUE(reloaded.find(kept.id));
  EXPECT_FALSE(reloaded.find(kept.id)->enabled);
  EXPECT_FALSE(reloaded.list().front().enabled);
  EXPECT_FALSE(reloaded.find(removed.id));
  std::filesystem::remove(path);
}

TEST(WebrtcTvAuthTest, TrustsNobodyFromAnUnreadableStore) {
  const auto path = temporary_store("corrupt");
  std::ofstream(path) << "{not json";
  EXPECT_EQ(tv_client_store_t(path).count(), 0);
  std::filesystem::remove(path);
}

TEST(WebrtcTvAuthTest, AcceptsOnlyThePinTheTvShows) {
  const auto now = pairing_request_t::clock::now();
  const auto request = make_pairing_request("0421", "Living room\n", "192.168.1.20", now);
  ASSERT_TRUE(request);
  EXPECT_TRUE(is_hex(request->id, PAIRING_ID_BYTES));
  EXPECT_EQ(request->client_name, "Living room");
  EXPECT_EQ(request->address, "192.168.1.20");
  EXPECT_EQ(check_pairing_pin(*request, "0421", now), pairing_check_e::accepted);
  EXPECT_EQ(check_pairing_pin(*request, "0420", now), pairing_check_e::incorrect_pin);
  EXPECT_EQ(check_pairing_pin(*request, "421", now), pairing_check_e::incorrect_pin);
}

TEST(WebrtcTvAuthTest, GivesEachRequestItsOwnId) {
  const auto now = pairing_request_t::clock::now();
  EXPECT_NE(make_pairing_request("0421", "", "a", now)->id, make_pairing_request("0421", "", "a", now)->id);
}

TEST(WebrtcTvAuthTest, RejectsPinsThatAreNotFourDigits) {
  const auto now = pairing_request_t::clock::now();
  EXPECT_FALSE(make_pairing_request("042", "", "a", now));
  EXPECT_FALSE(make_pairing_request("04a1", "", "a", now));
  EXPECT_FALSE(make_pairing_request("04210", "", "a", now));
}

TEST(WebrtcTvAuthTest, ExpiresAPairingRequest) {
  const auto now = pairing_request_t::clock::now();
  const auto request = make_pairing_request("0421", "", "a", now);
  EXPECT_EQ(check_pairing_pin(*request, "0421", now + PAIRING_REQUEST_LIFETIME), pairing_check_e::expired);
}
