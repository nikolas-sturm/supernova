/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

using namespace std::literals;

namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;  ///< Enabled paired-client certificates accepted by Sunshine's GameStream HTTPS server.
  std::mutex client_auth_mutex;  ///< Serializes paired-client state and certificate authorization changes.

  /**
   * @brief HTTPS server backend that adds Sunshine's client-certificate verification.
   */
  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    /**
     * @brief Initialize the HTTPS server with Sunshine's certificate and key files.
     *
     * @param certification_file Path to the server certificate file.
     * @param private_key_file Path to the matching private key file.
     */
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<int(SSL *, const boost::asio::ip::tcp::endpoint &)> verify;  ///< Callback that validates a client's TLS certificate after handshake.
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;  ///< Handler used to return the pairing challenge when client verification fails.

  protected:
    boost::asio::ssl::context context;  ///< TLS server context configured with Sunshine's certificate and protocol policy.

    /**
     * @brief Enable client-certificate verification after the listening socket is bound.
     */
    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    /**
     * @brief Accept a pending connection and arm the server for the next client.
     */
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              if (verify && !verify(session->connection->socket->native_handle(), session->connection->socket->lowest_layer().remote_endpoint())) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  /**
   * @brief HTTPS server type used for GameStream endpoints requiring TLS.
   */
  using https_server_t = SunshineHTTPSServer;
  /**
   * @brief Plain HTTP server type used for GameStream endpoints without TLS.
   */
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  /**
   * @brief Internal HTTPS credential paths for the configuration server.
   */
  struct conf_intern_t {
    std::string servercert;  ///< Server certificate PEM string.
    std::string pkey;  ///< Private key PEM string or path.
  } conf_intern;  ///< TLS credential paths loaded from Sunshine's runtime configuration.

  /**
   * @brief Certificate entry associated with a client name and UUID.
   */
  struct named_cert_t {
    std::string name;  ///< Human-readable name for this item.
    std::string uuid;  ///< Persistent Moonlight client UUID associated with the certificate.
    std::string cert;  ///< Certificate PEM string or path.
    std::string platform;  ///< Platform reported by the paired Eclipse client.
    eclipse_api::client_permissions_t permissions = eclipse_api::legacy_client_permissions();  ///< Certificate-bound permissions.
    bool enabled = true;  ///< Whether this persisted client entry may connect.
  };

  /**
   * @brief Immutable identity captured from one verified TLS connection.
   */
  struct verified_client_t {
    std::string uuid;  ///< Persistent paired-client UUID.
    std::string name;  ///< Paired-client friendly name.
    std::string cert;  ///< Canonical certificate PEM.
    eclipse_api::client_permissions_t permissions;  ///< Permission snapshot used by requests on this connection.
  };

  /**
   * @brief Persisted pairing data for one Moonlight client.
   */
  struct client_t {
    std::vector<named_cert_t> named_devices;  ///< Persisted Moonlight clients allowed to pair or reconnect.
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;  ///< Pairing sessions keyed by temporary unique ID.
  std::recursive_mutex map_id_sess_mutex;  ///< Mutex protecting pairing-session storage and lifecycle transitions.
  client_t client_root;  ///< In-memory representation of the paired-client database.
  std::atomic<uint32_t> session_id_counter;  ///< Monotonic counter used to allocate GameStream session IDs.
  constexpr std::size_t MAX_VERIFIED_CLIENT_CONNECTIONS = 1024;  ///< Bound for request-scoped TLS identity snapshots.
  std::map<std::string, verified_client_t, std::less<>> verified_clients;  ///< Verified clients keyed by TLS peer endpoint.

  /**
   * @brief Resumable logical session associated with Sunshine's single running application.
   */
  struct logical_session_t {
    std::string id;  ///< Stable Eclipse session UUID.
    std::string client_uuid;  ///< Persistent owning-client UUID.
    std::string app_uuid;  ///< Stable application UUID.
    int legacy_app_id;  ///< Legacy GameStream application ID.
    std::chrono::system_clock::time_point started_at;  ///< Session creation time.
    int width;  ///< Requested capture width.
    int height;  ///< Requested capture height.
    int fps;  ///< Requested refresh rate.
    bool hdr;  ///< Whether HDR was requested.
  };

  std::mutex logical_session_mutex;  ///< Protects resumable logical-session metadata.
  std::optional<logical_session_t> logical_session;  ///< Current resumable logical session.

  /**
   * @brief Case-insensitive map used for HTTP headers and query parameters.
   */
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  /**
   * @brief Shared HTTPS response object passed to GameStream handlers.
   */
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  /**
   * @brief Shared HTTPS request object received by GameStream handlers.
   */
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  /**
   * @brief Shared HTTP response object passed to redirect and discovery handlers.
   */
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  /**
   * @brief Shared HTTP request object received by redirect and discovery handlers.
   */
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  /**
   * @brief Certificate operations supported by the pairing API.
   */
  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  /**
   * @brief Read a named query argument from the HTTP request map.
   *
   * @param args Parsed query-string argument map.
   * @param name Query parameter name to read.
   * @param default_value Value returned when the parameter is absent.
   * @return Query parameter value, default value, or an empty string.
   */
  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  /**
   * @brief Format a TCP endpoint for verified-connection identity lookup.
   *
   * @param endpoint Remote TLS endpoint.
   * @return Stable address-and-port key for the connection lifetime.
   */
  std::string endpoint_key(const boost::asio::ip::tcp::endpoint &endpoint) {
    return std::format("{}:{}", net::addr_to_normalized_string(endpoint.address()), endpoint.port());
  }

  bool permissions_expired(const eclipse_api::client_permissions_t &permissions);

  /**
   * @brief Return authenticated identity captured for an HTTPS request.
   *
   * @param request Paired-client HTTPS request.
   * @return Immutable identity snapshot, or no value after revocation or expiry.
   */
  std::optional<verified_client_t> verified_client(const req_https_t &request) {
    std::lock_guard lock {client_auth_mutex};
    const auto client = verified_clients.find(endpoint_key(request->remote_endpoint()));
    if (client == verified_clients.end() || permissions_expired(client->second.permissions)) {
      return std::nullopt;
    }
    return client->second;
  }

  /**
   * @brief Check a scope in an authenticated client policy snapshot.
   *
   * @param client Authenticated paired client.
   * @param scope Required stable scope name.
   * @return `true` when the client holds the requested scope.
   */
  bool scope_allowed(const verified_client_t &client, const std::string_view scope) {
    return client.permissions.scopes.contains(scope);
  }

  /**
   * @brief Check application allowlist access for a client.
   *
   * @param client Authenticated paired client.
   * @param app_uuid Stable application UUID.
   * @return `true` when allowlist is empty or contains the application.
   */
  bool app_allowed(const verified_client_t &client, const std::string_view app_uuid) {
    return client.permissions.allowed_apps.empty() || client.permissions.allowed_apps.contains(app_uuid);
  }

  /**
   * @brief Persist the current state to its backing store.
   *
   * @return `true` when complete state was atomically replaced.
   */
  bool save_state() {
    pt::ptree root;

    if (fs::exists(config::nvhttp.file_state)) {
      try {
        pt::read_json(config::nvhttp.file_state, root);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        return false;
      }
    }

    root.erase("root"s);

    root.put("root.uniqueid", http::unique_id);
    client_t &client = client_root;
    pt::ptree node;

    pt::ptree named_cert_nodes;
    for (auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_node.put("enabled"s, named_cert.enabled);
      named_cert_node.put("platform"s, named_cert.platform);
      named_cert_node.put("eclipse_permissions.version"s, 1);
      named_cert_node.put("eclipse_permissions.expires_at"s, named_cert.permissions.expires_at);
      named_cert_node.put("eclipse_permissions.input.keyboard"s, named_cert.permissions.input.keyboard);
      named_cert_node.put("eclipse_permissions.input.mouse"s, named_cert.permissions.input.mouse);
      named_cert_node.put("eclipse_permissions.input.controller"s, named_cert.permissions.input.controller);
      named_cert_node.put("eclipse_permissions.input.touch"s, named_cert.permissions.input.touch);
      named_cert_node.put("eclipse_permissions.input.pen"s, named_cert.permissions.input.pen);
      pt::ptree scope_nodes;
      for (const auto &scope : named_cert.permissions.scopes) {
        pt::ptree scope_node;
        scope_node.put_value(scope);
        scope_nodes.push_back(std::make_pair(""s, scope_node));
      }
      named_cert_node.add_child("eclipse_permissions.scopes"s, scope_nodes);
      pt::ptree app_nodes;
      for (const auto &app_uuid : named_cert.permissions.allowed_apps) {
        pt::ptree app_node;
        app_node.put_value(app_uuid);
        app_nodes.push_back(std::make_pair(""s, app_node));
      }
      named_cert_node.add_child("eclipse_permissions.allowed_apps"s, app_nodes);
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root.add_child("root.named_devices"s, named_cert_nodes);

    try {
      std::ostringstream serialized;
      pt::write_json(serialized, root);
      if (file_handler::write_file_atomic(config::nvhttp.file_state.c_str(), serialized.str()) != 0) {
        BOOST_LOG(error) << "Couldn't atomically replace "sv << config::nvhttp.file_state;
        return false;
      }
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return false;
    }
    return true;
  }

  /**
   * @brief Convert a PEM certificate to Sunshine's canonical OpenSSL serialization.
   *
   * @param cert_pem PEM-encoded certificate to canonicalize.
   * @return Canonical PEM text, or an empty string when the certificate is invalid.
   */
  std::string canonical_certificate_pem(const std::string_view cert_pem) {
    auto certificate = crypto::x509(cert_pem);
    return certificate ? crypto::pem(certificate) : std::string {};
  }

  /**
   * @brief Check whether a client policy has passed its configured expiry.
   *
   * @param permissions Client policy to inspect.
   * @return `true` when policy expiry is set and has passed.
   */
  bool permissions_expired(const eclipse_api::client_permissions_t &permissions) {
    if (permissions.expires_at == 0) {
      return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return now >= permissions.expires_at;
  }

  /**
   * @brief Rebuild the GameStream trust stores from enabled paired-client records.
   *
   * @note The caller must hold `client_auth_mutex`.
   */
  void rebuild_client_cert_chain() {
    cert_chain.clear();
    for (const auto &named_cert : client_root.named_devices) {
      if (!named_cert.enabled || permissions_expired(named_cert.permissions)) {
        continue;
      }

      auto certificate = crypto::x509(named_cert.cert);
      if (!certificate) {
        BOOST_LOG(warning) << "Ignoring invalid paired-client certificate"sv;
        continue;
      }

      cert_chain.add(std::move(certificate));
    }
  }

  /**
   * @brief Check whether a certificate exactly matches an enabled paired-client record.
   *
   * @param certificate Parsed client certificate to compare by canonical X.509 identity.
   * @return `true` only when exactly one matching paired-client record is enabled.
   * @note The caller must hold `client_auth_mutex`.
   */
  bool is_client_enabled(const X509 *certificate) {
    bool matched = false;
    for (const auto &named_cert : client_root.named_devices) {
      auto stored_certificate = crypto::x509(named_cert.cert);
      if (!stored_certificate || X509_cmp(stored_certificate.get(), certificate) != 0) {
        continue;
      }

      if (matched || !named_cert.enabled || permissions_expired(named_cert.permissions)) {
        return false;
      }
      matched = true;
    }
    return matched;
  }

  /**
   * @brief Verify a client certificate against the exact enabled paired identity.
   *
   * @param certificate Parsed client certificate presented during the TLS handshake.
   * @return `nullptr` when authorized, otherwise a non-sensitive error string.
   * @note The caller must hold `client_auth_mutex`.
   */
  const char *verify_client_certificate(X509 *certificate) {
    auto error = cert_chain.verify(certificate);
    if (error) {
      return error;
    }
    return is_client_enabled(certificate) ? nullptr : "Client certificate identity is not enabled";
  }

  /**
   * @brief Load state from its backing store.
   */
  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(config::nvhttp.file_state, tree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    auto root = tree.get_child("root");
    client_t client;

    // Import from old format
    if (root.get_child_optional("devices")) {
      auto device_nodes = root.get_child("devices");
      for (auto &[_, device_node] : device_nodes) {
        auto uniqID = device_node.get<std::string>("uniqueid");

        if (device_node.count("certs")) {
          for (auto &[_, el] : device_node.get_child("certs")) {
            named_cert_t named_cert;
            named_cert.name = ""s;
            named_cert.cert = el.get_value<std::string>();
            named_cert.uuid = uuid_util::uuid_t::generate().string();
            client.named_devices.emplace_back(named_cert);
          }
        }
      }
    }

    if (root.count("named_devices")) {
      for (auto &[_, el] : root.get_child("named_devices")) {
        named_cert_t named_cert;
        named_cert.name = el.get_child("name").get_value<std::string>();
        named_cert.cert = el.get_child("cert").get_value<std::string>();
        named_cert.uuid = el.get_child("uuid").get_value<std::string>();
        named_cert.enabled = el.get<bool>("enabled", true);
        named_cert.platform = el.get<std::string>("platform", "");
        if (el.get<int>("eclipse_permissions.version", 0) == 1) {
          named_cert.permissions = {};
          named_cert.permissions.expires_at = el.get<std::int64_t>("eclipse_permissions.expires_at", 0);
          named_cert.permissions.input.keyboard = el.get<bool>("eclipse_permissions.input.keyboard", true);
          named_cert.permissions.input.mouse = el.get<bool>("eclipse_permissions.input.mouse", true);
          named_cert.permissions.input.controller = el.get<bool>("eclipse_permissions.input.controller", true);
          named_cert.permissions.input.touch = el.get<bool>("eclipse_permissions.input.touch", true);
          named_cert.permissions.input.pen = el.get<bool>("eclipse_permissions.input.pen", true);
          if (const auto scopes = el.get_child_optional("eclipse_permissions.scopes")) {
            for (const auto &[_, scope] : *scopes) {
              const auto value = scope.get_value<std::string>();
              if (eclipse_api::is_known_scope(value)) {
                named_cert.permissions.scopes.emplace(value);
              }
            }
          }
          if (const auto allowed_apps = el.get_child_optional("eclipse_permissions.allowed_apps")) {
            for (const auto &[_, app_uuid] : *allowed_apps) {
              named_cert.permissions.allowed_apps.emplace(app_uuid.get_value<std::string>());
            }
          }
        }
        client.named_devices.emplace_back(named_cert);
      }
    }

    for (auto &named_cert : client.named_devices) {
      auto canonical_certificate = canonical_certificate_pem(named_cert.cert);
      if (!canonical_certificate.empty()) {
        named_cert.cert = std::move(canonical_certificate);
      }
    }

    std::lock_guard lock {client_auth_mutex};
    client_root = std::move(client);
    rebuild_client_cert_chain();
  }

  /**
   * @brief Add authorized client data.
   *
   * @param name Human-readable name to assign.
   * @param cert Certificate data or object used by the operation.
   * @param platform Client platform reported during pairing.
   * @param permissions Certificate-bound policy approved during pairing.
   * @return Persistent UUID for the added client, or an empty string when the certificate is invalid.
   */
  std::string add_authorized_client(
    const std::string &name,
    std::string &&cert,
    std::string platform = {},
    eclipse_api::client_permissions_t permissions = eclipse_api::legacy_client_permissions()
  ) {
    auto canonical_certificate = canonical_certificate_pem(cert);
    if (canonical_certificate.empty()) {
      return {};
    }

    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = std::move(canonical_certificate);
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    named_cert.platform = std::move(platform);
    named_cert.permissions = std::move(permissions);

    std::lock_guard lock {client_auth_mutex};
    client_root.named_devices.emplace_back(std::move(named_cert));
    rebuild_client_cert_chain();

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      if (!save_state()) {
        client_root.named_devices.pop_back();
        rebuild_client_cert_chain();
        return {};
      }
    }
    return client_root.named_devices.back().uuid;
  }

  /**
   * @brief Create launch session.
   *
   * @param host_audio Host audio.
   * @param args Arguments forwarded to the callable or parser.
   * @param client Authenticated paired-client identity.
   * @param session_id Existing logical session UUID used for resume.
   * @return Constructed launch session object.
   */
  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(
    bool host_audio,
    const args_t &args,
    const verified_client_t &client,
    std::string session_id = {}
  ) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;
    launch_session->session_id = session_id.empty() ? uuid_util::uuid_t::generate().string() : std::move(session_id);
    launch_session->client_uuid = client.uuid;
    launch_session->client_cert = client.cert;
    launch_session->client_name = client.name;
    launch_session->input_permissions = client.permissions.input;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    std::stringstream mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        launch_session->fps = atoi(segment.c_str());
      }
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = (int) util::from_view(get_arg(args, "appid", "unknown"));
    const auto catalog = proc::catalog_snapshot();
    if (const auto app = std::ranges::find(catalog.apps, std::to_string(launch_session->appid), &proc::ctx_t::id); app != catalog.apps.end()) {
      launch_session->app_uuid = app->uuid;
    }
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = (int) util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = (int) util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;
    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>((int) util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  /**
   * @brief Write a completed response to the client waiting for PIN approval.
   *
   * @param sess Pairing session that owns the waiting response.
   * @param tree XML response body to write.
   * @return `true` when a live response was available.
   */
  bool write_pairing_response(pair_session_t &sess, const pt::ptree &tree) {
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &response = sess.async_insert_pin.response;
    if (response.has_left() && response.left()) {
      response.left()->close_connection_after_response = true;
      response.left()->write(data.str());
    } else if (response.has_right() && response.right()) {
      response.right()->close_connection_after_response = true;
      response.right()->write(data.str());
    } else {
      return false;
    }

    response = std::monostate {};
    return true;
  }

  /**
   * @brief Expire stale pairing sessions while the session mutex is held.
   *
   * @param now Monotonic time used to evaluate session deadlines.
   */
  void expire_pair_sessions_unlocked(const std::chrono::steady_clock::time_point now) {
    for (auto it = map_id_sess.begin(); it != map_id_sess.end();) {
      if (it->second.async_insert_pin.expires_at > now) {
        ++it;
        continue;
      }

      pt::ptree tree;
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 408);
      tree.put("root.<xmlattr>.status_message", "Pairing session expired");
      write_pairing_response(it->second, tree);
      it = map_id_sess.erase(it);
    }
  }

  pair_session_insert_e insert_pair_session(pair_session_t sess, std::string &pairing_id) {
    std::scoped_lock lock(map_id_sess_mutex);
    const auto now = std::chrono::steady_clock::now();
    expire_pair_sessions_unlocked(now);
    pairing_id.clear();

    if (map_id_sess.contains(sess.client.uniqueID)) {
      return pair_session_insert_e::ALREADY_EXISTS;
    }
    if (map_id_sess.size() >= MAX_PENDING_PAIRING_SESSIONS) {
      return pair_session_insert_e::FULL;
    }

    do {
      pairing_id = util::hex_vec(crypto::rand(PAIRING_ID_SIZE / 2), true);
    } while (std::ranges::any_of(map_id_sess, [&](const auto &entry) {
      return entry.second.async_insert_pin.id == pairing_id;
    }));

    sess.async_insert_pin.id = pairing_id;
    sess.async_insert_pin.expires_at = now + PAIRING_SESSION_TIMEOUT;
    map_id_sess.emplace(sess.client.uniqueID, std::move(sess));
    return pair_session_insert_e::ADDED;
  }

  bool is_valid_pairing_id(const std::string_view pairing_id) {
    return pairing_id.size() == PAIRING_ID_SIZE && std::ranges::all_of(pairing_id, [](const char character) {
             return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
           });
  }

  bool is_valid_pairing_pin(const std::string_view pin) {
    return pin.size() == 4 && std::ranges::all_of(pin, [](const char character) {
             return character >= '0' && character <= '9';
           });
  }

  bool is_valid_pairing_name(const std::string_view name) {
    return !name.empty() && name.size() <= MAX_PAIRING_CLIENT_NAME_SIZE;
  }

  void expire_pair_sessions(const std::chrono::steady_clock::time_point now) {
    std::scoped_lock lock(map_id_sess_mutex);
    expire_pair_sessions_unlocked(now);
  }

  std::vector<pending_pairing_t> get_pending_pairings() {
    std::scoped_lock lock(map_id_sess_mutex);
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());

    std::vector<const pair_session_t *> pending_sessions;
    pending_sessions.reserve(map_id_sess.size());
    for (const auto &entry : map_id_sess) {
      const auto &sess = entry.second;
      if (sess.last_phase == PAIR_PHASE::NONE) {
        pending_sessions.push_back(&sess);
      }
    }
    std::ranges::sort(pending_sessions, {}, [](const pair_session_t *sess) {
      return sess->async_insert_pin.expires_at;
    });

    std::vector<pending_pairing_t> result;
    result.reserve(pending_sessions.size());
    for (const auto *sess : pending_sessions) {
      std::vector<std::string> requested_scopes;
      if (sess->client.requested_eclipse_permissions) {
        requested_scopes.assign(sess->client.requested_permissions.scopes.begin(), sess->client.requested_permissions.scopes.end());
      }
      std::vector<std::string> requested_inputs;
      if (sess->client.requested_eclipse_permissions) {
        const auto &input = sess->client.requested_permissions.input;
        if (input.keyboard) {
          requested_inputs.emplace_back("keyboard");
        }
        if (input.mouse) {
          requested_inputs.emplace_back("mouse");
        }
        if (input.controller) {
          requested_inputs.emplace_back("controller");
        }
        if (input.touch) {
          requested_inputs.emplace_back("touch");
        }
        if (input.pen) {
          requested_inputs.emplace_back("pen");
        }
      }
      result.push_back({
        .id = sess->async_insert_pin.id,
        .name = sess->async_insert_pin.device_name,
        .address = sess->async_insert_pin.address,
        .platform = sess->client.platform,
        .requested_scopes = std::move(requested_scopes),
        .requested_inputs = std::move(requested_inputs),
      });
    }
    return result;
  }

  bool cancel_pairing(const std::string_view pairing_id) {
    if (!is_valid_pairing_id(pairing_id)) {
      return false;
    }

    std::scoped_lock lock(map_id_sess_mutex);
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());

    const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
      return entry.second.last_phase == PAIR_PHASE::NONE && entry.second.async_insert_pin.id == pairing_id;
    });
    if (sess_it == map_id_sess.end()) {
      return false;
    }

    pt::ptree tree;
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", "Pairing request cancelled by operator");
    write_pairing_response(sess_it->second, tree);
    map_id_sess.erase(sess_it);
    return true;
  }

  void remove_session(const pair_session_t &sess) {
    std::scoped_lock lock(map_id_sess_mutex);
    map_id_sess.erase(sess.client.uniqueID);
  }

  /**
   * @brief Return the GameStream pairing failure response.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param status_msg Status msg.
   */
  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    sess.failed = true;
  }

  /**
   * @brief Return the server certificate text for pairing responses.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param pin PIN supplied by the client during pairing.
   */
  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the client-challenge phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param challenge Client challenge bytes from the pairing request.
   */
  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the server-challenge response phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param encrypted_response Encrypted response.
   */
  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  /**
   * @brief Handle the client pairing-secret phase of GameStream pairing.
   *
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param client_pairing_secret Client pairing secret.
   */
  void clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      // The client is now successfully paired and will be authorized to connect
      auto permissions = client.requested_eclipse_permissions ? std::move(client.requested_permissions) : eclipse_api::legacy_client_permissions();
      const auto uuid = add_authorized_client(client.name, std::move(client.cert), std::move(client.platform), std::move(permissions));
      tree.put("root.paired", uuid.empty() ? 0 : 1);
      if (!uuid.empty()) {
        BOOST_LOG(info) << "Audit: paired client ["sv << uuid << "] as ["sv << client.name << ']';
      }
    } else {
      tree.put("root.paired", 0);
    }

    tree.put("root.<xmlattr>.status_code", 200);
  }

  template<class T>
  struct tunnel;

  /**
   * @brief HTTPS tunnel session used for encrypted client requests.
   */
  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;  ///< To string.
  };

  /**
   * @brief Plain HTTP server wrapper used for non-TLS endpoints.
   */
  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;  ///< To string.
  };

  /**
   * @brief Write req details to the log.
   *
   * @param request HTTP request data from the client.
   */
  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << (boost::iequals(name, "Authorization") ? "CREDENTIALS REDACTED" : val);
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      const bool sensitive = boost::iequals(name, "rikey") || boost::iequals(name, "rikeyid") || boost::iequals(name, "clientcert") || boost::iequals(name, "clientpairingsecret") || boost::iequals(name, "clientchallenge") || boost::iequals(name, "serverchallengeresp");
      BOOST_LOG(debug) << name << " -- " << (sensitive ? "CREDENTIALS REDACTED" : val);
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Return a GameStream HTTP not-found response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  /**
   * @brief Dispatch the top-level GameStream pairing request by phase.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void pair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        pair_session_t sess;

        sess.client.uniqueID = uniqID;
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);
        sess.async_insert_pin.salt = get_arg(args, "salt");
        sess.async_insert_pin.device_name = get_arg(args, "devicename");
        sess.async_insert_pin.address = net::addr_to_normalized_string(request->remote_endpoint().address());
        sess.client.platform = get_arg(args, "eclipsePlatform", "").substr(0, 64);
        bool invalid_eclipse_permission = false;
        if (const auto scopes = args.find("eclipseScopes"); scopes != args.end()) {
          sess.client.requested_eclipse_permissions = true;
          sess.client.requested_permissions.scopes.clear();
          std::stringstream values {scopes->second};
          std::string scope;
          while (std::getline(values, scope, ',')) {
            if (eclipse_api::is_known_scope(scope)) {
              sess.client.requested_permissions.scopes.emplace(std::move(scope));
            } else if (!scope.empty()) {
              invalid_eclipse_permission = true;
            }
          }
        }
        if (const auto inputs = args.find("eclipseInput"); inputs != args.end()) {
          sess.client.requested_eclipse_permissions = true;
          sess.client.requested_permissions.input = {};
          sess.client.requested_permissions.input.keyboard = false;
          sess.client.requested_permissions.input.mouse = false;
          sess.client.requested_permissions.input.controller = false;
          sess.client.requested_permissions.input.touch = false;
          sess.client.requested_permissions.input.pen = false;
          std::stringstream values {inputs->second};
          std::string input_class;
          while (std::getline(values, input_class, ',')) {
            if (input_class == "keyboard") {
              sess.client.requested_permissions.input.keyboard = true;
            } else if (input_class == "mouse") {
              sess.client.requested_permissions.input.mouse = true;
            } else if (input_class == "controller") {
              sess.client.requested_permissions.input.controller = true;
            } else if (input_class == "touch") {
              sess.client.requested_permissions.input.touch = true;
            } else if (input_class == "pen") {
              sess.client.requested_permissions.input.pen = true;
            } else if (!input_class.empty()) {
              invalid_eclipse_permission = true;
            }
          }
        }
        if (invalid_eclipse_permission) {
          tree.put("root.paired", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          tree.put("root.<xmlattr>.status_message", "Pairing request contains an unknown Eclipse permission");
          return;
        }

        const bool pin_stdin = config::sunshine.flags[config::flag::PIN_STDIN];
        if (!pin_stdin) {
          sess.async_insert_pin.response = response;
        }

        std::string pairing_id;
        switch (insert_pair_session(std::move(sess), pairing_id)) {
          case pair_session_insert_e::ALREADY_EXISTS:
            tree.put("root.paired", 0);
            tree.put("root.<xmlattr>.status_code", 409);
            tree.put("root.<xmlattr>.status_message", "A pairing session with this uniqueid already exists");
            return;
          case pair_session_insert_e::FULL:
            tree.put("root.paired", 0);
            tree.put("root.<xmlattr>.status_code", 503);
            tree.put("root.<xmlattr>.status_message", "Too many pending pairing sessions");
            return;
          case pair_session_insert_e::ADDED:
            break;
        }

        if (pin_stdin) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          std::scoped_lock lock(map_id_sess_mutex);
          expire_pair_sessions_unlocked(std::chrono::steady_clock::now());
          const auto sess_it = map_id_sess.find(uniqID);
          if (sess_it == map_id_sess.end() || sess_it->second.async_insert_pin.id != pairing_id) {
            tree.put("root.paired", 0);
            tree.put("root.<xmlattr>.status_code", 408);
            tree.put("root.<xmlattr>.status_message", "Pairing session expired");
            return;
          }

          getservercert(sess_it->second, tree, pin);
          if (sess_it->second.failed) {
            map_id_sess.erase(sess_it);
          }
          return;
        } else {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_require_pin();
#endif
          fg.disable();
          return;
        }
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    std::scoped_lock lock(map_id_sess_mutex);
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());
    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    bool pairing_complete = false;
    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, tree, pairingsecret);
      pairing_complete = true;
    } else {
      fail_pair(sess_it->second, tree, "Invalid pairing request");
    }

    if (pairing_complete || sess_it->second.failed) {
      map_id_sess.erase(sess_it);
    }
  }

  bool pin(const std::string_view pairing_id, std::string pin, std::string name) {
    if (!is_valid_pairing_id(pairing_id) || !is_valid_pairing_pin(pin) || !is_valid_pairing_name(name)) {
      return false;
    }

    std::scoped_lock lock(map_id_sess_mutex);
    expire_pair_sessions_unlocked(std::chrono::steady_clock::now());
    const auto sess_it = std::ranges::find_if(map_id_sess, [&](const auto &entry) {
      return entry.second.last_phase == PAIR_PHASE::NONE && entry.second.async_insert_pin.id == pairing_id;
    });
    if (sess_it == map_id_sess.end()) {
      return false;
    }

    auto &sess = sess_it->second;
    pt::ptree tree;
    getservercert(sess, tree, pin);
    if (!sess.failed) {
      sess.client.name = std::move(name);
    }

    const bool response_written = write_pairing_response(sess, tree);
    const bool success = response_written && !sess.failed;
    if (!response_written || sess.failed) {
      map_id_sess.erase(sess_it);
    }
    return success;
  }

  /**
   * @brief Get codec mode flags.
   *
   * @return Moonlight codec capability bitmask for the currently probed encoders.
   */
  uint32_t get_codec_mode_flags() {
    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (video::active_hevc_mode == 3 || video::active_hevc_mode == 5) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
    }
    if ((video::active_hevc_mode == 4 || video::active_hevc_mode == 5) && video::last_encoder_probe_supported_yuv444_for_codec[1]) {
      codec_mode_flags |= SCM_HEVC_REXT10_444;
    }

    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (video::active_av1_mode == 3 || video::active_av1_mode == 5) {
      codec_mode_flags |= SCM_AV1_MAIN10;
    }
    if ((video::active_av1_mode == 4 || video::active_av1_mode == 5) && video::last_encoder_probe_supported_yuv444_for_codec[2]) {
      codec_mode_flags |= SCM_AV1_HIGH10_444;
    }
    return codec_mode_flags;
  }

  /**
   * @brief Build the GameStream server-info response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
      tree.put("root.EclipseApiVersion", eclipse_api::API_VERSION);
      tree.put("root.EclipseCapabilities", "client-permissions,catalog-v2,session-ids,structured-errors");
      tree.put("root.EclipseApiPort", net::map_port(PORT_HTTPS));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    const uint32_t codec_mode_flags = get_codec_mode_flags();
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    if (!config::nvhttp.external_ip.empty()) {
      tree.put("root.ExternalIP", config::nvhttp.external_ip);
    }

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    std::lock_guard lock {client_auth_mutex};
    for (const auto &named_cert : client_root.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_node["enabled"] = named_cert.enabled;
      named_cert_node["platform"] = named_cert.platform;
      named_cert_node["expires_at"] = named_cert.permissions.expires_at;
      named_cert_node["scopes"] = named_cert.permissions.scopes;
      named_cert_node["allowed_apps"] = named_cert.permissions.allowed_apps;
      named_cert_node["input"] = {
        {"keyboard", named_cert.permissions.input.keyboard},
        {"mouse", named_cert.permissions.input.mouse},
        {"controller", named_cert.permissions.input.controller},
        {"touch", named_cert.permissions.input.touch},
        {"pen", named_cert.permissions.input.pen},
      };
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  /**
   * @brief Build the GameStream application list response.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    const auto client = verified_client(request);
    if (!client || !scope_allowed(*client, "catalog.read")) {
      apps.put("<xmlattr>.status_code", 403);
      apps.put("<xmlattr>.status_message", "Client certificate lacks catalog.read permission");
      return;
    }

    apps.put("<xmlattr>.status_code", 200);
    const bool include_eclipse_fields = request->parse_query_string().contains("eclipseApiVersion");

    const auto catalog = proc::catalog_snapshot();
    for (const auto &app_context : catalog.apps) {
      if (!app_allowed(*client, app_context.uuid)) {
        continue;
      }
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode >= 3 ? 1 : 0);
      app.put("AppTitle"s, app_context.name);
      app.put("ID", app_context.id);
      if (include_eclipse_fields) {
        app.put("IsAppCollectorGame", app_context.eclipse_metadata.value("kind", "unknown") == "game" ? 1 : 0);
      }

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  /**
   * @brief Launch the requested application for a GameStream session.
   *
   * @param host_audio Host audio.
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_device::revert_configuration();
      }
    });

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));
    const auto client = verified_client(request);
    const auto catalog = proc::catalog_snapshot();
    const auto app = std::ranges::find(catalog.apps, std::to_string(appid), &proc::ctx_t::id);
    if (!client || !scope_allowed(*client, "stream.launch") || (app != catalog.apps.end() && !app_allowed(*client, app->uuid)) || (app == catalog.apps.end() && !client->permissions.allowed_apps.empty())) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Client certificate lacks permission to launch this application");
      return;
    }

    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, args, *client);

    if (rtsp_stream::session_count() == 0) {
      // The display should be restored in case something fails as there are no other sessions.
      revert_display_configuration = true;

      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (appid > 0) {
      auto err = proc::proc.execute((int) appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    {
      std::lock_guard lock {logical_session_mutex};
      logical_session = logical_session_t {
        .id = launch_session->session_id,
        .client_uuid = client->uuid,
        .app_uuid = launch_session->app_uuid,
        .legacy_app_id = launch_session->appid,
        .started_at = std::chrono::system_clock::now(),
        .width = launch_session->width,
        .height = launch_session->height,
        .fps = launch_session->fps,
        .hdr = launch_session->enable_hdr,
      };
    }

    if (!rtsp_stream::launch_session_raise(launch_session)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "Another stream launch is already pending");
      if (appid > 0) {
        proc::proc.terminate();
      }
      std::lock_guard lock {logical_session_mutex};
      logical_session.reset();
      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);

    // Stream was started successfully, we will revert the config when the app or session terminates
    revert_display_configuration = false;
  }

  /**
   * @brief Resume an existing GameStream session.
   *
   * @param host_audio Host audio.
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    const auto client = verified_client(request);
    if (!client || !scope_allowed(*client, "stream.launch")) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Client certificate lacks stream.launch permission");
      return;
    }

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    std::string logical_session_id;
    std::string logical_app_uuid;
    std::string logical_client_uuid;
    {
      std::lock_guard lock {logical_session_mutex};
      if (logical_session && logical_session->client_uuid != client->uuid && !scope_allowed(*client, "host.control")) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 403);
        tree.put("root.<xmlattr>.status_message", "Only the owning client may resume this session");
        return;
      }
      if (logical_session) {
        logical_session_id = logical_session->id;
        logical_app_uuid = logical_session->app_uuid;
        logical_client_uuid = logical_session->client_uuid;
      }
    }
    if (!logical_app_uuid.empty() && !app_allowed(*client, logical_app_uuid)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Client certificate is not allowed to resume this application");
      return;
    }
    const auto launch_session = make_launch_session(host_audio, args, *client, std::move(logical_session_id));
    if (!logical_app_uuid.empty()) {
      launch_session->app_uuid = std::move(logical_app_uuid);
    }
    if (!logical_client_uuid.empty()) {
      launch_session->client_uuid = std::move(logical_client_uuid);
    }
    {
      std::lock_guard lock {logical_session_mutex};
      if (!logical_session) {
        logical_session = logical_session_t {
          .id = launch_session->session_id,
          .client_uuid = client->uuid,
          .app_uuid = launch_session->app_uuid,
          .legacy_app_id = current_appid,
          .started_at = std::chrono::system_clock::now(),
          .width = launch_session->width,
          .height = launch_session->height,
          .fps = launch_session->fps,
          .hdr = launch_session->enable_hdr,
        };
      }
    }

    if (no_active_sessions) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (!rtsp_stream::launch_session_raise(launch_session)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "Another stream launch is already pending");
      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);
  }

  /**
   * @brief Check whether cel.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    const auto client = verified_client(request);
    if (!client || !scope_allowed(*client, "host.control")) {
      tree.put("root.cancel", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Client certificate lacks host.control permission");
      return;
    }

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
    std::lock_guard lock {logical_session_mutex};
    logical_session.reset();
  }

  /**
   * @brief Return an application asset requested by the client.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    const auto app_id = static_cast<int>(util::from_view(get_arg(args, "appid")));
    const auto client = verified_client(request);
    const auto catalog = proc::catalog_snapshot();
    const auto app = std::ranges::find(catalog.apps, std::to_string(app_id), &proc::ctx_t::id);
    if (!client || !scope_allowed(*client, "catalog.read") || app == catalog.apps.end() || !app_allowed(*client, app->uuid)) {
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return;
    }
    auto app_image = proc::validate_app_image_path(app->image_path);

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  /**
   * @brief Send a versioned Eclipse JSON response.
   *
   * @param response HTTPS response to populate.
   * @param status HTTP status code.
   * @param body JSON response body.
   */
  void send_eclipse_response(const resp_https_t &response, const SimpleWeb::StatusCode status, nlohmann::json body) {
    body["schemaVersion"] = eclipse_api::API_VERSION;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"Cache-Control", "no-store"},
    };
    response->write(status, body.dump(), headers);
    response->close_connection_after_response = true;
  }

  /**
   * @brief Send a stable structured Eclipse API error.
   *
   * @param response HTTPS response to populate.
   * @param status HTTP status code.
   * @param code Stable machine-readable error code.
   * @param message Human-readable error description.
   */
  void send_eclipse_error(const resp_https_t &response, const SimpleWeb::StatusCode status, const std::string_view code, const std::string_view message) {
    send_eclipse_response(response, status, {
                                              {"error", {
                                                          {"code", code},
                                                          {"message", message},
                                                        }},
                                            });
  }

  /**
   * @brief Authenticate and authorize one Eclipse API request.
   *
   * @param response HTTPS response used for authorization errors.
   * @param request Paired-client HTTPS request.
   * @param scope Required scope, or empty when pairing alone is sufficient.
   * @return Authenticated client identity, or no value after sending an error.
   */
  std::optional<verified_client_t> authorize_eclipse_request(const resp_https_t &response, const req_https_t &request, const std::string_view scope = {}) {
    auto client = verified_client(request);
    if (!client) {
      send_eclipse_error(response, SimpleWeb::StatusCode::client_error_unauthorized, "authentication_required", "Reconnect using an enabled paired client certificate");
      return std::nullopt;
    }
    if (!scope.empty() && !scope_allowed(*client, scope)) {
      send_eclipse_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", std::format("Client certificate lacks {} permission", scope));
      return std::nullopt;
    }
    return client;
  }

  /**
   * @brief Convert configured application metadata to Eclipse Catalog V2 JSON.
   *
   * @param app Runtime application context.
   * @return Stable application resource.
   */
  nlohmann::json eclipse_app_json(const proc::ctx_t &app) {
    const auto &metadata = app.eclipse_metadata;
    const auto kind = metadata.value("kind", "unknown");
    static const std::set<std::string, std::less<>> kinds {"game", "desktop", "tool", "workspace", "unknown"};
    const auto normalized_kind = kinds.contains(kind) ? kind : "unknown";
    nlohmann::json classification = metadata.value("classification", nlohmann::json::object());
    if (!classification.is_object()) {
      classification = nlohmann::json::object();
    }
    classification["source"] = classification.value("source", metadata.contains("kind") ? "user" : "unknown");
    classification["confidence"] = classification.value("confidence", metadata.contains("kind") ? 1.0 : 0.0);

    return {
      {"uuid", app.uuid},
      {"legacyId", std::stoi(app.id)},
      {"name", app.name},
      {"kind", normalized_kind},
      {"classification", std::move(classification)},
      {"tags", metadata.value("tags", nlohmann::json::array())},
      {"description", metadata.value("description", "")},
      {"source", metadata.value("source", "unknown")},
      {"publisher", metadata.value("publisher", "")},
      {"installed", metadata.value("installed", true)},
      {"updateAvailable", metadata.value("updateAvailable", false)},
      {"hdr", metadata.value("hdr", video::active_hevc_mode >= 3)},
      {"inputRequirements", metadata.value("inputRequirements", nlohmann::json::array())},
      {"launchProfiles", metadata.value("launchProfiles", nlohmann::json::array())},
      {"assets", metadata.value("assets", nlohmann::json::object())},
      {"displayProfileId", metadata.value("displayProfileId", "")},
      {"streamProfileId", metadata.value("streamProfileId", "")},
      {"sandboxProfileId", metadata.value("sandboxProfileId", "")},
    };
  }

  /**
   * @brief Serialize an immutable stream-session snapshot.
   *
   * @param session Session snapshot.
   * @return Eclipse session resource.
   */
  nlohmann::json eclipse_session_json(const rtsp_stream::session_info_t &session) {
    const auto started_at = std::chrono::duration_cast<std::chrono::milliseconds>(session.started_at.time_since_epoch()).count();
    return {
      {"id", session.id},
      {"ownerClientUuid", session.client_uuid},
      {"appUuid", session.app_uuid},
      {"legacyAppId", session.legacy_app_id},
      {"state", session.state},
      {"startedAt", started_at},
      {"width", session.width},
      {"height", session.height},
      {"refreshRate", session.fps},
      {"hdr", session.hdr},
    };
  }

  /**
   * @brief Return active and resumable logical session snapshots.
   *
   * @return Immutable logical-session views, including disconnected resumable application state.
   */
  std::vector<rtsp_stream::session_info_t> eclipse_session_snapshots() {
    auto sessions = rtsp_stream::sessions();
    std::lock_guard lock {logical_session_mutex};
    if (!logical_session) {
      return sessions;
    }
    const bool active = std::ranges::any_of(sessions, [&](const auto &session) {
      return session.id == logical_session->id;
    });
    if (!active && proc::proc.running() > 0) {
      sessions.push_back({
        .id = logical_session->id,
        .client_uuid = logical_session->client_uuid,
        .app_uuid = logical_session->app_uuid,
        .legacy_app_id = logical_session->legacy_app_id,
        .state = "disconnected",
        .started_at = logical_session->started_at,
        .width = logical_session->width,
        .height = logical_session->height,
        .fps = logical_session->fps,
        .hdr = logical_session->hdr,
      });
    } else if (!active) {
      logical_session.reset();
    }
    return sessions;
  }

  /**
   * @brief Return Eclipse API capability and caller-policy metadata.
   */
  void eclipse_capabilities(resp_https_t response, req_https_t request) {
    const auto client = authorize_eclipse_request(response, request);
    if (!client) {
      return;
    }
    send_eclipse_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                         {"apiVersion", eclipse_api::API_VERSION},
                                                                         {"capabilities", eclipse_api::CAPABILITIES},
                                                                         {"client", {
                                                                                      {"uuid", client->uuid},
                                                                                      {"name", client->name},
                                                                                      {"scopes", client->permissions.scopes},
                                                                                      {"allowedApps", client->permissions.allowed_apps},
                                                                                      {"expiresAt", client->permissions.expires_at},
                                                                                    }},
                                                                       });
  }

  /**
   * @brief Return Eclipse Catalog V2 resources visible to the caller.
   */
  void eclipse_apps(resp_https_t response, req_https_t request) {
    const auto client = authorize_eclipse_request(response, request, "catalog.read");
    if (!client) {
      return;
    }
    const auto catalog = proc::catalog_snapshot();
    std::optional<std::uint64_t> since;
    const auto args = request->parse_query_string();
    if (const auto value = args.find("since"); value != args.end()) {
      try {
        std::size_t consumed {};
        since = std::stoull(value->second, &consumed);
        if (consumed != value->second.size()) {
          throw std::invalid_argument("trailing characters");
        }
      } catch (const std::exception &) {
        send_eclipse_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned catalog revision");
        return;
      }
    }

    const bool changed = !since || *since != catalog.revision;
    nlohmann::json apps = nlohmann::json::array();
    if (changed) {
      for (const auto &app : catalog.apps) {
        if (app_allowed(*client, app.uuid)) {
          apps.emplace_back(eclipse_app_json(app));
        }
      }
    }
    send_eclipse_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                         {"revision", catalog.revision},
                                                                         {"changed", changed},
                                                                         {"fullSnapshot", changed},
                                                                         {"apps", std::move(apps)},
                                                                       });
  }

  /**
   * @brief Return active Eclipse session resources visible to the caller.
   */
  void eclipse_sessions(resp_https_t response, req_https_t request) {
    const auto client = authorize_eclipse_request(response, request, "session.control");
    if (!client) {
      return;
    }
    const bool administer = scope_allowed(*client, "host.control");
    nlohmann::json sessions = nlohmann::json::array();
    for (const auto &session : eclipse_session_snapshots()) {
      if (administer || session.client_uuid == client->uuid) {
        sessions.emplace_back(eclipse_session_json(session));
      }
    }
    send_eclipse_response(response, SimpleWeb::StatusCode::success_ok, {{"sessions", std::move(sessions)}});
  }

  /**
   * @brief Return one active Eclipse session resource.
   */
  void eclipse_session(resp_https_t response, req_https_t request) {
    const auto client = authorize_eclipse_request(response, request, "session.control");
    if (!client) {
      return;
    }
    const auto session_id = request->path_match[1].str();
    const bool administer = scope_allowed(*client, "host.control");
    for (const auto &session : eclipse_session_snapshots()) {
      if (session.id == session_id && (administer || session.client_uuid == client->uuid)) {
        send_eclipse_response(response, SimpleWeb::StatusCode::success_ok, {{"session", eclipse_session_json(session)}});
        return;
      }
    }
    send_eclipse_error(response, SimpleWeb::StatusCode::client_error_not_found, "session_not_found", "Session does not exist or is not visible to this client");
  }

  /**
   * @brief Disconnect one logical session without stopping its host application.
   */
  void eclipse_disconnect_session(resp_https_t response, req_https_t request) {
    const auto client = authorize_eclipse_request(response, request, "session.control");
    if (!client) {
      return;
    }
    const auto session_id = request->path_match[1].str();
    const auto owner = scope_allowed(*client, "host.control") ? std::string_view {} : std::string_view {client->uuid};
    const auto sessions = eclipse_session_snapshots();
    const bool visible = std::ranges::any_of(sessions, [&](const auto &session) {
      return session.id == session_id && (owner.empty() || session.client_uuid == owner);
    });
    if (!visible) {
      send_eclipse_error(response, SimpleWeb::StatusCode::client_error_not_found, "session_not_found", "Session does not exist or is not owned by this client");
      return;
    }
    static_cast<void>(rtsp_stream::terminate_session(session_id, owner));
    BOOST_LOG(info) << "Audit: client ["sv << client->uuid << "] disconnected session ["sv << session_id << ']';
    send_eclipse_response(response, SimpleWeb::StatusCode::success_ok, {{"disconnected", true}, {"sessionId", session_id}});
  }

  /**
   * @brief Stop one logical session and its host application.
   */
  void eclipse_stop_session(resp_https_t response, req_https_t request) {
    const auto client = authorize_eclipse_request(response, request, "session.control");
    if (!client) {
      return;
    }
    const auto session_id = request->path_match[1].str();
    const auto owner = scope_allowed(*client, "host.control") ? std::string_view {} : std::string_view {client->uuid};
    const auto sessions = eclipse_session_snapshots();
    const bool visible = std::ranges::any_of(sessions, [&](const auto &session) {
      return session.id == session_id && (owner.empty() || session.client_uuid == owner);
    });
    if (!visible) {
      send_eclipse_error(response, SimpleWeb::StatusCode::client_error_not_found, "session_not_found", "Session does not exist or is not owned by this client");
      return;
    }
    static_cast<void>(rtsp_stream::terminate_session(session_id, owner));
    {
      std::lock_guard lock {logical_session_mutex};
      if (logical_session && logical_session->id == session_id && (owner.empty() || logical_session->client_uuid == owner)) {
        if (proc::proc.running() > 0) {
          proc::proc.terminate();
        }
        logical_session.reset();
      }
    }
    display_device::revert_configuration();
    BOOST_LOG(info) << "Audit: client ["sv << client->uuid << "] stopped session ["sv << session_id << ']';
    send_eclipse_response(response, SimpleWeb::StatusCode::success_ok, {{"stopped", true}, {"sessionId", session_id}});
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  /**
   * @brief Check whether a paired client certificate is allowed to connect and retrieve its friendly name.
   *
   * @param cert_pem PEM-encoded client certificate to look up.
   * @return Pair of (bool enabled, string name) where "enabled" is True when the client certificate belongs to an
             enabled device and "name" is the friendly client name set during pairing.
   */
  std::pair<bool, std::string> get_client_status(const std::string_view cert_pem);

  void start() {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [](SSL *ssl, const boost::asio::ip::tcp::endpoint &endpoint) {
      crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };
      if (!x509) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return 0;
      }

      int verified = 0;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      std::lock_guard lock {client_auth_mutex};
      auto err_str = verify_client_certificate(x509.get());
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;

        return verified;
      }

      auto pem = crypto::pem(x509);
      const auto client = std::ranges::find(client_root.named_devices, pem, &named_cert_t::cert);
      if (client == client_root.named_devices.end() || !client->enabled || permissions_expired(client->permissions)) {
        BOOST_LOG(info) << "Client is disabled -- denied"sv;
        return verified;
      }

      const auto key = endpoint_key(endpoint);
      if (!verified_clients.contains(key) && verified_clients.size() >= MAX_VERIFIED_CLIENT_CONNECTIONS) {
        verified_clients.erase(verified_clients.begin());
      }
      verified_clients[key] = {
        .uuid = client->uuid,
        .name = client->name,
        .cert = pem,
        .permissions = client->permissions,
      };
      verified = 1;

      return verified;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SunshineHTTPS>;
    https_server.resource["^/pair$"]["GET"] = [](auto resp, auto req) {
      pair<SunshineHTTPS>(resp, req);
    };
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;
    https_server.resource["^/eclipse/v1/capabilities$"]["GET"] = eclipse_capabilities;
    https_server.resource["^/eclipse/v1/apps$"]["GET"] = eclipse_apps;
    https_server.resource["^/eclipse/v1/sessions$"]["GET"] = eclipse_sessions;
    https_server.resource["^/eclipse/v1/sessions/([0-9a-fA-F-]+)$"]["GET"] = eclipse_session;
    https_server.resource["^/eclipse/v1/sessions/([0-9a-fA-F-]+)/disconnect$"]["POST"] = eclipse_disconnect_session;
    https_server.resource["^/eclipse/v1/sessions/([0-9a-fA-F-]+)/stop$"]["POST"] = eclipse_stop_session;

    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(resp, req);
    };

    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(http_server->config.port);
        platf::set_thread_name(name);
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::jthread ssl {accept_and_run, &https_server};
    std::jthread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  void erase_all_clients() {
    bool erased = false;
    {
      std::lock_guard lock {client_auth_mutex};
      auto previous_clients = client_root;
      client_root = {};
      cert_chain.clear();
      verified_clients.clear();
      if (!save_state()) {
        client_root = std::move(previous_clients);
        rebuild_client_cert_chain();
      } else {
        erased = true;
      }
    }
    if (erased) {
      rtsp_stream::terminate_sessions();
    }
  }

  bool unpair_client(const std::string_view uuid) {
    std::string certificate;
    bool removed = false;
    {
      std::lock_guard lock {client_auth_mutex};
      const auto previous_clients = client_root;
      for (auto it = client_root.named_devices.begin(); it != client_root.named_devices.end();) {
        if ((*it).uuid == uuid) {
          certificate = it->cert;
          it = client_root.named_devices.erase(it);
          removed = true;
        } else {
          ++it;
        }
      }

      rebuild_client_cert_chain();
      std::erase_if(verified_clients, [&](const auto &entry) {
        return entry.second.uuid == uuid;
      });
      if (!save_state()) {
        client_root = previous_clients;
        rebuild_client_cert_chain();
        removed = false;
      }
    }
    if (removed) {
      rtsp_stream::terminate_sessions_by_cert(certificate);
    }
    return removed;
  }

  bool set_client_enabled(const std::string_view uuid, bool enabled) {
    client_update_t update;
    update.enabled = enabled;
    return update_client(uuid, std::move(update));
  }

  bool update_client(const std::string_view uuid, client_update_t update) {
    if (update.permissions && (update.permissions->expires_at < 0 || std::ranges::any_of(update.permissions->scopes, [](const auto &scope) {
                                 return !eclipse_api::is_known_scope(scope);
                               }) ||
                               std::ranges::any_of(update.permissions->allowed_apps, [](const auto &app_uuid) {
                                 return !uuid_util::is_valid(app_uuid);
                               }))) {
      return false;
    }

    if (update.certificate) {
      *update.certificate = canonical_certificate_pem(*update.certificate);
      if (update.certificate->empty()) {
        return false;
      }
    }

    std::string previous_certificate;
    bool terminate_sessions = false;
    {
      std::lock_guard lock {client_auth_mutex};
      const auto client = std::ranges::find(client_root.named_devices, uuid, &named_cert_t::uuid);
      if (client == client_root.named_devices.end()) {
        return false;
      }
      if (update.certificate && std::ranges::any_of(client_root.named_devices, [&](const auto &named_cert) {
            return named_cert.uuid != uuid && named_cert.cert == *update.certificate;
          })) {
        return false;
      }

      previous_certificate = client->cert;
      const auto previous_client = *client;
      if (update.enabled) {
        client->enabled = *update.enabled;
        terminate_sessions = !*update.enabled;
      }
      if (update.permissions) {
        client->permissions = std::move(*update.permissions);
        terminate_sessions = true;
        BOOST_LOG(info) << "Audit: changed permissions for client ["sv << uuid << ']';
      }
      if (update.certificate && client->cert != *update.certificate) {
        client->cert = std::move(*update.certificate);
        terminate_sessions = true;
        BOOST_LOG(info) << "Audit: rotated certificate for client ["sv << uuid << ']';
      }

      rebuild_client_cert_chain();
      std::erase_if(verified_clients, [&](const auto &entry) {
        return entry.second.uuid == uuid;
      });
      if (!save_state()) {
        *client = previous_client;
        rebuild_client_cert_chain();
        return false;
      }
    }
    if (terminate_sessions) {
      rtsp_stream::terminate_sessions_by_cert(previous_certificate);
    }
    return true;
  }

  std::optional<eclipse_api::client_permissions_t> get_client_permissions(const std::string_view uuid) {
    std::lock_guard lock {client_auth_mutex};
    const auto client = std::ranges::find(client_root.named_devices, uuid, &named_cert_t::uuid);
    if (client == client_root.named_devices.end()) {
      return std::nullopt;
    }
    return client->permissions;
  }

  bool set_client_permissions(const std::string_view uuid, eclipse_api::client_permissions_t permissions) {
    client_update_t update;
    update.permissions = std::move(permissions);
    return update_client(uuid, std::move(update));
  }

  bool rotate_client_certificate(const std::string_view uuid, std::string cert) {
    client_update_t update;
    update.certificate = std::move(cert);
    return update_client(uuid, std::move(update));
  }

  /**
   * @brief Get cert by UUID.
   */
  std::string get_cert_by_uuid(const std::string_view uuid) {
    std::lock_guard lock {client_auth_mutex};
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == uuid) {
        return named_cert.cert;
      }
    }
    return {};
  }

  /**
   * @brief Check whether a paired client certificate is allowed to connect and return its friendly name.
   */
  std::pair<bool, std::string> get_client_status(const std::string_view cert_pem) {
    const client_t &client = client_root;
    for (const auto &named_cert : client.named_devices) {
      if (named_cert.cert == cert_pem) {
        return {named_cert.enabled && !permissions_expired(named_cert.permissions), named_cert.name};
      }
    }
    return {true, {}};
  }

#ifdef SUNSHINE_TESTS
  namespace test_support {
    void reset_client_state() {
      std::lock_guard lock {client_auth_mutex};
      client_root = {};
      cert_chain.clear();
      verified_clients.clear();
    }

    std::string add_client(const std::string &name, std::string cert, bool enabled) {
      auto uuid = add_authorized_client(name, std::move(cert));
      if (!uuid.empty() && !enabled) {
        set_client_enabled(uuid, false);
      }
      return uuid;
    }

    std::optional<eclipse_api::client_permissions_t> client_permissions(const std::string_view uuid) {
      return get_client_permissions(uuid);
    }

    bool authorize_client_certificate(const std::string_view cert) {
      auto certificate = crypto::x509(cert);
      if (!certificate) {
        return false;
      }

      std::lock_guard lock {client_auth_mutex};
      return verify_client_certificate(certificate.get()) == nullptr;
    }

    void reload_client_state() {
      load_state();
    }
  }  // namespace test_support
#endif
}  // namespace nvhttp
