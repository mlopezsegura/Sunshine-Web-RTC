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
  constexpr int MAXIMUM_PIN_ATTEMPTS = 3;  ///< Wrong PINs that close a pairing window.
  constexpr std::chrono::seconds PAIRING_WINDOW_LIFETIME {120};  ///< How long a PIN stays valid.

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
     * @brief Forget every paired TV and persist the store.
     * @return Number of TVs removed.
     */
    std::size_t remove_all();

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
   * @brief Outcome of one PIN entered on a TV.
   */
  enum class pairing_attempt_e {
    accepted,  ///< The PIN matched.
    incorrect_pin,  ///< The PIN did not match; the window stays open.
    not_open,  ///< No pairing window is open.
    too_many_attempts,  ///< The PIN did not match and the window closed.
  };

  /**
   * @brief State of the pairing window as shown on the PC.
   */
  enum class pairing_state_e {
    idle,  ///< No window was opened.
    waiting,  ///< A PIN is waiting for a TV.
    paired,  ///< A TV paired.
    expired,  ///< The PIN expired unused.
    failed,  ///< Too many wrong PINs were entered.
  };

  /**
   * @brief A short, explicitly opened window in which one TV may pair with the PIN shown on the PC.
   *
   * It closes on success, on expiry, or after @ref MAXIMUM_PIN_ATTEMPTS wrong PINs, so a device
   * on the network cannot guess its way through the 10,000 possible PINs.
   */
  class tv_pairing_window_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Clock used for expiry.

    /**
     * @brief Open the window with a new PIN.
     * @param now Current time.
     * @return The PIN to show on the PC.
     */
    std::string open(clock::time_point now);

    /**
     * @brief Check a PIN entered on a TV.
     * @param pin PIN sent by the TV.
     * @param now Current time.
     * @return The outcome.
     */
    pairing_attempt_e attempt(std::string_view pin, clock::time_point now);

    /**
     * @brief Report the window state.
     * @param now Current time.
     * @return The state.
     */
    pairing_state_e state(clock::time_point now);

    /**
     * @brief Close an open window.
     */
    void close();

  private:
    /**
     * @brief Expire the window when its PIN is past its lifetime.
     * @param now Current time.
     */
    void expire(clock::time_point now);

    std::string _pin;  ///< Current PIN.
    clock::time_point _expires_at {};  ///< When the PIN expires.
    int _failed_attempts = 0;  ///< Wrong PINs entered so far.
    pairing_state_e _state = pairing_state_e::idle;  ///< Current state.
  };
}  // namespace webrtc_stream::tv_auth
