/**
 * @file src/webrtc/tv_auth.h
 * @brief Declarations for pairing and authenticating Moonlight WebRTC TVs.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webrtc_stream::tv_auth {
  constexpr std::size_t CLIENT_ID_BYTES = 16;  ///< Length of an issued client ID.
  constexpr std::size_t CLIENT_SECRET_BYTES = 32;  ///< Length of an issued client secret.
  constexpr std::size_t NONCE_BYTES = 32;  ///< Length of a connection nonce.
  constexpr std::size_t MAXIMUM_CLIENTS = 32;  ///< Paired TVs kept; the oldest is dropped beyond this.
  constexpr std::size_t MAXIMUM_CLIENT_NAME_LENGTH = 64;  ///< Longest stored TV name.
  constexpr std::size_t PAIRING_ID_BYTES = 16;  ///< Length of a pairing request ID.
  constexpr std::chrono::seconds PAIRING_REQUEST_LIFETIME {300};  ///< How long a TV's PIN waits for approval.

  /**
   * @brief Generate random bytes as lower-case hex.
   * @param bytes Number of random bytes.
   * @return Hex string of twice @p bytes characters.
   */
  std::string random_hex(std::size_t bytes);

  /**
   * @brief Check that a value is lower-case hex of a given byte length.
   * @param value Value to check.
   * @param bytes Expected byte length.
   * @return True when the value matches.
   */
  bool is_hex(std::string_view value, std::size_t bytes);

  /**
   * @brief Compute the proof a TV sends for a nonce.
   *
   * The TV proves it holds its secret without sending it: HMAC-SHA256 keyed with the secret
   * over a label and the single-use nonce. The label keeps the proof from being reusable as an
   * HMAC over the bare nonce in any other context.
   *
   * @param client_secret_hex Client secret.
   * @param nonce_hex Connection nonce.
   * @return Proof in lower-case hex.
   */
  std::string authentication_proof(std::string_view client_secret_hex, std::string_view nonce_hex);

  /**
   * @brief Compare two strings without leaking the position of the first difference.
   * @param left First string.
   * @param right Second string.
   * @return True when the strings are equal.
   */
  bool constant_time_equals(std::string_view left, std::string_view right);

  /**
   * @brief Reduce a TV-supplied name to a short printable label.
   * @param name Name sent by the TV.
   * @return Sanitized name, or "Samsung TV" when nothing printable remains.
   */
  std::string sanitize_client_name(std::string_view name);

  /**
   * @brief A paired TV.
   */
  struct tv_client_t {
    std::string id;  ///< Client ID.
    std::string name;  ///< Display name.
    std::string secret;  ///< Shared secret.
    bool enabled = true;  ///< Whether the TV may connect; a disabled TV stays paired.
  };

  /**
   * @brief Paired TVs, persisted in Sunshine's configuration directory.
   *
   * Secrets are stored as issued because Sunshine must recompute each proof; the file is
   * protected like Sunshine's own private key next to it.
   */
  class tv_client_store_t {
  public:
    /**
     * @brief Load the store from a file.
     * @param path Store file; a missing or unreadable file trusts no TV.
     */
    explicit tv_client_store_t(std::filesystem::path path);

    /**
     * @brief Find a paired TV.
     * @param id Client ID.
     * @return The TV, or nothing when it is not paired.
     */
    std::optional<tv_client_t> find(std::string_view id) const;

    /**
     * @brief Pair a new TV and persist the store.
     * @param name Display name sent by the TV.
     * @return The issued credentials.
     */
    tv_client_t add(std::string_view name);

    /**
     * @brief Forget one paired TV and persist the store.
     * @param id Client ID.
     * @return True when the TV was paired.
     */
    bool remove(std::string_view id);

    /**
     * @brief Forget every paired TV and persist the store.
     * @return Number of TVs removed.
     */
    std::size_t remove_all();

    /**
     * @brief Allow or refuse a paired TV and persist the store.
     * @param id Client ID.
     * @param enabled Whether the TV may connect.
     * @return True when the TV is paired.
     */
    bool set_enabled(std::string_view id, bool enabled);

    /**
     * @brief List paired TVs without their secrets.
     * @return Paired TVs with empty secrets.
     */
    std::vector<tv_client_t> list() const;

    /**
     * @brief Count paired TVs.
     * @return Number of paired TVs.
     */
    std::size_t count() const;

  private:
    /**
     * @brief Read the store file.
     */
    void load();

    /**
     * @brief Atomically replace the store file.
     */
    void save() const;

    std::filesystem::path _path;  ///< Store file.
    mutable std::mutex _mutex;  ///< Protects @ref _clients.
    std::vector<tv_client_t> _clients;  ///< Paired TVs, oldest first.
  };

  /**
   * @brief Check that a PIN has four digits.
   * @param pin PIN to check.
   * @return True when it is valid.
   */
  bool is_valid_pin(std::string_view pin);

  /**
   * @brief Outcome of the PIN entered in Sunshine's Web UI for a TV's pairing request.
   */
  enum class pairing_check_e {
    accepted,  ///< The PIN matched the one the TV shows.
    incorrect_pin,  ///< The PIN did not match; the request ends.
    expired,  ///< The request outlived @ref PAIRING_REQUEST_LIFETIME.
  };

  /**
   * @brief A TV asking to pair, showing a PIN that the user enters in Sunshine's Web UI.
   *
   * Pairing works like Moonlight's: the TV picks the PIN and shows it, and only a user signed in
   * to the Web UI can approve it. The TV is selected there by this request's unguessable ID, so a
   * device that copies the TV's name and PIN still cannot take its place.
   */
  struct pairing_request_t {
    using clock = std::chrono::steady_clock;  ///< Clock used for expiry.

    std::string id;  ///< Unguessable approval ID shown to the Web UI.
    std::string pin;  ///< PIN the TV shows.
    std::string client_name;  ///< Sanitized TV name.
    std::string address;  ///< TV address, for the Web UI.
    clock::time_point expires_at;  ///< When the request lapses.
  };

  /**
   * @brief Start a pairing request for a PIN the TV shows.
   * @param pin PIN chosen by the TV.
   * @param client_name Name sent by the TV.
   * @param address TV address.
   * @param now Current time.
   * @return The request, or nothing when the PIN is not four digits.
   */
  std::optional<pairing_request_t> make_pairing_request(std::string_view pin, std::string_view client_name, std::string address, pairing_request_t::clock::time_point now);

  /**
   * @brief Check the PIN entered in the Web UI against a request.
   * @param request Pairing request.
   * @param pin PIN entered in the Web UI.
   * @param now Current time.
   * @return The outcome; every outcome but acceptance ends the request.
   */
  pairing_check_e check_pairing_pin(const pairing_request_t &request, std::string_view pin, pairing_request_t::clock::time_point now);
}  // namespace webrtc_stream::tv_auth
