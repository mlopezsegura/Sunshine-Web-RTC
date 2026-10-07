/**
 * @file src/webrtc/webrtc_stream.cpp
 * @brief Definitions for streaming to Moonlight WebRTC TVs directly from Sunshine.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>

// lib includes
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <rtc/rtc.hpp>

// local includes
#include "input_bridge.h"
#include "protocol.h"
#include "sdp.h"
#include "src/audio.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/globals.h"
#include "src/input.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/system_tray.h"
#include "src/video.h"
#include "tv_auth.h"
#include "webrtc_stream.h"

namespace webrtc_stream {
  namespace {
    using json = nlohmann::json;
    using namespace std::literals;

    /**
     * @brief RTP SSRC of the video track.
     */
    constexpr rtc::SSRC VIDEO_SSRC = 42;
    /**
     * @brief RTP SSRC of the audio track.
     */
    constexpr rtc::SSRC AUDIO_SSRC = 43;
    /**
     * @brief RTP clock rate of video.
     */
    constexpr std::uint32_t VIDEO_CLOCK_RATE = 90000;
    /**
     * @brief RTP clock rate of Opus.
     */
    constexpr std::uint32_t AUDIO_CLOCK_RATE = 48000;
    /**
     * @brief RTCP canonical name of both tracks.
     */
    constexpr auto RTP_CNAME = "sunshine";
    /**
     * @brief Media stream both tracks belong to.
     */
    constexpr auto MEDIA_STREAM_ID = "stream1";
    /**
     * @brief Opus frame duration, as Moonlight requests on a LAN.
     */
    constexpr int AUDIO_PACKET_DURATION_MS = 5;
    // Artwork is capped at 8 MiB before Base64 and JSON framing expand it for the WebSocket.
    /**
     * @brief Largest artwork sent to the TV.
     */
    constexpr std::size_t MAX_ARTWORK_BYTES = 8 * 1024 * 1024;
    /**
     * @brief Largest WebSocket message, sized for Base64 artwork.
     */
    constexpr std::size_t MAX_WEBSOCKET_MESSAGE_SIZE = 12 * 1024 * 1024;
    // Connections that have not authenticated yet; the oldest is dropped beyond this.
    /**
     * @brief Unauthenticated connections kept; the oldest is dropped beyond this.
     */
    constexpr std::size_t MAX_PENDING_CONNECTIONS = 8;
    /**
     * @brief How often a change of encoder support is checked.
     */
    constexpr auto CODEC_CHECK_INTERVAL = 5s;
    /**
     * @brief How long a stopped application may take to exit.
     */
    constexpr auto HOST_IDLE_TIMEOUT = 10s;

    std::atomic_int active_streams {0};  ///< TV streams whose capture is running.
    std::mutex host_mutex;  ///< Serializes launching and stopping host applications.

    /**
     * @brief Codecs the current encoder can produce, as probed by Sunshine.
     */
    struct codec_support_t {
      bool hevc;  ///< HEVC Main.
      bool hevc_main10;  ///< HEVC Main10.
      bool av1;  ///< AV1 Main 8-bit.
      bool av1_main10;  ///< AV1 Main 10-bit.

      /**
       * @brief Compare two probe results.
       * @return True when every codec matches.
       */
      bool operator==(const codec_support_t &) const = default;
    };

    /**
     * @brief Read the codecs found by Sunshine's last encoder probe.
     * @return Codec support.
     */
    codec_support_t current_codec_support() {
      const int hevc = video::active_hevc_mode;
      const int av1 = video::active_av1_mode;
      return {hevc >= 2, hevc == 3 || hevc == 5, av1 >= 2, av1 == 3 || av1 == 5};
    }

    /**
     * @brief Describe codec support as the TV protocol does.
     * @param codecs Codec support.
     * @return Encoders beyond H.264 and HEVC.
     */
    protocol::encoder_support_t encoder_support(const codec_support_t &codecs) {
      return {codecs.av1, codecs.av1 && codecs.av1_main10};
    }

    /**
     * @brief Check requested settings against the encoder.
     * @param settings Requested settings.
     * @param codecs Codec support.
     * @return Why the encoder cannot produce them, or nothing.
     */
    std::optional<std::string> unsupported_codec(const protocol::stream_settings_t &settings, const codec_support_t &codecs) {
      switch (settings.codec) {
        case protocol::video_codec_e::h264:
          return std::nullopt;
        case protocol::video_codec_e::hevc:
          if (!codecs.hevc) {
            return "This PC's encoder does not support HEVC"s;
          }
          if (settings.hdr && !codecs.hevc_main10) {
            return "This PC's encoder does not support HEVC Main10 HDR"s;
          }
          return std::nullopt;
        case protocol::video_codec_e::av1:
          if (!codecs.av1) {
            return "This PC's encoder does not support AV1"s;
          }
          if (settings.hdr && !codecs.av1_main10) {
            return "This PC's encoder does not support AV1 10-bit HDR"s;
          }
          return std::nullopt;
      }
      return "Unsupported video codec"s;
    }

    /**
     * @brief Encode bytes as Base64.
     * @param bytes Bytes to encode.
     * @return Base64 text.
     */
    std::string base64_encode(const std::vector<std::uint8_t> &bytes) {
      if (bytes.empty()) {
        return {};
      }
      std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
      const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), bytes.data(), static_cast<int>(bytes.size()));
      encoded.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
      return encoded;
    }

    /**
     * @brief Identify PNG, JPEG or WebP data.
     * @param bytes Image bytes.
     * @return The media type, or nothing for other data.
     */
    std::optional<std::string> image_mime_type(const std::vector<std::uint8_t> &bytes) {
      const auto starts_with = [&bytes](std::initializer_list<std::uint8_t> prefix, std::size_t offset = 0) {
        return bytes.size() >= offset + prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin() + offset);
      };
      if (starts_with({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) {
        return "image/png"s;
      }
      if (starts_with({0xFF, 0xD8, 0xFF})) {
        return "image/jpeg"s;
      }
      if (starts_with({'R', 'I', 'F', 'F'}) && starts_with({'W', 'E', 'B', 'P'}, 8)) {
        return "image/webp"s;
      }
      return std::nullopt;
    }

    /**
     * @brief Parse a Sunshine application ID.
     * @param text ID text.
     * @return The ID, or nothing when it is not a non-negative integer.
     */
    std::optional<int> parse_app_id(std::string_view text) {
      int value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() || value < 0) {
        return std::nullopt;
      }
      return value;
    }

    /**
     * @brief Find the Wake-on-LAN address of the adapter that routes to a peer.
     * @param remote_address Peer address as "host:port".
     * @return Upper-case colon-separated MAC address, or nothing when unknown.
     */
    std::optional<std::string> wake_on_lan_address(const std::optional<std::string> &remote_address) {
      if (!remote_address) {
        return std::nullopt;
      }
      try {
        auto host = remote_address->substr(0, remote_address->rfind(':'));
        if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
          host = host.substr(1, host.size() - 2);
        }
        // Connecting a UDP socket only selects the route; it sends nothing.
        boost::asio::io_context io;
        boost::asio::ip::udp::socket socket(io);
        const auto peer = boost::asio::ip::make_address(host);
        socket.connect({peer, 9});
        const auto local = socket.local_endpoint().address();
        if (local.is_loopback()) {
          return std::nullopt;
        }
        auto mac = platf::get_mac_address(net::addr_to_normalized_string(local));
        std::ranges::transform(mac, mac.begin(), [](unsigned char character) {
          return static_cast<char>(std::toupper(character));
        });
        if (mac.empty() || mac == "00:00:00:00:00:00") {
          return std::nullopt;
        }
        return mac;
      } catch (const std::exception &) {
        return std::nullopt;
      }
    }

    /**
     * @brief Apply Sunshine's encoded-byte substitutions to an IDR frame, as the GameStream sender does.
     */
    std::vector<std::uint8_t> apply_replacements(std::string_view payload, const std::vector<video::packet_raw_t::replace_t> &replacements) {
      std::vector<std::uint8_t> result(payload.begin(), payload.end());
      for (const auto &replacement : replacements) {
        const std::string_view current {reinterpret_cast<const char *>(result.data()), result.size()};
        const auto position = current.find(replacement.old);
        if (position == std::string_view::npos) {
          continue;
        }
        std::vector<std::uint8_t> replaced;
        replaced.reserve(result.size() - replacement.old.size() + replacement._new.size());
        replaced.insert(replaced.end(), result.begin(), result.begin() + position);
        replaced.insert(replaced.end(), replacement._new.begin(), replacement._new.end());
        replaced.insert(replaced.end(), result.begin() + position + replacement.old.size(), result.end());
        result = std::move(replaced);
      }
      return result;
    }

    /**
     * @brief One WebRTC stream: its PeerConnection and, once connected, its capture pipeline.
     */
    struct stream_t {
      std::uint64_t id = 0;  ///< Session ID shared with the TV.
      protocol::stream_settings_t settings;  ///< Requested settings.
      int app_id = 0;  ///< Sunshine application ID.
      std::string client_id;  ///< Paired TV ID, used as the stable input identity.
      std::string client_name;  ///< Paired TV name.

      std::shared_ptr<rtc::PeerConnection> peer;  ///< PeerConnection to the TV.
      std::shared_ptr<rtc::Track> video_track;  ///< Send-only video track.
      std::shared_ptr<rtc::Track> audio_track;  ///< Send-only audio track.
      std::shared_ptr<rtc::DataChannel> control_channel;  ///< Ordered, reliable control channel.
      std::shared_ptr<rtc::DataChannel> gamepad_channel;  ///< Unordered, unreliable gamepad snapshots.
      std::shared_ptr<rtc::RtpPacketizationConfig> video_rtp;  ///< Video RTP state.
      std::shared_ptr<rtc::RtpPacketizationConfig> audio_rtp;  ///< Audio RTP state.
      std::shared_ptr<input_bridge_t> input_bridge;  ///< Gamepad translation for the whole stream.

      std::mutex input_mutex;  ///< Protects @ref input.
      std::shared_ptr<input::input_t> input;  ///< Sunshine input state while capturing.

      std::mutex mutex;  ///< Protects the flags below.
      std::condition_variable condition;  ///< Signals flag changes.
      bool peer_connected = false;  ///< PeerConnection is connected.
      bool video_open = false;  ///< Video track is open.
      bool audio_open = false;  ///< Audio track is open.
      bool stop_requested = false;  ///< The stream must end.
      std::optional<std::string> failure;  ///< Why the host ended the stream.
      safe::mail_raw_t::event_t<bool> idr_event;  ///< Keyframe requests, once capture runs.

      std::mutex candidate_mutex;  ///< Protects candidate buffering.
      bool has_remote_description = false;  ///< Whether the answer was applied.
      std::vector<std::pair<std::string, std::string>> pending_candidates;  ///< Candidates received before the answer.

      std::jthread control_thread;  ///< Runs the capture pipeline.

      /**
       * @brief Ask the stream to end.
       * @param reason Failure shown on the TV, when the host ended the stream.
       */
      void request_stop(std::optional<std::string> reason = std::nullopt) {
        {
          std::lock_guard lock(mutex);
          if (reason && !stop_requested && !failure) {
            failure = std::move(reason);
          }
          stop_requested = true;
        }
        condition.notify_all();
      }

      /**
       * @brief Check whether the stream must end.
       * @return True once a stop was requested.
       */
      bool stopping() {
        std::lock_guard lock(mutex);
        return stop_requested;
      }
    };

    /**
     * @brief One TV WebSocket connection.
     */
    struct connection_t {
      std::shared_ptr<rtc::WebSocket> socket;  ///< Signaling WebSocket.
      std::mutex send_mutex;  ///< Serializes sends.

      std::mutex mutex;  ///< Protects the state below.
      bool authenticated = false;  ///< Whether the TV proved it is paired.
      std::string nonce;  ///< Single-use authentication nonce.
      std::optional<std::string> mac_address;  ///< Wake-on-LAN address facing this TV.
      std::string client_id;  ///< Authenticated TV ID.
      std::string client_name;  ///< Authenticated TV name.
      std::shared_ptr<stream_t> stream;  ///< Current stream.
      bool host_operation_active = false;  ///< A stop or switch is in progress.
    };

    /**
     * @brief The TV signaling server.
     */
    class server_t {
    public:
      server_t():
          _tv_clients(platf::appdata() / "webrtc_tv_clients.json") {
        rtc::WebSocketServer::Configuration configuration;
        configuration.port = static_cast<std::uint16_t>(config::webrtc.port);
        configuration.enableTls = false;
        configuration.bindAddress = "0.0.0.0";
        configuration.maxMessageSize = MAX_WEBSOCKET_MESSAGE_SIZE;
        _server = std::make_unique<rtc::WebSocketServer>(configuration);
        _server->onClient([this](std::shared_ptr<rtc::WebSocket> socket) {
          accept(std::move(socket));
        });
        _codecs = current_codec_support();
        BOOST_LOG(info) << "WebRTC: TV server listening on port "sv << config::webrtc.port << ", "sv << _tv_clients.count() << " paired TV(s)"sv;
      }

      ~server_t() {
        // Stop accepting first; the callbacks below capture this server.
        _server->stop();
        std::vector<std::shared_ptr<connection_t>> connections;
        {
          std::lock_guard lock(_connections_mutex);
          connections = _pending;
          if (_active) {
            connections.push_back(_active);
          }
          _pending.clear();
          _active.reset();
        }
        for (const auto &connection : connections) {
          connection->socket->resetCallbacks();
          stop_stream(connection, false);
          connection->socket->close();
        }
      }

      /**
       * @brief Serve until Sunshine shuts down, announcing encoder changes to the TV.
       */
      void run() {
        auto shutdown_event = mail::man->event<bool>(mail::shutdown);
        auto next_check = std::chrono::steady_clock::now() + CODEC_CHECK_INTERVAL;
        while (!shutdown_event->view(500ms)) {
          if (std::chrono::steady_clock::now() < next_check) {
            continue;
          }
          next_check = std::chrono::steady_clock::now() + CODEC_CHECK_INTERVAL;
          // Encoders are re-probed before each stream, and a hardware encoder can appear later.
          const auto codecs = current_codec_support();
          std::shared_ptr<connection_t> active;
          {
            std::lock_guard lock(_connections_mutex);
            if (codecs == _codecs) {
              continue;
            }
            _codecs = codecs;
            active = _active;
          }
          BOOST_LOG(info) << "WebRTC: encoder codec support changed"sv;
          if (active) {
            send(active, protocol::make_capabilities(encoder_support(codecs)));
          }
        }
      }

      /**
       * @brief Open the TV pairing window.
       * @return The PIN.
       */
      std::string open_pairing() {
        std::string pin;
        {
          std::lock_guard lock(_pairing_mutex);
          pin = _pairing.open(tv_auth::tv_pairing_window_t::clock::now());
        }
        BOOST_LOG(info) << "WebRTC: TV pairing opened for two minutes"sv;
        return pin;
      }

      /**
       * @brief Report the TV pairing window state.
       * @return The state code.
       */
      std::string pairing_status() {
        std::lock_guard lock(_pairing_mutex);
        switch (_pairing.state(tv_auth::tv_pairing_window_t::clock::now())) {
          case tv_auth::pairing_state_e::waiting:
            return "tv-pairing-waiting";
          case tv_auth::pairing_state_e::paired:
            return "tv-paired";
          case tv_auth::pairing_state_e::expired:
            return "tv-pairing-expired";
          case tv_auth::pairing_state_e::failed:
            return "tv-pairing-locked";
          case tv_auth::pairing_state_e::idle:
            break;
        }
        return "tv-pairing-idle";
      }

      /**
       * @brief List paired TVs.
       * @return Paired TVs.
       */
      std::vector<paired_tv_t> paired() const {
        std::vector<paired_tv_t> result;
        for (const auto &client : _tv_clients.list()) {
          result.push_back({client.id, client.name});
        }
        return result;
      }

      /**
       * @brief Forget every TV, cutting off the connected one too.
       * @return Number of TVs removed.
       */
      std::size_t forget_tvs() {
        const auto removed = _tv_clients.remove_all();
        {
          std::lock_guard lock(_pairing_mutex);
          _pairing.close();
        }
        std::vector<std::shared_ptr<connection_t>> connections;
        {
          std::lock_guard lock(_connections_mutex);
          connections = _pending;
          if (_active) {
            connections.push_back(_active);
          }
        }
        for (const auto &connection : connections) {
          connection->socket->close();
        }
        BOOST_LOG(info) << "WebRTC: removed "sv << removed << " paired TV(s)"sv;
        return removed;
      }

    private:
      /**
       * @brief Accept a TV connection and wait for it to authenticate.
       * @param socket New WebSocket.
       */
      void accept(std::shared_ptr<rtc::WebSocket> socket) {
        auto connection = std::make_shared<connection_t>();
        connection->socket = std::move(socket);

        // A connection that has not authenticated must not displace the TV that may be
        // streaming, so it waits beside the active one until it authenticates.
        std::shared_ptr<connection_t> evicted;
        {
          std::lock_guard lock(_connections_mutex);
          if (_pending.size() >= MAX_PENDING_CONNECTIONS) {
            evicted = _pending.front();
            _pending.erase(_pending.begin());
          }
          _pending.push_back(connection);
        }
        if (evicted) {
          evicted->socket->close();
        }

        const std::weak_ptr<connection_t> weak = connection;
        connection->socket->onOpen([this, weak] {
          if (const auto connection = weak.lock()) {
            auto mac = wake_on_lan_address(connection->socket->remoteAddress());
            auto nonce = tv_auth::random_hex(tv_auth::NONCE_BYTES);
            {
              std::lock_guard lock(connection->mutex);
              connection->nonce = nonce;
              connection->mac_address = mac;
            }
            BOOST_LOG(info) << "WebRTC: TV connected from "sv << connection->socket->remoteAddress().value_or("unknown"s) << "; awaiting authentication"sv;
            // Sunshine is the server itself, so streaming is available whenever it answers.
            send(connection, protocol::make_auth_required(nonce, mac, true));
          }
        });
        connection->socket->onClosed([this, weak] {
          if (const auto connection = weak.lock()) {
            BOOST_LOG(info) << "WebRTC: TV disconnected"sv;
            close(connection);
          }
        });
        connection->socket->onError([](const std::string &message) {
          BOOST_LOG(warning) << "WebRTC: WebSocket error: "sv << message;
        });
        connection->socket->onMessage([this, weak](rtc::message_variant data) {
          if (!std::holds_alternative<std::string>(data)) {
            return;
          }
          if (const auto connection = weak.lock()) {
            handle_message(connection, std::get<std::string>(data));
          }
        });
      }

      /**
       * @brief Forget a closed connection and end its stream.
       * @param connection Connection.
       */
      void close(const std::shared_ptr<connection_t> &connection) {
        stop_stream(connection, false);
        std::lock_guard lock(_connections_mutex);
        std::erase(_pending, connection);
        if (_active == connection) {
          _active.reset();
        }
      }

      /**
       * @brief Send a message to a TV, ignoring a closed socket.
       * @param connection Connection.
       * @param message Message.
       */
      void send(const std::shared_ptr<connection_t> &connection, const json &message) {
        const auto payload = message.dump();
        std::lock_guard lock(connection->send_mutex);
        try {
          if (connection->socket && connection->socket->isOpen() && payload.size() <= MAX_WEBSOCKET_MESSAGE_SIZE) {
            connection->socket->send(payload);
          }
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "WebRTC: message not sent: "sv << e.what();
        }
      }

      /**
       * @brief Dispatch a TV message; only authentication and pairing are allowed before authenticating.
       * @param connection Connection.
       * @param text JSON text.
       */
      void handle_message(const std::shared_ptr<connection_t> &connection, const std::string &text) {
        try {
          const auto message = protocol::parse_client_message(text);
          bool authenticated;
          {
            std::lock_guard lock(connection->mutex);
            authenticated = connection->authenticated;
          }
          if (const auto *request = std::get_if<protocol::authenticate_t>(&message.payload)) {
            if (authenticated) {
              send(connection, protocol::make_error(message.type, "already-authenticated", "This TV is already authenticated"));
            } else {
              authenticate(connection, *request);
            }
            return;
          }
          if (const auto *request = std::get_if<protocol::pair_client_t>(&message.payload)) {
            if (authenticated) {
              send(connection, protocol::make_error(message.type, "already-authenticated", "This TV is already paired"));
            } else {
              pair(connection, *request);
            }
            return;
          }
          if (!authenticated) {
            send(connection, protocol::make_error(message.type, "not-authenticated", "Pair this TV with Sunshine first"));
            return;
          }

          if (std::holds_alternative<protocol::get_apps_t>(message.payload)) {
            send_apps(connection);
          } else if (const auto *artwork = std::get_if<protocol::get_app_artwork_t>(&message.payload)) {
            send_artwork(connection, artwork->app_id);
          } else if (const auto *start = std::get_if<protocol::start_session_t>(&message.payload)) {
            start_stream(connection, start->app_id, start->settings);
          } else if (std::holds_alternative<protocol::stop_session_t>(message.payload)) {
            stop_stream(connection, true);
          } else if (std::holds_alternative<protocol::stop_host_session_t>(message.payload)) {
            stop_or_switch_host(connection, std::nullopt);
          } else if (const auto *target = std::get_if<protocol::switch_session_t>(&message.payload)) {
            stop_or_switch_host(connection, protocol::start_session_t {target->app_id, target->settings});
          } else if (const auto *answer = std::get_if<protocol::answer_t>(&message.payload)) {
            handle_answer(connection, *answer);
          } else if (const auto *candidate = std::get_if<protocol::candidate_t>(&message.payload)) {
            handle_candidate(connection, *candidate);
          }
        } catch (const protocol::protocol_error_t &e) {
          BOOST_LOG(warning) << "WebRTC: protocol error: "sv << e.what();
          send(connection, protocol::make_error("unknown", e.code(), e.what()));
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "WebRTC: control error: "sv << e.what();
          send(connection, protocol::make_error("unknown", "internal-error", e.what()));
        }
      }

      /**
       * @brief Check a paired TV's proof against the connection nonce.
       * @param connection Connection.
       * @param request Authentication request.
       */
      void authenticate(const std::shared_ptr<connection_t> &connection, const protocol::authenticate_t &request) {
        std::string nonce;
        {
          std::lock_guard lock(connection->mutex);
          nonce = std::exchange(connection->nonce, {});
        }
        const auto client = _tv_clients.find(request.client_id);
        if (nonce.empty() || !client || !tv_auth::constant_time_equals(tv_auth::authentication_proof(client->secret, nonce), request.proof)) {
          BOOST_LOG(warning) << "WebRTC: TV authentication rejected"sv;
          send(connection, protocol::make_error("authenticate", "authentication-failed", "This TV is not paired with Sunshine"));
          return;
        }
        BOOST_LOG(info) << "WebRTC: TV authenticated: "sv << client->name;
        send(connection, protocol::make_authenticated());
        promote(connection, *client);
      }

      /**
       * @brief Pair a TV with the PIN shown on the PC.
       * @param connection Connection.
       * @param request Pairing request.
       */
      void pair(const std::shared_ptr<connection_t> &connection, const protocol::pair_client_t &request) {
        tv_auth::pairing_attempt_e outcome;
        {
          std::lock_guard lock(_pairing_mutex);
          outcome = _pairing.attempt(request.pin, tv_auth::tv_pairing_window_t::clock::now());
        }
        switch (outcome) {
          case tv_auth::pairing_attempt_e::not_open:
            send(connection, protocol::make_error("pair-client", "pairing-not-open", "Open Sunshine's Web UI on the PC, choose Pair TV, then enter the PIN it shows"));
            return;
          case tv_auth::pairing_attempt_e::incorrect_pin:
            BOOST_LOG(warning) << "WebRTC: TV pairing, incorrect PIN"sv;
            send(connection, protocol::make_error("pair-client", "incorrect-pin", "Incorrect PIN. Check the PIN shown on the PC"));
            return;
          case tv_auth::pairing_attempt_e::too_many_attempts:
            BOOST_LOG(warning) << "WebRTC: TV pairing closed after too many incorrect PINs"sv;
            send(connection, protocol::make_error("pair-client", "too-many-attempts", "Too many incorrect PINs. Start pairing again on the PC"));
            return;
          case tv_auth::pairing_attempt_e::accepted:
            break;
        }

        tv_auth::tv_client_t client;
        try {
          client = _tv_clients.add(request.client_name);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "WebRTC: TV pairing could not be saved: "sv << e.what();
          send(connection, protocol::make_error("pair-client", "pairing-failed", "Sunshine could not save this TV"));
          return;
        }
        BOOST_LOG(info) << "WebRTC: TV paired: "sv << client.name;
        send(connection, protocol::make_paired(client.id, client.secret));
        promote(connection, client);
      }

      /**
       * @brief Make an authenticated connection the active TV, closing the one it replaces.
       */
      void promote(const std::shared_ptr<connection_t> &connection, const tv_auth::tv_client_t &client) {
        std::shared_ptr<connection_t> previous;
        codec_support_t codecs;
        {
          std::lock_guard lock(_connections_mutex);
          const auto pending = std::ranges::find(_pending, connection);
          if (pending == _pending.end()) {
            return;
          }
          _pending.erase(pending);
          previous = std::exchange(_active, connection);
          codecs = _codecs;
        }
        {
          std::lock_guard lock(connection->mutex);
          connection->authenticated = true;
          connection->nonce.clear();
          connection->client_id = client.id;
          connection->client_name = client.name;
        }
        if (previous) {
          previous->socket->close();
        }
        send(connection, protocol::make_gateway_status(status(connection)));
        send(connection, protocol::make_capabilities(encoder_support(codecs)));
        send(connection, protocol::make_session_status("idle"));
      }

      /**
       * @brief Describe the server for a TV.
       * @param connection Connection.
       * @return Server status.
       */
      protocol::gateway_status_t status(const std::shared_ptr<connection_t> &connection) {
        protocol::gateway_status_t status;
        status.gateway_name = config::nvhttp.sunshine_name;
        {
          std::lock_guard lock(connection->mutex);
          status.session_active = connection->stream != nullptr;
          status.mac_address = connection->mac_address;
        }
        if (const int running = proc::proc.running(); running != 0) {
          status.running_app_id = std::to_string(running);
        }
        return status;
      }

      /**
       * @brief List Sunshine's applications, re-reading the apps file when it changed.
       * @return Applications.
       */
      std::vector<protocol::application_t> applications() {
        proc::refresh(config::stream.file_apps);
        const int running = proc::proc.running();
        std::vector<protocol::application_t> result;
        std::lock_guard lock(_artwork_mutex);
        for (const auto &app : proc::proc.get_apps()) {
          const auto cached = _artwork.find(app.id);
          result.push_back({app.id, app.name, cached != _artwork.end() && cached->second.has_value(), app.id == std::to_string(running)});
        }
        return result;
      }

      /**
       * @brief Send the application list.
       * @param connection Connection.
       */
      void send_apps(const std::shared_ptr<connection_t> &connection) {
        try {
          const auto apps = applications();
          send(connection, protocol::make_apps(apps));
        } catch (const std::exception &e) {
          send(connection, protocol::make_error("get-apps", "sunshine-unavailable", e.what()));
        }
      }

      /**
       * @brief Send an application's cover art, caching the result.
       * @param connection Connection.
       * @param app_id Application ID.
       */
      void send_artwork(const std::shared_ptr<connection_t> &connection, const std::string &app_id) {
        std::optional<std::pair<std::string, std::string>> artwork;
        {
          std::lock_guard lock(_artwork_mutex);
          if (const auto cached = _artwork.find(app_id); cached != _artwork.end()) {
            artwork = cached->second;
          }
        }
        if (!artwork) {
          if (const auto id = parse_app_id(app_id)) {
            std::ifstream file(proc::proc.get_app_image(*id), std::ios::binary);
            std::vector<std::uint8_t> bytes;
            if (file) {
              bytes.assign(std::istreambuf_iterator<char>(file), {});
            }
            if (const auto mime = image_mime_type(bytes); mime && bytes.size() <= MAX_ARTWORK_BYTES) {
              artwork = std::make_pair(*mime, base64_encode(bytes));
            }
          }
          std::lock_guard lock(_artwork_mutex);
          _artwork[app_id] = artwork;
        }
        send(connection, artwork ? protocol::make_app_artwork(app_id, true, artwork->first, artwork->second) : protocol::make_app_artwork(app_id, false));
      }

      /**
       * @brief Send a stream state change.
       * @param connection Connection.
       * @param stream Stream.
       * @param state New state.
       * @param detail Optional detail.
       */
      void send_session_status(const std::shared_ptr<connection_t> &connection, const std::shared_ptr<stream_t> &stream, std::string_view state, std::optional<std::string> detail = std::nullopt) {
        send(connection, protocol::make_session_status(state, stream->id, stream->settings, std::move(detail)));
        BOOST_LOG(info) << "WebRTC: session "sv << stream->id << ": "sv << state;
      }

      /**
       * @brief Check whether a stream is still the connection's stream.
       * @param connection Connection.
       * @param stream Stream.
       * @return True when it is.
       */
      bool is_current(const std::shared_ptr<connection_t> &connection, const std::shared_ptr<stream_t> &stream) {
        std::lock_guard lock(connection->mutex);
        return connection->stream == stream;
      }

      /**
       * @brief Validate a start request and begin WebRTC negotiation.
       * @param connection Connection.
       * @param app_id Application ID.
       * @param settings Validated settings.
       */
      void start_stream(const std::shared_ptr<connection_t> &connection, const std::string &app_id, const protocol::stream_settings_t &settings) {
        std::string client_id;
        std::string client_name;
        {
          std::lock_guard lock(connection->mutex);
          if (connection->host_operation_active) {
            send(connection, protocol::make_error("start-session", "host-operation-active", "Wait for the current Sunshine operation to complete"));
            return;
          }
          if (connection->stream) {
            send(connection, protocol::make_error("start-session", "session-active", "Stop the active session before starting another"));
            return;
          }
          client_id = connection->client_id;
          client_name = connection->client_name;
        }

        std::optional<int> numeric_id;
        std::string title;
        for (const auto &app : applications()) {
          if (app.id == app_id) {
            numeric_id = parse_app_id(app.id);
            title = app.title;
          }
        }
        if (!numeric_id) {
          send(connection, protocol::make_error("start-session", "invalid-application", "Selected Sunshine application no longer exists"));
          return;
        }

        auto stream = std::make_shared<stream_t>();
        stream->id = _next_session_id++;
        stream->settings = settings;
        stream->app_id = *numeric_id;
        stream->client_id = client_id;
        stream->client_name = client_name;
        {
          std::lock_guard lock(connection->mutex);
          connection->stream = stream;
        }
        BOOST_LOG(info) << "WebRTC: starting "sv << title << " at "sv << settings.width << 'x' << settings.height << '@' << settings.fps << ", "sv << protocol::codec_name(settings.codec) << ", "sv << settings.bitrate_kbps << " kbps, HDR "sv << (settings.hdr ? "on"sv : "off"sv);

        send_session_status(connection, stream, "starting");
        send_session_status(connection, stream, "starting-webrtc");
        try {
          create_peer_connection(connection, stream);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "WebRTC: session startup failed: "sv << e.what();
          send_session_status(connection, stream, "error", e.what());
          stop_stream(connection, true);
        }
      }

      /**
       * @brief Create the PeerConnection, its tracks and channels, and send the offer.
       * @param connection Connection.
       * @param stream Stream.
       */
      void create_peer_connection(const std::shared_ptr<connection_t> &connection, const std::shared_ptr<stream_t> &stream) {
        const auto &settings = stream->settings;
        const std::weak_ptr<connection_t> weak_connection = connection;
        const std::weak_ptr<stream_t> weak_stream = stream;

        rtc::Configuration configuration;
        configuration.disableAutoNegotiation = true;
        stream->peer = std::make_shared<rtc::PeerConnection>(configuration);

        stream->peer->onStateChange([weak_stream](rtc::PeerConnection::State state) {
          const auto stream = weak_stream.lock();
          if (!stream) {
            return;
          }
          BOOST_LOG(debug) << "WebRTC: PeerConnection state "sv << static_cast<int>(state);
          const bool lost = state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed;
          if (lost && stream->input_bridge) {
            stream->input_bridge->on_transport_closed();
          }
          {
            std::lock_guard lock(stream->mutex);
            stream->peer_connected = state == rtc::PeerConnection::State::Connected;
            if (lost) {
              stream->stop_requested = true;
            }
          }
          stream->condition.notify_all();
        });

        stream->peer->onLocalDescription([this, weak_connection, weak_stream](rtc::Description description) {
          const auto connection = weak_connection.lock();
          const auto stream = weak_stream.lock();
          if (!connection || !stream || description.type() != rtc::Description::Type::Offer || !is_current(connection, stream)) {
            return;
          }
          const std::string offer(description);
          std::optional<std::string> problem;
          if (!sdp::has_valid_game_mode_image_attribute(offer, stream->settings)) {
            problem = "Invalid Samsung Game Mode SDP imageattr";
          } else if (!sdp::has_expected_video_codec(offer, stream->settings.codec)) {
            problem = "WebRTC SDP does not contain the requested video codec";
          } else if (!sdp::has_expected_hevc_format_parameters(offer, stream->settings)) {
            problem = "WebRTC SDP does not contain the requested HEVC Main10 profile";
          } else if (!sdp::has_data_channel_section(offer)) {
            problem = "Missing WebRTC data-channel SDP section";
          }
          if (problem) {
            BOOST_LOG(error) << "WebRTC: "sv << *problem;
            send_session_status(connection, stream, "error", problem);
            stream->request_stop();
            return;
          }
          send(connection, protocol::make_offer(stream->id, offer));
        });

        stream->peer->onLocalCandidate([this, weak_connection, weak_stream](rtc::Candidate candidate) {
          const auto connection = weak_connection.lock();
          const auto stream = weak_stream.lock();
          if (connection && stream && is_current(connection, stream)) {
            send(connection, protocol::make_candidate(stream->id, candidate.candidate(), candidate.mid()));
          }
        });

        // Controllers the TV announces before capture starts are kept and announced to Sunshine
        // once its input state exists; input arriving before then is dropped.
        stream->input_bridge = std::make_shared<input_bridge_t>(
          make_packet_input_api([weak_stream](std::vector<std::uint8_t> &&packet) {
            if (const auto stream = weak_stream.lock()) {
              std::lock_guard lock(stream->input_mutex);
              if (stream->input) {
                input::passthrough(stream->input, std::move(packet));
              }
            }
          }),
          [weak_stream](const std::string &message) {
            const auto stream = weak_stream.lock();
            if (!stream || !stream->control_channel || !stream->control_channel->isOpen()) {
              return;
            }
            try {
              stream->control_channel->send(message);
            } catch (const std::exception &) {
              // The TV can close while a queued haptic message is sent.
            }
          }
        );

        rtc::DataChannelInit control_options;
        control_options.reliability.unordered = false;
        stream->control_channel = stream->peer->createDataChannel("control", control_options);
        stream->control_channel->onMessage([weak_stream](rtc::message_variant data) {
          const auto stream = weak_stream.lock();
          if (stream && std::holds_alternative<std::string>(data) && !stream->input_bridge->handle_control_message(std::get<std::string>(data))) {
            BOOST_LOG(warning) << "WebRTC: rejected malformed gamepad control message"sv;
          }
        });
        stream->control_channel->onClosed([weak_stream] {
          if (const auto stream = weak_stream.lock()) {
            stream->input_bridge->on_transport_closed();
          }
        });

        rtc::DataChannelInit gamepad_options;
        gamepad_options.reliability.unordered = true;
        gamepad_options.reliability.maxRetransmits = 0;
        stream->gamepad_channel = stream->peer->createDataChannel("gamepad", gamepad_options);
        stream->gamepad_channel->onMessage([weak_stream](rtc::message_variant data) {
          const auto stream = weak_stream.lock();
          if (stream && std::holds_alternative<std::string>(data)) {
            stream->input_bridge->handle_gamepad_message(std::get<std::string>(data));
          }
        });
        stream->gamepad_channel->onClosed([weak_stream] {
          if (const auto stream = weak_stream.lock()) {
            stream->input_bridge->on_transport_closed();
          }
        });

        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        switch (settings.codec) {
          case protocol::video_codec_e::hevc:
            video.addH265Codec(sdp::VIDEO_PAYLOAD_TYPE, sdp::hevc_format_parameters(settings));
            break;
          case protocol::video_codec_e::av1:
            video.addAV1Codec(sdp::VIDEO_PAYLOAD_TYPE);
            break;
          case protocol::video_codec_e::h264:
            video.addH264Codec(sdp::VIDEO_PAYLOAD_TYPE);
            break;
        }
        video.addAttribute(sdp::game_mode_image_attribute(settings));
        if (settings.hdr) {
          // Offered as the Gateway did, but never sent: Tizen decoded no frames with it present,
          // and the HEVC VUI/SEI or AV1 sequence header already carry the colour description.
          video.addExtMap(rtc::Description::Entry::ExtMap(9, "http://www.webrtc.org/experiments/rtp-hdrext/color-space"));
        }
        video.addSSRC(VIDEO_SSRC, RTP_CNAME, MEDIA_STREAM_ID, "video");
        stream->video_track = stream->peer->addTrack(video);

        stream->video_rtp = std::make_shared<rtc::RtpPacketizationConfig>(VIDEO_SSRC, RTP_CNAME, sdp::VIDEO_PAYLOAD_TYPE, VIDEO_CLOCK_RATE);
        std::shared_ptr<rtc::RtpPacketizer> packetizer;
        switch (settings.codec) {
          case protocol::video_codec_e::hevc:
            packetizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, stream->video_rtp);
            break;
          case protocol::video_codec_e::av1:
            // Each encoded frame is one AV1 temporal unit of size-delimited OBUs.
            packetizer = std::make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit, stream->video_rtp);
            break;
          case protocol::video_codec_e::h264:
            packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, stream->video_rtp);
            break;
        }
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(stream->video_rtp));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([weak_stream] {
          const auto stream = weak_stream.lock();
          if (!stream) {
            return;
          }
          // Sunshine encodes the next frame as IDR; no intermediate client has to ask for it.
          std::lock_guard lock(stream->mutex);
          if (stream->idr_event) {
            stream->idr_event->raise(true);
          }
        }));
        stream->video_track->setMediaHandler(packetizer);
        stream->video_track->onOpen([weak_stream] {
          if (const auto stream = weak_stream.lock()) {
            {
              std::lock_guard lock(stream->mutex);
              stream->video_open = true;
            }
            stream->condition.notify_all();
          }
        });
        stream->video_track->onClosed([weak_stream] {
          if (const auto stream = weak_stream.lock()) {
            stream->request_stop();
          }
        });

        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
        audio.addOpusCodec(sdp::AUDIO_PAYLOAD_TYPE, "minptime=20;maxaveragebitrate=128000;stereo=1;sprop-stereo=1;useinbandfec=0");
        audio.addSSRC(AUDIO_SSRC, RTP_CNAME, MEDIA_STREAM_ID, "audio");
        stream->audio_track = stream->peer->addTrack(audio);

        stream->audio_rtp = std::make_shared<rtc::RtpPacketizationConfig>(AUDIO_SSRC, RTP_CNAME, sdp::AUDIO_PAYLOAD_TYPE, AUDIO_CLOCK_RATE);
        auto audio_packetizer = std::make_shared<rtc::OpusRtpPacketizer>(stream->audio_rtp);
        audio_packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(stream->audio_rtp));
        audio_packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        stream->audio_track->setMediaHandler(audio_packetizer);
        stream->audio_track->onOpen([weak_stream] {
          if (const auto stream = weak_stream.lock()) {
            {
              std::lock_guard lock(stream->mutex);
              stream->audio_open = true;
            }
            stream->condition.notify_all();
          }
        });
        stream->audio_track->onClosed([weak_stream] {
          if (const auto stream = weak_stream.lock()) {
            stream->request_stop();
          }
        });

        stream->control_thread = std::jthread([this, weak_connection, stream] {
          run_stream(weak_connection, stream);
        });

        stream->peer->setLocalDescription();
      }

      /**
       * @brief Launch or resume the application and prepare the display, as a GameStream launch does.
       * @return Why the host cannot stream, or nothing on success.
       */
      std::optional<std::string> prepare_host(const stream_t &stream) {
        std::lock_guard lock(host_mutex);
        const int running = proc::proc.running();
        if (running != 0 && running != stream.app_id) {
          return "An app is already running on this host"s;
        }

        auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
        launch_session->id = 0;
        launch_session->host_audio = false;
        launch_session->unique_id = "webrtc-" + stream.client_id;
        launch_session->client_name = stream.client_name;
        launch_session->width = stream.settings.width;
        launch_session->height = stream.settings.height;
        launch_session->fps = stream.settings.fps;
        launch_session->gcmap = 0;
        launch_session->appid = stream.app_id;
        launch_session->surround_info = 196610;  // Stereo, as Moonlight sends by default.
        launch_session->continuous_audio = false;
        launch_session->enable_hdr = stream.settings.hdr;
        launch_session->enable_sops = false;

        // Only the first stream may reconfigure the display, before encoders are probed on it.
        const bool first_stream = rtsp_stream::session_count() == 0 && active_streams == 0;
        if (first_stream) {
          display_device::configure_display(config::video, *launch_session);
          if (video::probe_encoders()) {
            display_device::revert_configuration();
            return "Failed to initialize video capture/encoding. Is a display connected and turned on?"s;
          }
        }
        if (const auto problem = unsupported_codec(stream.settings, current_codec_support())) {
          if (first_stream) {
            display_device::revert_configuration();
          }
          return problem;
        }
        if (running == 0 && stream.app_id > 0 && proc::proc.execute(stream.app_id, launch_session)) {
          if (first_stream) {
            display_device::revert_configuration();
          }
          return "Failed to start the specified application"s;
        }
        if (++active_streams == 1 && rtsp_stream::session_count() == 0) {
          platf::streaming_will_start();
        }
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
        system_tray::update_tray_playing(proc::proc.get_last_run_app_name());
#endif
        return std::nullopt;
      }

      /**
       * @brief Release the host after a stream, as the last GameStream session does.
       */
      void release_host() {
        std::lock_guard lock(host_mutex);
        if (--active_streams != 0 || rtsp_stream::session_count() != 0) {
          return;
        }
        bool revert_display = config::video.dd.config_revert_on_disconnect;
        if (proc::proc.running()) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_pausing(proc::proc.get_last_run_app_name());
#endif
        } else {
          revert_display = true;
          input::terminate_gamepads();
        }
        if (revert_display) {
          display_device::revert_configuration();
        }
        platf::streaming_will_stop();
      }

      /**
       * @brief Run one stream: wait for WebRTC, then pipe Sunshine's encoders into the tracks.
       */
      void run_stream(std::weak_ptr<connection_t> weak_connection, std::shared_ptr<stream_t> stream) {
        platf::set_thread_name("webrtc::stream");
        {
          std::unique_lock lock(stream->mutex);
          stream->condition.wait(lock, [&] {
            return stream->stop_requested || (stream->peer_connected && stream->video_open && stream->audio_open);
          });
          if (stream->stop_requested) {
            return;
          }
        }

        const auto notify = [&](std::string_view state, std::optional<std::string> detail = std::nullopt) {
          if (const auto connection = weak_connection.lock(); connection && is_current(connection, stream)) {
            send_session_status(connection, stream, state, std::move(detail));
          }
        };

        notify("connecting-sunshine");
        if (const auto problem = prepare_host(*stream)) {
          BOOST_LOG(error) << "WebRTC: "sv << *problem;
          notify("error", problem);
          return;
        }
        notify("starting-moonlight");
        capture(stream, [&] {
          notify("streaming");
        });
        release_host();

        std::optional<std::string> failure;
        {
          std::lock_guard lock(stream->mutex);
          failure = std::exchange(stream->failure, std::nullopt);
        }
        BOOST_LOG(info) << "WebRTC: session "sv << stream->id << " capture stopped"sv;
        // Without this the TV kept showing the last frame; "error" makes it end the session.
        if (failure) {
          notify("error", failure);
        }
      }

      /**
       * @brief Capture, encode and send until the stream ends.
       * @param stream Stream to feed.
       * @param on_streaming Called once the pipeline runs.
       */
      void capture(const std::shared_ptr<stream_t> &stream, const std::function<void()> &on_streaming) {
        const auto &settings = stream->settings;
        const bool hdr = settings.hdr;

        video::config_t video_config {};
        video_config.width = settings.width;
        video_config.height = settings.height;
        video_config.framerate = settings.fps;
        video_config.framerateX100 = settings.fps * 100;
        // WebRTC needs no GameStream FEC, so only the audio and RTP overhead come off the total.
        video_config.bitrate = settings.bitrate_kbps - std::min(512, settings.bitrate_kbps / 5) - std::min(500, settings.bitrate_kbps / 10);
        video_config.slicesPerFrame = 1;
        // The TV recovers losses through NACK and keyframes, not reference frame invalidation.
        video_config.numRefFrames = 1;
        // (colorspace << 1) | range: Rec.709 or Rec.2020, limited range.
        video_config.encoderCscMode = hdr ? (2 << 1) : (1 << 1);
        video_config.videoFormat = settings.codec == protocol::video_codec_e::h264 ? 0 : settings.codec == protocol::video_codec_e::hevc ? 1 :
                                                                                                                                           2;
        video_config.dynamicRange = hdr ? 1 : 0;
        video_config.chromaSamplingType = 0;
        video_config.enableIntraRefresh = 0;

        audio::config_t audio_config {};
        audio_config.packetDuration = AUDIO_PACKET_DURATION_MS;
        audio_config.channels = 2;
        audio_config.mask = 0x3;
        audio_config.flags[audio::config_t::HIGH_QUALITY] = true;
        audio_config.flags[audio::config_t::HOST_AUDIO] = false;

        auto mail = std::make_shared<safe::mail_raw_t>();
        // Registering these queues makes the encoders deliver to this stream instead of the
        // GameStream broadcast threads: the encoded frame goes straight to the RTP packetizer.
        auto video_packets = mail->queue<video::packet_t>(mail::video_packets);
        auto audio_packets = mail->queue<audio::packet_t>(mail::audio_packets);
        auto shutdown_event = mail->event<bool>(mail::shutdown);
        auto feedback = mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
        auto hdr_event = mail->event<video::hdr_info_t>(mail::hdr);
        {
          std::lock_guard lock(stream->input_mutex);
          stream->input = input::alloc(mail, "webrtc-" + stream->client_id);
        }
        {
          std::lock_guard lock(stream->mutex);
          stream->idr_event = mail->event<bool>(mail::idr);
        }
        stream->input_bridge->set_stream_active(true);

        const auto epoch = std::chrono::steady_clock::now();
        std::jthread video_capture([mail, video_config, stream] {
          platf::set_thread_name("webrtc::video");
          video::capture(mail, video_config, stream.get());
          stream->request_stop("Video capture stopped");
        });
        std::jthread audio_capture([mail, audio_config, stream] {
          platf::set_thread_name("webrtc::audio");
          audio::capture(mail, audio_config, stream.get());
        });
        std::jthread video_sender([video_packets, stream, epoch] {
          platf::set_thread_name("webrtc::video_send");
          platf::adjust_thread_priority(platf::thread_priority_e::high);
          std::optional<std::uint32_t> last_timestamp;
          const std::uint32_t frame_ticks = VIDEO_CLOCK_RATE / std::max(1, stream->settings.fps);
          while (auto packet = video_packets->pop()) {
            std::string_view payload {reinterpret_cast<const char *>(packet->data()), packet->data_size()};
            std::vector<std::uint8_t> replaced;
            if (packet->is_idr() && packet->replacements) {
              replaced = apply_replacements(payload, *packet->replacements);
              payload = {reinterpret_cast<const char *>(replaced.data()), replaced.size()};
            }
            // The capture time drives the RTP clock so the TV paces frames as they were captured.
            auto timestamp = last_timestamp ? *last_timestamp + frame_ticks : 0U;
            if (packet->frame_timestamp) {
              const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(*packet->frame_timestamp - epoch).count();
              const auto captured = static_cast<std::uint32_t>(std::max<std::int64_t>(0, elapsed) * VIDEO_CLOCK_RATE / 1'000'000);
              if (!last_timestamp || static_cast<std::int32_t>(captured - *last_timestamp) > 0) {
                timestamp = captured;
              }
            }
            last_timestamp = timestamp;
            try {
              stream->video_track->sendFrame(reinterpret_cast<const rtc::byte *>(payload.data()), payload.size(), rtc::FrameInfo(stream->video_rtp->startTimestamp + timestamp));
            } catch (const std::exception &e) {
              BOOST_LOG(warning) << "WebRTC: video frame not sent: "sv << e.what();
              stream->request_stop();
              break;
            }
          }
        });
        std::jthread audio_sender([audio_packets, stream] {
          platf::set_thread_name("webrtc::audio_send");
          platf::adjust_thread_priority(platf::thread_priority_e::high);
          std::uint32_t timestamp = 0;
          constexpr std::uint32_t samples_per_packet = AUDIO_CLOCK_RATE * AUDIO_PACKET_DURATION_MS / 1000;
          while (auto packet = audio_packets->pop()) {
            auto &bytes = packet->second;
            try {
              stream->audio_track->sendFrame(reinterpret_cast<const rtc::byte *>(bytes.begin()), bytes.size(), rtc::FrameInfo(stream->audio_rtp->startTimestamp + timestamp));
            } catch (const std::exception &e) {
              BOOST_LOG(warning) << "WebRTC: audio packet not sent: "sv << e.what();
              stream->request_stop();
              break;
            }
            timestamp += samples_per_packet;
          }
        });

        on_streaming();

        const bool launched_app = stream->app_id > 0;
        while (true) {
          {
            std::unique_lock lock(stream->mutex);
            if (stream->condition.wait_for(lock, 100ms, [&] {
                  return stream->stop_requested;
                })) {
              break;
            }
          }
          while (feedback->peek()) {
            const auto message = feedback->pop();
            if (!message) {
              break;
            }
            if (message->type == platf::gamepad_feedback_e::rumble) {
              stream->input_bridge->handle_rumble(message->id, message->data.rumble.lowfreq, message->data.rumble.highfreq);
            } else if (message->type == platf::gamepad_feedback_e::rumble_triggers) {
              stream->input_bridge->handle_trigger_rumble(message->id, message->data.rumble_triggers.left_trigger, message->data.rumble_triggers.right_trigger);
            }
          }
          if (hdr_event->peek()) {
            const auto hdr_info = hdr_event->pop();
            const bool enabled = hdr_info && hdr_info->enabled;
            BOOST_LOG(info) << "WebRTC: host display HDR "sv << (enabled ? "enabled"sv : "disabled"sv);
            if (hdr && !enabled) {
              // A Main10 stream with SDR content would look washed out; never fall back silently.
              stream->request_stop("HDR is not enabled on the host display"s);
            }
          }
          if (launched_app) {
            std::lock_guard lock(host_mutex);
            if (proc::proc.running() != stream->app_id) {
              stream->request_stop("The application exited"s);
            }
          }
        }

        stream->input_bridge->set_stream_active(false);
        {
          std::lock_guard lock(stream->mutex);
          stream->idr_event.reset();
        }
        shutdown_event->raise(true);
        video_packets->stop();
        audio_packets->stop();
        video_capture.join();
        audio_capture.join();
        video_sender.join();
        audio_sender.join();
        {
          std::lock_guard lock(stream->input_mutex);
          // Release anything still held so no key or button stays pressed on the host.
          input::reset(stream->input);
          stream->input.reset();
        }
      }

      /**
       * @brief End a connection's stream, leaving the application running.
       * @param connection Connection.
       * @param notify Whether to report the transition to the TV.
       */
      void stop_stream(const std::shared_ptr<connection_t> &connection, bool notify) {
        std::shared_ptr<stream_t> stream;
        {
          std::lock_guard lock(connection->mutex);
          stream = connection->stream;
        }
        if (!stream) {
          if (notify) {
            send(connection, protocol::make_session_status("idle"));
          }
          return;
        }
        if (notify) {
          send_session_status(connection, stream, "stopping");
        }
        stream->input_bridge->on_transport_closed();
        stream->request_stop();
        if (stream->control_thread.joinable() && stream->control_thread.get_id() != std::this_thread::get_id()) {
          stream->control_thread.join();
        }
        stream->input_bridge->shutdown();
        if (stream->peer) {
          // Its callbacks capture this server, which may be shutting down.
          stream->peer->resetCallbacks();
          stream->peer->close();
        }
        {
          std::lock_guard lock(connection->mutex);
          if (connection->stream == stream) {
            connection->stream.reset();
          }
        }
        BOOST_LOG(info) << "WebRTC: session "sv << stream->id << " stopped"sv;
        if (notify) {
          send(connection, protocol::make_session_status("idle"));
          send(connection, protocol::make_gateway_status(status(connection)));
        }
      }

      /**
       * @brief Apply the TV's answer, checking that HEVC Main10 survived negotiation.
       * @param connection Connection.
       * @param answer Answer.
       */
      void handle_answer(const std::shared_ptr<connection_t> &connection, const protocol::answer_t &answer) {
        std::shared_ptr<stream_t> stream;
        {
          std::lock_guard lock(connection->mutex);
          stream = connection->stream;
        }
        if (!stream || stream->id != answer.session_id || !stream->peer) {
          BOOST_LOG(debug) << "WebRTC: ignored answer for inactive session "sv << answer.session_id;
          return;
        }
        if (!sdp::has_expected_hevc_format_parameters(answer.sdp, stream->settings)) {
          BOOST_LOG(error) << "WebRTC: the TV did not negotiate HEVC Main10"sv;
          send_session_status(connection, stream, "error", "Tizen did not negotiate HEVC Main10"s);
          stream->request_stop();
          return;
        }
        if (stream->settings.hdr && stream->settings.codec == protocol::video_codec_e::hevc) {
          if (const auto level = sdp::hevc_level_id(answer.sdp)) {
            BOOST_LOG(info) << "WebRTC: TV HEVC Main10 answer level-id "sv << *level;
          }
        }
        stream->peer->setRemoteDescription(rtc::Description(answer.sdp, "answer"));

        std::vector<std::pair<std::string, std::string>> pending;
        {
          std::lock_guard lock(stream->candidate_mutex);
          stream->has_remote_description = true;
          pending.swap(stream->pending_candidates);
        }
        for (auto &[candidate, mid] : pending) {
          stream->peer->addRemoteCandidate(rtc::Candidate(candidate, mid));
        }
      }

      /**
       * @brief Apply a remote candidate, holding it until the answer arrives.
       * @param connection Connection.
       * @param candidate Candidate.
       */
      void handle_candidate(const std::shared_ptr<connection_t> &connection, const protocol::candidate_t &candidate) {
        std::shared_ptr<stream_t> stream;
        {
          std::lock_guard lock(connection->mutex);
          stream = connection->stream;
        }
        if (!stream || stream->id != candidate.session_id || !stream->peer) {
          return;
        }
        {
          std::lock_guard lock(stream->candidate_mutex);
          if (!stream->has_remote_description) {
            stream->pending_candidates.emplace_back(candidate.candidate, candidate.mid);
            return;
          }
        }
        stream->peer->addRemoteCandidate(rtc::Candidate(candidate.candidate, candidate.mid));
      }

      /**
       * @brief Start a stop or switch, refusing a second one.
       * @param connection Connection.
       * @param request_type Request being served.
       * @return True when the operation may proceed.
       */
      bool begin_host_operation(const std::shared_ptr<connection_t> &connection, std::string_view request_type) {
        std::lock_guard lock(connection->mutex);
        if (connection->host_operation_active) {
          send(connection, protocol::make_error(request_type, "host-operation-active", "A Sunshine session operation is already in progress"));
          return false;
        }
        connection->host_operation_active = true;
        return true;
      }

      /**
       * @brief Allow the next stop or switch.
       * @param connection Connection.
       */
      void finish_host_operation(const std::shared_ptr<connection_t> &connection) {
        std::lock_guard lock(connection->mutex);
        connection->host_operation_active = false;
      }

      /**
       * @brief Send the application list and server status after the host changed.
       * @param connection Connection.
       */
      void reconcile(const std::shared_ptr<connection_t> &connection) {
        send_apps(connection);
        send(connection, protocol::make_gateway_status(status(connection)));
      }

      /**
       * @brief Stop the running application, then optionally start another, as one ordered operation.
       */
      void stop_or_switch_host(const std::shared_ptr<connection_t> &connection, std::optional<protocol::start_session_t> target) {
        const std::string request_type = target ? "switch-session" : "stop-host-session";
        if (!begin_host_operation(connection, request_type)) {
          return;
        }
        try {
          const int running = proc::proc.running();
          const auto running_id = running == 0 ? std::optional<std::string> {} : std::optional<std::string>(std::to_string(running));
          if (target) {
            const auto apps = applications();
            if (std::ranges::none_of(apps, [&](const auto &app) {
                  return app.id == target->app_id;
                })) {
              throw std::runtime_error("Selected Sunshine application no longer exists");
            }
            if (running_id && *running_id == target->app_id) {
              send(connection, protocol::make_host_session_status("resuming", running_id, target->app_id));
              finish_host_operation(connection);
              start_stream(connection, target->app_id, target->settings);
              return;
            }
          }
          if (!running_id) {
            if (target) {
              send(connection, protocol::make_host_session_status("starting", std::nullopt, target->app_id));
              finish_host_operation(connection);
              start_stream(connection, target->app_id, target->settings);
            } else {
              reconcile(connection);
              send(connection, protocol::make_host_session_status("stopped"));
              finish_host_operation(connection);
            }
            return;
          }

          send(connection, protocol::make_host_session_status(target ? "switching" : "stopping", running_id, target ? std::optional<std::string>(target->app_id) : std::nullopt));
          // A local disconnect stays distinct from stopping the application; here both happen.
          stop_stream(connection, false);
          {
            std::lock_guard lock(host_mutex);
            rtsp_stream::terminate_sessions();
            if (proc::proc.running() > 0) {
              proc::proc.terminate();
            }
            display_device::revert_configuration();
          }
          const auto deadline = std::chrono::steady_clock::now() + HOST_IDLE_TIMEOUT;
          while (proc::proc.running() != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
              throw std::runtime_error("Sunshine still reports a running application after cancellation");
            }
            std::this_thread::sleep_for(250ms);
          }

          reconcile(connection);
          if (target) {
            send(connection, protocol::make_host_session_status("starting", std::nullopt, target->app_id));
            finish_host_operation(connection);
            start_stream(connection, target->app_id, target->settings);
          } else {
            send(connection, protocol::make_host_session_status("stopped"));
            finish_host_operation(connection);
          }
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "WebRTC: host session operation failed: "sv << e.what();
          reconcile(connection);
          send(connection, protocol::make_host_session_status("failed", std::nullopt, std::nullopt, e.what()));
          finish_host_operation(connection);
        }
      }

      tv_auth::tv_client_store_t _tv_clients;  ///< Paired TVs.
      std::mutex _pairing_mutex;  ///< Protects @ref _pairing.
      tv_auth::tv_pairing_window_t _pairing;  ///< TV pairing window.
      std::mutex _connections_mutex;  ///< Protects the connections and codec state.
      std::vector<std::shared_ptr<connection_t>> _pending;  ///< Connections not authenticated yet.
      std::shared_ptr<connection_t> _active;  ///< The authenticated TV.
      codec_support_t _codecs {};  ///< Codec support last announced.
      std::mutex _artwork_mutex;  ///< Protects @ref _artwork.
      std::unordered_map<std::string, std::optional<std::pair<std::string, std::string>>> _artwork;  ///< MIME type and Base64 by app ID.
      std::atomic<std::uint64_t> _next_session_id {1};  ///< Next session ID.
      std::unique_ptr<rtc::WebSocketServer> _server;  ///< Signaling server.
    };

    std::mutex server_mutex;  ///< Protects @ref running_server.
    server_t *running_server = nullptr;  ///< The server while it runs.
  }  // namespace

  void start() {
    if (!config::webrtc.enabled) {
      BOOST_LOG(info) << "WebRTC: TV server disabled"sv;
      return;
    }
    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, const std::string &message) {
      if (level <= rtc::LogLevel::Error) {
        BOOST_LOG(error) << "libdatachannel: "sv << message;
      } else {
        BOOST_LOG(warning) << "libdatachannel: "sv << message;
      }
    });
    rtc::Preload();

    std::unique_ptr<server_t> server;
    try {
      server = std::make_unique<server_t>();
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "WebRTC: TV server failed to start on port "sv << config::webrtc.port << ": "sv << e.what();
      return;
    }
    {
      std::lock_guard lock(server_mutex);
      running_server = server.get();
    }
    server->run();
    {
      std::lock_guard lock(server_mutex);
      running_server = nullptr;
    }
    server.reset();
    rtc::Cleanup();
  }

  int session_count() {
    return active_streams.load();
  }

  pairing_t open_tv_pairing() {
    std::lock_guard lock(server_mutex);
    if (!running_server) {
      return {false, {}, "The TV server is not running"};
    }
    try {
      return {true, running_server->open_pairing(), "Enter this PIN on the TV within two minutes"};
    } catch (const std::exception &) {
      return {false, {}, "TV pairing could not be started"};
    }
  }

  std::string tv_pairing_status() {
    std::lock_guard lock(server_mutex);
    return running_server ? running_server->pairing_status() : "tv-pairing-idle"s;
  }

  std::vector<paired_tv_t> paired_tvs() {
    std::lock_guard lock(server_mutex);
    return running_server ? running_server->paired() : std::vector<paired_tv_t> {};
  }

  std::optional<std::size_t> unpair_all_tvs() {
    std::lock_guard lock(server_mutex);
    if (!running_server) {
      return std::nullopt;
    }
    return running_server->forget_tvs();
  }
}  // namespace webrtc_stream
