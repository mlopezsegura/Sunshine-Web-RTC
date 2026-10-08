/**
 * @file src/webrtc/tv_auth.cpp
 * @brief Definitions for pairing and authenticating Moonlight WebRTC TVs.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

// lib includes
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

// local includes
#include "tv_auth.h"

namespace webrtc_stream::tv_auth {
  namespace {
    using json = nlohmann::json;

    /**
     * @brief Version of the paired TV file.
     */
    constexpr int STORE_VERSION = 1;
    /**
     * @brief Label that binds a proof to this protocol.
     */
    constexpr std::string_view PROOF_LABEL = "moonlight-webrtc-tv-auth-v1:";

    /**
     * @brief Encode bytes as lower-case hex.
     * @param bytes Bytes to encode.
     * @param size Number of bytes.
     * @return Hex text.
     */
    std::string to_hex(const unsigned char *bytes, std::size_t size) {
      static constexpr char DIGITS[] = "0123456789abcdef";
      std::string result(size * 2, '\0');
      for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = DIGITS[bytes[index] >> 4];
        result[index * 2 + 1] = DIGITS[bytes[index] & 0x0F];
      }
      return result;
    }

    /**
     * @brief Decode one lower-case hex digit.
     * @param digit Digit to decode.
     * @return Its value, or -1 when it is not a lower-case hex digit.
     */
    int hex_value(char digit) {
      if (digit >= '0' && digit <= '9') {
        return digit - '0';
      }
      if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
      }
      return -1;
    }

    /**
     * @brief Decode lower-case hex.
     * @param value Hex text.
     * @return Decoded bytes.
     */
    std::vector<unsigned char> from_hex(std::string_view value) {
      if (value.size() % 2 != 0) {
        throw std::invalid_argument("Hex value has an odd length");
      }
      std::vector<unsigned char> bytes(value.size() / 2);
      for (std::size_t index = 0; index < bytes.size(); ++index) {
        const int high = hex_value(value[index * 2]);
        const int low = hex_value(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
          throw std::invalid_argument("Hex value contains a non-hex digit");
        }
        bytes[index] = static_cast<unsigned char>((high << 4) | low);
      }
      return bytes;
    }

    /**
     * @brief Compute HMAC-SHA256.
     * @param key_hex Key in hex.
     * @param message Message.
     * @return The digest in lower-case hex.
     */
    std::string hmac_sha256_hex(std::string_view key_hex, std::string_view message) {
      const auto key = from_hex(key_hex);
      std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
      unsigned int digest_length = 0;
      if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(message.data()), message.size(), digest.data(), &digest_length)) {
        throw std::runtime_error("OpenSSL failed to compute HMAC-SHA256");
      }
      return to_hex(digest.data(), digest_length);
    }
  }  // namespace

  std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
      throw std::runtime_error("OpenSSL failed to generate random bytes");
    }
    return to_hex(buffer.data(), buffer.size());
  }

  bool is_hex(std::string_view value, std::size_t bytes) {
    return value.size() == bytes * 2 && std::ranges::all_of(value, [](char digit) {
             return hex_value(digit) >= 0;
           });
  }

  std::string authentication_proof(std::string_view client_secret_hex, std::string_view nonce_hex) {
    return hmac_sha256_hex(client_secret_hex, std::string(PROOF_LABEL) + std::string(nonce_hex));
  }

  bool constant_time_equals(std::string_view left, std::string_view right) {
    return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
  }

  std::string sanitize_client_name(std::string_view name) {
    std::string result;
    for (const char character : name) {
      if (result.size() >= MAXIMUM_CLIENT_NAME_LENGTH) {
        break;
      }
      // Keep printable ASCII only; the name is a label shown in the Web UI.
      if (character >= 0x20 && character < 0x7F) {
        result.push_back(character);
      }
    }
    const auto first = result.find_first_not_of(' ');
    const auto last = result.find_last_not_of(' ');
    result = first == std::string::npos ? std::string() : result.substr(first, last - first + 1);
    return result.empty() ? "Samsung TV" : result;
  }

  tv_client_store_t::tv_client_store_t(std::filesystem::path path):
      _path(std::move(path)) {
    load();
  }

  void tv_client_store_t::load() {
    std::error_code error;
    if (!std::filesystem::exists(_path, error)) {
      return;
    }
    try {
      std::ifstream input(_path, std::ios::binary);
      const json value = json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
      if (value.value("version", 0) != STORE_VERSION || !value.contains("clients") || !value.at("clients").is_array()) {
        return;
      }
      for (const auto &entry : value.at("clients")) {
        tv_client_t client {entry.value("id", ""), entry.value("name", ""), entry.value("secret", ""), entry.value("enabled", true)};
        if (is_hex(client.id, CLIENT_ID_BYTES) && is_hex(client.secret, CLIENT_SECRET_BYTES)) {
          client.name = sanitize_client_name(client.name);
          _clients.push_back(std::move(client));
        }
      }
    } catch (const std::exception &) {
      // An unreadable store trusts nobody; affected TVs pair again.
      _clients.clear();
    }
  }

  void tv_client_store_t::save() const {
    json clients = json::array();
    for (const auto &client : _clients) {
      clients.push_back({{"id", client.id}, {"name", client.name}, {"secret", client.secret}, {"enabled", client.enabled}});
    }
    const std::string contents = json {{"version", STORE_VERSION}, {"clients", std::move(clients)}}.dump(2);

    auto temporary_path = _path;
    temporary_path += ".tmp";
    {
      std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
      output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
      if (!output) {
        throw std::runtime_error("Unable to write the paired TV list");
      }
    }
    std::error_code error;
    std::filesystem::rename(temporary_path, _path, error);
    if (error) {
      std::filesystem::remove(temporary_path, error);
      throw std::runtime_error("Unable to persist the paired TV list");
    }
  }

  std::optional<tv_client_t> tv_client_store_t::find(std::string_view id) const {
    std::lock_guard lock(_mutex);
    const auto found = std::ranges::find_if(_clients, [id](const tv_client_t &client) {
      return client.id == id;
    });
    return found == _clients.end() ? std::nullopt : std::optional<tv_client_t>(*found);
  }

  tv_client_t tv_client_store_t::add(std::string_view name) {
    tv_client_t client {random_hex(CLIENT_ID_BYTES), sanitize_client_name(name), random_hex(CLIENT_SECRET_BYTES)};
    std::lock_guard lock(_mutex);
    auto updated = _clients;
    if (updated.size() >= MAXIMUM_CLIENTS) {
      updated.erase(updated.begin());
    }
    updated.push_back(client);
    std::swap(_clients, updated);
    try {
      save();
    } catch (...) {
      std::swap(_clients, updated);
      throw;
    }
    return client;
  }

  bool tv_client_store_t::remove(std::string_view id) {
    std::lock_guard lock(_mutex);
    auto updated = _clients;
    if (std::erase_if(updated, [id](const tv_client_t &client) {
          return client.id == id;
        }) == 0) {
      return false;
    }
    std::swap(_clients, updated);
    try {
      save();
    } catch (...) {
      std::swap(_clients, updated);
      throw;
    }
    return true;
  }

  bool tv_client_store_t::set_enabled(std::string_view id, bool enabled) {
    std::lock_guard lock(_mutex);
    auto updated = _clients;
    const auto found = std::ranges::find_if(updated, [id](const tv_client_t &client) {
      return client.id == id;
    });
    if (found == updated.end()) {
      return false;
    }
    found->enabled = enabled;
    std::swap(_clients, updated);
    try {
      save();
    } catch (...) {
      std::swap(_clients, updated);
      throw;
    }
    return true;
  }

  std::size_t tv_client_store_t::remove_all() {
    std::lock_guard lock(_mutex);
    auto removed = std::move(_clients);
    _clients.clear();
    try {
      save();
    } catch (...) {
      _clients = std::move(removed);
      throw;
    }
    return removed.size();
  }

  std::vector<tv_client_t> tv_client_store_t::list() const {
    std::lock_guard lock(_mutex);
    std::vector<tv_client_t> clients;
    for (const auto &client : _clients) {
      clients.push_back({client.id, client.name, {}, client.enabled});
    }
    return clients;
  }

  std::size_t tv_client_store_t::count() const {
    std::lock_guard lock(_mutex);
    return _clients.size();
  }

  bool is_valid_pin(std::string_view pin) {
    return pin.size() == 4 && std::ranges::all_of(pin, [](char digit) {
             return digit >= '0' && digit <= '9';
           });
  }

  std::optional<pairing_request_t> make_pairing_request(std::string_view pin, std::string_view client_name, std::string address, pairing_request_t::clock::time_point now) {
    if (!is_valid_pin(pin)) {
      return std::nullopt;
    }
    return pairing_request_t {random_hex(PAIRING_ID_BYTES), std::string(pin), sanitize_client_name(client_name), std::move(address), now + PAIRING_REQUEST_LIFETIME};
  }

  pairing_check_e check_pairing_pin(const pairing_request_t &request, std::string_view pin, pairing_request_t::clock::time_point now) {
    if (now >= request.expires_at) {
      return pairing_check_e::expired;
    }
    return is_valid_pin(pin) && constant_time_equals(pin, request.pin) ? pairing_check_e::accepted : pairing_check_e::incorrect_pin;
  }
}  // namespace webrtc_stream::tv_auth
