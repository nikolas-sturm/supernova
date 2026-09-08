/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <utility>

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <openssl/evp.h>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "terra_assets.h"
#include "terra_events.h"
#include "terra_operations.h"
#include "terra_peripherals.h"
#include "terra_profiles.h"
#include "terra_sandboxes.h"
#include "terra_virtual_display.h"
#include "terra_workspaces.h"
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

#ifdef _WIN32
  #include "platform/windows/terra_display.h"
  #include "platform/windows/terra_sandbox_provider.h"
  #include "platform/windows/terra_virtual_display_provider.h"
#endif

using namespace std::literals;

namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sol. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;  ///< Enabled paired-client certificates accepted by Sol's GameStream HTTPS server.
  std::mutex client_auth_mutex;  ///< Serializes paired-client state and certificate authorization changes.

  /**
   * @brief HTTPS server backend that adds Sol's client-certificate verification.
   */
  class SolHTTPSServer: public SimpleWeb::ServerBase<SolHTTPS> {
  public:
    /**
     * @brief Initialize the HTTPS server with Sol's certificate and key files.
     *
     * @param certification_file Path to the server certificate file.
     * @param private_key_file Path to the matching private key file.
     */
    SolHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SolHTTPS>::ServerBase(443),
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
    boost::asio::ssl::context context;  ///< TLS server context configured with Sol's certificate and protocol policy.

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
  using https_server_t = SolHTTPSServer;
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
  } conf_intern;  ///< TLS credential paths loaded from Sol's runtime configuration.

  /**
   * @brief Certificate entry associated with a client name and UUID.
   */
  struct named_cert_t {
    std::string name;  ///< Human-readable name for this item.
    std::string uuid;  ///< Persistent Moonlight client UUID associated with the certificate.
    std::string cert;  ///< Certificate PEM string or path.
    std::string platform;  ///< Platform reported by the paired Terra client.
    terra_api::client_permissions_t permissions = terra_api::legacy_client_permissions();  ///< Certificate-bound permissions.
    bool enabled = true;  ///< Whether this persisted client entry may connect.
  };

  /**
   * @brief Immutable identity captured from one verified TLS connection.
   */
  struct verified_client_t {
    std::string uuid;  ///< Persistent paired-client UUID.
    std::string name;  ///< Paired-client friendly name.
    std::string cert;  ///< Canonical certificate PEM.
    terra_api::client_permissions_t permissions;  ///< Permission snapshot used by requests on this connection.
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
   * @brief Resumable logical session associated with Sol's single running application.
   */
  struct logical_session_t {
    std::string id;  ///< Stable Terra session UUID.
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
   * @brief Resolved profile and runtime associations for one logical session.
   */
  struct terra_session_binding_t {
    std::string workspace_id;  ///< Associated workspace UUID, empty when none.
    std::string sandbox_id;  ///< Associated sandbox UUID, empty when none.
    std::string display_profile_id;  ///< Resolved display-profile UUID, empty when none.
    std::string stream_profile_id;  ///< Resolved stream-profile UUID, empty when none.
    std::string launch_profile_id;  ///< Resolved launch-profile UUID, empty when none.
    std::string sandbox_profile_id;  ///< Resolved sandbox-profile UUID, empty when none.
  };

  /**
   * @brief Published lifecycle tracking state for one logical session.
   */
  struct terra_session_tracking_t {
    terra_session_binding_t binding;  ///< Resolved profile and runtime associations.
    std::string owner_client_uuid;  ///< Owning client UUID, empty when unknown.
    std::string app_uuid;  ///< Associated application UUID, empty when unknown.
    std::string state;  ///< Last observed session state.
    std::uint64_t revision = 1;  ///< Monotonic session-resource revision.
    std::int64_t updated_at = 0;  ///< Last published transition in Unix milliseconds.
    std::optional<std::int64_t> terminal_since;  ///< Retention start for terminal sessions.
    bool announced = false;  ///< Whether session.created has been published.
  };

  std::mutex terra_session_tracking_mutex;  ///< Protects published session tracking state.
  std::map<std::string, terra_session_tracking_t> terra_session_tracking;  ///< Published session resources keyed by session UUID.
  std::uint64_t terra_session_collection_revision = 0;  ///< Monotonic sessions-collection revision.
  std::chrono::steady_clock::time_point terra_start_time = std::chrono::steady_clock::now();  ///< Host uptime reference for telemetry.
  std::unique_ptr<terra_events::hub_t> terra_event_hub;  ///< Per-client SSE replay and delivery hub.
  std::mutex terra_event_stream_mutex;  ///< Protects owned SSE worker threads.
  std::vector<std::future<void>> terra_event_streams;  ///< Owned long-lived SSE response workers.
  std::unique_ptr<terra_operations::store_t> terra_operation_store;  ///< Persistent Terra operation and idempotency records.
  thread_pool_util::ThreadPool terra_operation_pool;  ///< Dedicated worker pool for durable Terra operations, isolated from the shared task pool.
  std::unique_ptr<terra::profiles::manager_t> terra_profile_manager;  ///< Persistent profiles-v1 resource manager.
  std::unique_ptr<terra_peripherals::manager_t> terra_peripheral_manager;  ///< Peripheral device registry and claim manager.
  std::uint64_t terra_peripheral_collection_revision = 0;  ///< Monotonic peripherals collection revision.
  std::mutex terra_peripheral_revision_mutex;  ///< Protects the peripherals collection revision.
  std::unique_ptr<terra_workspaces::manager_t> terra_workspace_manager;  ///< Persistent workspaces-v1 lifecycle manager.
#ifdef _WIN32
  std::unique_ptr<terra_virtual_display::manager_t> terra_virtual_display_manager;  ///< MttVDD virtual-display lifecycle manager.
  std::unique_ptr<terra_sandboxes::manager_t> terra_sandbox_manager;  ///< Persistent sandboxes-v1 lifecycle manager.
  void revoke_terra_profiles(const std::string &owner, const std::optional<terra_api::client_permissions_t> &permissions = std::nullopt);
  void revoke_terra_sandboxes(const std::string &owner);
  void revoke_terra_workspaces(const std::string &owner);
  bool terra_workspace_visible(const verified_client_t &client, const terra_workspaces::resource_t &workspace, bool lifecycle);
#endif

#ifdef _WIN32
  /**
   * @brief Process-lifetime revision state for physical display snapshots.
   */
  struct physical_display_revision_t {
    std::mutex mutex;  ///< Protects fingerprint and revision.
    std::string fingerprint;  ///< Canonical JSON fingerprint of last snapshot.
    std::uint64_t revision = 0;  ///< Monotonic collection revision.
  } physical_display_revision;  ///< Shared physical display revision state.

  /**
   * @brief Process-lifetime revision state for the unified display inventory.
   */
  struct terra_unified_display_revision_t {
    std::mutex mutex;  ///< Protects combined revision inputs.
    std::pair<std::uint64_t, std::uint64_t> inputs {};  ///< Last observed physical and virtual revisions.
    std::uint64_t revision = 0;  ///< Monotonic unified collection revision.
  } terra_unified_display_revision;  ///< Shared unified display revision state.

  /**
   * @brief Process-lifetime state for the display topology resource.
   */
  struct terra_topology_state_t {
    std::mutex mutex;  ///< Protects fingerprint and revision.
    std::string fingerprint;  ///< Canonical JSON fingerprint of last topology snapshot.
    std::uint64_t revision = 0;  ///< Monotonic topology revision.
  } terra_topology_state;  ///< Shared display topology revision state.
#endif

  /**
   * @brief Case-insensitive map used for HTTP headers and query parameters.
   */
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  /**
   * @brief Shared HTTPS response object passed to GameStream handlers.
   */
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SolHTTPS>::Response>;
  /**
   * @brief Shared HTTPS request object received by GameStream handlers.
   */
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SolHTTPS>::Request>;

  std::optional<std::string> terra_header(const req_https_t &request, std::string_view name);
  nlohmann::json terra_session_json(const rtsp_stream::session_info_t &session, const terra_session_tracking_t *tracking = nullptr);
  std::optional<terra_session_binding_t> terra_resolve_launch_references(const verified_client_t &client, const proc::ctx_t &app, const args_t &args, const terra_workspaces::resource_t *workspace, std::string_view *error_code, std::string &error_message);
  bool terra_launch_direct_sandbox(const verified_client_t &client, const proc::ctx_t &app, terra_session_binding_t &binding, std::string &error_message);
  void terra_destroy_sandbox_by_id(const std::string &sandbox_id);
  void terra_destroy_session_sandbox(const terra_session_binding_t &binding);
  void terra_unregister_session(const std::string &session_id);
  bool terra_json_contains_string(const nlohmann::json &values, const std::string &value);
  void publish_terra_sandbox_event(const std::string &type, const terra_sandboxes::resource_t &sandbox, nlohmann::json data, const std::string &owner);
  void publish_terra_peripheral_event(const std::string &type, nlohmann::json data);
  void terra_bump_peripheral_revision();
  std::vector<rtsp_stream::session_info_t> terra_session_snapshots();
#ifdef _WIN32
  std::optional<std::pair<std::uint64_t, nlohmann::json>> terra_display_snapshot();
#endif
  nlohmann::json terra_telemetry_session_json(const rtsp_stream::session_info_t &session, const terra_session_tracking_t *tracking);
  nlohmann::json terra_telemetry_document(const std::string &owner_filter = {});
  void terra_patch_virtual_display(resp_https_t response, req_https_t request);
  std::optional<std::uint64_t> terra_if_match(const req_https_t &request);
  std::optional<nlohmann::json> terra_request_json(const resp_https_t &response, const req_https_t &request);
  std::optional<std::pair<std::uint64_t, nlohmann::json>> terra_unified_displays();
  nlohmann::json terra_topology_document(nlohmann::json displays);
  namespace asio = boost::asio;
  void terra_close_peripheral_channel(const std::string &claim_id);
  void terra_peripheral_channel_upgrade(std::unique_ptr<SolHTTPS> &socket, std::shared_ptr<SimpleWeb::ServerBase<SolHTTPS>::Request> request);
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

  bool permissions_expired(const terra_api::client_permissions_t &permissions);

  /**
   * @brief Return authenticated identity captured for an HTTPS request.
   *
   * @param request Paired-client HTTPS request.
   * @return Immutable identity snapshot, or no value after revocation or expiry.
   */
  std::optional<verified_client_t> verified_client(const req_https_t &request) {
    std::optional<verified_client_t> result;
    std::string expired_owner;
    {
      std::lock_guard lock {client_auth_mutex};
      const auto client = verified_clients.find(endpoint_key(request->remote_endpoint()));
      if (client == verified_clients.end()) {
        return std::nullopt;
      }
      if (permissions_expired(client->second.permissions)) {
        expired_owner = client->second.uuid;
        verified_clients.erase(client);
      } else {
        result = client->second;
      }
    }
    if (!expired_owner.empty() && terra_event_hub) {
      terra_event_hub->disconnect_client(expired_owner);
    }
#ifdef _WIN32
    if (!expired_owner.empty() && terra_virtual_display_manager) {
      static_cast<void>(terra_virtual_display_manager->revoke_owner(expired_owner));
    }
    if (!expired_owner.empty()) {
      revoke_terra_sandboxes(expired_owner);
      revoke_terra_profiles(expired_owner);
      revoke_terra_workspaces(expired_owner);
    }
#endif
    return result;
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
   * @brief Convert a PEM certificate to Sol's canonical OpenSSL serialization.
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
  bool permissions_expired(const terra_api::client_permissions_t &permissions) {
    if (permissions.expires_at == 0) {
      return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return now >= permissions.expires_at;
  }

  /**
   * @brief Select paired clients authorized to observe one event.
   *
   * @param scope Required domain read scope, or empty for pairing-wide events.
   * @param owner_uuid Optional resource owner restricting visibility.
   * @param app_uuids Application associations that must all pass caller allowlist.
   * @return Enabled, unexpired recipient UUIDs without duplicates.
   */
  std::vector<std::string> terra_event_recipients(const std::string_view scope, const std::string_view owner_uuid = {}, const std::vector<std::string> &app_uuids = {}) {
    std::vector<std::string> recipients;
    std::lock_guard lock {client_auth_mutex};
    for (const auto &client : client_root.named_devices) {
      if (!client.enabled || permissions_expired(client.permissions) || (!scope.empty() && !client.permissions.scopes.contains(scope))) {
        continue;
      }
      if (!owner_uuid.empty() && client.uuid != owner_uuid && !client.permissions.scopes.contains("host.control")) {
        continue;
      }
      if (std::ranges::any_of(app_uuids, [&](const auto &app_uuid) {
            return !client.permissions.allowed_apps.empty() && !client.permissions.allowed_apps.contains(app_uuid);
          })) {
        continue;
      }
      recipients.push_back(client.uuid);
    }
    return recipients;
  }

  /**
   * @brief Publish one event after applying current client policy projection.
   *
   * @param event Event payload.
   * @param scope Required event scope.
   * @param owner_uuid Optional owner visibility restriction.
   * @param app_uuids Associated application UUIDs.
   */
  void publish_terra_event(terra_events::event_t event, const std::string_view scope = {}, const std::string_view owner_uuid = {}, const std::vector<std::string> &app_uuids = {}) {
    if (!terra_event_hub) {
      return;
    }
    try {
      terra_event_hub->publish(std::move(event), terra_event_recipients(scope, owner_uuid, app_uuids));
    } catch (const std::exception &exception) {
      BOOST_LOG(error) << "Terra event publication failed: " << exception.what();
    }
  }

  /**
   * @brief Resolve domain scope required to observe an asynchronous operation.
   *
   * @param action Stable operation action.
   * @return Required scope, or empty when action is not recognized.
   */
  std::string_view terra_operation_scope(const std::string_view action) {
    if (action.starts_with("virtual-display.")) {
      return "virtual-display.manage";
    }
    if (action.starts_with("workspace.")) {
      if (action.ends_with(".start")) {
        return "stream.launch";
      }
      if (action.ends_with(".stop")) {
        return "session.control";
      }
      return "host.control";
    }
    if (action.starts_with("sandbox.")) {
      return "sandbox.manage";
    }
    if (action.starts_with("peripheral.")) {
      return "peripheral.forward";
    }
    if (action.starts_with("display.")) {
      return "display.manage";
    }
    if (action.starts_with("profile.display.")) {
      return "display.manage";
    }
    if (action.starts_with("profile.sandbox.")) {
      return "sandbox.manage";
    }
    if (action.starts_with("profile.stream.") || action.starts_with("profile.launch.")) {
      return "host.control";
    }
    return {};
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
        // Older releases generated uppercase UUID text; canonicalize so Terra
        // resource ownership keeps working with previously paired clients.
        std::ranges::transform(named_cert.uuid, named_cert.uuid.begin(), [](const unsigned char character) {
          return static_cast<char>(std::tolower(character));
        });
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
              if (terra_api::is_known_scope(value)) {
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
    terra_api::client_permissions_t permissions = terra_api::legacy_client_permissions()
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

    if (!config::sol.flags[config::flag::FRESH_STATE]) {
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
      response.left()->write(data.str());
      response.left()->close_connection_after_response = true;
    } else if (response.has_right() && response.right()) {
      response.right()->write(data.str());
      response.right()->close_connection_after_response = true;
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
      if (sess->client.requested_terra_permissions) {
        requested_scopes.assign(sess->client.requested_permissions.scopes.begin(), sess->client.requested_permissions.scopes.end());
      }
      std::vector<std::string> requested_inputs;
      if (sess->client.requested_terra_permissions) {
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
      auto permissions = client.requested_terra_permissions ? std::move(client.requested_permissions) : terra_api::legacy_client_permissions();
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
  struct tunnel<SolHTTPS> {
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
        const auto scope_argument = args.find("eclipseScopes");
        const auto input_argument = args.find("eclipseInput");
        const auto pairing_policy = terra_api::parse_pairing_policy(
          scope_argument == args.end() ? std::nullopt : std::optional<std::string_view> {scope_argument->second},
          input_argument == args.end() ? std::nullopt : std::optional<std::string_view> {input_argument->second}
        );
        if (!pairing_policy.valid) {
          tree.put("root.paired", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          tree.put("root.<xmlattr>.status_message", "Pairing request contains incomplete or unknown Terra permissions");
          return;
        }
        sess.client.requested_terra_permissions = pairing_policy.explicit_policy;
        sess.client.requested_permissions = pairing_policy.permissions;

        const bool pin_stdin = config::sol.flags[config::flag::PIN_STDIN];
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
#if defined SOL_TRAY && SOL_TRAY >= 1
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
    if constexpr (std::is_same_v<SolHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sol_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SolHTTPS, T>) {
      const auto mac_address = platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address()));
      if (terra_api::wake_on_lan_available(mac_address)) {
        tree.put("root.mac", mac_address);
      }
      tree.put("root.EclipseApiVersion", terra_api::API_VERSION);
      tree.put("root.EclipseCapabilities", terra_api::capabilities_csv());
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
    print_req<SolHTTPS>(request);

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
    const bool include_terra_fields = request->parse_query_string().contains("eclipseApiVersion");

    const auto catalog = proc::catalog_snapshot();
    for (const auto &app_context : catalog.apps) {
      if (!app_allowed(*client, app_context.uuid)) {
        continue;
      }
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode >= 3 ? 1 : 0);
      app.put("AppTitle"s, app_context.name);
      app.put("ID", app_context.id);
      if (include_terra_fields) {
        app.put("IsAppCollectorGame", app_context.terra_metadata.value("kind", "unknown") == "game" ? 1 : 0);
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
    print_req<SolHTTPS>(request);

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
    const bool terra_v1 = terra_api::api_v1_requested(get_arg(args, "eclipseApiVersion", ""));
    const auto client = verified_client(request);
    if (!client || !scope_allowed(*client, "stream.launch")) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Client certificate lacks stream.launch permission");
      return;
    }
    const bool has_legacy_app_id = args.find("appid"s) != std::end(args);
    const bool has_terra_app_id = terra_v1 && args.find("eclipseAppUuid"s) != std::end(args);
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      (!has_legacy_app_id && !has_terra_app_id)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    const auto catalog = proc::catalog_snapshot();
    auto app = catalog.apps.end();
    auto appid = has_legacy_app_id ? util::from_view(get_arg(args, "appid")) : 0;
    if (has_terra_app_id) {
      const auto app_uuid = get_arg(args, "eclipseAppUuid");
      if (!uuid_util::is_valid(app_uuid)) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "eclipseAppUuid must be a canonical UUID");
        return;
      }
      app = std::ranges::find(catalog.apps, app_uuid, &proc::ctx_t::uuid);
      if (app == catalog.apps.end() || (has_legacy_app_id && app->id != std::to_string(appid))) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "appid and eclipseAppUuid must identify one catalog application");
        return;
      }
      appid = std::stoi(app->id);
      if (!has_legacy_app_id) {
        args.emplace("appid", app->id);
      }
    } else {
      app = std::ranges::find(catalog.apps, std::to_string(appid), &proc::ctx_t::id);
    }

    if ((app != catalog.apps.end() && !app_allowed(*client, app->uuid)) || (app == catalog.apps.end() && !client->permissions.allowed_apps.empty())) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Client certificate lacks permission to launch this application");
      return;
    }

    constexpr std::array terra_resource_arguments {
      "eclipseDisplayProfileId",
      "eclipseStreamProfileId",
      "eclipseLaunchProfileId",
      "eclipseSandboxProfileId",
    };
    if (terra_v1 && std::ranges::any_of(terra_resource_arguments, [&](const auto argument) {
          return !get_arg(args, argument, "").empty();
        })) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "Requested Terra resource provider is unavailable");
      return;
    }

#ifdef _WIN32
    std::optional<terra_workspaces::resource_t> terra_workspace;
    if (terra_v1) {
      const auto workspace_id = get_arg(args, "eclipseWorkspaceId", "");
      if (!workspace_id.empty()) {
        terra_workspace = terra_workspace_manager ? terra_workspace_manager->get(workspace_id) : std::nullopt;
        const bool app_permitted = terra_workspace && app != catalog.apps.end() && (app->uuid == terra_workspace->definition.desktop_app_uuid || std::ranges::contains(terra_workspace->definition.permitted_app_uuids, app->uuid));
        if (!uuid_util::is_valid(workspace_id) || !terra_workspace || !terra_workspace_visible(*client, *terra_workspace, true) || terra_workspace->state != terra_workspaces::state_t::ready || !app_permitted) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 404);
          tree.put("root.<xmlattr>.status_message", "Workspace does not exist, is not ready, or does not permit this application");
          return;
        }
      }
    }
#endif

    bool application_prelaunched = false;
#ifdef _WIN32
    application_prelaunched = terra_workspace && terra_workspace->sandbox_id.has_value();
#endif
    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, args, *client);

    terra_session_binding_t binding;
    if (terra_v1) {
      std::string_view reference_error_code = "invalid_argument";
      std::string reference_error_message;
      const auto resolved = terra_resolve_launch_references(*client, *app, args, terra_workspace ? &*terra_workspace : nullptr, &reference_error_code, reference_error_message);
      if (!resolved) {
        tree.put("root.gamesession", 0);
        tree.put("root.<xmlattr>.status_code", reference_error_code == "invalid_argument" ? 400 : reference_error_code == "resource_not_found" ? 404 :
                                                                                                reference_error_code == "permission_denied"    ? 403 :
                                                                                                                                                 422);
        tree.put("root.<xmlattr>.status_message", reference_error_message);
        return;
      }
      binding = *resolved;
      std::string sandbox_error;
      if (!terra_workspace && !terra_launch_direct_sandbox(*client, *app, binding, sandbox_error)) {
        tree.put("root.gamesession", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", sandbox_error);
        return;
      }
      if (terra_workspace) {
        binding.sandbox_id = terra_workspace->sandbox_id ? *terra_workspace->sandbox_id : std::string {};
      }
    }

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
        if (terra_v1) {
          terra_destroy_session_sandbox(binding);
        }

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);
      if (terra_v1) {
        terra_destroy_session_sandbox(binding);
      }

      return;
    }

    bool application_launched = false;
    if (appid > 0 && !application_prelaunched && binding.sandbox_id.empty()) {
      auto err = proc::proc.execute((int) appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);
        if (terra_v1) {
          terra_destroy_session_sandbox(binding);
        }
        return;
      }
      application_launched = true;
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

#ifdef _WIN32
    if (terra_workspace) {
      const auto activated = terra_workspace_manager->activate(terra_workspace->id, terra_workspace->revision, launch_session->session_id);
      if (activated.status != terra_workspaces::status_t::success || !activated.resource) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Workspace session association could not be persisted");
        if (application_launched) {
          proc::proc.terminate();
        }
        const auto stopped = terra_workspace_manager->stop(terra_workspace->id, terra_workspace->revision, true);
        static_cast<void>(stopped);
        std::lock_guard lock {logical_session_mutex};
        logical_session.reset();
        terra_unregister_session(launch_session->session_id);
        terra_destroy_session_sandbox(binding);
        return;
      }
      terra_workspace = *activated.resource;
    }
#endif

    if (terra_v1) {
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      nlohmann::json created_resource;
      {
        std::lock_guard lock {terra_session_tracking_mutex};
        auto &entry = terra_session_tracking[launch_session->session_id];
        if (!entry.announced) {
          entry.binding = binding;
          entry.owner_client_uuid = client->uuid;
          entry.app_uuid = launch_session->app_uuid;
          entry.state = "preparing";
          entry.revision = 1;
          entry.updated_at = now_ms;
          entry.announced = true;
          ++terra_session_collection_revision;
        }
        created_resource = terra_session_json(
          {
            .id = launch_session->session_id,
            .client_uuid = client->uuid,
            .app_uuid = launch_session->app_uuid,
            .legacy_app_id = launch_session->appid,
            .state = "preparing",
            .started_at = std::chrono::system_clock::now(),
            .width = launch_session->width,
            .height = launch_session->height,
            .fps = launch_session->fps,
            .hdr = launch_session->enable_hdr,
          },
          &entry
        );
      }
      publish_terra_event({"session.created", launch_session->session_id, 1, std::move(created_resource)}, "session.control", client->uuid, {launch_session->app_uuid});
    }
    if (!rtsp_stream::launch_session_raise(launch_session)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "Another stream launch is already pending");
      if (application_launched) {
        proc::proc.terminate();
      }
      {
        std::lock_guard lock {logical_session_mutex};
        logical_session.reset();
      }
      if (terra_v1) {
        terra_unregister_session(launch_session->session_id);
        terra_destroy_session_sandbox(binding);
      }
#ifdef _WIN32
      if (terra_workspace) {
        const auto stopped = terra_workspace_manager->stop(terra_workspace->id, terra_workspace->revision, true);
        static_cast<void>(stopped);
      }
#endif
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
    if (terra_v1) {
      tree.put("root.EclipseSessionId", launch_session->session_id);
    }

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
    print_req<SolHTTPS>(request);

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
    const bool terra_v1 = terra_api::api_v1_requested(get_arg(args, "eclipseApiVersion", ""));
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
    if (terra_v1) {
      tree.put("root.EclipseSessionId", launch_session->session_id);
    }
  }

  /**
   * @brief Check whether cel.
   *
   * @param response HTTP response object to populate.
   * @param request HTTP request data from the client.
   */
  void cancel(resp_https_t response, req_https_t request) {
    print_req<SolHTTPS>(request);

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
    print_req<SolHTTPS>(request);

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
   * @brief Send a versioned Terra JSON response.
   *
   * @param response HTTPS response to populate.
   * @param status HTTP status code.
   * @param body JSON response body.
   */
  void send_terra_response(const resp_https_t &response, const SimpleWeb::StatusCode status, nlohmann::json body) {
    body["schemaVersion"] = terra_api::API_VERSION;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"Cache-Control", "no-store"},
    };
    response->write(status, body.dump(), headers);
    response->close_connection_after_response = true;
  }

  /**
   * @brief Send a stable structured Terra API error.
   *
   * @param response HTTPS response to populate.
   * @param status HTTP status code.
   * @param code Stable machine-readable error code.
   * @param message Human-readable error description.
   */
  void send_terra_error(const resp_https_t &response, const SimpleWeb::StatusCode status, const std::string_view code, const std::string_view message, nlohmann::json details = {}) {
    nlohmann::json error {
      {"code", code},
      {"message", message},
    };
    if (!details.is_null() && !details.empty()) {
      error["details"] = std::move(details);
    }
    send_terra_response(response, status, {
                                              {"error", std::move(error)},
                                            });
  }

  /**
   * @brief Authenticate and authorize one Terra API request.
   *
   * @param response HTTPS response used for authorization errors.
   * @param request Paired-client HTTPS request.
   * @param scope Required scope, or empty when pairing alone is sufficient.
   * @return Authenticated client identity, or no value after sending an error.
   */
  std::optional<verified_client_t> authorize_terra_request(const resp_https_t &response, const req_https_t &request, const std::string_view scope = {}) {
    auto client = verified_client(request);
    if (!client) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_unauthorized, "authentication_required", "Reconnect using an enabled paired client certificate");
      return std::nullopt;
    }
    if (!scope.empty() && !scope_allowed(*client, scope)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", std::format("Client certificate lacks {} permission", scope));
      return std::nullopt;
    }
    return client;
  }

  /**
   * @brief Flush one SSE header or payload and wait for socket completion.
   *
   * @param response Long-lived HTTPS response.
   * @return True when queued bytes reached the asynchronous transport layer.
   */
  bool flush_terra_event_stream(const resp_https_t &response) {
    auto completion = std::make_shared<std::promise<SimpleWeb::error_code>>();
    auto completed = completion->get_future();
    response->send([completion](const SimpleWeb::error_code &error) {
      try {
        completion->set_value(error);
      } catch (const std::future_error &) {}
    });
    return completed.wait_for(std::chrono::seconds {5}) == std::future_status::ready && !completed.get();
  }

  /**
   * @brief Project resynchronization collections from current client permissions.
   *
   * @param permissions Current certificate-bound policy.
   * @return Collection names caller can read directly.
   */
  std::vector<std::string> terra_event_collections(const terra_api::client_permissions_t &permissions) {
    std::vector<std::string> collections {"host", "capabilities", "operations"};
    const auto &scopes = permissions.scopes;
    if (scopes.contains("catalog.read")) {
      collections.emplace_back("catalog");
      collections.emplace_back("workspaces");
    }
    if (scopes.contains("session.control")) {
      collections.emplace_back("sessions");
    }
    if (scopes.contains("display.read")) {
      collections.emplace_back("displays");
      collections.emplace_back("virtualDisplays");
    }
    if (scopes.contains("telemetry.read")) {
      collections.emplace_back("telemetry");
    }
    if (scopes.contains("peripheral.forward")) {
      collections.emplace_back("peripherals");
    }
    if (scopes.contains("sandbox.manage")) {
      collections.emplace_back("sandboxes");
    }
    if (scopes.contains("catalog.read") || scopes.contains("display.read") || scopes.contains("sandbox.manage")) {
      collections.emplace_back("profiles");
    }
    return collections;
  }

  /**
   * @brief Return authenticated Terra events as replayable server-sent events.
   */
  void terra_events_stream(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request);
    if (!client) {
      return;
    }
    if (!terra_event_hub) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Event delivery is unavailable");
      return;
    }

    auto *const hub = terra_event_hub.get();
    const auto opened = hub->open(client->uuid, terra_header(request, "Last-Event-ID"), terra_event_collections(client->permissions));
    std::lock_guard stream_lock {terra_event_stream_mutex};
    std::erase_if(terra_event_streams, [](auto &stream) {
      return stream.wait_for(std::chrono::seconds::zero()) == std::future_status::ready;
    });
    terra_event_streams.emplace_back(std::async(std::launch::async, [response = std::move(response), opened, hub]() mutable {
      response->close_connection_after_response = true;
      response->write(SimpleWeb::StatusCode::success_ok, {
                                                           {"Content-Type", "text/event-stream; charset=utf-8"},
                                                           {"Cache-Control", "no-store"},
                                                           {"Connection", "keep-alive"},
                                                         });
      if (!flush_terra_event_stream(response)) {
        hub->disconnect(opened.stream);
        return;
      }
      for (const auto &record : opened.replay) {
        *response << terra_events::to_sse(record);
        if (!flush_terra_event_stream(response)) {
          hub->disconnect(opened.stream);
          return;
        }
      }
      while (true) {
        const auto next = hub->wait(opened.stream);
        if (next.status == terra_events::wait_status_t::disconnected) {
          return;
        }
        if (next.status == terra_events::wait_status_t::idle) {
          *response << ": keep-alive\n\n";
        } else {
          *response << terra_events::to_sse(*next.record);
        }
        if (!flush_terra_event_stream(response)) {
          hub->disconnect(opened.stream);
          return;
        }
      }
    }));
  }

  /**
   * @brief Build authenticated Terra capability and discovery metadata.
   *
   * @param client_uuid Stable paired-client UUID.
   * @param client_name Paired-client friendly name.
   * @param permissions Certificate-bound client policy.
   * @param host_uuid Stable Sol host UUID.
   * @param host_name Sol host name.
   * @param host_platform Sol platform identifier.
   * @param host_version Sol build version.
   * @param wake_available Whether usable Wake-on-LAN information exists.
   * @return Complete Terra API v1 capability response body without `schemaVersion`.
   *
   * On Windows the `sandboxes-v1` capability is appended and marked available only
   * when the live provider health probe succeeds; an unhealthy provider reports its
   * stable failure code under `features.sandboxes-v1` instead.
   */
  nlohmann::json terra_capabilities_document(
    const std::string_view client_uuid,
    const std::string_view client_name,
    const terra_api::client_permissions_t &permissions,
    const std::string_view host_uuid,
    const std::string_view host_name,
    const std::string_view host_platform,
    const std::string_view host_version,
    const bool wake_available
  ) {
    nlohmann::json capabilities = nlohmann::json::array();
    for (const auto capability : terra_api::CAPABILITIES) {
      capabilities.emplace_back(capability);
    }

    nlohmann::json features = nlohmann::json::object();
    for (const auto capability : terra_api::KNOWN_CAPABILITIES) {
      const bool available = std::ranges::find(terra_api::CAPABILITIES, capability) != terra_api::CAPABILITIES.end();
      features[capability] = {{"available", available}};
      if (!available) {
        features[capability]["reasonCode"] = "not_implemented";
        features[capability]["reason"] = "Capability is not implemented by this Sol build";
      }
    }
#ifdef _WIN32
    const auto sandbox_health = terra::windows::sandbox::health();
    if (sandbox_health.available) {
      capabilities.emplace_back("sandboxes-v1");
      features["sandboxes-v1"] = {{"available", true}};
    } else {
      features["sandboxes-v1"]["reasonCode"] = sandbox_health.reason_code;
      features["sandboxes-v1"]["reason"] = sandbox_health.reason;
    }
#endif
    if (terra_peripheral_manager) {
      capabilities.emplace_back("peripherals-v1");
      features["peripherals-v1"] = {{"available", true}};
    } else {
      features["peripherals-v1"]["reasonCode"] = "provider_unavailable";
      features["peripherals-v1"]["reason"] = "Peripheral forwarding manager failed to start";
    }
#ifdef _WIN32
    capabilities.emplace_back("displays-v1");
    features["displays-v1"] = {{"available", true}};
#endif

    const bool sandboxes_operational = features.value("sandboxes-v1", nlohmann::json::object()).value("available", false);
    const bool peripherals_operational = terra_peripheral_manager != nullptr;
    return {
      {"apiVersion", terra_api::API_VERSION},
      {"capabilities", std::move(capabilities)},
      {"client", {
                   {"uuid", client_uuid},
                   {"name", client_name},
                   {"scopes", permissions.scopes},
                   {"allowedApps", permissions.allowed_apps},
                   {"expiresAt", permissions.expires_at},
                 }},
      {"host", {
                 {"uuid", host_uuid},
                 {"name", host_name},
                 {"platform", host_platform},
                 {"version", host_version},
                 {"wakeOnLanAvailable", wake_available},
               }},
      {"features", std::move(features)},
      {"limits", {
                   {"sessions", {{"maxActive", 1}}},
                   {"displays", {{"maxManaged", 0}}},
                   {"virtualDisplays", {{"maxActive", 0}}},
                   {"workspaces", {{"maxActive", 1}}},
                   {"sandboxes", {{"maxActive", sandboxes_operational ? 4 : 0}}},
                   {"peripherals", {
                                     {"maxDevices", peripherals_operational ? 32 : 0},
                                     {"maxClaims", peripherals_operational ? 32 : 0},
                                     {"maxMessageBytes", peripherals_operational ? 65536 : 0},
                                     {"maxPayloadBytes", peripherals_operational ? 49152 : 0},
                                   }},
                 }},
    };
  }

  /**
   * @brief Convert configured application metadata to Terra Catalog V2 JSON.
   *
   * @param app Runtime application context.
   * @return Stable application resource.
   */
  nlohmann::json terra_app_json(const proc::ctx_t &app) {
    const auto &metadata = app.terra_metadata;
    const auto kind = metadata.value("kind", "unknown");
    static const std::set<std::string, std::less<>> kinds {"game", "desktop", "tool", "workspace", "unknown"};
    const auto normalized_kind = kinds.contains(kind) ? kind : "unknown";
    nlohmann::json classification = metadata.value("classification", nlohmann::json::object());
    if (!classification.is_object()) {
      classification = nlohmann::json::object();
    }
    const auto classification_source = classification.value("source", std::string {});
    classification["source"] = classification_source.empty() ? (metadata.contains("kind") ? "user" : "unknown") : classification_source;
    const auto classification_confidence = classification.value("confidence", metadata.contains("kind") ? 1.0 : 0.0);
    classification["confidence"] = std::isfinite(classification_confidence) && classification_confidence >= 0.0 && classification_confidence <= 1.0 ? classification_confidence : 0.0;

    const auto unique_strings = [](const nlohmann::json &source, const std::set<std::string, std::less<>> *allowed = nullptr) {
      nlohmann::json result = nlohmann::json::array();
      std::set<std::string, std::less<>> seen;
      if (!source.is_array()) {
        return result;
      }
      for (const auto &entry : source) {
        if (!entry.is_string()) {
          continue;
        }
        const auto value = entry.get<std::string>();
        if ((allowed == nullptr || allowed->contains(value)) && seen.emplace(value).second) {
          result.emplace_back(value);
        }
      }
      return result;
    };
    static const std::set<std::string, std::less<>> input_classes {"keyboard", "mouse", "controller", "touch", "pen"};

    nlohmann::json launch_profiles = nlohmann::json::array();
    std::set<std::string, std::less<>> launch_profile_ids;
    bool has_default_launch_profile = false;
    for (const auto &profile : metadata.value("launchProfiles", nlohmann::json::array())) {
      try {
        const auto id = profile.at("id").get<std::string>();
        const auto name = profile.at("name").get<std::string>();
        const auto is_default = profile.at("default").get<bool>();
        if (!profile.is_object() || !uuid_util::is_valid(id) || name.empty() || !launch_profile_ids.emplace(id).second || (is_default && has_default_launch_profile)) {
          continue;
        }
        has_default_launch_profile = has_default_launch_profile || is_default;
        launch_profiles.push_back({{"id", id}, {"name", name}, {"default", is_default}});
      } catch (const std::exception &) {}
    }

    nlohmann::json assets = nlohmann::json::object();
    const auto image_path = proc::validate_app_image_path(app.image_path);
    for (const auto asset_id : {"poster", "icon"}) {
      if (const auto asset = terra_assets::inspect(image_path, asset_id)) {
        assets[asset->id] = {
          {"id", asset->id},
          {"mediaType", asset->media_type},
          {"width", asset->width},
          {"height", asset->height},
          {"revision", asset->revision},
          {"url", std::format("/eclipse/v1/apps/{}/assets/{}", app.uuid, asset->id)},
        };
      }
    }

    return {
      {"uuid", app.uuid},
      {"legacyId", std::stoi(app.id)},
      {"name", app.name},
      {"kind", normalized_kind},
      {"classification", std::move(classification)},
      {"tags", unique_strings(metadata.value("tags", nlohmann::json::array()))},
      {"description", metadata.value("description", "")},
      {"source", metadata.value("source", "unknown")},
      {"publisher", metadata.value("publisher", "")},
      {"installed", metadata.value("installed", true)},
      {"updateAvailable", metadata.value("updateAvailable", false)},
      {"hdr", metadata.value("hdr", false)},
      {"inputRequirements", unique_strings(metadata.value("inputRequirements", nlohmann::json::array()), &input_classes)},
      {"launchProfiles", std::move(launch_profiles)},
      {"assets", std::move(assets)},
      {"displayProfileId", nullptr},
      {"streamProfileId", nullptr},
      {"sandboxProfileId", nullptr},
    };
  }

  /**
   * @brief Return one authenticated Terra catalog image asset.
   *
   * @param response HTTPS response to populate.
   * @param request Paired-client HTTPS request.
   */
  void terra_app_asset(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "catalog.read");
    if (!client) {
      return;
    }
    const auto app_uuid = request->path_match[1].str();
    const auto asset_id = request->path_match[2].str();
    const auto catalog = proc::catalog_snapshot();
    const auto app = std::ranges::find(catalog.apps, app_uuid, &proc::ctx_t::uuid);
    if (!uuid_util::is_valid(app_uuid) || app == catalog.apps.end() || !app_allowed(*client, app_uuid)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "asset_not_found", "Asset does not exist or is not visible to this client");
      return;
    }
    if (asset_id != "poster" && asset_id != "icon") {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "asset_not_found", "Asset does not exist or is not visible to this client");
      return;
    }
    const auto asset = terra_assets::inspect(proc::validate_app_image_path(app->image_path), asset_id);
    if (!asset) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "asset_not_found", "Asset does not exist or is not visible to this client");
      return;
    }

    std::ifstream stream {asset->path, std::ios::binary};
    if (!stream) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "host_failure", "Asset became unavailable while opening it");
      return;
    }
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", asset->media_type},
      {"Content-Length", std::to_string(asset->size)},
      {"ETag", asset->etag},
      {"Cache-Control", "no-store"},
    };
    response->write(SimpleWeb::StatusCode::success_ok, stream, headers);
    response->close_connection_after_response = true;
  }

  /**
   * @brief Resolve the machine-readable state reason for a session state.
   *
   * @param state Published session state.
   * @return Stable state-reason token.
   */
  std::string_view terra_session_reason(std::string_view state) {
    if (state == "preparing") {
      return "queued";
    }
    if (state == "starting") {
      return "transport-negotiating";
    }
    if (state == "running") {
      return "streaming";
    }
    if (state == "disconnected") {
      return "transport-disconnected";
    }
    if (state == "stopping") {
      return "requested";
    }
    if (state == "stopped") {
      return "terminated";
    }
    if (state == "failed") {
      return "error";
    }
    return "unknown";
  }

  /**
   * @brief Serialize an immutable stream-session snapshot with tracking metadata.
   *
   * @param session Session snapshot.
   * @param tracking Published lifecycle tracking state, or null for untracked sessions.
   * @return Terra session resource.
   */
  nlohmann::json terra_session_json(const rtsp_stream::session_info_t &session, const terra_session_tracking_t *tracking) {
    const auto started_at = std::chrono::duration_cast<std::chrono::milliseconds>(session.started_at.time_since_epoch()).count();
    const auto uuid_or_null = [](const std::string &value) {
      return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
    };
    const auto updated_at = tracking && tracking->updated_at > 0 ? tracking->updated_at : started_at;
    return {
      {"id", session.id},
      {"ownerClientUuid", session.client_uuid},
      {"appUuid", session.app_uuid},
      {"legacyAppId", session.legacy_app_id},
      {"workspaceId", uuid_or_null(tracking ? tracking->binding.workspace_id : std::string {})},
      {"state", session.state},
      {"stateReason", std::string {terra_session_reason(session.state)}},
      {"startedAt", started_at},
      {"updatedAt", updated_at},
      {"width", session.width},
      {"height", session.height},
      {"refreshRate", session.fps},
      {"hdr", session.hdr},
      {"displayId", nullptr},
      {"displayProfileId", uuid_or_null(tracking ? tracking->binding.display_profile_id : std::string {})},
      {"streamProfileId", uuid_or_null(tracking ? tracking->binding.stream_profile_id : std::string {})},
      {"launchProfileId", uuid_or_null(tracking ? tracking->binding.launch_profile_id : std::string {})},
      {"sandboxProfileId", uuid_or_null(tracking ? tracking->binding.sandbox_profile_id : std::string {})},
      {"sandboxId", uuid_or_null(tracking ? tracking->binding.sandbox_id : std::string {})},
      {"peripheralClaimIds", nlohmann::json::array()},
      {"revision", tracking ? tracking->revision : 1},
    };
  }

  /**
   * @brief Snapshot the published tracking state for one session.
   *
   * @param session_id Session UUID.
   * @return Tracking snapshot, or no value when the session is untracked.
   */
  std::optional<terra_session_tracking_t> terra_session_tracking_for(const std::string &session_id) {
    std::lock_guard lock {terra_session_tracking_mutex};
    const auto found = terra_session_tracking.find(session_id);
    return found == terra_session_tracking.end() ? std::nullopt : std::optional {found->second};
  }

  /**
   * @brief Resolve and authorize profile references requested for a launch.
   *
   * Explicit `eclipse*ProfileId` query fields take precedence over workspace
   * definition references, which take precedence over application metadata
   * defaults. Every resolved reference is checked for canonical syntax, type,
   * caller read authority, and application compatibility.
   *
   * @param client Authenticated caller.
   * @param app Catalog application being launched.
   * @param args Parsed launch query arguments.
   * @param workspace Workspace launch association, or null for direct launches.
   * @param error_code Receives the stable error code on failure.
   * @param error_message Receives the human-readable failure description.
   * @return Resolved references, or no value after failure.
   */
  std::optional<terra_session_binding_t> terra_resolve_launch_references(const verified_client_t &client, const proc::ctx_t &app, const args_t &args, const terra_workspaces::resource_t *workspace, std::string_view *error_code, std::string &error_message) {
    static constexpr std::string_view SANDBOX_READ_SCOPE = "sandbox.manage";
    const auto fail = [&](std::string_view code, std::string message) {
      if (error_code) {
        *error_code = code;
      }
      error_message = std::move(message);
      return std::nullopt;
    };
    const auto explicit_reference = [&](const char *name) {
      const auto found = args.find(name);
      return found == args.end() || found->second.empty() ? std::optional<std::string> {} : std::optional<std::string> {found->second};
    };
    const auto resolve_one = [&](const char *name, const std::string &type, const std::string_view read_scope, const std::optional<std::string> &workspace_default, const std::function<bool(const nlohmann::json &)> &compatible, std::string &resolved) -> bool {
      auto requested = explicit_reference(name);
      if (!requested && workspace_default) {
        requested = workspace_default;
      }
      if (!requested && type == "launch") {
        for (const auto &profile : app.terra_metadata.value("launchProfiles", nlohmann::json::array())) {
          if (profile.value("default", false) && profile.contains("id")) {
            requested = profile.at("id").get<std::string>();
          }
        }
      } else if (!requested) {
        // Strip the retained wire prefix, not the private implementation name.
        std::string metadata_key {name + std::char_traits<char>::length("eclipse")};
        metadata_key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(metadata_key[0])));
        const auto metadata_default = app.terra_metadata.value(metadata_key, "");
        if (!metadata_default.empty()) {
          requested = metadata_default;
        }
      }
      if (!requested) {
        resolved.clear();
        return true;
      }
      if (!uuid_util::is_valid(*requested)) {
        fail("invalid_argument", std::format("{} must be a canonical UUID", name));
        return false;
      }
      const auto profile = terra_profile_manager ? terra_profile_manager->inspect(*requested) : std::nullopt;
      if (!profile || profile->type != type) {
        fail("resource_not_found", std::format("{} does not resolve to a readable {}", name, type));
        return false;
      }
      if (!scope_allowed(client, read_scope)) {
        fail("permission_denied", std::format("{} requires the {} scope", name, read_scope));
        return false;
      }
      if (compatible && !compatible(profile->configuration)) {
        fail("unsupported_configuration", std::format("{} is not compatible with this application", name));
        return false;
      }
      resolved = *requested;
      return true;
    };
    terra_session_binding_t binding;
    if (workspace) {
      binding.workspace_id = workspace->id;
    }
    if (!resolve_one("eclipseDisplayProfileId", "display", "display.read", workspace ? workspace->definition.display_profile_id : std::nullopt, {}, binding.display_profile_id)) {
      return std::nullopt;
    }
    if (!resolve_one("eclipseStreamProfileId", "stream", "catalog.read", workspace ? workspace->definition.stream_profile_id : std::nullopt, {}, binding.stream_profile_id)) {
      return std::nullopt;
    }
    if (!resolve_one("eclipseLaunchProfileId", "launch", "catalog.read", workspace ? workspace->definition.launch_profile_id : std::nullopt, [&](const nlohmann::json &configuration) {
          return configuration.at("appUuid") == app.uuid;
        },
                     binding.launch_profile_id)) {
      return std::nullopt;
    }
    if (!resolve_one("eclipseSandboxProfileId", "sandbox", SANDBOX_READ_SCOPE, workspace ? workspace->definition.sandbox_profile_id : std::nullopt, [&](const nlohmann::json &configuration) {
          return terra_json_contains_string(configuration.at("allowedAppUuids"), app.uuid);
        },
                     binding.sandbox_profile_id)) {
      return std::nullopt;
    }
    return binding;
  }

  /**
   * @brief Create and start one session-scoped sandbox for a direct launch.
   *
   * @param client Authenticated caller.
   * @param app Catalog application being launched.
   * @param binding Resolved launch references; receives the sandbox UUID.
   * @param error_message Receives the human-readable failure description.
   * @return True when the sandbox is running.
   */
  bool terra_launch_direct_sandbox(const verified_client_t &client, const proc::ctx_t &app, terra_session_binding_t &binding, std::string &error_message) {
    if (binding.sandbox_profile_id.empty() || !terra_sandbox_manager) {
      return true;
    }
    const auto profile = terra_profile_manager ? terra_profile_manager->inspect(binding.sandbox_profile_id) : std::nullopt;
    if (!profile) {
      error_message = "Sandbox profile became unavailable before launch";
      return false;
    }
    const auto created = terra_sandbox_manager->create(client.uuid, {
                                                                        binding.sandbox_profile_id,
                                                                        std::nullopt,
                                                                        app.uuid,
                                                                        false,
                                                                        app.name,
                                                                        profile->configuration,
                                                                      });
    if (created.status != terra_sandboxes::status_t::success || !created.resource) {
      error_message = "Sandbox isolation could not be created for this launch";
      return false;
    }
    publish_terra_sandbox_event("sandbox.created", *created.resource, terra_sandboxes::to_json(*created.resource), client.uuid);
    const auto started = terra_sandbox_manager->start(created.resource->id, created.resource->revision, terra_sandboxes::start_t {.app_uuid = app.uuid});
    if (started.status != terra_sandboxes::status_t::success || !started.resource) {
      error_message = "Sandbox isolation could not be started for this launch";
      terra_destroy_sandbox_by_id(created.resource->id);
      return false;
    }
    publish_terra_sandbox_event("sandbox.updated", *started.resource, terra_sandboxes::to_json(*started.resource), client.uuid);
    binding.sandbox_id = started.resource->id;
    return true;
  }

  /**
   * @brief Destroy one session-scoped sandbox, forcing termination when required.
   *
   * @param sandbox_id Canonical sandbox UUID.
   */
  void terra_destroy_sandbox_by_id(const std::string &sandbox_id) {
    if (sandbox_id.empty() || !terra_sandbox_manager) {
      return;
    }
    const auto sandbox = terra_sandbox_manager->get(sandbox_id);
    if (!sandbox) {
      return;
    }
    auto removed = terra_sandbox_manager->remove(sandbox->id, sandbox->revision);
    if (removed.status != terra_sandboxes::status_t::success) {
      const auto stopped = terra_sandbox_manager->stop(sandbox->id, sandbox->revision, true);
      if (stopped.status == terra_sandboxes::status_t::success && stopped.resource) {
        removed = terra_sandbox_manager->remove(sandbox->id, stopped.resource->revision);
      }
    }
    if (removed.status != terra_sandboxes::status_t::success) {
      BOOST_LOG(error) << "Failed to destroy session sandbox [" << sandbox_id << ']';
    } else if (terra_peripheral_manager) {
      terra_peripheral_manager->target_ended("sandbox", sandbox_id, true);
    }
  }

  /**
   * @brief Destroy the sandbox bound to one session binding.
   *
   * @param binding Resolved launch references.
   */
  void terra_destroy_session_sandbox(const terra_session_binding_t &binding) {
    terra_destroy_sandbox_by_id(binding.sandbox_id);
  }

  /**
   * @brief Forget published tracking state for one session without side effects.
   *
   * @param session_id Session UUID.
   */
  void terra_unregister_session(const std::string &session_id) {
    std::lock_guard lock {terra_session_tracking_mutex};
    if (terra_session_tracking.erase(session_id) > 0) {
      ++terra_session_collection_revision;
    }
  }

  /**
   * @brief Serialize one session telemetry snapshot.
   *
   * @param session Session snapshot.
   * @param tracking Published tracking state, or null when untracked.
   * @return Session telemetry object following the telemetry-v1 schema.
   */
  nlohmann::json terra_telemetry_session_json(const rtsp_stream::session_info_t &session, const terra_session_tracking_t *tracking) {
    const auto uuid_or_null = [](const std::string &value) {
      return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
    };
    return {
      {"sessionId", session.id},
      {"generation", 1},
      {"state", session.state},
      {"codec", nullptr},
      {"width", session.width},
      {"height", session.height},
      {"refreshRate", session.fps},
      {"hdr", session.hdr},
      {"captureFps", nullptr},
      {"encodeFps", nullptr},
      {"transmitFps", nullptr},
      {"capturedFrames", nullptr},
      {"encodedFrames", nullptr},
      {"droppedFrames", nullptr},
      {"transmittedFrames", nullptr},
      {"captureLatencyMs", nullptr},
      {"encodeLatencyMs", nullptr},
      {"bitrateKbps", nullptr},
      {"networkRttMs", nullptr},
      {"networkJitterMs", nullptr},
      {"networkLossPercent", nullptr},
      {"videoBytes", nullptr},
      {"audioBytes", nullptr},
      {"controlBytes", nullptr},
      {"inputBytes", nullptr},
      {"queueDepth", nullptr},
      {"queueDrops", nullptr},
      {"displayId", nullptr},
      {"sandboxId", uuid_or_null(tracking ? tracking->binding.sandbox_id : std::string {})},
      {"workspaceId", uuid_or_null(tracking ? tracking->binding.workspace_id : std::string {})},
      {"peripheralClaimIds", nlohmann::json::array()},
    };
  }

  /**
   * @brief Serialize host telemetry and the visible session telemetry collection.
   *
   * @param owner_filter When non-empty, restrict session entries to this owner UUID.
   * @return Complete telemetry document without `schemaVersion`.
   */
  nlohmann::json terra_telemetry_document(const std::string &owner_filter) {
    const auto snapshots = terra_session_snapshots();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - terra_start_time).count();
    std::size_t transport_sessions = 0;
    nlohmann::json sessions = nlohmann::json::array();
    for (const auto &session : snapshots) {
      if (!owner_filter.empty() && session.client_uuid != owner_filter) {
        continue;
      }
      if (session.state == "starting" || session.state == "running") {
        ++transport_sessions;
      }
      const auto tracking = terra_session_tracking_for(session.id);
      sessions.push_back(terra_telemetry_session_json(session, tracking ? &*tracking : nullptr));
    }
#ifdef _WIN32
    const auto display_snapshot = terra_display_snapshot();
    const std::optional<std::uint64_t> healthy_displays = display_snapshot ? std::optional<std::uint64_t> {static_cast<std::uint64_t>(display_snapshot->second.size())} : std::nullopt;
#else
    const std::optional<std::uint64_t> healthy_displays;
#endif
#ifdef _WIN32
    std::optional<std::uint64_t> healthy_virtual_displays;
    if (terra_virtual_display_manager) {
      healthy_virtual_displays = 0;
      for (const auto &virtual_display : terra_virtual_display_manager->list().resources) {
        if (virtual_display.state == terra_virtual_display::state_t::ready || virtual_display.state == terra_virtual_display::state_t::attached) {
          ++*healthy_virtual_displays;
        }
      }
    }
#else
    std::optional<std::uint64_t> healthy_virtual_displays;
#endif
#ifdef _WIN32
    std::optional<std::uint64_t> running_sandboxes;
    if (terra_sandbox_manager) {
      running_sandboxes = 0;
      for (const auto &sandbox : terra_sandbox_manager->list().resources) {
        if (sandbox.state == terra_sandboxes::state_t::running || sandbox.state == terra_sandboxes::state_t::starting) {
          ++*running_sandboxes;
        }
      }
    }
#else
    std::optional<std::uint64_t> running_sandboxes;
#endif
    const auto count_or_null = [](const std::optional<std::uint64_t> &value) {
      return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
    };
    return {
      {"timestamp", now_ms},
      {
        "host",
        {
          {"uptimeMs", uptime_ms},
          {"healthy", true},
          {"captureHealthy", nullptr},
          {"captureFps", nullptr},
          {"encoderHealthy", nullptr},
          {"encoderName", nullptr},
          {"encoderCodec", nullptr},
          {"encoderPixelFormat", nullptr},
          {"encoderUtilizationPercent", nullptr},
          {"encoderLatencyMs", nullptr},
          {"audioCaptureHealthy", nullptr},
          {"audioQueueDepth", nullptr},
          {"sunshineCpuPercent", nullptr},
          {"sunshineMemoryBytes", nullptr},
          {"gpuPercent", nullptr},
          {"gpuMemoryBytes", nullptr},
          {"gpuTemperatureC", nullptr},
          {"gpuDriverReset", nullptr},
          {"activeLogicalSessions", snapshots.size()},
          {"activeTransportSessions", transport_sessions},
          {"healthyDisplayCount", count_or_null(healthy_displays)},
          {"healthyVirtualDisplayCount", count_or_null(healthy_virtual_displays)},
          {"runningSandboxCount", count_or_null(running_sandboxes)},
          {"activePeripheralClaimCount", nullptr},
        },
      },
      {"sessions", std::move(sessions)},
    };
  }

  /**
   * @brief Return active and resumable logical session snapshots.
   *
   * @return Immutable logical-session views, including disconnected resumable application state.
   */
  std::vector<rtsp_stream::session_info_t> terra_session_snapshots() {
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
   * @brief Return Terra API capability and caller-policy metadata.
   */
  void terra_capabilities(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request);
    if (!client) {
      return;
    }
    const auto local_address = net::addr_to_normalized_string(request->local_endpoint().address());
    const auto wake_available = terra_api::wake_on_lan_available(platf::get_mac_address(local_address));
    send_terra_response(
      response,
      SimpleWeb::StatusCode::success_ok,
      terra_capabilities_document(
        client->uuid,
        client->name,
        client->permissions,
        http::unique_id,
        config::nvhttp.sol_name,
        SOL_PLATFORM,
        PROJECT_VERSION,
        wake_available
      )
    );
  }

  /**
   * @brief Project one workspace as a first-class Catalog V2 entry.
   *
   * @param workspace Workspace resource.
   * @return Catalog application resource with `kind: workspace` whose UUID is the workspace UUID.
   */
  nlohmann::json terra_workspace_catalog_json(const terra_workspaces::resource_t &workspace) {
    const auto &definition = workspace.definition;
    const auto uuid_or_null = [](const std::optional<std::string> &value) {
      return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
    };
    nlohmann::json tags = nlohmann::json::array();
    tags.push_back("workspace");
    return {
      {"uuid", workspace.id},
      {"legacyId", nullptr},
      {"name", definition.name},
      {"kind", "workspace"},
      {"classification", {
                           {"source", "user"},
                           {"confidence", 1.0},
                         }},
      {"tags", std::move(tags)},
      {"description", definition.description},
      {"source", "eclipse"},
      {"publisher", nullptr},
      {"installed", true},
      {"updateAvailable", false},
      {"hdr", false},
      {"inputRequirements", nlohmann::json::array()},
      {"assets", nlohmann::json::object()},
      {"launchProfiles", nlohmann::json::array()},
      {"displayProfileId", uuid_or_null(definition.display_profile_id)},
      {"streamProfileId", uuid_or_null(definition.stream_profile_id)},
      {"sandboxProfileId", uuid_or_null(definition.sandbox_profile_id)},
    };
  }

  /**
   * @brief Return Terra Catalog V2 resources visible to the caller.
   */
  void terra_apps(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "catalog.read");
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
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned catalog revision");
        return;
      }
    }

    const bool changed = !since || *since != catalog.revision;
    nlohmann::json apps = nlohmann::json::array();
    if (changed) {
      for (const auto &app : catalog.apps) {
        if (app_allowed(*client, app.uuid)) {
          apps.emplace_back(terra_app_json(app));
        }
      }
#ifdef _WIN32
      if (terra_workspace_manager) {
        for (const auto &workspace : terra_workspace_manager->list().workspaces) {
          if (workspace.definition.desktop_app_uuid.empty() || !app_allowed(*client, workspace.definition.desktop_app_uuid)) {
            continue;
          }
          if (!terra_workspace_visible(*client, workspace, false)) {
            continue;
          }
          apps.push_back(terra_workspace_catalog_json(workspace));
        }
      }
#endif
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                         {"revision", catalog.revision},
                                                                         {"changed", changed},
                                                                         {"fullSnapshot", changed},
                                                                         {"apps", std::move(apps)},
                                                                       });
  }

#ifdef _WIN32
  /**
   * @brief Enumerate displays and advance collection revision after observable changes.
   *
   * @return Current collection revision and serialized display resources, or no value
   * when display enumeration is unavailable.
   */
  std::optional<std::pair<std::uint64_t, nlohmann::json>> terra_display_snapshot() {
    const auto devices = display_device::enumerate_devices();
    if (devices.empty()) {
      return std::nullopt;
    }
    nlohmann::json displays = nlohmann::json::array();
    for (const auto &snapshot : terra::windows::display::enumerate_snapshot(http::unique_id, devices)) {
      displays.push_back(terra::windows::display::to_json(snapshot));
    }
    const auto fingerprint = displays.dump();
    std::scoped_lock lock {physical_display_revision.mutex};
    if (physical_display_revision.revision == 0 || physical_display_revision.fingerprint != fingerprint) {
      physical_display_revision.fingerprint = fingerprint;
      ++physical_display_revision.revision;
    }
    for (auto &display : displays) {
      display["revision"] = physical_display_revision.revision;
    }
    return std::pair {physical_display_revision.revision, std::move(displays)};
  }

  /**
   * @brief Project one virtual display resource into the unified inventory schema.
   *
   * @param resource Virtual display resource.
   * @return Unified display resource with contract-required inventory fields.
   */
  nlohmann::json terra_unified_virtual_display_json(const terra_virtual_display::resource_t &resource) {
    auto display = terra_virtual_display::to_json(resource);
    const auto ready = resource.state == terra_virtual_display::state_t::ready || resource.state == terra_virtual_display::state_t::attached;
    const auto actual_size = display.at("actualMode");
    display["kind"] = "virtual";
    display["platformId"] = nullptr;
    display["connected"] = ready;
    display["enabled"] = ready;
    display["logicalSize"] = {
      {"width", actual_size.at("width")},
      {"height", actual_size.at("height")},
    };
    display["currentMode"] = std::move(actual_size);
    display["supportedModes"] = nlohmann::json::array({display.at("currentMode")});
    display["hdr"] = {
      {"supported", resource.hdr},
      {"enabled", resource.hdr},
    };
    display["captureEligible"] = resource.state == terra_virtual_display::state_t::attached;
    return display;
  }

  /**
   * @brief Build the unified physical and virtual display inventory.
   *
   * @return Unified collection revision and display resources, or no value when
   * physical display enumeration is unavailable.
   */
  std::optional<std::pair<std::uint64_t, nlohmann::json>> terra_unified_displays() {
    auto physical = terra_display_snapshot();
    if (!physical) {
      return std::nullopt;
    }
    std::uint64_t virtual_revision = 0;
    if (terra_virtual_display_manager) {
      const auto virtual_displays = terra_virtual_display_manager->list();
      virtual_revision = virtual_displays.revision;
      for (const auto &resource : virtual_displays.resources) {
        physical->second.push_back(terra_unified_virtual_display_json(resource));
      }
    }
    std::scoped_lock lock {terra_unified_display_revision.mutex};
    if (terra_unified_display_revision.revision == 0 || terra_unified_display_revision.inputs != std::pair {physical->first, virtual_revision}) {
      terra_unified_display_revision.inputs = {physical->first, virtual_revision};
      ++terra_unified_display_revision.revision;
    }
    for (auto &display : physical->second) {
      display["revision"] = terra_unified_display_revision.revision;
    }
    return std::pair {terra_unified_display_revision.revision, std::move(physical->second)};
  }

  /**
   * @brief Serialize the current topology resource from the unified inventory.
   *
   * @param displays Unified display inventory resources.
   * @return Topology object with stable identifier and revision.
   */
  nlohmann::json terra_topology_document(nlohmann::json displays) {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto &display : displays) {
      entries.push_back({
        {"id", display.at("id")},
        {"enabled", display.at("enabled")},
        {"primary", display.at("primary")},
        {"x", display.at("position").at("x")},
        {"y", display.at("position").at("y")},
        {"scale", static_cast<double>(display.at("scale").at("numerator").get<std::uint64_t>()) / static_cast<double>(display.at("scale").at("denominator").get<std::uint64_t>())},
        {"rotation", display.value("rotation", 0)},
        {"modeId", display.at("currentMode").is_null() ? nlohmann::json(nullptr) : nlohmann::json(display.at("currentMode").at("id"))},
        {"hdr", display.at("hdr").is_object() ? nlohmann::json(display.at("hdr").value("enabled", false)) : nlohmann::json(nullptr)},
      });
    }
    const auto fingerprint = entries.dump();
    std::scoped_lock lock {terra_topology_state.mutex};
    if (terra_topology_state.revision == 0 || terra_topology_state.fingerprint != fingerprint) {
      terra_topology_state.fingerprint = fingerprint;
      ++terra_topology_state.revision;
    }
    return {
      {"id", terra::windows::display::display_resource_uuid(http::unique_id, "eclipse-display-topology")},
      {"revision", terra_topology_state.revision},
      {"displays", std::move(entries)},
    };
  }

  /**
   * @brief Return the current display topology resource.
   */
  void terra_display_topology_get(resp_https_t response, req_https_t request) {
    if (!authorize_terra_request(response, request, "display.read")) {
      return;
    }
    auto snapshot = terra_unified_displays();
    if (!snapshot) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "Windows display inventory is unavailable");
      return;
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"topology", terra_topology_document(std::move(snapshot->second))}});
  }

  /**
   * @brief Resolve one requested display mode by stable identifier.
   *
   * @param display Unified display resource.
   * @param mode_id Requested stable mode identifier.
   * @return Matching mode object, or null when the mode is unknown.
   */
  nlohmann::json terra_resolve_display_mode(const nlohmann::json &display, const std::string &mode_id) {
    for (const auto &mode : display.at("supportedModes")) {
      if (mode.at("id") == mode_id) {
        return mode;
      }
    }
    return nullptr;
  }

  /**
   * @brief Apply one validated physical-display configuration and wait for observable change.
   *
   * @param device_id Stable libdisplaydevice identifier.
   * @param mode Requested mode object, or null to leave the mode unchanged.
   * @param primary Whether the display must become primary.
   * @param hdr Requested HDR state, or no value to leave HDR unchanged.
   * @param previous_fingerprint Inventory fingerprint observed before mutation.
   * @return `true` when configuration was scheduled for application.
   */
  bool terra_apply_physical_display(const std::string &device_id, const nlohmann::json &mode, bool primary, std::optional<bool> hdr, const std::string &previous_fingerprint) {
    display_device::SingleDisplayConfiguration configuration;
    configuration.m_device_id = device_id;
    configuration.m_device_prep = primary ? display_device::SingleDisplayConfiguration::DevicePreparation::EnsurePrimary : display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive;
    if (!mode.is_null()) {
      configuration.m_resolution = display_device::Resolution {
        mode.at("width").get<unsigned int>(),
        mode.at("height").get<unsigned int>(),
      };
      configuration.m_refresh_rate = display_device::Rational {
        mode.at("refreshNumerator").get<unsigned int>(),
        mode.at("refreshDenominator").get<unsigned int>(),
      };
    }
    if (hdr) {
      configuration.m_hdr_state = *hdr ? display_device::HdrState::Enabled : display_device::HdrState::Disabled;
    }
    if (configuration.m_device_prep == display_device::SingleDisplayConfiguration::DevicePreparation::EnsureActive && !configuration.m_resolution && !configuration.m_hdr_state) {
      return true;
    }
    display_device::configure_display(configuration);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {2};
    while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshot = terra_display_snapshot();
      if (!snapshot) {
        return false;
      }
      if (snapshot->second.dump() != previous_fingerprint) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    return true;
  }

  /**
   * @brief Replace the desired topology for managed physical displays.
   */
  void terra_display_topology_put(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "display.manage");
    if (!client) {
      return;
    }
    const auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    if (!body->contains("displays") || !body->at("displays").is_array()) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Body must contain a displays array");
      return;
    }
    const auto if_match = terra_if_match(request);
    if (!if_match) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_precondition_required, "precondition_required", "If-Match header with the current topology revision is required");
      return;
    }
    auto before = terra_unified_displays();
    if (!before) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "Windows display inventory is unavailable");
      return;
    }
    std::uint64_t topology_revision = 0;
    {
      std::scoped_lock lock {terra_topology_state.mutex};
      topology_revision = terra_topology_state.revision;
    }
    if (topology_revision == 0 || *if_match != topology_revision) {
      nlohmann::json details {{"currentRevision", topology_revision}};
      send_terra_error(response, SimpleWeb::StatusCode::client_error_conflict, "revision_conflict", "Topology revision does not match If-Match", std::move(details));
      return;
    }

    struct requested_display_t {
      nlohmann::json display;  ///< Matching unified inventory resource.
      bool primary {};  ///< Requested primary state.
      std::optional<bool> hdr;  ///< Requested HDR state.
      nlohmann::json mode;  ///< Requested mode object or null.
    };

    std::vector<requested_display_t> requested;
    for (const auto &entry : body->at("displays")) {
      if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string() || !entry.contains("enabled") || !entry.at("enabled").is_boolean() || !entry.contains("primary") || !entry.at("primary").is_boolean() || !entry.contains("x") || !entry.at("x").is_number_integer() || !entry.contains("y") || !entry.at("y").is_number_integer() || !entry.contains("scale") || !entry.at("scale").is_number() || !entry.contains("rotation") || !entry.at("rotation").is_number_integer()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Each displays entry requires id, enabled, primary, x, y, scale, and rotation");
        return;
      }
      const auto display_id = entry.at("id").get<std::string>();
      const auto found = std::ranges::find_if(before->second, [&](const nlohmann::json &display) {
        return display.at("id") == display_id;
      });
      if (found == before->second.end() || found->at("kind") != "physical") {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Topology entries may only list known physical displays");
        return;
      }
      if (!entry.at("enabled").get<bool>()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Disabling displays is not supported by this host");
        return;
      }
      if (entry.at("rotation").get<int>() != 0) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Rotation values other than zero are not supported by this host");
        return;
      }
      nlohmann::json mode = nullptr;
      if (entry.contains("modeId") && !entry.at("modeId").is_null()) {
        mode = terra_resolve_display_mode(*found, entry.at("modeId").get<std::string>());
        if (mode.is_null()) {
          send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Requested display mode is not supported by the display");
          return;
        }
      }
      requested.push_back({*found, entry.at("primary").get<bool>(), entry.contains("hdr") && !entry.at("hdr").is_null() ? std::optional<bool> {entry.at("hdr").get<bool>()} : std::nullopt, std::move(mode)});
    }
    if (std::ranges::count_if(requested, [](const auto &entry) {
          return entry.primary;
        }) > 1) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Only one display may be requested as primary");
      return;
    }
    if (requested.empty()) {
      const auto after = terra_unified_displays();
      send_terra_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                           {"topology", terra_topology_document(after ? after->second : nlohmann::json::array())},
                                                                           {"displays", after ? after->second : nlohmann::json::array()},
                                                                         });
      return;
    }
    const auto previous_fingerprint = before->second.dump();
    bool applied = true;
    for (const auto &entry : requested) {
      std::string libdevice_id;
      for (const auto &snapshot : terra::windows::display::enumerate_snapshot(http::unique_id, display_device::enumerate_devices())) {
        if (entry.display.at("id") == snapshot.resource_uuid) {
          libdevice_id = snapshot.device_id;
        }
      }
      if (libdevice_id.empty()) {
        applied = false;
        break;
      }
      if (!terra_apply_physical_display(libdevice_id, entry.mode, entry.primary, entry.hdr, previous_fingerprint)) {
        applied = false;
        break;
      }
    }
    const auto after = terra_unified_displays();
    if (!applied || !after) {
      display_device::revert_configuration();
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "host_failure", "Topology could not be applied; requested state was reverted");
      return;
    }
    BOOST_LOG(info) << "Audit: client ["sv << client->uuid << "] applied display topology";
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                         {"topology", terra_topology_document(after->second)},
                                                                         {"displays", after->second},
                                                                       });
  }

  /**
   * @brief Patch one managed display resource.
   */
  void terra_patch_display(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "display.manage");
    if (!client) {
      return;
    }
    const auto display_id = request->path_match[1].str();
    const auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    auto before = terra_unified_displays();
    if (!before) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "Windows display inventory is unavailable");
      return;
    }
    const auto found = std::ranges::find_if(before->second, [&](const nlohmann::json &display) {
      return display.at("id") == display_id;
    });
    if (found == before->second.end()) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "display_not_found", "Display does not exist or is not visible to this client");
      return;
    }
    if (found->at("kind") == "virtual") {
      terra_patch_virtual_display(response, request);
      return;
    }
    for (const auto &field : body->items()) {
      if (field.key() == "position" || field.key() == "scale" || field.key() == "rotation") {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", std::format("{} is immutable on this host", field.key()));
        return;
      }
      if (field.key() != "enabled" && field.key() != "primary" && field.key() != "modeId" && field.key() != "hdr") {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", std::format("{} is not a mutable display field", field.key()));
        return;
      }
    }
    if (body->contains("enabled") && !body->at("enabled").get<bool>()) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Disabling displays is not supported by this host");
      return;
    }
    nlohmann::json mode = nullptr;
    if (body->contains("modeId") && !body->at("modeId").is_null()) {
      mode = terra_resolve_display_mode(*found, body->at("modeId").get<std::string>());
      if (mode.is_null()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Requested display mode is not supported by the display");
        return;
      }
    }
    std::string libdevice_id;
    for (const auto &snapshot : terra::windows::display::enumerate_snapshot(http::unique_id, display_device::enumerate_devices())) {
      if (snapshot.resource_uuid == display_id) {
        libdevice_id = snapshot.device_id;
      }
    }
    if (libdevice_id.empty()) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "host_failure", "Display could not be resolved for mutation");
      return;
    }
    const auto applied = terra_apply_physical_display(
      libdevice_id,
      mode,
      body->contains("primary") && body->at("primary").get<bool>(),
      body->contains("hdr") && !body->at("hdr").is_null() ? std::optional<bool> {body->at("hdr").get<bool>()} : std::nullopt,
      before->second.dump()
    );
    const auto after = terra_unified_displays();
    if (!applied || !after) {
      display_device::revert_configuration();
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "host_failure", "Display state could not be applied; requested state was reverted");
      return;
    }
    const auto updated = std::ranges::find_if(after->second, [&](const nlohmann::json &display) {
      return display.at("id") == display_id;
    });
    if (updated == after->second.end()) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "host_failure", "Display disappeared during mutation");
      return;
    }
    BOOST_LOG(info) << "Audit: client ["sv << client->uuid << "] patched display ["sv << display_id << ']';
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"display", *updated}});
  }

  /**
   * @brief Return physical display resources visible to caller.
   */
  void terra_displays(resp_https_t response, req_https_t request) {
    if (!authorize_terra_request(response, request, "display.read")) {
      return;
    }
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
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned display revision");
        return;
      }
    }
    auto snapshot = terra_unified_displays();
    if (!snapshot) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "Windows display inventory is unavailable");
      return;
    }
    const bool changed = !since || *since != snapshot->first;
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                         {"revision", snapshot->first},
                                                                         {"changed", changed},
                                                                         {"fullSnapshot", changed},
                                                                         {"displays", changed ? std::move(snapshot->second) : nlohmann::json::array()},
                                                                       });
  }

  /**
   * @brief Return one physical display resource.
   */
  void terra_display(resp_https_t response, req_https_t request) {
    if (!authorize_terra_request(response, request, "display.read")) {
      return;
    }
    const auto display_id = request->path_match[1].str();
    auto snapshot = terra_unified_displays();
    if (snapshot) {
      for (auto &display : snapshot->second) {
        if (display.at("id") == display_id) {
          send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"display", std::move(display)}});
          return;
        }
      }
    }
    send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "display_not_found", "Display does not exist or is not visible to this client");
  }
#endif

  /**
   * @brief Read one request header.
   *
   * @param request HTTPS request.
   * @param name Case-insensitive header name.
   * @return Header value, or no value when absent.
   */
  std::optional<std::string> terra_header(const req_https_t &request, const std::string_view name) {
    const auto found = request->header.find(std::string {name});
    return found == request->header.end() ? std::nullopt : std::optional<std::string> {found->second};
  }

  /**
   * @brief Parse required strong numeric `If-Match` revision.
   *
   * @param request HTTPS request.
   * @return Parsed revision, or no value for absent or malformed input.
   */
  std::optional<std::uint64_t> terra_if_match(const req_https_t &request) {
    auto value = terra_header(request, "If-Match");
    if (!value || value->size() < 3 || value->front() != '"' || value->back() != '"') {
      return std::nullopt;
    }
    value->erase(value->begin());
    value->pop_back();
    try {
      std::size_t consumed {};
      const auto revision = std::stoull(*value, &consumed);
      return consumed == value->size() && revision > 0 ? std::optional<std::uint64_t> {revision} : std::nullopt;
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }

  /**
   * @brief Parse bounded Terra JSON request body.
   *
   * @param response HTTPS response used for structured errors.
   * @param request HTTPS request.
   * @return Parsed object with schema version one, or no value after sending error.
   */
  std::optional<nlohmann::json> terra_request_json(const resp_https_t &response, const req_https_t &request) {
    constexpr std::size_t MAX_BODY_BYTES = 64 * 1024;
    if (const auto content_length = terra_header(request, "Content-Length")) {
      std::uint64_t length {};
      const auto [end, error] = std::from_chars(content_length->data(), content_length->data() + content_length->size(), length);
      if (error != std::errc {} || end != content_length->data() + content_length->size()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Content-Length must be an unsigned integer");
        return std::nullopt;
      }
      if (length > MAX_BODY_BYTES) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_payload_too_large, "payload_too_large", "Request body exceeds 64 KiB");
        return std::nullopt;
      }
    }

    auto body = terra_api::read_bounded_body(request->content, MAX_BODY_BYTES);
    if (body.status == terra_api::body_read_status_t::too_large) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_payload_too_large, "payload_too_large", "Request body exceeds 64 KiB");
      return std::nullopt;
    }
    try {
      auto json = nlohmann::json::parse(body.text);
      if (!json.is_object() || json.value("schemaVersion", 0) != terra_api::API_VERSION) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Request body must be a schemaVersion 1 JSON object");
        return std::nullopt;
      }
      return json;
    } catch (const std::exception &) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Request body is not valid JSON");
      return std::nullopt;
    }
  }

#ifdef _WIN32
  /**
   * @brief Parse exact virtual-display mode object.
   *
   * @param value JSON mode object.
   * @return Parsed mode, or no value for invalid shape or type.
   */
  std::optional<terra_virtual_display::mode_t> terra_virtual_mode(const nlohmann::json &value) {
    try {
      if (!value.is_object()) {
        return std::nullopt;
      }
      return terra_virtual_display::mode_t {
        value.at("width").get<int>(),
        value.at("height").get<int>(),
        value.at("refreshNumerator").get<std::uint32_t>(),
        value.at("refreshDenominator").get<std::uint32_t>(),
        value.at("bitDepth").get<int>(),
        value.at("hdr").get<bool>(),
      };
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }

  /**
   * @brief Parse complete virtual-display creation specification.
   *
   * @param value Schema-versioned request object.
   * @return Parsed specification, or no value for invalid shape or type.
   */
  std::optional<terra_virtual_display::specification_t> terra_virtual_specification(const nlohmann::json &value) {
    try {
      const auto mode = terra_virtual_mode(value.at("mode"));
      const auto &position = value.at("position");
      if (!mode || !position.is_object()) {
        return std::nullopt;
      }
      terra_virtual_display::specification_t result {
        value.at("name").get<std::string>(),
        *mode,
        {position.at("x").get<int>(), position.at("y").get<int>()},
        value.at("scale").get<double>(),
        value.at("rotation").get<int>(),
        value.at("primary").get<bool>(),
        value.at("hdr").get<bool>(),
        value.at("persistent").get<bool>(),
        std::nullopt,
      };
      if (value.contains("workspaceId") && !value.at("workspaceId").is_null()) {
        result.workspace_id = value.at("workspaceId").get<std::string>();
      }
      return result;
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }

  /**
   * @brief Parse virtual-display patch fields.
   *
   * @param value Schema-versioned request object.
   * @return Parsed nonempty patch, or no value for invalid shape or type.
   */
  std::optional<terra_virtual_display::patch_t> terra_virtual_patch(const nlohmann::json &value) {
    terra_virtual_display::patch_t result;
    try {
      if (value.contains("name")) {
        result.name = value.at("name").get<std::string>();
      }
      if (value.contains("mode")) {
        result.mode = terra_virtual_mode(value.at("mode"));
        if (!result.mode) {
          return std::nullopt;
        }
      }
      if (value.contains("position")) {
        result.position = terra_virtual_display::position_t {value.at("position").at("x").get<int>(), value.at("position").at("y").get<int>()};
      }
      if (value.contains("scale")) {
        result.scale = value.at("scale").get<double>();
      }
      if (value.contains("rotation")) {
        result.rotation = value.at("rotation").get<int>();
      }
      if (value.contains("primary")) {
        result.primary = value.at("primary").get<bool>();
      }
      if (value.contains("hdr")) {
        result.hdr = value.at("hdr").get<bool>();
      }
      if (value.contains("persistent")) {
        result.persistent = value.at("persistent").get<bool>();
      }
      if (value.contains("workspaceId")) {
        result.workspace_id = value.at("workspaceId").is_null() ? std::optional<std::string> {} : std::optional<std::string> {value.at("workspaceId").get<std::string>()};
      }
    } catch (const std::exception &) {
      return std::nullopt;
    }
    if (!result.name && !result.mode && !result.position && !result.scale && !result.rotation && !result.primary && !result.hdr && !result.persistent && !result.workspace_id) {
      return std::nullopt;
    }
    return result;
  }

  /**
   * @brief Check virtual-display visibility under owner masking.
   *
   * @param client Authenticated client.
   * @param resource Virtual-display resource.
   * @return `true` for owner or host administrator.
   */
  bool terra_virtual_visible(const verified_client_t &client, const terra_virtual_display::resource_t &resource) {
    return scope_allowed(client, "host.control") || (resource.owner_client_uuid && *resource.owner_client_uuid == client.uuid);
  }

  /**
   * @brief Convert virtual-display mutation status to operation failure object.
   *
   * @param status Mutation status.
   * @return Stable structured operation error.
   */
  nlohmann::json terra_virtual_error(const terra_virtual_display::status_t status) {
    using terra_virtual_display::status_t;
    switch (status) {
      case status_t::not_found:
        return {{"code", "resource_not_found"}, {"message", "Virtual display no longer exists"}};
      case status_t::invalid:
        return {{"code", "invalid_argument"}, {"message", "Virtual display request is invalid"}};
      case status_t::conflict:
        return {{"code", "revision_conflict"}, {"message", "Virtual display state or revision changed"}};
      case status_t::provider_error:
        return {{"code", "provider_failure"}, {"message", "MttVDD or Windows display operation failed"}};
      case status_t::persistence_error:
        return {{"code", "persistence_failure"}, {"message", "Virtual display state could not be persisted"}};
      case status_t::unavailable:
        return {{"code", "provider_unavailable"}, {"message", "Virtual display provider is unavailable"}};
      case status_t::success:
        break;
    }
    return nullptr;
  }

  /** @brief Completed result from one deferred Terra mutation. */
  struct terra_operation_completion_t {
    bool succeeded {};  ///< Whether operation reached `succeeded`.
    std::optional<std::string> resource_id;  ///< Created or changed resource UUID.
    nlohmann::json result;  ///< Success result document.
    nlohmann::json error;  ///< Structured failure document.
  };

  /**
   * @brief Submit and schedule one idempotent Terra mutation.
   *
   * @param response HTTPS response.
   * @param client Authenticated client.
   * @param request HTTPS request carrying idempotency key.
   * @param action Stable mutation action used for authorization and replay.
   * @param body Canonical request body.
   * @param mutation Deferred mutation receiving current client authorization.
   */
  void submit_terra_operation(const resp_https_t &response, const verified_client_t &client, const req_https_t &request, std::string action, const nlohmann::json &body, std::function<terra_operation_completion_t(const verified_client_t &)> mutation) {
    const auto key = terra_header(request, "Idempotency-Key");
    if (!key || key->empty()) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "idempotency_key_required", "Idempotency-Key header is required");
      return;
    }
    if (!terra_operation_store) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "Operation store is unavailable");
      return;
    }
    const auto submission = terra_operation_store->submit(client.uuid, action, *key, body, [](const auto &operation) {
      return nlohmann::json {
        {"operation", terra_operations::to_json(operation)},
        {"operationUrl", std::format("/eclipse/v1/operations/{}", operation.id)},
      };
    });
    if (submission.status == terra_operations::submission_status_t::conflict) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_conflict, "idempotency_conflict", "Idempotency-Key was already used with different request content");
      return;
    }
    if (submission.status == terra_operations::submission_status_t::persistence_failed || !submission.operation) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, "persistence_failure", "Operation could not be persisted");
      return;
    }
    if (submission.status == terra_operations::submission_status_t::created) {
      const auto operation_id = submission.operation->id;
      const auto required_scope = std::string {terra_operation_scope(action)};
      terra_operation_pool.push([operation_id, client_uuid = client.uuid, required_scope, mutation = std::move(mutation)]() mutable {
        if (!terra_operation_store || !terra_operation_store->transition(operation_id, terra_operations::state_t::running)) {
          return;
        }

        std::optional<verified_client_t> current_client;
        {
          std::lock_guard authorization_lock {client_auth_mutex};
          const auto paired_client = std::ranges::find(client_root.named_devices, client_uuid, &named_cert_t::uuid);
          if (paired_client != client_root.named_devices.end() && paired_client->enabled && !permissions_expired(paired_client->permissions) && !required_scope.empty() && paired_client->permissions.scopes.contains(required_scope)) {
            current_client = verified_client_t {
              .uuid = paired_client->uuid,
              .name = paired_client->name,
              .cert = paired_client->cert,
              .permissions = paired_client->permissions,
            };
          }
        }
        if (!current_client) {
          terra_operation_store->transition(operation_id, terra_operations::state_t::failed, std::nullopt, nullptr, {
                                                                                                                          {"code", "authorization_revoked"},
                                                                                                                          {"message", "Client authorization was revoked before operation execution"},
                                                                                                                        });
          return;
        }
        const auto result = mutation(*current_client);
        if (result.succeeded) {
          terra_operation_store->transition(operation_id, terra_operations::state_t::succeeded, result.resource_id, std::move(result.result));
        } else {
          terra_operation_store->transition(operation_id, terra_operations::state_t::failed, std::nullopt, nullptr, std::move(result.error));
        }
      });
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_accepted, submission.response);
  }

  /**
   * @brief Submit and schedule idempotent virtual-display mutation.
   *
   * @param response HTTPS response.
   * @param client Authenticated client.
   * @param request HTTPS request carrying idempotency key.
   * @param action Stable mutation action.
   * @param body Canonical request body.
   * @param mutation Deferred manager mutation.
   */
  void submit_virtual_operation(const resp_https_t &response, const verified_client_t &client, const req_https_t &request, std::string action, const nlohmann::json &body, std::function<terra_virtual_display::result_t()> mutation) {
    submit_terra_operation(response, client, request, std::move(action), body, [mutation = std::move(mutation)](const verified_client_t &) mutable {
      const auto result = mutation();
      if (result.status == terra_virtual_display::status_t::success && result.resource) {
        return terra_operation_completion_t {true, result.resource->id, {{"resource", terra_virtual_display::to_json(*result.resource)}}, nullptr};
      }
      return terra_operation_completion_t {false, std::nullopt, nullptr, terra_virtual_error(result.status)};
    });
  }

  /**
   * @brief Return virtual displays visible to caller.
   */
  void terra_virtual_displays(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "display.read");
    if (!client) {
      return;
    }
    if (!terra_virtual_display_manager || !terra_virtual_display_manager->available()) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "MttVDD virtual display provider is unavailable");
      return;
    }
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
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned virtual-display revision");
        return;
      }
    }
    auto listed = terra_virtual_display_manager->list(since);
    nlohmann::json resources = nlohmann::json::array();
    if (listed.changed) {
      for (const auto &resource : listed.resources) {
        if (terra_virtual_visible(*client, resource)) {
          resources.push_back(terra_virtual_display::to_json(resource));
        }
      }
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"revision", listed.revision}, {"changed", listed.changed}, {"fullSnapshot", listed.changed}, {"virtualDisplays", std::move(resources)}});
  }

  /**
   * @brief Return one owner-visible virtual display.
   */
  void terra_virtual_display(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "display.read");
    if (!client) {
      return;
    }
    const auto resource = terra_virtual_display_manager ? terra_virtual_display_manager->get(request->path_match[1].str()) : std::nullopt;
    if (!resource || !terra_virtual_visible(*client, *resource)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "virtual_display_not_found", "Virtual display does not exist or is not visible to this client");
      return;
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"virtualDisplay", terra_virtual_display::to_json(*resource)}});
  }

  /**
   * @brief Create one managed MttVDD display asynchronously.
   */
  void terra_create_virtual_display(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "virtual-display.manage");
    if (!client) {
      return;
    }
    const auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    const auto specification = terra_virtual_specification(*body);
    if (!specification || specification->scale != 1.0) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "unsupported_configuration", "Virtual display request is invalid or uses unsupported scale");
      return;
    }
    submit_virtual_operation(response, *client, request, "virtual-display.create", *body, [owner = client->uuid, specification = *specification]() {
      return terra_virtual_display_manager->create(owner, specification);
    });
  }

  /**
   * @brief Patch owner-visible virtual display asynchronously.
   */
  void terra_patch_virtual_display(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "virtual-display.manage");
    if (!client) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto resource = terra_virtual_display_manager ? terra_virtual_display_manager->get(id) : std::nullopt;
    if (!resource || !terra_virtual_visible(*client, *resource)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "virtual_display_not_found", "Virtual display does not exist or is not visible to this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_precondition_required, "revision_required", "Strong numeric If-Match header is required");
      return;
    }
    if (*revision != resource->revision) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_precondition_failed, "revision_conflict", "Virtual display revision does not match");
      return;
    }
    const auto body = terra_request_json(response, request);
    const auto patch = body ? terra_virtual_patch(*body) : std::nullopt;
    if (!body || !patch || (patch->scale && *patch->scale != 1.0)) {
      if (body) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "unsupported_configuration", "Virtual display patch is empty, invalid, or uses unsupported scale");
      }
      return;
    }
    submit_virtual_operation(response, *client, request, "virtual-display.patch", *body, [id, revision = *revision, patch = *patch]() {
      return terra_virtual_display_manager->patch(id, revision, patch);
    });
  }

  /**
   * @brief Delete owner-visible virtual display asynchronously.
   */
  void terra_delete_virtual_display(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "virtual-display.manage");
    if (!client) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto resource = terra_virtual_display_manager ? terra_virtual_display_manager->get(id) : std::nullopt;
    if (!resource || !terra_virtual_visible(*client, *resource)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "virtual_display_not_found", "Virtual display does not exist or is not visible to this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_precondition_required, "revision_required", "Strong numeric If-Match header is required");
      return;
    }
    if (*revision != resource->revision) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_precondition_failed, "revision_conflict", "Virtual display revision does not match");
      return;
    }
    const nlohmann::json body {{"id", id}, {"revision", *revision}};
    submit_virtual_operation(response, *client, request, "virtual-display.delete", body, [id, revision = *revision]() {
      return terra_virtual_display_manager->remove(id, revision);
    });
  }

  /**
   * @brief Attach virtual display to exactly one runtime owner asynchronously.
   */
  void terra_attach_virtual_display(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "virtual-display.manage");
    if (!client) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto resource = terra_virtual_display_manager ? terra_virtual_display_manager->get(id) : std::nullopt;
    if (!resource || !terra_virtual_visible(*client, *resource)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "virtual_display_not_found", "Virtual display does not exist or is not visible to this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != resource->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Virtual display revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    terra_virtual_display::attachment_t attachment;
    try {
      if (body->contains("sessionId") && !body->at("sessionId").is_null()) {
        attachment.session_id = body->at("sessionId").get<std::string>();
      }
      if (body->contains("workspaceId") && !body->at("workspaceId").is_null()) {
        attachment.workspace_id = body->at("workspaceId").get<std::string>();
      }
    } catch (const std::exception &) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Attachment target is invalid");
      return;
    }
    if (attachment.session_id.has_value() == attachment.workspace_id.has_value()) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Exactly one sessionId or workspaceId is required");
      return;
    }
    submit_virtual_operation(response, *client, request, "virtual-display.attach", *body, [id, revision = *revision, attachment]() {
      return terra_virtual_display_manager->attach(id, revision, attachment);
    });
  }

  /**
   * @brief Detach owner-visible virtual display asynchronously.
   */
  void terra_detach_virtual_display(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "virtual-display.manage");
    if (!client) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto resource = terra_virtual_display_manager ? terra_virtual_display_manager->get(id) : std::nullopt;
    if (!resource || !terra_virtual_visible(*client, *resource)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "virtual_display_not_found", "Virtual display does not exist or is not visible to this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != resource->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Virtual display revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const nlohmann::json body {{"id", id}, {"revision", *revision}};
    submit_virtual_operation(response, *client, request, "virtual-display.detach", body, [id, revision = *revision]() {
      return terra_virtual_display_manager->detach(id, revision);
    });
  }

  /**
   * @brief Claim an orphaned persistent virtual display asynchronously.
   */
  void terra_adopt_virtual_display(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "virtual-display.manage");
    if (!client) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto resource = terra_virtual_display_manager ? terra_virtual_display_manager->get(id) : std::nullopt;
    if (!resource || resource->owner_client_uuid) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "virtual_display_not_found", "Orphaned virtual display does not exist");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != resource->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Virtual display revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const nlohmann::json body {{"id", id}, {"revision", *revision}};
    submit_virtual_operation(response, *client, request, "virtual-display.adopt", body, [id, revision = *revision, owner = client->uuid]() {
      return terra_virtual_display_manager->adopt(id, revision, owner);
    });
  }

  /**
   * @brief Resolve profile-domain scope for reads or mutations.
   *
   * @param type Profile type.
   * @param mutation Whether caller will mutate resource.
   * @return Required scope, or empty for unknown type.
   */
  std::string_view terra_profile_scope(const std::string_view type, const bool mutation) {
    if (type == "display") {
      return mutation ? "display.manage" : "display.read";
    }
    if (type == "sandbox") {
      return "sandbox.manage";
    }
    if (type == "stream" || type == "launch") {
      return mutation ? "host.control" : "catalog.read";
    }
    return {};
  }

  /**
   * @brief Convert current paired-client policy to profile actor context.
   *
   * @param client Current client identity and permissions.
   * @return Profile manager actor.
   */
  terra::profiles::actor_t terra_profile_actor(const verified_client_t &client) {
    return {
      client.uuid,
      {client.permissions.scopes.begin(), client.permissions.scopes.end()},
      {client.permissions.allowed_apps.begin(), client.permissions.allowed_apps.end()},
    };
  }

  /**
   * @brief Convert profile manager failure to stable operation error.
   *
   * @param status Profile manager status.
   * @param message Diagnostic detail.
   * @return Structured operation error.
   */
  nlohmann::json terra_profile_error(const terra::profiles::status_t status, const std::string_view message) {
    using terra::profiles::status_t;
    std::string_view code = "internal_error";
    switch (status) {
      case status_t::invalid:
        code = "invalid_argument";
        break;
      case status_t::unsupported:
        code = "unsupported_configuration";
        break;
      case status_t::not_found:
        code = "profile_not_found";
        break;
      case status_t::forbidden:
        code = "permission_denied";
        break;
      case status_t::resource_busy:
        code = "resource_busy";
        break;
      case status_t::conflict:
        code = "revision_conflict";
        break;
      case status_t::persistence:
        code = "persistence_failure";
        break;
      case status_t::unavailable:
        code = "provider_unavailable";
        break;
      case status_t::success:
        break;
    }
    return {{"code", code}, {"message", message}};
  }

  /**
   * @brief Return application associations used for profile event projection.
   *
   * @param profile Profile resource.
   * @return Associated application UUIDs.
   */
  std::vector<std::string> terra_profile_apps(const terra::profiles::profile_t &profile) {
    std::vector<std::string> result;
    if (profile.type == "launch") {
      result.push_back(profile.configuration.at("appUuid"));
    } else if (profile.type == "sandbox") {
      for (const auto &app_uuid : profile.configuration.at("allowedAppUuids")) {
        result.push_back(app_uuid);
      }
    }
    return result;
  }

  /**
   * @brief Check owner and application projection for a profile mutation.
   *
   * @param client Current client authorization.
   * @param profile Stored profile.
   * @return `true` when caller may observe profile for mutation.
   */
  bool terra_profile_mutable_visible(const verified_client_t &client, const terra::profiles::profile_t &profile) {
    if (!profile.owner_client_uuid || (*profile.owner_client_uuid != client.uuid && !scope_allowed(client, "host.control"))) {
      return false;
    }
    return std::ranges::all_of(terra_profile_apps(profile), [&](const auto &app_uuid) {
      return app_allowed(client, app_uuid);
    });
  }

  /**
   * @brief Verify profile manager startup availability.
   *
   * @param response HTTPS response used for structured failure.
   * @return `true` when profile manager is usable.
   */
  bool terra_profiles_available(const resp_https_t &response) {
    if (terra_profile_manager && terra_profile_manager->availability() == terra::profiles::status_t::success) {
      return true;
    }
    send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "Profile store is unavailable");
    return false;
  }

  /**
   * @brief Publish profile event using current scope, ownership, and allowlist policy.
   *
   * @param type Event type.
   * @param profile Resulting or removed profile.
   * @param data Event data.
   */
  void publish_terra_profile_event(const std::string &type, const terra::profiles::profile_t &profile, nlohmann::json data) {
    publish_terra_event({type, profile.id, profile.revision, std::move(data)}, terra_profile_scope(profile.type, false), profile.shared ? std::string {} : profile.owner_client_uuid.value_or(""), terra_profile_apps(profile));
  }

  /**
   * @brief Orphan profiles after client removal or policy replacement.
   *
   * @param owner Previous owner UUID.
   * @param permissions Current policy, or no value when identity is revoked.
   */
  void revoke_terra_profiles(const std::string &owner, const std::optional<terra_api::client_permissions_t> &permissions) {
    if (!terra_profile_manager) {
      return;
    }
    terra::profiles::result_t result;
    if (permissions) {
      verified_client_t client {.uuid = owner, .permissions = *permissions};
      result = terra_profile_manager->revoke_unauthorized(terra_profile_actor(client));
    } else {
      result = terra_profile_manager->revoke_owner(owner);
    }
    if (result.status != terra::profiles::status_t::success) {
      BOOST_LOG(error) << "Failed to persist profile revocation for client [" << owner << ']';
      return;
    }
    for (const auto &profile : result.profiles) {
      const auto event_owner = profile.shared ? std::string {} : owner;
      publish_terra_event({"profile.updated", profile.id, profile.revision, terra::profiles::to_json(profile)}, terra_profile_scope(profile.type, false), event_owner, terra_profile_apps(profile));
    }
  }

  /** @brief Return profiles visible to caller. */
  void terra_profiles(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request);
    if (!client) {
      return;
    }
    if (!terra_profiles_available(response)) {
      return;
    }
    const auto args = request->parse_query_string();
    std::optional<std::string> type;
    if (const auto value = args.find("type"); value != args.end()) {
      type = value->second;
      const auto scope = terra_profile_scope(*type, false);
      if (scope.empty()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Unknown profile type");
        return;
      }
      if (!scope_allowed(*client, scope)) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", std::format("Client certificate lacks {} permission", scope));
        return;
      }
    }
    std::optional<std::uint64_t> since;
    if (const auto value = args.find("since"); value != args.end()) {
      std::uint64_t parsed {};
      const auto [end, error] = std::from_chars(value->second.data(), value->second.data() + value->second.size(), parsed);
      if (error != std::errc {} || end != value->second.data() + value->second.size()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned profile revision");
        return;
      }
      since = parsed;
    }
    const auto listed = terra_profile_manager->list(terra_profile_actor(*client), type);
    if (listed.status == terra::profiles::status_t::forbidden) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", "Client certificate lacks profile read permission");
      return;
    }
    const bool changed = !since || *since != listed.collection_revision;
    nlohmann::json profiles = nlohmann::json::array();
    if (changed) {
      for (const auto &profile : listed.profiles) {
        profiles.push_back(terra::profiles::to_json(profile));
      }
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"revision", listed.collection_revision}, {"changed", changed}, {"fullSnapshot", changed}, {"profiles", std::move(profiles)}});
  }

  /** @brief Return one profile after type-specific authorization and visibility masking. */
  void terra_profile(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request);
    if (!client) {
      return;
    }
    if (!terra_profiles_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto type = terra_profile_manager->type(id);
    if (!type) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "profile_not_found", "Profile does not exist or is not visible to this client");
      return;
    }
    const auto scope = terra_profile_scope(*type, false);
    if (!scope_allowed(*client, scope)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", std::format("Client certificate lacks {} permission", scope));
      return;
    }
    const auto result = terra_profile_manager->get(terra_profile_actor(*client), id);
    if (result.status != terra::profiles::status_t::success || !result.profile) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "profile_not_found", "Profile does not exist or is not visible to this client");
      return;
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"profile", terra::profiles::to_json(*result.profile)}});
  }

  /** @brief Create one profile through durable idempotent operation handling. */
  void terra_create_profile(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request);
    if (!client) {
      return;
    }
    if (!terra_profiles_available(response)) {
      return;
    }
    auto body = terra_request_json(response, request);
    if (!body || !body->contains("type") || !body->at("type").is_string()) {
      if (body) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Profile type is required");
      }
      return;
    }
    const auto type = body->at("type").get<std::string>();
    const auto scope = terra_profile_scope(type, true);
    if (scope.empty()) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Unknown profile type");
      return;
    }
    if (!scope_allowed(*client, scope)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", std::format("Client certificate lacks {} permission", scope));
      return;
    }
    auto mutation = *body;
    mutation.erase("schemaVersion");
    submit_terra_operation(response, *client, request, std::format("profile.{}.create", type), *body, [mutation = std::move(mutation)](const verified_client_t &current_client) {
      const auto result = terra_profile_manager->create(terra_profile_actor(current_client), mutation);
      if (result.status != terra::profiles::status_t::success || !result.profile) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_profile_error(result.status, result.message)};
      }
      publish_terra_profile_event("profile.created", *result.profile, terra::profiles::to_json(*result.profile));
      return terra_operation_completion_t {true, result.profile->id, {{"profile", terra::profiles::to_json(*result.profile)}}, nullptr};
    });
  }

  /** @brief Patch one owner-visible profile through durable operation handling. */
  void terra_patch_profile(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request);
    if (!client) {
      return;
    }
    if (!terra_profiles_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto stored = terra_profile_manager->inspect(id);
    if (!stored) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "profile_not_found", "Profile does not exist or is not visible to this client");
      return;
    }
    const auto scope = terra_profile_scope(stored->type, true);
    if (!scope_allowed(*client, scope)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", std::format("Client certificate lacks {} permission", scope));
      return;
    }
    if (!terra_profile_mutable_visible(*client, *stored)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "profile_not_found", "Profile does not exist or is not visible to this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != stored->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Profile revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    auto mutation = *body;
    mutation.erase("schemaVersion");
    submit_terra_operation(response, *client, request, std::format("profile.{}.patch", stored->type), *body, [id, revision = *revision, mutation = std::move(mutation)](const verified_client_t &current_client) {
      const auto result = terra_profile_manager->patch(terra_profile_actor(current_client), id, revision, mutation);
      if (result.status != terra::profiles::status_t::success || !result.profile) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_profile_error(result.status, result.message)};
      }
      publish_terra_profile_event("profile.updated", *result.profile, terra::profiles::to_json(*result.profile));
      return terra_operation_completion_t {true, result.profile->id, {{"profile", terra::profiles::to_json(*result.profile)}}, nullptr};
    });
  }

  /** @brief Delete one owner-visible profile through durable operation handling. */
  void terra_delete_profile(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request);
    if (!client) {
      return;
    }
    if (!terra_profiles_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto stored = terra_profile_manager->inspect(id);
    if (!stored) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "profile_not_found", "Profile does not exist or is not visible to this client");
      return;
    }
    const auto scope = terra_profile_scope(stored->type, true);
    if (!scope_allowed(*client, scope)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", std::format("Client certificate lacks {} permission", scope));
      return;
    }
    if (!terra_profile_mutable_visible(*client, *stored)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "profile_not_found", "Profile does not exist or is not visible to this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != stored->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Profile revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const nlohmann::json body {{"id", id}, {"revision", *revision}};
    submit_terra_operation(response, *client, request, std::format("profile.{}.delete", stored->type), body, [id, revision = *revision, removed = *stored](const verified_client_t &current_client) {
      const auto result = terra_profile_manager->erase(terra_profile_actor(current_client), id, revision);
      if (result.status != terra::profiles::status_t::success) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_profile_error(result.status, result.message)};
      }
      publish_terra_profile_event("profile.removed", removed, {{"id", id}, {"revision", result.collection_revision}});
      return terra_operation_completion_t {true, id, {{"removed", true}, {"id", id}}, nullptr};
    });
  }

  /** @brief Adopt one orphaned profile through durable operation handling. */
  void terra_adopt_profile(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "host.control");
    if (!client) {
      return;
    }
    if (!terra_profiles_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto stored = terra_profile_manager->inspect(id);
    if (!stored || stored->owner_client_uuid) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "profile_not_found", "Orphaned profile does not exist");
      return;
    }
    const auto scope = terra_profile_scope(stored->type, true);
    if (!scope_allowed(*client, scope)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", std::format("Client certificate lacks {} permission", scope));
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != stored->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Profile revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const nlohmann::json body {{"id", id}, {"revision", *revision}};
    submit_terra_operation(response, *client, request, std::format("profile.{}.adopt", stored->type), body, [id, revision = *revision](const verified_client_t &current_client) {
      const auto result = terra_profile_manager->adopt(terra_profile_actor(current_client), id, revision);
      if (result.status != terra::profiles::status_t::success || !result.profile) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_profile_error(result.status, result.message)};
      }
      publish_terra_profile_event("profile.updated", *result.profile, terra::profiles::to_json(*result.profile));
      return terra_operation_completion_t {true, result.profile->id, {{"profile", terra::profiles::to_json(*result.profile)}}, nullptr};
    });
  }

  /**
   * @brief Return application UUIDs associated with workspace definition.
   *
   * @param definition Workspace definition.
   * @return Deduplicated application UUIDs.
   */
  std::vector<std::string> terra_workspace_apps(const terra_workspaces::definition_t &definition) {
    std::set<std::string> apps {definition.desktop_app_uuid};
    apps.insert(definition.permitted_app_uuids.begin(), definition.permitted_app_uuids.end());
    return {apps.begin(), apps.end()};
  }

  /**
   * @brief Apply workspace patch to definition snapshot.
   *
   * @param definition Existing definition.
   * @param patch Complete field replacements.
   * @return Candidate definition.
   */
  terra_workspaces::definition_t terra_workspace_apply_patch(terra_workspaces::definition_t definition, const terra_workspaces::patch_t &patch) {
  #define TERRA_WORKSPACE_PATCH(field) \
    if (patch.field) { \
      definition.field = *patch.field; \
    }
    TERRA_WORKSPACE_PATCH(name)
    TERRA_WORKSPACE_PATCH(description)
    TERRA_WORKSPACE_PATCH(shared)
    TERRA_WORKSPACE_PATCH(desktop_app_uuid)
    TERRA_WORKSPACE_PATCH(permitted_app_uuids)
    TERRA_WORKSPACE_PATCH(display_profile_id)
    TERRA_WORKSPACE_PATCH(stream_profile_id)
    TERRA_WORKSPACE_PATCH(launch_profile_id)
    TERRA_WORKSPACE_PATCH(sandbox_profile_id)
    TERRA_WORKSPACE_PATCH(virtual_displays)
    TERRA_WORKSPACE_PATCH(peripheral_policy)
    TERRA_WORKSPACE_PATCH(persistent)
    TERRA_WORKSPACE_PATCH(cleanup_policy)
  #undef TERRA_WORKSPACE_PATCH
    return definition;
  }

  /**
   * @brief Check workspace ownership, sharing, and application allowlist projection.
   *
   * @param client Current client authorization.
   * @param workspace Workspace resource.
   * @param lifecycle Whether cross-client lifecycle control is requested.
   * @return `true` when workspace is visible for requested operation.
   */
  bool terra_workspace_visible(const verified_client_t &client, const terra_workspaces::resource_t &workspace, const bool lifecycle = false) {
    if (!workspace.owner_client_uuid) {
      return false;
    }
    const bool owner = *workspace.owner_client_uuid == client.uuid;
    if ((!workspace.definition.shared && !owner && !scope_allowed(client, "host.control")) || (lifecycle && !owner && !scope_allowed(client, "host.control"))) {
      return false;
    }
    return std::ranges::all_of(terra_workspace_apps(workspace.definition), [&](const auto &app_uuid) {
      return app_allowed(client, app_uuid);
    });
  }

  /**
   * @brief Validate workspace profile references against caller visibility.
   *
   * @param client Current client authorization.
   * @param definition Candidate workspace definition.
   * @return `true` when every referenced profile is visible with matching type.
   */
  bool terra_workspace_profiles_visible(const verified_client_t &client, const terra_workspaces::definition_t &definition) {
    if (!terra_profile_manager) {
      return !definition.display_profile_id && !definition.stream_profile_id && !definition.launch_profile_id && !definition.sandbox_profile_id;
    }
    const auto actor = terra_profile_actor(client);
    const std::pair<std::string_view, const std::optional<std::string> *> profiles[] {
      {"display", &definition.display_profile_id},
      {"stream", &definition.stream_profile_id},
      {"launch", &definition.launch_profile_id},
      {"sandbox", &definition.sandbox_profile_id},
    };
    return std::ranges::all_of(profiles, [&](const auto &entry) {
      const auto &[type, id] = entry;
      if (!*id) {
        return true;
      }
      const auto result = terra_profile_manager->get(actor, **id);
      return result.status == terra::profiles::status_t::success && result.profile && result.profile->type == type;
    });
  }

  /**
   * @brief Validate complete workspace definition against current caller policy.
   *
   * @param client Current client authorization.
   * @param definition Candidate definition.
   * @return `true` when applications and profiles remain authorized.
   */
  bool terra_workspace_definition_authorized(const verified_client_t &client, const terra_workspaces::definition_t &definition) {
    return std::ranges::all_of(terra_workspace_apps(definition), [&](const auto &app_uuid) {
             return app_allowed(client, app_uuid);
           }) &&
           terra_workspace_profiles_visible(client, definition);
  }

  /**
   * @brief Convert workspace status to stable operation error.
   *
   * @param status Workspace manager status.
   * @return Structured error object.
   */
  nlohmann::json terra_workspace_error(const terra_workspaces::status_t status) {
    using terra_workspaces::status_t;
    switch (status) {
      case status_t::not_found:
        return {{"code", "workspace_not_found"}, {"message", "Workspace no longer exists"}};
      case status_t::invalid:
        return {{"code", "invalid_argument"}, {"message", "Workspace request is invalid or references unavailable resources"}};
      case status_t::conflict:
        return {{"code", "revision_conflict"}, {"message", "Workspace revision or lifecycle state changed"}};
      case status_t::resource_busy:
        return {{"code", "resource_busy"}, {"message", "Workspace is already owned"}};
      case status_t::preparation_error:
        return {{"code", "preparation_failed"}, {"message", "Workspace resources could not be prepared"}};
      case status_t::provider_error:
        return {{"code", "provider_failure"}, {"message", "Workspace provider operation failed"}};
      case status_t::persistence_error:
        return {{"code", "persistence_failure"}, {"message", "Workspace state could not be persisted"}};
      case status_t::unavailable:
        return {{"code", "provider_unavailable"}, {"message", "Workspace manager is unavailable"}};
      case status_t::success:
        break;
    }
    return nullptr;
  }

  /**
   * @brief Publish workspace event through current read projection.
   *
   * @param type Event type.
   * @param workspace Resulting or removed workspace.
   * @param data Event payload.
   */
  void publish_terra_workspace_event(const std::string &type, const terra_workspaces::resource_t &workspace, nlohmann::json data) {
    publish_terra_event({type, workspace.id, workspace.revision, std::move(data)}, "catalog.read", workspace.definition.shared ? std::string {} : workspace.owner_client_uuid.value_or(""), terra_workspace_apps(workspace.definition));
  }

  /**
   * @brief Stop and revoke all workspaces for removed authorization identity.
   *
   * @param owner Revoked owner UUID.
   */
  void revoke_terra_workspaces(const std::string &owner) {
    if (!terra_workspace_manager) {
      return;
    }
    const auto status = terra_workspace_manager->revoke_owner(owner);
    if (status != terra_workspaces::status_t::success) {
      BOOST_LOG(error) << "Failed to revoke workspaces for client [" << owner << ']';
      return;
    }
  }

  /**
   * @brief Verify workspace manager startup availability.
   *
   * @param response HTTPS response used for structured failure.
   * @return `true` when manager is usable.
   */
  bool terra_workspaces_available(const resp_https_t &response) {
    if (terra_workspace_manager && terra_workspace_manager->available()) {
      return true;
    }
    send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "Workspace manager is unavailable");
    return false;
  }

  /** @brief Return caller-visible workspace collection. */
  void terra_workspaces(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "catalog.read");
    if (!client || !terra_workspaces_available(response)) {
      return;
    }
    std::optional<std::uint64_t> since;
    const auto args = request->parse_query_string();
    if (const auto value = args.find("since"); value != args.end()) {
      std::uint64_t parsed {};
      const auto [end, error] = std::from_chars(value->second.data(), value->second.data() + value->second.size(), parsed);
      if (error != std::errc {} || end != value->second.data() + value->second.size()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned workspace revision");
        return;
      }
      since = parsed;
    }
    const auto listed = terra_workspace_manager->list(since);
    nlohmann::json workspaces = nlohmann::json::array();
    if (listed.changed) {
      for (const auto &workspace : listed.workspaces) {
        if (terra_workspace_visible(*client, workspace)) {
          workspaces.push_back(terra_workspaces::to_json(workspace));
        }
      }
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"revision", listed.revision}, {"changed", listed.changed}, {"fullSnapshot", listed.changed}, {"workspaces", std::move(workspaces)}});
  }

  /** @brief Return one caller-visible workspace. */
  void terra_workspace(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "catalog.read");
    if (!client || !terra_workspaces_available(response)) {
      return;
    }
    const auto workspace = terra_workspace_manager->get(request->path_match[1].str());
    if (!workspace || !terra_workspace_visible(*client, *workspace)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "workspace_not_found", "Workspace does not exist or is not visible to this client");
      return;
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"workspace", terra_workspaces::to_json(*workspace)}});
  }

  /** @brief Create workspace through durable idempotent operation handling. */
  void terra_create_workspace(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "host.control");
    if (!client || !terra_workspaces_available(response)) {
      return;
    }
    auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    auto mutation = *body;
    mutation.erase("schemaVersion");
    const auto definition = terra_workspaces::parse_definition(mutation);
    if (!definition) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Workspace creation object is invalid");
      return;
    }
    submit_terra_operation(response, *client, request, "workspace.create", *body, [definition = *definition](const verified_client_t &current_client) {
      if (!terra_workspace_definition_authorized(current_client, definition)) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "resource_not_found"}, {"message", "Workspace references are not visible to this client"}}};
      }
      const auto result = terra_workspace_manager->create(current_client.uuid, definition);
      if (result.status != terra_workspaces::status_t::success || !result.resource) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_workspace_error(result.status)};
      }
      return terra_operation_completion_t {true, result.resource->id, {{"workspace", terra_workspaces::to_json(*result.resource)}}, nullptr};
    });
  }

  /** @brief Patch workspace definition through durable operation handling. */
  void terra_patch_workspace(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "host.control");
    if (!client || !terra_workspaces_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto workspace = terra_workspace_manager->get(id);
    if (!workspace || !terra_workspace_visible(*client, *workspace, true)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "workspace_not_found", "Workspace does not exist or is not visible to this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != workspace->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Workspace revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    auto mutation = *body;
    mutation.erase("schemaVersion");
    const auto patch = terra_workspaces::parse_patch(mutation);
    if (!patch) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Workspace patch is empty or invalid");
      return;
    }
    submit_terra_operation(response, *client, request, "workspace.patch", *body, [id, revision = *revision, patch = *patch](const verified_client_t &current_client) {
      const auto current = terra_workspace_manager->get(id);
      if (!current || !terra_workspace_visible(current_client, *current, true) || !terra_workspace_definition_authorized(current_client, terra_workspace_apply_patch(current->definition, patch))) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "workspace_not_found"}, {"message", "Workspace or references are not visible to this client"}}};
      }
      const auto result = terra_workspace_manager->patch(id, revision, patch);
      if (result.status != terra_workspaces::status_t::success || !result.resource) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_workspace_error(result.status)};
      }
      return terra_operation_completion_t {true, id, {{"workspace", terra_workspaces::to_json(*result.resource)}}, nullptr};
    });
  }

  /** @brief Delete stopped workspace through durable operation handling. */
  void terra_delete_workspace(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "host.control");
    if (!client || !terra_workspaces_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto workspace = terra_workspace_manager->get(id);
    if (!workspace || !terra_workspace_visible(*client, *workspace, true)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "workspace_not_found", "Workspace does not exist or is not visible to this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != workspace->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Workspace revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const nlohmann::json body {{"id", id}, {"revision", *revision}};
    submit_terra_operation(response, *client, request, "workspace.delete", body, [id, revision = *revision](const verified_client_t &current_client) {
      const auto current = terra_workspace_manager->get(id);
      if (!current || !terra_workspace_visible(current_client, *current, true)) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "workspace_not_found"}, {"message", "Workspace does not exist or is not visible to this client"}}};
      }
      const auto result = terra_workspace_manager->remove(id, revision);
      if (result.status != terra_workspaces::status_t::success) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_workspace_error(result.status)};
      }
      return terra_operation_completion_t {true, id, {{"deleted", true}, {"id", id}}, nullptr};
    });
  }

  /** @brief Prepare workspace runtime through durable operation handling. */
  void terra_start_workspace(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "stream.launch");
    if (!client || !terra_workspaces_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto workspace = terra_workspace_manager->get(id);
    if (!workspace || !terra_workspace_visible(*client, *workspace, true)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "workspace_not_found", "Workspace does not exist or is not controllable by this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != workspace->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Workspace revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    auto mutation = *body;
    mutation.erase("schemaVersion");
    const auto start = terra_workspaces::parse_start_request(mutation);
    if (!start) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Workspace start object is invalid");
      return;
    }
    submit_terra_operation(response, *client, request, "workspace.start", *body, [id, revision = *revision, start = *start](const verified_client_t &current_client) {
      const auto current = terra_workspace_manager->get(id);
      const auto app_uuid = start.app_uuid.value_or(current ? current->definition.desktop_app_uuid : std::string {});
      if (!current || !terra_workspace_visible(current_client, *current, true) || !app_allowed(current_client, app_uuid) || !terra_workspace_definition_authorized(current_client, current->definition)) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "workspace_not_found"}, {"message", "Workspace or application is not visible to this client"}}};
      }
      const auto result = terra_workspace_manager->start(id, revision, start);
      if (result.status != terra_workspaces::status_t::success || !result.resource) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_workspace_error(result.status)};
      }
      return terra_operation_completion_t {true, id, {{"workspace", terra_workspaces::to_json(*result.resource)}}, nullptr};
    });
  }

  /** @brief Stop or disconnect workspace runtime through durable operation handling. */
  void terra_stop_workspace(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "session.control");
    if (!client || !terra_workspaces_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto workspace = terra_workspace_manager->get(id);
    if (!workspace || !terra_workspace_visible(*client, *workspace, true)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "workspace_not_found", "Workspace does not exist or is not controllable by this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != workspace->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Workspace revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const auto body = terra_request_json(response, request);
    if (!body || body->size() != 2 || !body->contains("terminateApplication") || !body->at("terminateApplication").is_boolean()) {
      if (body) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Workspace stop requires terminateApplication boolean");
      }
      return;
    }
    const bool terminate = body->at("terminateApplication");
    submit_terra_operation(response, *client, request, "workspace.stop", *body, [id, revision = *revision, terminate](const verified_client_t &current_client) {
      const auto current = terra_workspace_manager->get(id);
      if (!current || !terra_workspace_visible(current_client, *current, true)) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "workspace_not_found"}, {"message", "Workspace does not exist or is not controllable by this client"}}};
      }
      const auto result = terra_workspace_manager->stop(id, revision, terminate);
      if (result.status != terra_workspaces::status_t::success || !result.resource) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_workspace_error(result.status)};
      }
      if (terra_peripheral_manager) {
        terra_peripheral_manager->target_ended("workspace", id, terminate);
      }
      return terra_operation_completion_t {true, id, {{"workspace", terra_workspaces::to_json(*result.resource)}}, nullptr};
    });
  }

  /** @brief Adopt orphaned persistent workspace through durable operation handling. */
  void terra_adopt_workspace(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "host.control");
    if (!client || !terra_workspaces_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto workspace = terra_workspace_manager->get(id);
    if (!workspace || workspace->owner_client_uuid) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "workspace_not_found", "Orphaned workspace does not exist");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != workspace->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_precondition_failed : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "revision_required", revision ? "Workspace revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const nlohmann::json body {{"id", id}, {"revision", *revision}};
    submit_terra_operation(response, *client, request, "workspace.adopt", body, [id, revision = *revision](const verified_client_t &current_client) {
      const auto current = terra_workspace_manager->get(id);
      if (!current || current->owner_client_uuid || !terra_workspace_definition_authorized(current_client, current->definition)) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "workspace_not_found"}, {"message", "Orphaned workspace or references are unavailable"}}};
      }
      const auto result = terra_workspace_manager->adopt(id, revision, current_client.uuid);
      if (result.status != terra_workspaces::status_t::success || !result.resource) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_workspace_error(result.status)};
      }
      return terra_operation_completion_t {true, id, {{"workspace", terra_workspaces::to_json(*result.resource)}}, nullptr};
    });
  }

  /**
   * @brief Return whether text is a canonical lowercase UUID.
   * @param value Candidate UUID text.
   * @return `true` for canonical lowercase UUID text.
   */
  bool terra_canonical_uuid(const std::string &value) {
    return uuid_util::is_valid(value) && std::ranges::none_of(value, [](const unsigned char character) {
             return character >= 'A' && character <= 'F';
           });
  }

  /**
   * @brief Find exact string in validated JSON string array.
   * @param values JSON string array.
   * @param value String to find.
   * @return `true` when array contains value.
   */
  bool terra_json_contains_string(const nlohmann::json &values, const std::string &value) {
    return std::ranges::any_of(values, [&](const auto &entry) {
      return entry.template get<std::string>() == value;
    });
  }

  /**
   * @brief Return application associations used for sandbox visibility and events.
   * @param sandbox Sandbox resource.
   * @return Deduplicated associated application UUIDs.
   */
  std::vector<std::string> terra_sandbox_apps(const terra_sandboxes::resource_t &sandbox) {
    std::set<std::string> apps;
    if (sandbox.app_uuid) {
      apps.emplace(*sandbox.app_uuid);
    }
    for (const auto &app_uuid : sandbox.effective_policy.at("allowedAppUuids")) {
      apps.emplace(app_uuid.get<std::string>());
    }
    return {apps.begin(), apps.end()};
  }

  /**
   * @brief Check sandbox ownership and application allowlist projection.
   * @param client Current authenticated client.
   * @param sandbox Sandbox resource.
   * @return `true` when resource is visible to caller.
   */
  bool terra_sandbox_visible(const verified_client_t &client, const terra_sandboxes::resource_t &sandbox) {
    if (!sandbox.owner_client_uuid || (*sandbox.owner_client_uuid != client.uuid && !scope_allowed(client, "host.control"))) {
      return false;
    }
    return std::ranges::all_of(terra_sandbox_apps(sandbox), [&](const auto &app_uuid) {
      return app_allowed(client, app_uuid);
    });
  }

  /**
   * @brief Resolve caller-visible profile with required type.
   * @param client Current authenticated client.
   * @param id Profile UUID.
   * @param type Required profile type.
   * @return Visible matching profile, or no value.
   */
  std::optional<terra::profiles::profile_t> terra_sandbox_profile(const verified_client_t &client, const std::string &id, const std::string_view type) {
    if (!terra_profile_manager) {
      return std::nullopt;
    }
    const auto result = terra_profile_manager->get(terra_profile_actor(client), id);
    return result.status == terra::profiles::status_t::success && result.profile && result.profile->type == type ? result.profile : std::nullopt;
  }

  /**
   * @brief Resolve configured Sol application into exact native launch metadata.
   * @param app_uuid Stable application UUID.
   * @param launch_profile_id Optional launch-profile UUID.
   * @return Resolved executable and launch fields, or no value.
   */
  std::optional<terra_sandboxes::application_t> terra_resolve_sandbox_application(const std::string &app_uuid, const std::optional<std::string> &launch_profile_id) {
    const auto &apps = proc::proc.get_apps();
    const auto app = std::ranges::find(apps, app_uuid, &proc::ctx_t::uuid);
    if (app == apps.end() || app->cmd.empty() || app->elevated) {
      return std::nullopt;
    }
    std::vector<std::string> command;
    try {
      command = boost::program_options::split_winmain(app->cmd);
    } catch (const std::exception &) {
      return std::nullopt;
    }
    if (command.empty() || !fs::path(command.front()).is_absolute()) {
      return std::nullopt;
    }
    nlohmann::json arguments = nlohmann::json::array();
    for (auto entry = std::next(command.begin()); entry != command.end(); ++entry) {
      arguments.push_back(*entry);
    }
    nlohmann::json launch_data {{"arguments", std::move(arguments)}};
    if (!app->working_dir.empty()) {
      launch_data["workingDirectory"] = app->working_dir;
    }
    if (launch_profile_id) {
      const auto profile = terra_profile_manager ? terra_profile_manager->inspect(*launch_profile_id) : std::nullopt;
      if (!profile || profile->type != "launch" || profile->configuration.at("appUuid") != app_uuid) {
        return std::nullopt;
      }
      for (const auto &argument : profile->configuration.at("arguments")) {
        launch_data["arguments"].push_back(argument);
      }
      launch_data["environment"] = profile->configuration.at("environment");
      if (!profile->configuration.at("workingDirectory").is_null()) {
        launch_data["workingDirectory"] = profile->configuration.at("workingDirectory");
      }
    }
    return terra_sandboxes::application_t {app_uuid, launch_profile_id, command.front(), std::move(launch_data)};
  }

  /**
   * @brief Validate sandbox launch configuration semantics supported by native provider.
   * @param configuration Complete launch profile configuration.
   * @param app_uuid Selected application UUID.
   * @param sandbox_profile_id Sandbox policy profile UUID.
   * @return `true` when all launch semantics can be honored.
   */
  bool terra_sandbox_launch_configuration_supported(const nlohmann::json &configuration, const std::string &app_uuid, const std::string &sandbox_profile_id) {
    return configuration.at("appUuid") == app_uuid && configuration.at("displayProfileId").is_null() && configuration.at("streamProfileId").is_null() && (configuration.at("sandboxProfileId").is_null() || configuration.at("sandboxProfileId") == sandbox_profile_id) && !configuration.at("elevated").get<bool>() && configuration.at("preLaunchPolicy").empty() && configuration.at("postExitPolicy").empty() && configuration.at("cleanupPolicy") == "on-stop" && configuration.at("resumePolicy") == "deny" && configuration.at("concurrentLaunchPolicy") == "deny";
  }

  /**
   * @brief Validate stored sandbox launch-profile semantics supported by native provider.
   * @param profile Launch profile.
   * @param app_uuid Selected application UUID.
   * @param sandbox_profile_id Sandbox policy profile UUID.
   * @return `true` when all launch semantics can be honored.
   */
  bool terra_sandbox_launch_profile_supported(const terra::profiles::profile_t &profile, const std::string &app_uuid, const std::string &sandbox_profile_id) {
    return profile.type == "launch" && terra_sandbox_launch_configuration_supported(profile.configuration, app_uuid, sandbox_profile_id);
  }

  /** @brief Verify sandbox manager startup availability. */
  bool terra_sandboxes_available(const resp_https_t &response) {
    if (terra_sandbox_manager && terra_sandbox_manager->available()) {
      return true;
    }
    send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Sandbox provider is unavailable");
    return false;
  }

  /** @brief Convert sandbox lifecycle status to stable operation error. */
  nlohmann::json terra_sandbox_error(const terra_sandboxes::status_t status) {
    using terra_sandboxes::status_t;
    switch (status) {
      case status_t::not_found:
        return {{"code", "sandbox_not_found"}, {"message", "Sandbox no longer exists"}};
      case status_t::invalid:
        return {{"code", "invalid_argument"}, {"message", "Sandbox request is invalid"}};
      case status_t::unsupported_configuration:
        return {{"code", "unsupported_configuration"}, {"message", "Sandbox policy cannot be enforced by this host"}};
      case status_t::conflict:
        return {{"code", "revision_conflict"}, {"message", "Sandbox revision or lifecycle state changed"}};
      case status_t::application_not_found:
        return {{"code", "application_not_found"}, {"message", "Sandbox application or launch profile is unavailable"}};
      case status_t::provider_error:
        return {{"code", "provider_failure"}, {"message", "Sandbox provider operation failed"}};
      case status_t::persistence_error:
        return {{"code", "persistence_failure"}, {"message", "Sandbox state could not be persisted"}};
      case status_t::unavailable:
        return {{"code", "feature_unavailable"}, {"message", "Sandbox provider is unavailable"}};
      case status_t::success:
        break;
    }
    return nullptr;
  }

  /** @brief Publish sandbox event through current ownership and allowlist projection. */
  void publish_terra_sandbox_event(const std::string &type, const terra_sandboxes::resource_t &sandbox, nlohmann::json data, const std::string &owner) {
    publish_terra_event({type, sandbox.id, sandbox.revision, std::move(data)}, "sandbox.manage", owner, terra_sandbox_apps(sandbox));
  }

  /** @brief Stop and orphan or remove sandboxes owned by revoked client. */
  void revoke_terra_sandboxes(const std::string &owner) {
    if (terra_sandbox_manager && terra_sandbox_manager->revoke_owner(owner) != terra_sandboxes::status_t::success) {
      BOOST_LOG(error) << "Failed to revoke sandboxes for client [" << owner << ']';
    }
  }

  /**
   * @brief Stop and delete one workspace-owned sandbox.
   * @param id Sandbox UUID.
   * @return `true` when resource no longer exists.
   */
  bool terra_destroy_workspace_sandbox(const std::string &id) {
    if (!terra_sandbox_manager) {
      return false;
    }
    auto sandbox = terra_sandbox_manager->get(id);
    if (!sandbox) {
      return true;
    }
    return terra_sandbox_manager->remove(id, sandbox->revision).status == terra_sandboxes::status_t::success;
  }

  /**
   * @brief Stop one workspace sandbox while retaining its definition for rollback.
   * @param id Sandbox UUID.
   * @param force Whether provider termination should be forced.
   * @return `true` when sandbox is stopped.
   */
  bool terra_stop_workspace_sandbox(const std::string &id, const bool force) {
    if (!terra_sandbox_manager) {
      return false;
    }
    const auto sandbox = terra_sandbox_manager->get(id);
    if (!sandbox) {
      return false;
    }
    if (sandbox->state == terra_sandboxes::state_t::stopped || sandbox->state == terra_sandboxes::state_t::created) {
      return true;
    }
    const auto stopped = terra_sandbox_manager->stop(id, sandbox->revision, force);
    return stopped.status == terra_sandboxes::status_t::success;
  }

  /** @brief Return caller-visible sandbox collection. */
  void terra_sandboxes_route(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "sandbox.manage");
    if (!client || !terra_sandboxes_available(response)) {
      return;
    }
    std::optional<std::uint64_t> since;
    const auto args = request->parse_query_string();
    if (const auto value = args.find("since"); value != args.end()) {
      std::uint64_t parsed {};
      const auto [end, error] = std::from_chars(value->second.data(), value->second.data() + value->second.size(), parsed);
      if (error != std::errc {} || end != value->second.data() + value->second.size()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned sandbox revision");
        return;
      }
      since = parsed;
    }
    const auto listed = terra_sandbox_manager->list(since);
    nlohmann::json sandboxes = nlohmann::json::array();
    if (listed.changed) {
      for (const auto &sandbox : listed.resources) {
        if (terra_sandbox_visible(*client, sandbox)) {
          sandboxes.push_back(terra_sandboxes::to_json(sandbox));
        }
      }
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"revision", listed.revision}, {"changed", listed.changed}, {"fullSnapshot", listed.changed}, {"sandboxes", std::move(sandboxes)}});
  }

  /** @brief Return one caller-visible sandbox. */
  void terra_sandbox_route(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "sandbox.manage");
    if (!client || !terra_sandboxes_available(response)) {
      return;
    }
    const auto sandbox = terra_sandbox_manager->get(request->path_match[1].str());
    if (!sandbox || !terra_sandbox_visible(*client, *sandbox)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "sandbox_not_found", "Sandbox does not exist or is not visible to this client");
      return;
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"sandbox", terra_sandboxes::to_json(*sandbox)}});
  }

  /** @brief Create sandbox through durable idempotent operation handling. */
  void terra_create_sandbox(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "sandbox.manage");
    if (!client || !terra_sandboxes_available(response)) {
      return;
    }
    const auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    terra_sandboxes::create_t creation;
    try {
      if (body->size() != 6 || !body->contains("profileId") || !body->contains("workspaceId") || !body->contains("appUuid") || !body->contains("persistent") || !body->contains("name")) {
        throw std::invalid_argument("shape");
      }
      creation.profile_id = body->at("profileId").get<std::string>();
      if (!body->at("workspaceId").is_null()) {
        creation.workspace_id = body->at("workspaceId").get<std::string>();
      }
      if (!body->at("appUuid").is_null()) {
        creation.app_uuid = body->at("appUuid").get<std::string>();
      }
      creation.persistent = body->at("persistent").get<bool>();
      creation.name = body->at("name").get<std::string>();
    } catch (const std::exception &) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Sandbox creation object is invalid");
      return;
    }
    const auto profile = terra_canonical_uuid(creation.profile_id) ? terra_sandbox_profile(*client, creation.profile_id, "sandbox") : std::nullopt;
    if (creation.workspace_id) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Workspace-bound sandboxes are created through workspace lifecycle operations");
      return;
    }
    const bool app_valid = !creation.app_uuid || (terra_canonical_uuid(*creation.app_uuid) && app_allowed(*client, *creation.app_uuid) && profile && terra_json_contains_string(profile->configuration.at("allowedAppUuids"), *creation.app_uuid));
    if (!profile || !app_valid) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "resource_not_found", "Sandbox references are unavailable to this client");
      return;
    }
    creation.profile_configuration = profile->configuration;
    submit_terra_operation(response, *client, request, "sandbox.create", *body, [creation = std::move(creation)](const verified_client_t &current_client) {
      const auto profile = terra_sandbox_profile(current_client, creation.profile_id, "sandbox");
      if (!profile || creation.workspace_id || (creation.app_uuid && (!app_allowed(current_client, *creation.app_uuid) || !terra_json_contains_string(profile->configuration.at("allowedAppUuids"), *creation.app_uuid)))) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "resource_not_found"}, {"message", "Sandbox references are unavailable to this client"}}};
      }
      auto current_creation = creation;
      current_creation.profile_configuration = profile->configuration;
      const auto result = terra_sandbox_manager->create(current_client.uuid, current_creation);
      if (result.status != terra_sandboxes::status_t::success || !result.resource) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_sandbox_error(result.status)};
      }
      BOOST_LOG(info) << "Audit: client [" << current_client.uuid << "] created sandbox [" << result.resource->id << ']';
      return terra_operation_completion_t {true, result.resource->id, {{"sandbox", terra_sandboxes::to_json(*result.resource)}}, nullptr};
    });
  }

  /** @brief Start sandbox through durable idempotent operation handling. */
  void terra_start_sandbox(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "sandbox.manage");
    if (!client || !terra_sandboxes_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto sandbox = terra_sandbox_manager->get(id);
    if (!sandbox || !terra_sandbox_visible(*client, *sandbox)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "sandbox_not_found", "Sandbox does not exist or is not visible to this client");
      return;
    }
    if (sandbox->workspace_id) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_conflict, "resource_busy", "Workspace-bound sandbox must be controlled through its workspace");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != sandbox->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_conflict : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "precondition_required", revision ? "Sandbox revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const auto body = terra_request_json(response, request);
    terra_sandboxes::start_t start;
    try {
      if (!body || body->size() > 3 || std::ranges::any_of(body->items(), [](const auto &item) {
            return item.key() != "schemaVersion" && item.key() != "appUuid" && item.key() != "launchProfileId";
          })) {
        throw std::invalid_argument("shape");
      }
      if (body->contains("appUuid") && !body->at("appUuid").is_null()) {
        start.app_uuid = body->at("appUuid").get<std::string>();
      }
      if (body->contains("launchProfileId") && !body->at("launchProfileId").is_null()) {
        start.launch_profile_id = body->at("launchProfileId").get<std::string>();
      }
    } catch (const std::exception &) {
      if (body) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Sandbox start object is invalid");
      }
      return;
    }
    const auto app_uuid = start.app_uuid ? start.app_uuid : sandbox->app_uuid;
    const auto launch_profile = start.launch_profile_id && terra_canonical_uuid(*start.launch_profile_id) ? terra_sandbox_profile(*client, *start.launch_profile_id, "launch") : std::nullopt;
    if (!app_uuid || !terra_canonical_uuid(*app_uuid) || !app_allowed(*client, *app_uuid) || !terra_json_contains_string(sandbox->effective_policy.at("allowedAppUuids"), *app_uuid) || (start.launch_profile_id && (!launch_profile || !terra_sandbox_launch_profile_supported(*launch_profile, *app_uuid, sandbox->profile_id))) || !terra_resolve_sandbox_application(*app_uuid, start.launch_profile_id)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Sandbox application or launch profile cannot be executed under this policy");
      return;
    }
    submit_terra_operation(response, *client, request, "sandbox.start", *body, [id, revision = *revision, start](const verified_client_t &current_client) {
      const auto current = terra_sandbox_manager->get(id);
      const auto app_uuid = start.app_uuid ? start.app_uuid : (current ? current->app_uuid : std::nullopt);
      const auto launch_profile = start.launch_profile_id && terra_canonical_uuid(*start.launch_profile_id) ? terra_sandbox_profile(current_client, *start.launch_profile_id, "launch") : std::nullopt;
      if (!current || current->workspace_id || !terra_sandbox_visible(current_client, *current) || !app_uuid || !app_allowed(current_client, *app_uuid) || !terra_json_contains_string(current->effective_policy.at("allowedAppUuids"), *app_uuid) || (start.launch_profile_id && (!launch_profile || !terra_sandbox_launch_profile_supported(*launch_profile, *app_uuid, current->profile_id)))) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "sandbox_not_found"}, {"message", "Sandbox or launch references are unavailable to this client"}}};
      }
      const auto result = terra_sandbox_manager->start(id, revision, start);
      if (result.status != terra_sandboxes::status_t::success || !result.resource) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_sandbox_error(result.status)};
      }
      BOOST_LOG(info) << "Audit: client [" << current_client.uuid << "] started sandbox [" << id << ']';
      return terra_operation_completion_t {true, id, {{"sandbox", terra_sandboxes::to_json(*result.resource)}}, nullptr};
    });
  }

  /** @brief Stop or restart sandbox through durable operation handling. */
  void terra_stop_or_restart_sandbox(resp_https_t response, req_https_t request, const bool restart) {
    const auto client = authorize_terra_request(response, request, "sandbox.manage");
    if (!client || !terra_sandboxes_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto sandbox = terra_sandbox_manager->get(id);
    if (!sandbox || !terra_sandbox_visible(*client, *sandbox)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "sandbox_not_found", "Sandbox does not exist or is not visible to this client");
      return;
    }
    if (sandbox->workspace_id) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_conflict, "resource_busy", "Workspace-bound sandbox must be controlled through its workspace");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != sandbox->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_conflict : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "precondition_required", revision ? "Sandbox revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const auto body = terra_request_json(response, request);
    if (!body || body->size() != 2 || !body->contains("force") || !body->at("force").is_boolean()) {
      if (body) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Sandbox stop and restart require force boolean");
      }
      return;
    }
    const bool force = body->at("force");
    const auto action = restart ? "sandbox.restart" : "sandbox.stop";
    submit_terra_operation(response, *client, request, action, *body, [id, revision = *revision, force, restart](const verified_client_t &current_client) {
      const auto current = terra_sandbox_manager->get(id);
      if (!current || current->workspace_id || !terra_sandbox_visible(current_client, *current)) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "sandbox_not_found"}, {"message", "Sandbox does not exist or is not visible to this client"}}};
      }
      const auto result = restart ? terra_sandbox_manager->restart(id, revision, force) : terra_sandbox_manager->stop(id, revision, force);
      if (result.status != terra_sandboxes::status_t::success || !result.resource) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_sandbox_error(result.status)};
      }
      BOOST_LOG(info) << "Audit: client [" << current_client.uuid << "] " << (restart ? "restarted" : "stopped") << " sandbox [" << id << ']';
      return terra_operation_completion_t {true, id, {{"sandbox", terra_sandboxes::to_json(*result.resource)}}, nullptr};
    });
  }

  /** @brief Stop sandbox through durable operation handling. */
  void terra_stop_sandbox(resp_https_t response, req_https_t request) {
    terra_stop_or_restart_sandbox(std::move(response), std::move(request), false);
  }

  /** @brief Restart sandbox through durable operation handling. */
  void terra_restart_sandbox(resp_https_t response, req_https_t request) {
    terra_stop_or_restart_sandbox(std::move(response), std::move(request), true);
  }

  /** @brief Delete sandbox through durable operation handling. */
  void terra_delete_sandbox(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "sandbox.manage");
    if (!client || !terra_sandboxes_available(response)) {
      return;
    }
    const auto id = request->path_match[1].str();
    const auto sandbox = terra_sandbox_manager->get(id);
    if (!sandbox || !terra_sandbox_visible(*client, *sandbox)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "sandbox_not_found", "Sandbox does not exist or is not visible to this client");
      return;
    }
    if (sandbox->workspace_id) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_conflict, "resource_busy", "Workspace-bound sandbox must be controlled through its workspace");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != sandbox->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_conflict : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "precondition_required", revision ? "Sandbox revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const nlohmann::json body {{"id", id}, {"revision", *revision}};
    submit_terra_operation(response, *client, request, "sandbox.delete", body, [id, revision = *revision](const verified_client_t &current_client) {
      const auto current = terra_sandbox_manager->get(id);
      if (!current || current->workspace_id || !terra_sandbox_visible(current_client, *current)) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "sandbox_not_found"}, {"message", "Sandbox does not exist or is not visible to this client"}}};
      }
      const auto result = terra_sandbox_manager->remove(id, revision);
      if (result.status != terra_sandboxes::status_t::success) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_sandbox_error(result.status)};
      }
      if (terra_peripheral_manager) {
        terra_peripheral_manager->target_ended("sandbox", id, true);
      }
      BOOST_LOG(info) << "Audit: client [" << current_client.uuid << "] deleted sandbox [" << id << ']';
      return terra_operation_completion_t {true, id, {{"deleted", true}, {"id", id}}, nullptr};
    });
  }

  /** @brief Adopt orphaned persistent sandbox through durable operation handling. */
  void terra_adopt_sandbox(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "sandbox.manage");
    if (!client || !scope_allowed(*client, "host.control") || !terra_sandboxes_available(response)) {
      if (client && !scope_allowed(*client, "host.control")) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", "Client certificate lacks host.control permission");
      }
      return;
    }
    const auto id = request->path_match[1].str();
    const auto sandbox = terra_sandbox_manager->get(id);
    if (!sandbox) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "sandbox_not_found", "Orphaned sandbox does not exist");
      return;
    }
    if (sandbox->owner_client_uuid) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_conflict, "resource_busy", "Sandbox is already owned");
      return;
    }
    if (sandbox->workspace_id) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_conflict, "resource_busy", "Workspace-bound sandbox must be controlled through its workspace");
      return;
    }
    if (!std::ranges::all_of(terra_sandbox_apps(*sandbox), [&](const auto &app_uuid) {
          return app_allowed(*client, app_uuid);
        })) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "sandbox_not_found", "Orphaned sandbox does not exist or is not visible to this client");
      return;
    }
    const auto revision = terra_if_match(request);
    if (!revision || *revision != sandbox->revision) {
      send_terra_error(response, revision ? SimpleWeb::StatusCode::client_error_conflict : SimpleWeb::StatusCode::client_error_precondition_required, revision ? "revision_conflict" : "precondition_required", revision ? "Sandbox revision does not match" : "Strong numeric If-Match header is required");
      return;
    }
    const auto body = terra_request_json(response, request);
    if (!body || body->size() != 1) {
      if (body) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Sandbox adoption requires an empty object");
      }
      return;
    }
    submit_terra_operation(response, *client, request, "sandbox.adopt", *body, [id, revision = *revision](const verified_client_t &current_client) {
      const auto current = terra_sandbox_manager->get(id);
      if (!current || current->workspace_id || current->owner_client_uuid || !std::ranges::all_of(terra_sandbox_apps(*current), [&](const auto &app_uuid) {
            return app_allowed(current_client, app_uuid);
          })) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, {{"code", "sandbox_not_found"}, {"message", "Orphaned sandbox is unavailable to this client"}}};
      }
      const auto result = terra_sandbox_manager->adopt(id, revision, current_client.uuid);
      if (result.status != terra_sandboxes::status_t::success || !result.resource) {
        return terra_operation_completion_t {false, std::nullopt, nullptr, terra_sandbox_error(result.status)};
      }
      BOOST_LOG(info) << "Audit: client [" << current_client.uuid << "] adopted sandbox [" << id << ']';
      return terra_operation_completion_t {true, id, {{"sandbox", terra_sandboxes::to_json(*result.resource)}}, nullptr};
    });
  }
#endif

  /**
   * @brief Return one asynchronous operation visible to caller.
   */
  void terra_operation(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request);
    if (!client) {
      return;
    }
    const auto operation_id = request->path_match[1].str();
    const auto operation = terra_operation_store ? terra_operation_store->get(operation_id) : std::nullopt;
    if (!operation) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "operation_not_found", "Operation does not exist or is not visible to this client");
      return;
    }
    const auto operation_scope = terra_operation_scope(operation->action);
    if (operation_scope.empty() || !scope_allowed(*client, operation_scope)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_forbidden, "permission_denied", "Client certificate lacks operation domain permission");
      return;
    }
    if (operation->client_uuid != client->uuid && !scope_allowed(*client, "host.control")) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "operation_not_found", "Operation does not exist or is not visible to this client");
      return;
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"operation", terra_operations::to_json(*operation)}});
  }

  /**
   * @brief Return active Terra session resources visible to the caller.
   */
  void terra_sessions(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "session.control");
    if (!client) {
      return;
    }
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
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned session revision");
        return;
      }
    }
    const bool administer = scope_allowed(*client, "host.control");
    std::uint64_t revision = 0;
    nlohmann::json sessions = nlohmann::json::array();
    {
      std::lock_guard lock {terra_session_tracking_mutex};
      revision = terra_session_collection_revision;
    }
    const bool changed = !since || *since != revision;
    if (changed) {
      for (const auto &session : terra_session_snapshots()) {
        if (administer || session.client_uuid == client->uuid) {
          const auto tracking = terra_session_tracking_for(session.id);
          sessions.emplace_back(terra_session_json(session, tracking ? &*tracking : nullptr));
        }
      }
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                         {"revision", revision},
                                                                         {"changed", changed},
                                                                         {"fullSnapshot", changed},
                                                                         {"sessions", std::move(sessions)},
                                                                       });
  }

  /**
   * @brief Return one active Terra session resource.
   */
  void terra_session(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "session.control");
    if (!client) {
      return;
    }
    const auto session_id = request->path_match[1].str();
    const bool administer = scope_allowed(*client, "host.control");
    for (const auto &session : terra_session_snapshots()) {
      if (session.id == session_id && (administer || session.client_uuid == client->uuid)) {
        const auto tracking = terra_session_tracking_for(session.id);
        send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"session", terra_session_json(session, tracking ? &*tracking : nullptr)}});
        return;
      }
    }
    send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "session_not_found", "Session does not exist or is not visible to this client");
  }

  /**
   * @brief Close any live forwarding channels for claims bound to one target.
   *
   * @param target_type Target type filter.
   * @param target_id Canonical target UUID.
   */
  void terra_close_target_channels(const std::string &target_type, const std::string &target_id) {
    if (!terra_peripheral_manager) {
      return;
    }
    for (const auto &claim : terra_peripheral_manager->list_claims({})) {
      if (claim.target.type == target_type && claim.target.id == target_id) {
        terra_close_peripheral_channel(claim.id);
      }
    }
  }

  /**
   * @brief Close any live forwarding channels owned by one client.
   *
   * @param owner_uuid Canonical owner UUID.
   */
  void terra_close_owner_channels(const std::string &owner_uuid) {
    if (!terra_peripheral_manager) {
      return;
    }
    for (const auto &claim : terra_peripheral_manager->list_claims(owner_uuid)) {
      terra_close_peripheral_channel(claim.id);
    }
  }

  /**
   * @brief Advance the peripherals collection revision.
   */
  void terra_bump_peripheral_revision() {
    std::lock_guard lock {terra_peripheral_revision_mutex};
    ++terra_peripheral_collection_revision;
  }

  /**
   * @brief Publish one peripherals-v1 event to authorized subscribers.
   *
   * @param type Event type name.
   * @param data Event payload.
   */
  void publish_terra_peripheral_event(const std::string &type, nlohmann::json data) {
    publish_terra_event({type, data.contains("id") ? std::optional<std::string> {data.at("id").get<std::string>()} : std::nullopt, data.contains("revision") ? std::optional<std::uint64_t> {data.at("revision").get<std::uint64_t>()} : std::nullopt, std::move(data)}, "peripheral.forward");
  }

  /**
   * @brief Resolve caller visibility for peripheral resources.
   *
   * @param client Authenticated caller.
   * @return Owner filter; empty grants administrative visibility.
   */
  std::string terra_peripheral_owner_filter(const verified_client_t &client) {
    return scope_allowed(client, "host.control") ? std::string {} : std::string {client.uuid};
  }

  /**
   * @brief Report whether workspaces, launch profiles, or applications reference a profile.
   *
   * @param profile_id Canonical profile UUID.
   * @return True when at least one published reference resolves to the profile.
   */
  bool terra_profile_referenced(const std::string &profile_id) {
    if (profile_id.empty()) {
      return false;
    }
    if (terra_workspace_manager) {
      for (const auto &workspace : terra_workspace_manager->list().workspaces) {
        const auto &definition = workspace.definition;
        if (definition.display_profile_id == profile_id || definition.stream_profile_id == profile_id || definition.launch_profile_id == profile_id || definition.sandbox_profile_id == profile_id) {
          return true;
        }
      }
    }
    if (terra_profile_manager && terra_profile_manager->launch_profile_references(profile_id)) {
      return true;
    }
    for (const auto &app : proc::catalog_snapshot().apps) {
      const auto &metadata = app.terra_metadata;
      if (metadata.value("displayProfileId", "") == profile_id || metadata.value("streamProfileId", "") == profile_id || metadata.value("sandboxProfileId", "") == profile_id) {
        return true;
      }
      for (const auto &launch_profile : metadata.value("launchProfiles", nlohmann::json::array())) {
        if (launch_profile.value("id", "") == profile_id) {
          return true;
        }
      }
    }
    return false;
  }

  /**
   * @brief Return registered peripheral devices visible to the caller.
   */
  void terra_peripherals_list(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "peripheral.forward");
    if (!client) {
      return;
    }
    if (!terra_peripheral_manager) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Peripheral forwarding is unavailable");
      return;
    }
    std::uint64_t revision = 0;
    {
      std::lock_guard lock {terra_peripheral_revision_mutex};
      revision = terra_peripheral_collection_revision;
    }
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
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned peripheral revision");
        return;
      }
    }
    const bool changed = !since || *since != revision;
    nlohmann::json peripherals = nlohmann::json::array();
    if (changed) {
      for (const auto &[device, active_claim] : terra_peripheral_manager->list_devices(terra_peripheral_owner_filter(*client))) {
        peripherals.push_back(terra_peripherals::device_json(device, active_claim));
      }
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                         {"revision", revision},
                                                                         {"changed", changed},
                                                                         {"fullSnapshot", changed},
                                                                         {"peripherals", std::move(peripherals)},
                                                                       });
  }

  /**
   * @brief Parse a peripheral device registration body.
   *
   * @param response HTTPS response used for structured errors.
   * @param body Parsed request body.
   * @return Creation fields, or no value after sending an error.
   */
  std::optional<terra_peripherals::create_device_t> terra_peripheral_create_body(const resp_https_t &response, const nlohmann::json &body) {
    for (const char *field : {"class", "platformId", "name", "vendorId", "productId", "capabilities"}) {
      if (!body.contains(field)) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", std::format("{} is required", field));
        return std::nullopt;
      }
    }
    if (!body.at("class").is_string() || !body.at("platformId").is_string() || !body.at("name").is_string() || !body.at("vendorId").is_number_unsigned() || !body.at("productId").is_number_unsigned() || !body.at("capabilities").is_array()) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Peripheral fields have invalid types");
      return std::nullopt;
    }
    terra_peripherals::create_device_t creation;
    creation.device_class = body.at("class").get<std::string>();
    creation.platform_id = body.at("platformId").get<std::string>();
    creation.name = body.at("name").get<std::string>();
    creation.vendor_id = body.at("vendorId").get<std::uint32_t>();
    creation.product_id = body.at("productId").get<std::uint32_t>();
    if (body.contains("serial") && body.at("serial").is_string()) {
      creation.serial = body.at("serial").get<std::string>();
    }
    if (body.contains("reportDescriptorBase64") && body.at("reportDescriptorBase64").is_string()) {
      creation.report_descriptor_base64 = body.at("reportDescriptorBase64").get<std::string>();
    }
    for (const auto &capability : body.at("capabilities")) {
      if (!capability.is_string()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "capabilities must be strings");
        return std::nullopt;
      }
      creation.capabilities.push_back(capability.get<std::string>());
    }
    return creation;
  }

  /**
   * @brief Register one peripheral device descriptor.
   */
  void terra_peripherals_create(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "peripheral.forward");
    if (!client) {
      return;
    }
    if (!terra_peripheral_manager) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Peripheral forwarding is unavailable");
      return;
    }
    const auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    const auto creation = terra_peripheral_create_body(response, *body);
    if (!creation) {
      return;
    }
    const auto created = terra_peripheral_manager->create_device(client->uuid, *creation);
    if (created.status == terra_peripherals::status_t::invalid) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Peripheral registration fields are invalid");
      return;
    }
    if (created.status == terra_peripherals::status_t::unsupported_configuration || !created.resource) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Peripheral class or capabilities are not supported by this host");
      return;
    }
    terra_bump_peripheral_revision();
    publish_terra_peripheral_event("peripheral.added", terra_peripherals::device_json(*created.resource));
    send_terra_response(response, SimpleWeb::StatusCode::success_created, {{"peripheral", terra_peripherals::device_json(*created.resource)}});
  }

  /**
   * @brief Return one peripheral device resource.
   */
  void terra_peripheral_get(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "peripheral.forward");
    if (!client) {
      return;
    }
    if (!terra_peripheral_manager) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Peripheral forwarding is unavailable");
      return;
    }
    const auto found = terra_peripheral_manager->get_device(terra_peripheral_owner_filter(*client), request->path_match[1].str());
    if (!found) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "peripheral_not_found", "Peripheral does not exist or is not visible to this client");
      return;
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"peripheral", terra_peripherals::device_json(found->first, found->second)}});
  }

  /**
   * @brief Unregister one peripheral device and release its claims.
   */
  void terra_peripheral_delete(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "peripheral.forward");
    if (!client) {
      return;
    }
    if (!terra_peripheral_manager) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Peripheral forwarding is unavailable");
      return;
    }
    const auto removed = terra_peripheral_manager->delete_device(terra_peripheral_owner_filter(*client), request->path_match[1].str());
    if (removed.status != terra_peripherals::status_t::success || !removed.resource) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "peripheral_not_found", "Peripheral does not exist or is not visible to this client");
      return;
    }
    terra_bump_peripheral_revision();
    publish_terra_peripheral_event("peripheral.removed", {{"id", removed.resource->id}, {"revision", removed.resource->revision + 1}});
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"deleted", true}, {"id", removed.resource->id}});
  }

  /**
   * @brief Return registered peripheral claims visible to the caller.
   */
  void terra_peripheral_claims_list(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "peripheral.forward");
    if (!client) {
      return;
    }
    if (!terra_peripheral_manager) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Peripheral forwarding is unavailable");
      return;
    }
    std::uint64_t revision = 0;
    {
      std::lock_guard lock {terra_peripheral_revision_mutex};
      revision = terra_peripheral_collection_revision;
    }
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
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "since must be an unsigned peripheral revision");
        return;
      }
    }
    const bool changed = !since || *since != revision;
    nlohmann::json claims = nlohmann::json::array();
    if (changed) {
      for (const auto &claim : terra_peripheral_manager->list_claims(terra_peripheral_owner_filter(*client))) {
        claims.push_back(terra_peripherals::claim_json(claim));
      }
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                         {"revision", revision},
                                                                         {"changed", changed},
                                                                         {"fullSnapshot", changed},
                                                                         {"claims", std::move(claims)},
                                                                       });
  }

  /**
   * @brief Create one peripheral claim bound to a visible target.
   */
  void terra_peripheral_claims_create(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "peripheral.forward");
    if (!client) {
      return;
    }
    if (!terra_peripheral_manager) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Peripheral forwarding is unavailable");
      return;
    }
    const auto body = terra_request_json(response, request);
    if (!body) {
      return;
    }
    for (const char *field : {"deviceId", "target", "requestedCapabilities", "exclusive", "disconnectPolicy"}) {
      if (!body->contains(field)) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", std::format("{} is required", field));
        return;
      }
    }
    if (!body->at("deviceId").is_string() || !body->at("target").is_object() || !body->at("target").contains("type") || !body->at("target").contains("id") || !body->at("target").at("type").is_string() || !body->at("target").at("id").is_string() || !body->at("requestedCapabilities").is_array() || !body->at("exclusive").is_boolean() || !body->at("disconnectPolicy").is_string()) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Claim fields have invalid types");
      return;
    }
    terra_peripherals::create_claim_t creation;
    creation.device_id = body->at("deviceId").get<std::string>();
    creation.target.type = body->at("target").at("type").get<std::string>();
    creation.target.id = body->at("target").at("id").get<std::string>();
    creation.exclusive = body->at("exclusive").get<bool>();
    creation.disconnect_policy = body->at("disconnectPolicy").get<std::string>();
    for (const auto &capability : body->at("requestedCapabilities")) {
      if (!capability.is_string()) {
        send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "requestedCapabilities must be strings");
        return;
      }
      creation.requested_capabilities.push_back(capability.get<std::string>());
    }
    if (!uuid_util::is_valid(creation.device_id) || !uuid_util::is_valid(creation.target.id)) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Claim identifiers must be canonical UUIDs");
      return;
    }

    // Bind to a visible target before granting anything.
    bool target_visible = false;
    if (creation.target.type == "session") {
      const auto owner = scope_allowed(*client, "host.control") ? std::string_view {} : std::string_view {client->uuid};
      target_visible = std::ranges::any_of(terra_session_snapshots(), [&](const auto &session) {
        return session.id == creation.target.id && (owner.empty() || session.client_uuid == owner);
      });
    }
#ifdef _WIN32
    if (creation.target.type == "workspace" && terra_workspace_manager) {
      const auto workspace = terra_workspace_manager->get(creation.target.id);
      target_visible = workspace && terra_workspace_visible(*client, *workspace, true);
    }
    if (creation.target.type == "sandbox" && terra_sandbox_manager) {
      const auto sandbox = terra_sandbox_manager->get(creation.target.id);
      target_visible = sandbox && terra_sandbox_visible(*client, *sandbox);
    }
#endif
    if (!target_visible) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "claim_target_not_found", "Claim target does not exist or is not visible to this client");
      return;
    }

    const auto created = terra_peripheral_manager->create_claim(client->uuid, creation);
    if (created.status == terra_peripherals::status_t::not_found) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "peripheral_not_found", "Peripheral does not exist or is not visible to this client");
      return;
    }
    if (created.status == terra_peripherals::status_t::conflict) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_conflict, "resource_busy", "An exclusive claim already exists for this peripheral");
      return;
    }
    if (created.status == terra_peripherals::status_t::invalid) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_bad_request, "invalid_argument", "Claim fields are invalid");
      return;
    }
    if (created.status != terra_peripherals::status_t::success || !created.resource) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_unprocessable_entity, "unsupported_configuration", "Requested claim capabilities are not supported by this host");
      return;
    }
    terra_bump_peripheral_revision();
    const auto device = terra_peripheral_manager->get_device({}, creation.device_id);
    if (device) {
      publish_terra_peripheral_event("peripheral.updated", terra_peripherals::device_json(device->first, device->second));
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_created, {{"claim", terra_peripherals::claim_json(*created.resource)}});
  }

  /**
   * @brief Return one peripheral claim resource.
   */
  void terra_peripheral_claim_get(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "peripheral.forward");
    if (!client) {
      return;
    }
    if (!terra_peripheral_manager) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Peripheral forwarding is unavailable");
      return;
    }
    const auto claim = terra_peripheral_manager->get_claim(terra_peripheral_owner_filter(*client), request->path_match[1].str());
    if (!claim) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "claim_not_found", "Claim does not exist or is not visible to this client");
      return;
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"claim", terra_peripherals::claim_json(*claim)}});
  }

  /**
   * @brief Release one peripheral claim idempotently.
   */
  void terra_peripheral_claim_delete(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "peripheral.forward");
    if (!client) {
      return;
    }
    if (!terra_peripheral_manager) {
      send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "feature_unavailable", "Peripheral forwarding is unavailable");
      return;
    }
    const auto released = terra_peripheral_manager->release_claim(terra_peripheral_owner_filter(*client), request->path_match[1].str());
    if (released.status != terra_peripherals::status_t::success || !released.resource) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "claim_not_found", "Claim does not exist or is not visible to this client");
      return;
    }
    terra_bump_peripheral_revision();
    publish_terra_peripheral_event("peripheral.updated", terra_peripherals::claim_json(*released.resource));
    if (released.resource->state == terra_peripherals::claim_state_t::released) {
      terra_close_peripheral_channel(request->path_match[1].str());
    }
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"released", true}, {"id", released.resource->id}});
  }

  /**
   * @brief Derive an RFC 6455 handshake accept key from a client key.
   *
   * @param key Raw `Sec-WebSocket-Key` header value.
   * @return Base64 accept key.
   */
  std::string websocket_accept_key(std::string_view key) {
    static const std::string_view GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string material;
    material.reserve(key.size() + GUID.size());
    material.append(key);
    material.append(GUID);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (!EVP_Digest(material.data(), material.size(), digest, &digest_length, EVP_sha1(), nullptr) || digest_length != 20) {
      return {};
    }
    return SimpleWeb::Crypto::Base64::encode(std::string {reinterpret_cast<const char *>(digest), digest_length});
  }

  /**
   * @brief Encode one unmasked server WebSocket frame.
   *
   * @param opcode Frame opcode.
   * @param payload Frame payload.
   * @return Complete encoded frame.
   */
  std::vector<std::uint8_t> websocket_frame(int opcode, std::string_view payload) {
    std::vector<std::uint8_t> frame;
    frame.push_back(static_cast<std::uint8_t>(0x80 | opcode));
    if (payload.size() < 126) {
      frame.push_back(static_cast<std::uint8_t>(payload.size()));
    } else if (payload.size() < 65536) {
      frame.push_back(126);
      frame.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF));
      frame.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
    } else {
      frame.push_back(127);
      for (int shift = 56; shift >= 0; shift -= 8) {
        frame.push_back(static_cast<std::uint8_t>((payload.size() >> shift) & 0xFF));
      }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
  }

  /**
   * @brief Decode one complete masked client WebSocket frame.
   *
   * @param frame Complete frame bytes.
   * @return Opcode and unmasked payload, or no value for malformed input.
   */
  std::optional<std::pair<int, std::string>> websocket_decode_frame(const std::vector<std::uint8_t> &frame) {
    if (frame.size() < 2) {
      return std::nullopt;
    }
    const bool masked = (frame[1] & 0x80) != 0;
    std::size_t length = frame[1] & 0x7F;
    std::size_t offset = 2;
    if (length == 126) {
      if (frame.size() < offset + 2) {
        return std::nullopt;
      }
      length = (static_cast<std::size_t>(frame[2]) << 8) | frame[3];
      offset += 2;
    } else if (length == 127) {
      if (frame.size() < offset + 8) {
        return std::nullopt;
      }
      length = 0;
      for (std::size_t index = 0; index < 8; ++index) {
        length = (length << 8) | frame[offset + index];
      }
      offset += 8;
    }
    if (!masked || frame.size() < offset + 4 + length) {
      return std::nullopt;
    }
    std::string payload(length, '\0');
    for (std::size_t index = 0; index < length; ++index) {
      payload[index] = static_cast<char>(frame[offset + 4 + index] ^ frame[offset + (index % 4)]);
    }
    return std::pair {frame[0] & 0x0F, std::move(payload)};
  }

  /**
   * @brief One live peripheral-forwarding WebSocket channel.
   */
  struct terra_peripheral_channel_t {
    std::shared_ptr<SolHTTPS> socket;  ///< Upgraded TLS socket.
    std::shared_ptr<input::input_t> input;  ///< Injection context bound to claim identity.
    std::vector<std::uint8_t> keyboard_state {};  ///< Last keyboard boot report.
    std::uint8_t mouse_buttons = 0;  ///< Last mouse button bitmap.
  };

  std::mutex terra_peripheral_channels_mutex;  ///< Protects the live channel registry.
  std::map<std::string, terra_peripheral_channel_t> terra_peripheral_channels;  ///< Live channels keyed by claim UUID.
  std::map<std::string, std::jthread> terra_peripheral_channel_threads;  ///< Channel worker threads keyed by claim UUID.

  /**
   * @brief Release channel resources after its worker thread ends.
   *
   * @param claim_id Canonical claim UUID.
   * @param socket Channel socket used for ownership comparison.
   */
  void terra_peripheral_channel_finish(const std::string &claim_id, const std::shared_ptr<SolHTTPS> &socket) {
    static_cast<void>(terra_peripheral_manager->close_channel(claim_id));
    terra_bump_peripheral_revision();
    std::jthread worker;
    {
      std::lock_guard lock {terra_peripheral_channels_mutex};
      const auto channel = terra_peripheral_channels.find(claim_id);
      if (channel != terra_peripheral_channels.end() && channel->second.socket == socket) {
        terra_peripheral_channels.erase(channel);
      }
      const auto thread = terra_peripheral_channel_threads.find(claim_id);
      if (thread != terra_peripheral_channel_threads.end()) {
        worker = std::move(thread->second);
        terra_peripheral_channel_threads.erase(thread);
      }
    }
    if (worker.joinable()) {
      // The worker jthread wraps the calling thread; detaching avoids self-join.
      worker.detach();
    }
  }

  /**
   * @brief Translate one HID keyboard usage byte into a Windows virtual-key code.
   *
   * @param usage HID usage identifier 0x04 through 0xE7.
   * @return Virtual-key code, or zero when the usage has no mapping.
   */
  std::uint16_t terra_hid_usage_to_vk(std::uint8_t usage) {
    if (usage >= 0x04 && usage <= 0x1D) {
      return usage - 0x04 + 'A';
    }
    if (usage >= 0x1E && usage <= 0x26) {
      return usage - 0x1E + '1';
    }
    if (usage == 0x27) {
      return '0';
    }
    if (usage >= 0x3A && usage <= 0x45) {
      return usage - 0x3A + VK_F1;
    }
    if (usage >= 0x54 && usage <= 0x63) {
      return usage - 0x54 + VK_NUMPAD0;
    }
    switch (usage) {
      case 0x28:
        return VK_RETURN;
      case 0x29:
        return VK_ESCAPE;
      case 0x2A:
        return VK_BACK;
      case 0x2B:
        return VK_TAB;
      case 0x2C:
        return VK_SPACE;
      case 0x2D:
        return VK_OEM_MINUS;
      case 0x2E:
        return VK_OEM_PLUS;
      case 0x2F:
        return VK_OEM_4;
      case 0x30:
        return VK_OEM_6;
      case 0x31:
        return VK_OEM_5;
      case 0x33:
        return VK_OEM_1;
      case 0x34:
        return VK_OEM_7;
      case 0x35:
        return VK_OEM_3;
      case 0x36:
        return VK_OEM_COMMA;
      case 0x37:
        return VK_OEM_PERIOD;
      case 0x38:
        return VK_OEM_2;
      case 0x39:
        return VK_CAPITAL;
      case 0x46:
        return VK_PRIOR;
      case 0x47:
        return VK_HOME;
      case 0x48:
        return VK_UP;
      case 0x49:
        return VK_NEXT;
      case 0x4A:
        return VK_LEFT;
      case 0x4B:
        return VK_DOWN;
      case 0x4C:
        return VK_DELETE;
      case 0x4D:
        return VK_RIGHT;
      case 0x4E:
        return VK_END;
      case 0x4F:
        return VK_NEXT;
      case 0x50:
        return VK_LEFT;
      case 0x51:
        return VK_DOWN;
      case 0x52:
        return VK_UP;
      default:
        return 0;
    }
  }

  /**
   * @brief Apply one HID keyboard boot report by injecting key and modifier diffs.
   *
   * @param channel Live channel owning the previous report state.
   * @param report Eight-byte HID boot keyboard report.
   */
  void terra_inject_hid_keyboard(terra_peripheral_channel_t &channel, const std::vector<std::uint8_t> &report) {
    static constexpr std::uint8_t HID_MODIFIERS[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
    const auto &previous = channel.keyboard_state;
    std::uint8_t modifiers = 0;
    for (std::size_t bit = 0; bit < 8; ++bit) {
      if (report[0] & (1u << bit)) {
        modifiers = HID_MODIFIERS[bit];
      }
    }
    if (previous.size() == report.size()) {
      for (std::size_t index = 2; index < report.size(); ++index) {
        const auto usage = previous[index];
        const bool still_down = usage != 0 && std::ranges::find(report.begin() + 2, report.end(), usage) != report.end();
        if (!still_down) {
          const auto key = terra_hid_usage_to_vk(usage);
          if (key) {
            input::peripheral_forward_keyboard(channel.input, key, modifiers, true);
          }
        }
      }
    }
    for (std::size_t index = 2; index < report.size(); ++index) {
      const auto usage = report[index];
      if (usage == 0) {
        break;
      }
      const bool was_down = previous.size() == report.size() && std::ranges::find(previous.begin() + 2, previous.end(), usage) != previous.end();
      if (!was_down) {
        const auto key = terra_hid_usage_to_vk(usage);
        if (key) {
          input::peripheral_forward_keyboard(channel.input, key, modifiers, false);
        }
      }
    }
    channel.keyboard_state = report;
  }

  /**
   * @brief Apply one HID mouse boot report by injecting button and motion diffs.
   *
   * @param channel Live channel owning the previous button state.
   * @param report Four-byte HID boot mouse report.
   */
  void terra_inject_hid_mouse(terra_peripheral_channel_t &channel, const std::vector<std::uint8_t> &report) {
    static constexpr std::uint8_t HID_BUTTONS[3] = {0x01, 0x02, 0x04};
    static constexpr std::uint8_t NV_BUTTONS[3] = {1, 2, 3};
    const auto buttons = report[0];
    for (std::size_t index = 0; index < 3; ++index) {
      const bool down = (buttons & HID_BUTTONS[index]) != 0;
      const bool was_down = (channel.mouse_buttons & HID_BUTTONS[index]) != 0;
      if (down != was_down) {
        input::peripheral_forward_mouse_button(channel.input, NV_BUTTONS[index], !down);
      }
    }
    channel.mouse_buttons = buttons;
    const auto delta_x = static_cast<std::int8_t>(report[1]);
    const auto delta_y = static_cast<std::int8_t>(report[2]);
    if (delta_x != 0 || delta_y != 0) {
      input::peripheral_forward_mouse_move(channel.input, delta_x, delta_y);
    }
    if (report.size() > 3 && report[3] != 0) {
      input::peripheral_forward_scroll(channel.input, static_cast<std::int8_t>(report[3]) > 0 ? 1 : -1);
    }
  }

  /**
   * @brief Write one text frame to a channel socket.
   *
   * @param socket Upgraded TLS socket.
   * @param payload JSON text payload.
   * @return True when the frame was written completely.
   */
  bool terra_channel_write(const std::shared_ptr<SolHTTPS> &socket, std::string_view payload) {
    boost::system::error_code error;
    const auto frame = websocket_frame(0x1, payload);
    asio::write(*socket, asio::buffer(frame), error);
    return !error;
  }

  /**
   * @brief Send a close frame and shut down the channel socket.
   *
   * @param socket Upgraded TLS socket.
   * @param code RFC 6455 close status code.
   */
  void terra_channel_close(const std::shared_ptr<SolHTTPS> &socket, std::uint16_t code) {
    const std::string payload {static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
    boost::system::error_code error;
    const auto frame = websocket_frame(0x8, payload);
    asio::write(*socket, asio::buffer(frame), error);
    error.clear();
    socket->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, error);
  }

  /**
   * @brief Read exactly the requested byte count from a channel socket.
   *
   * @param socket Upgraded TLS socket.
   * @param size Byte count to read.
   * @return Bytes read, or empty on EOF or error.
   */
  std::vector<std::uint8_t> terra_channel_read_exact(const std::shared_ptr<SolHTTPS> &socket, std::size_t size) {
    std::vector<std::uint8_t> buffer(size);
    std::size_t total = 0;
    while (total < size) {
      boost::system::error_code error;
      const auto read = asio::read(*socket, asio::buffer(buffer.data() + total, size - total), error);
      if (error) {
        return {};
      }
      total += read;
    }
    return buffer;
  }

  /**
   * @brief Read one complete WebSocket frame from a channel socket.
   *
   * Control frames are answered inline; text frames are returned.
   *
   * @param socket Upgraded TLS socket.
   * @return Opcode and raw (still masked) payload bytes, or empty on EOF or protocol failure.
   */
  std::optional<std::pair<int, std::vector<std::uint8_t>>> terra_channel_read_frame(const std::shared_ptr<SolHTTPS> &socket) {
    const auto header = terra_channel_read_exact(socket, 2);
    if (header.size() < 2) {
      return std::nullopt;
    }
    const int opcode = header[0] & 0x0F;
    const bool masked = (header[1] & 0x80) != 0;
    std::size_t length = header[1] & 0x7F;
    if (length == 126) {
      const auto extended = terra_channel_read_exact(socket, 2);
      if (extended.size() < 2) {
        return std::nullopt;
      }
      length = (static_cast<std::size_t>(extended[0]) << 8) | extended[1];
    } else if (length == 127) {
      const auto extended = terra_channel_read_exact(socket, 8);
      if (extended.size() < 8) {
        return std::nullopt;
      }
      length = 0;
      for (std::size_t index = 0; index < 8; ++index) {
        length = (length << 8) | extended[index];
      }
    }
    if (length > 64 * 1024) {
      terra_channel_close(socket, 1009);
      return std::nullopt;
    }
    std::vector<std::uint8_t> mask;
    if (masked) {
      mask = terra_channel_read_exact(socket, 4);
      if (mask.size() < 4) {
        return std::nullopt;
      }
    }
    auto payload = terra_channel_read_exact(socket, length);
    if (payload.size() < length) {
      return std::nullopt;
    }
    if (masked) {
      for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] ^= mask[index % 4];
      }
    }
    if (opcode == 0x9) {
      const auto pong = websocket_frame(0xA, std::string_view {reinterpret_cast<const char *>(payload.data()), payload.size()});
      boost::system::error_code error;
      asio::write(*socket, asio::buffer(pong), error);
      return terra_channel_read_frame(socket);
    }
    return std::pair {opcode, std::move(payload)};
  }

  /**
   * @brief Close the live channel for one claim, if any.
   *
   * @param claim_id Canonical claim UUID.
   */
  void terra_close_peripheral_channel(const std::string &claim_id) {
    std::shared_ptr<SolHTTPS> socket;
    {
      std::lock_guard lock {terra_peripheral_channels_mutex};
      const auto found = terra_peripheral_channels.find(claim_id);
      if (found == terra_peripheral_channels.end()) {
        return;
      }
      socket = found->second.socket;
    }
    terra_channel_close(socket, 1000);
  }

  /**
   * @brief Run one authenticated peripheral-forwarding channel to completion.
   *
   * Exchanges the `eclipse-peripheral-json` v1 protocol: `claim.open` activates the
   * claim, `hid.input` reports inject forwarded keyboard and mouse state, and
   * `claim.closed` ends the channel. Closing applies the claim disconnect policy.
   *
   * @param claim_id Canonical claim UUID.
   * @param socket Upgraded TLS socket.
   */
  void terra_peripheral_channel_loop(const std::string &claim_id, std::shared_ptr<SolHTTPS> socket) {
    bool activated = false;
    while (true) {
      const auto frame = terra_channel_read_frame(socket);
      if (!frame) {
        break;
      }
      const auto &[opcode, payload] = *frame;
      if (opcode == 0x8) {
        break;
      }
      if (opcode != 0x1) {
        terra_channel_close(socket, 1002);
        break;
      }
      nlohmann::json message;
      try {
        message = nlohmann::json::parse(payload);
      } catch (const std::exception &) {
        terra_channel_close(socket, 1002);
        break;
      }
      const auto type = message.value("type", "");
      if (type == "claim.open") {
        std::shared_ptr<input::input_t> injected_input;
        if (message.value("payload", nlohmann::json::object()).value("claimId", "") != claim_id) {
          terra_channel_close(socket, 1002);
          break;
        }
        {
          std::lock_guard lock {terra_peripheral_channels_mutex};
          injected_input = terra_peripheral_channels[claim_id].input;
        }
        if (activated || !injected_input) {
          terra_channel_close(socket, 1002);
          break;
        }
        const auto opened = terra_peripheral_manager->open_channel(claim_id);
        if (opened.status != terra_peripherals::status_t::success || !opened.resource) {
          terra_channel_close(socket, 1008);
          break;
        }
        activated = true;
        terra_bump_peripheral_revision();
        nlohmann::json granted = nlohmann::json::array();
        for (const auto &capability : opened.resource->granted_capabilities) {
          granted.push_back(capability);
        }
        const nlohmann::json ready {{"schemaVersion", 1}, {"sequence", 1}, {"type", "claim.ready"}, {"deviceId", opened.resource->device_id}, {"payload", {{"claimId", claim_id}, {"grantedCapabilities", granted}}}};
        if (!terra_channel_write(socket, ready.dump())) {
          break;
        }
        const nlohmann::json attached {{"schemaVersion", 1}, {"sequence", 2}, {"type", "device.attached"}, {"deviceId", opened.resource->device_id}, {"payload", {{"claimId", claim_id}}}};
        static_cast<void>(terra_channel_write(socket, attached.dump()));
        continue;
      }
      if (type == "hid.input") {
        const auto payload_object = message.value("payload", nlohmann::json::object());
        const auto encoded = payload_object.value("dataBase64", "");
        if (encoded.empty() || !activated) {
          continue;
        }
        std::vector<std::uint8_t> report;
        try {
          const auto decoded = SimpleWeb::Crypto::Base64::decode(encoded);
          report.assign(decoded.begin(), decoded.end());
        } catch (const std::exception &) {
          terra_channel_close(socket, 1002);
          break;
        }
        const auto device = terra_peripheral_manager->get_claim({}, claim_id);
        if (device && device->device_class == "keyboard" && report.size() >= 8) {
          std::lock_guard lock {terra_peripheral_channels_mutex};
          auto &channel = terra_peripheral_channels[claim_id];
          if (channel.input) {
            terra_inject_hid_keyboard(channel, report);
          }
        } else if (device && device->device_class == "mouse" && report.size() >= 3) {
          std::lock_guard lock {terra_peripheral_channels_mutex};
          auto &channel = terra_peripheral_channels[claim_id];
          if (channel.input) {
            terra_inject_hid_mouse(channel, report);
          }
        }
        continue;
      }
      if (type == "device.error") {
        BOOST_LOG(warning) << "Peripheral claim [" << claim_id << "] reported a device error";
        continue;
      }
      if (type == "claim.closed") {
        break;
      }
      terra_channel_close(socket, 1002);
      break;
    }
    terra_peripheral_channel_finish(claim_id, socket);
  }

  /**
   * @brief Authorize and upgrade one peripheral-forwarding WebSocket request.
   *
   * @param socket Upgraded TLS connection socket.
   * @param request Original upgrade request.
   */
  void terra_peripheral_channel_upgrade(std::unique_ptr<SolHTTPS> &socket, std::shared_ptr<SimpleWeb::ServerBase<SolHTTPS>::Request> request) {
    static const std::regex CHANNEL_PATTERN {"^/eclipse/v1/peripherals/claims/([0-9a-fA-F-]+)/channel$"};
    const std::string path = request->path;
    std::smatch match;
    const auto client = verified_client(request);
    const auto token = terra_header(request, "X-Eclipse-Claim-Token");
    std::string claim_id;
    if (std::regex_match(path, match, CHANNEL_PATTERN)) {
      claim_id = match[1].str();
    }
    const auto deny = [&](std::string_view code, std::string_view message) {
      const nlohmann::json body {{"schemaVersion", 1}, {"error", {{"code", code}, {"message", message}}}};
      const std::string text = body.dump();
      const std::string response = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(text.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + text;
      boost::system::error_code write_error;
      asio::write(*socket, asio::buffer(response), write_error);
    };
    if (!client || !token) {
      deny("authentication_required", "Peripheral channel requires a paired client and claim credential");
      return;
    }
    if (claim_id.empty() || !terra_peripheral_manager || !terra_peripheral_manager->authenticate_claim(claim_id, *token)) {
      deny("claim_not_found", "Claim does not exist or credential is invalid");
      return;
    }
    if (!scope_allowed(*client, "peripheral.forward")) {
      deny("permission_denied", "Client certificate lacks peripheral.forward permission");
      return;
    }
    const auto key_header = request->header.find("Sec-WebSocket-Key");
    if (key_header == request->header.end()) {
      return;
    }
    const std::string handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + websocket_accept_key(key_header->second) + "\r\n\r\n";
    boost::system::error_code error;
    asio::write(*socket, asio::buffer(handshake), error);
    if (error) {
      return;
    }
    auto channel = std::make_shared<terra_peripheral_channel_t>();
    channel->socket = std::shared_ptr<SolHTTPS>(socket.release());
    const auto claim = terra_peripheral_manager->authenticate_claim(claim_id, *token);
    if (claim && claim->owner_client_uuid) {
      std::lock_guard clients_lock {client_auth_mutex};
      const auto found = std::find_if(client_root.named_devices.begin(), client_root.named_devices.end(), [&](const auto &entry) {
        return entry.uuid == *claim->owner_client_uuid;
      });
      if (found != client_root.named_devices.end()) {
        channel->input = input::alloc(std::make_shared<safe::mail_raw_t>(), "terra-claim-" + claim_id, found->permissions.input);
      }
    }
    std::jthread previous_worker;
    {
      std::lock_guard lock {terra_peripheral_channels_mutex};
      const auto previous_socket = terra_peripheral_channels[claim_id].socket;
      if (previous_socket) {
        terra_channel_close(previous_socket, 1000);
      }
      terra_peripheral_channels[claim_id] = *channel;
      const auto thread = terra_peripheral_channel_threads.find(claim_id);
      if (thread != terra_peripheral_channel_threads.end()) {
        previous_worker = std::move(thread->second);
        terra_peripheral_channel_threads.erase(thread);
      }
      terra_peripheral_channel_threads.emplace(claim_id, std::jthread([claim_id, owned = channel->socket]() {
                                                   platf::set_thread_name("terra-claim");
                                                   terra_peripheral_channel_loop(claim_id, owned);
                                                 }));
    }
    if (previous_worker.joinable()) {
      previous_worker.join();
    }
  }

  void terra_disconnect_session(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "session.control");
    if (!client) {
      return;
    }
    const auto session_id = request->path_match[1].str();
    const auto owner = scope_allowed(*client, "host.control") ? std::string_view {} : std::string_view {client->uuid};
    const auto sessions = terra_session_snapshots();
    const bool visible = std::ranges::any_of(sessions, [&](const auto &session) {
      return session.id == session_id && (owner.empty() || session.client_uuid == owner);
    });
    if (!visible) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "session_not_found", "Session does not exist or is not owned by this client");
      return;
    }
    static_cast<void>(rtsp_stream::terminate_session(session_id, owner));
    if (terra_peripheral_manager) {
      terra_peripheral_manager->target_ended("session", session_id, false);
    }
#ifdef _WIN32
    if (terra_workspace_manager) {
      for (const auto &workspace : terra_workspace_manager->list().workspaces) {
        if (workspace.session_id == session_id && workspace.definition.cleanup_policy == terra_workspaces::cleanup_policy_t::on_disconnect) {
          const auto stopped = terra_workspace_manager->stop(workspace.id, workspace.revision, true);
          if (stopped.status != terra_workspaces::status_t::success) {
            send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "Workspace resources could not be cleaned after disconnect");
            return;
          }
          if (terra_peripheral_manager) {
            terra_peripheral_manager->target_ended("workspace", workspace.id, true);
          }
          break;
        }
      }
    }
#endif
    BOOST_LOG(info) << "Audit: client ["sv << client->uuid << "] disconnected session ["sv << session_id << ']';
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"disconnected", true}, {"sessionId", session_id}});
  }

  /**
   * @brief Stop one logical session and its host application.
   */
  void terra_stop_session(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "session.control");
    if (!client) {
      return;
    }
    const auto session_id = request->path_match[1].str();
    const auto owner = scope_allowed(*client, "host.control") ? std::string_view {} : std::string_view {client->uuid};
    const auto sessions = terra_session_snapshots();
    const bool visible = std::ranges::any_of(sessions, [&](const auto &session) {
      return session.id == session_id && (owner.empty() || session.client_uuid == owner);
    });
    if (!visible) {
      send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "session_not_found", "Session does not exist or is not owned by this client");
      return;
    }
    static_cast<void>(rtsp_stream::terminate_session(session_id, owner));
    if (terra_peripheral_manager) {
      terra_peripheral_manager->target_ended("session", session_id, true);
    }
#ifdef _WIN32
    if (terra_workspace_manager) {
      for (const auto &workspace : terra_workspace_manager->list().workspaces) {
        if (workspace.session_id == session_id) {
          const auto stopped = terra_workspace_manager->stop(workspace.id, workspace.revision, true);
          if (stopped.status != terra_workspaces::status_t::success) {
            send_terra_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "provider_unavailable", "Workspace resources could not be stopped");
            return;
          }
          if (terra_peripheral_manager) {
            terra_peripheral_manager->target_ended("workspace", workspace.id, true);
          }
          break;
        }
      }
    }
#endif
    {
      std::lock_guard lock {logical_session_mutex};
      if (logical_session && logical_session->id == session_id && (owner.empty() || logical_session->client_uuid == owner)) {
        if (proc::proc.running() > 0) {
          proc::proc.terminate();
        }
        logical_session.reset();
      }
    }
    {
      const auto binding = terra_session_tracking_for(session_id);
      if (binding && binding->binding.workspace_id.empty()) {
        terra_destroy_session_sandbox(binding->binding);
      }
    }
    display_device::revert_configuration();
    BOOST_LOG(info) << "Audit: client ["sv << client->uuid << "] stopped session ["sv << session_id << ']';
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, {{"stopped", true}, {"sessionId", session_id}});
  }

  /**
   * @brief Return host telemetry and telemetry for visible sessions.
   */
  void terra_telemetry(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "telemetry.read");
    if (!client) {
      return;
    }
    const bool administer = scope_allowed(*client, "host.control");
    send_terra_response(response, SimpleWeb::StatusCode::success_ok, terra_telemetry_document(administer ? std::string {} : client->uuid));
  }

  /**
   * @brief Return telemetry for one visible session.
   */
  void terra_session_telemetry(resp_https_t response, req_https_t request) {
    const auto client = authorize_terra_request(response, request, "telemetry.read");
    if (!client) {
      return;
    }
    const auto session_id = request->path_match[1].str();
    const bool administer = scope_allowed(*client, "host.control");
    for (const auto &session : terra_session_snapshots()) {
      if (session.id != session_id || (!administer && session.client_uuid != client->uuid)) {
        continue;
      }
      const auto tracking = terra_session_tracking_for(session.id);
      const auto document = terra_telemetry_document(std::string {});
      send_terra_response(response, SimpleWeb::StatusCode::success_ok, {
                                                                           {"timestamp", document.at("timestamp")},
                                                                           {"session", terra_telemetry_session_json(session, tracking ? &*tracking : nullptr)},
                                                                         });
      return;
    }
    send_terra_error(response, SimpleWeb::StatusCode::client_error_not_found, "session_not_found", "Session does not exist or is not visible to this client");
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

  /**
   * @brief Run the production paired HTTPS and discovery HTTP servers until shutdown.
   *
   * Creates every Terra manager, registers all routes, serves clients, runs the
   * periodic change monitor, and tears everything down before returning.
   *
   * @param port_http_override Discovery HTTP port, or no value for the configured port.
   * @param port_https_override Paired HTTPS port, or no value for the configured port.
   * @param https_ready Receives the bound HTTPS port after listening starts.
   * @param external_stop When set, requests shutdown in addition to the global shutdown event.
   */
  void run_nvhttp_servers(const std::optional<unsigned short> &port_http_override, const std::optional<unsigned short> &port_https_override, const std::function<void(unsigned short)> &https_ready, const std::atomic_bool &external_stop) {
    platf::set_thread_name("nvhttp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    terra_operation_pool.start(1);

    auto port_http = port_http_override.value_or(net::map_port(PORT_HTTP));
    auto port_https = port_https_override.value_or(net::map_port(PORT_HTTPS));
    auto address_family = net::af_from_enum_string(config::sol.address_family);

    bool clean_slate = config::sol.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    terra_event_hub = std::make_unique<terra_events::hub_t>(2048, []() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    });
    const auto operation_path = platf::appdata() / "eclipse_operations.json";
    terra_operation_store = std::make_unique<terra_operations::store_t>(terra_operations::callbacks_t {
      [operation_path, clean_slate]() -> std::optional<std::string> {
        if (clean_slate || !fs::exists(operation_path)) {
          return std::nullopt;
        }
        return file_handler::read_file(operation_path.string().c_str());
      },
      [operation_path](const std::string &document) {
        return file_handler::write_file_atomic(operation_path.string().c_str(), document) == 0;
      },
      {},
      {},
      [](const terra_operations::operation_t &operation) {
        if (!terra_operation_pool.running()) {
          return;
        }
        terra_operation_pool.push([operation]() {
          const auto scope = terra_operation_scope(operation.action);
          if (!scope.empty()) {
            publish_terra_event({"operation.updated", operation.id, operation.revision, terra_operations::to_json(operation)}, scope, operation.client_uuid);
          }
        });
      },
    });
#ifdef _WIN32
    const auto sandbox_path = platf::appdata() / "eclipse_sandboxes.json";
    if (clean_slate) {
      std::error_code error;
      fs::remove(sandbox_path, error);
    }
    auto sandbox_callbacks = terra::windows::sandbox::make_callbacks({sandbox_path, terra_resolve_sandbox_application, {}});
    const auto profile_path = platf::appdata() / "eclipse_profiles.json";
    if (clean_slate) {
      std::error_code error;
      fs::remove(profile_path, error);
    }
    terra_profile_manager = std::make_unique<terra::profiles::manager_t>(terra::profiles::callbacks_t {
      [profile_path]() -> std::optional<std::string> {
        if (!fs::exists(profile_path)) {
          return std::nullopt;
        }
        return file_handler::read_file(profile_path.string().c_str());
      },
      [profile_path](const std::string &document) {
        return file_handler::write_file_atomic(profile_path.string().c_str(), document) == 0;
      },
      {},
      []() {
        return uuid_util::uuid_t::generate().string();
      },
      [](const std::string &kind, const std::string &id) {
        if (kind != "application") {
          return terra::profiles::status_t::invalid;
        }
        const auto &apps = proc::proc.get_apps();
        return std::ranges::find(apps, id, &proc::ctx_t::uuid) == apps.end() ? terra::profiles::status_t::not_found : terra::profiles::status_t::success;
      },
      [sandbox_capable = sandbox_callbacks.provider_capable](const std::string &type, const nlohmann::json &configuration) {
        if (type == "display") {
          return false;
        }
        if (type == "sandbox") {
          std::string reason;
          return sandbox_capable && sandbox_capable({"", {}, configuration, std::nullopt, false}, reason);
        }
        if (type == "launch") {
          return configuration.at("workingDirectory").is_null() && configuration.at("preLaunchPolicy").empty() && configuration.at("postExitPolicy").empty();
        }
        return type == "stream";
      },
      [](const terra::profiles::profile_t &profile) {
        return terra_profile_referenced(profile.id);
      },
    });
    sandbox_callbacks.on_change = [](const std::optional<terra_sandboxes::resource_t> &previous, const std::optional<terra_sandboxes::resource_t> &current) {
      const auto &sandbox = current ? *current : *previous;
      const auto owner = current && current->owner_client_uuid ? *current->owner_client_uuid : previous && previous->owner_client_uuid ? *previous->owner_client_uuid :
                                                                                                                                         std::string {};
      if (current) {
        publish_terra_sandbox_event(previous ? "sandbox.updated" : "sandbox.created", sandbox, terra_sandboxes::to_json(sandbox), owner);
      } else {
        publish_terra_sandbox_event("sandbox.removed", sandbox, {{"id", sandbox.id}, {"revision", sandbox.revision + 1}}, owner);
      }
    };
    terra_sandbox_manager = std::make_unique<terra_sandboxes::manager_t>(std::move(sandbox_callbacks));
    const auto workspace_path = platf::appdata() / "eclipse_workspaces.json";
    if (clean_slate) {
      std::error_code error;
      fs::remove(workspace_path, error);
    }
    terra_peripheral_manager = std::make_unique<terra_peripherals::manager_t>(
      []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      },
      []() {
        return uuid_util::uuid_t::generate().string();
      },
      []() {
        return uuid_util::uuid_t::generate().string() + uuid_util::uuid_t::generate().string();
      }
    );
    terra_workspace_manager = std::make_unique<terra_workspaces::manager_t>(terra_workspaces::callbacks_t {
      [workspace_path]() -> std::optional<std::string> {
        if (!fs::exists(workspace_path)) {
          return std::nullopt;
        }
        return file_handler::read_file(workspace_path.string().c_str());
      },
      [workspace_path](const std::string &document) {
        return file_handler::write_file_atomic(workspace_path.string().c_str(), document) == 0;
      },
      []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      },
      []() {
        return uuid_util::uuid_t::generate().string();
      },
      [](const std::string &id) {
        const auto &apps = proc::proc.get_apps();
        return std::ranges::find(apps, id, &proc::ctx_t::uuid) != apps.end();
      },
      [](const std::string &type, const std::string &id) {
        const auto profile = terra_profile_manager ? terra_profile_manager->inspect(id) : std::nullopt;
        return profile && profile->type == type && terra_profile_manager->validate_configuration(type, profile->configuration) == terra::profiles::status_t::success;
      },
      [](const std::string &type, const nlohmann::json &configuration) {
        return terra_profile_manager && terra_profile_manager->validate_configuration(type, configuration) == terra::profiles::status_t::success;
      },
      [](const nlohmann::json &) {
        return false;
      },
      [](const terra_workspaces::peripheral_policy_t &policy) {
        return policy.required_device_ids.empty() && policy.required_classes.empty();
      },
      [](const terra_workspaces::preparation_t &request) -> std::optional<terra_workspaces::prepared_t> {
        if (!request.definition.sandbox_profile_id || (request.profile_overrides.sandbox && request.profile_overrides.sandbox->is_null())) {
          return terra_workspaces::prepared_t {true};
        }
        const auto profile = terra_profile_manager ? terra_profile_manager->inspect(*request.definition.sandbox_profile_id) : std::nullopt;
        if (!profile || profile->type != "sandbox" || !terra_sandbox_manager) {
          return std::nullopt;
        }
        const auto &sandbox_configuration = request.profile_overrides.sandbox ? *request.profile_overrides.sandbox : profile->configuration;
        terra_sandboxes::start_t start {.app_uuid = request.app_uuid, .launch_profile_id = request.definition.launch_profile_id};
        if (request.profile_overrides.launch) {
          if (request.profile_overrides.launch->is_null()) {
            start.launch_profile_id.reset();
          } else {
            if (!request.definition.launch_profile_id || !terra_sandbox_launch_configuration_supported(*request.profile_overrides.launch, request.app_uuid, *request.definition.sandbox_profile_id)) {
              return std::nullopt;
            }
            const auto application = terra_resolve_sandbox_application(request.app_uuid, std::nullopt);
            if (!application) {
              return std::nullopt;
            }
            auto launch_data = application->launch_data;
            for (const auto &argument : request.profile_overrides.launch->at("arguments")) {
              launch_data["arguments"].push_back(argument);
            }
            launch_data["environment"] = request.profile_overrides.launch->at("environment");
            if (!request.profile_overrides.launch->at("workingDirectory").is_null()) {
              launch_data["workingDirectory"] = request.profile_overrides.launch->at("workingDirectory");
            }
            start.launch_data = std::move(launch_data);
          }
        } else if (request.definition.launch_profile_id) {
          const auto launch_profile = terra_profile_manager->inspect(*request.definition.launch_profile_id);
          if (!launch_profile || !terra_sandbox_launch_profile_supported(*launch_profile, request.app_uuid, *request.definition.sandbox_profile_id)) {
            return std::nullopt;
          }
        }
        const auto created = terra_sandbox_manager->create(request.owner_client_uuid, {
                                                                                          *request.definition.sandbox_profile_id,
                                                                                          request.workspace_id,
                                                                                          request.app_uuid,
                                                                                          request.definition.persistent,
                                                                                          request.definition.name,
                                                                                          sandbox_configuration,
                                                                                        });
        if (created.status != terra_sandboxes::status_t::success || !created.resource) {
          return std::nullopt;
        }
        const auto started = terra_sandbox_manager->start(created.resource->id, created.resource->revision, start);
        if (started.status != terra_sandboxes::status_t::success || !started.resource) {
          const auto cleaned = terra_destroy_workspace_sandbox(created.resource->id);
          return terra_workspaces::prepared_t {false, std::nullopt, cleaned ? std::nullopt : std::optional {created.resource->id}};
        }
        return terra_workspaces::prepared_t {true, std::nullopt, started.resource->id, started.resource->display_ids, started.resource->peripheral_claim_ids};
      },
      [](const terra_workspaces::prepared_t &runtime) {
        if (runtime.sandbox_id && !terra_destroy_workspace_sandbox(*runtime.sandbox_id)) {
          BOOST_LOG(error) << "Failed to clean workspace sandbox [" << *runtime.sandbox_id << ']';
        }
      },
      [](const terra_workspaces::resource_t &workspace, const bool terminate_application) {
        if (!workspace.display_ids.empty() || !workspace.peripheral_claim_ids.empty()) {
          return false;
        }
        bool stopped = true;
        if (workspace.session_id) {
          const auto owner = workspace.owner_client_uuid ? std::string_view {*workspace.owner_client_uuid} : std::string_view {};
          stopped = rtsp_stream::terminate_session(*workspace.session_id, owner);
          if (terminate_application) {
            std::lock_guard lock {logical_session_mutex};
            if (logical_session && logical_session->id == *workspace.session_id) {
              if (proc::proc.running() > 0) {
                proc::proc.terminate();
              }
              logical_session.reset();
              stopped = true;
            }
            display_device::revert_configuration();
          } else {
            std::lock_guard lock {logical_session_mutex};
            stopped = stopped || (logical_session && logical_session->id == *workspace.session_id);
          }
        }
        stopped = (!terminate_application || !workspace.sandbox_id || terra_stop_workspace_sandbox(*workspace.sandbox_id, true)) && stopped;
        return stopped;
      },
      [](const terra_workspaces::resource_t &workspace) {
        if (workspace.session_id || !workspace.display_ids.empty() || !workspace.peripheral_claim_ids.empty()) {
          return false;
        }
        if (!workspace.sandbox_id) {
          return true;
        }
        const auto sandbox = terra_sandbox_manager ? terra_sandbox_manager->get(*workspace.sandbox_id) : std::nullopt;
        if (!sandbox || !sandbox->app_uuid || sandbox->workspace_id != workspace.id) {
          return false;
        }
        if (sandbox->state == terra_sandboxes::state_t::running) {
          return true;
        }
        if (sandbox->state != terra_sandboxes::state_t::stopped && sandbox->state != terra_sandboxes::state_t::created) {
          return false;
        }
        if (workspace.definition.launch_profile_id) {
          const auto profile = terra_profile_manager ? terra_profile_manager->inspect(*workspace.definition.launch_profile_id) : std::nullopt;
          if (!profile || !terra_sandbox_launch_profile_supported(*profile, *sandbox->app_uuid, sandbox->profile_id)) {
            return false;
          }
        }
        const auto restarted = terra_sandbox_manager->start(sandbox->id, sandbox->revision, {*sandbox->app_uuid, workspace.definition.launch_profile_id});
        return restarted.status == terra_sandboxes::status_t::success;
      },
      [](const terra_workspaces::resource_t &workspace) {
        if (!workspace.display_ids.empty() || !workspace.peripheral_claim_ids.empty()) {
          return false;
        }
        if (workspace.sandbox_id) {
          const auto sandbox = terra_sandbox_manager ? terra_sandbox_manager->get(*workspace.sandbox_id) : std::nullopt;
          if (!sandbox || sandbox->workspace_id != workspace.id || sandbox->state != terra_sandboxes::state_t::running) {
            return false;
          }
        }
        if (workspace.state != terra_workspaces::state_t::active) {
          return true;
        }
        return workspace.session_id && std::ranges::any_of(terra_session_snapshots(), [&](const auto &session) {
                 return session.id == *workspace.session_id;
               });
      },
      [](const std::optional<terra_workspaces::resource_t> &previous, const std::optional<terra_workspaces::resource_t> &current) {
        if (previous && previous->sandbox_id && (!current || current->sandbox_id != previous->sandbox_id) && !terra_destroy_workspace_sandbox(*previous->sandbox_id)) {
          BOOST_LOG(error) << "Failed to remove committed workspace sandbox [" << *previous->sandbox_id << ']';
        }
        if (current) {
          if (!current->owner_client_uuid) {
            return;
          }
          publish_terra_workspace_event(previous ? "workspace.updated" : "workspace.created", *current, terra_workspaces::to_json(*current));
        } else if (previous) {
          publish_terra_workspace_event("workspace.removed", *previous, {{"id", previous->id}, {"revision", previous->revision + 1}});
        }
      },
    });
    const auto virtual_display_path = platf::appdata() / "eclipse_virtual_displays.json";
    if (clean_slate) {
      std::error_code error;
      fs::remove(virtual_display_path, error);
    }
    terra_virtual_display_manager = std::make_unique<terra_virtual_display::manager_t>(terra::windows::virtual_display::make_callbacks(virtual_display_path));
#endif

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

    https_server.default_resource["GET"] = not_found<SolHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SolHTTPS>;
    https_server.on_upgrade = terra_peripheral_channel_upgrade;
    https_server.resource["^/pair$"]["GET"] = [](auto resp, auto req) {
      pair<SolHTTPS>(resp, req);
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
    https_server.resource["^/eclipse/v1/capabilities$"]["GET"] = terra_capabilities;
    https_server.resource["^/eclipse/v1/events$"]["GET"] = terra_events_stream;
    https_server.resource["^/eclipse/v1/apps$"]["GET"] = terra_apps;
    https_server.resource["^/eclipse/v1/apps/([0-9a-fA-F-]+)/assets/([A-Za-z0-9._-]+)$"]["GET"] = terra_app_asset;
#ifdef _WIN32
    https_server.resource["^/eclipse/v1/profiles$"]["GET"] = terra_profiles;
    https_server.resource["^/eclipse/v1/profiles$"]["POST"] = terra_create_profile;
    https_server.resource["^/eclipse/v1/profiles/([0-9a-fA-F-]+)$"]["GET"] = terra_profile;
    https_server.resource["^/eclipse/v1/profiles/([0-9a-fA-F-]+)$"]["PATCH"] = terra_patch_profile;
    https_server.resource["^/eclipse/v1/profiles/([0-9a-fA-F-]+)$"]["DELETE"] = terra_delete_profile;
    https_server.resource["^/eclipse/v1/profiles/([0-9a-fA-F-]+)/adopt$"]["POST"] = terra_adopt_profile;
    https_server.resource["^/eclipse/v1/workspaces$"]["GET"] = terra_workspaces;
    https_server.resource["^/eclipse/v1/workspaces$"]["POST"] = terra_create_workspace;
    https_server.resource["^/eclipse/v1/workspaces/([0-9a-fA-F-]+)$"]["GET"] = terra_workspace;
    https_server.resource["^/eclipse/v1/workspaces/([0-9a-fA-F-]+)$"]["PATCH"] = terra_patch_workspace;
    https_server.resource["^/eclipse/v1/workspaces/([0-9a-fA-F-]+)$"]["DELETE"] = terra_delete_workspace;
    https_server.resource["^/eclipse/v1/workspaces/([0-9a-fA-F-]+)/start$"]["POST"] = terra_start_workspace;
    https_server.resource["^/eclipse/v1/workspaces/([0-9a-fA-F-]+)/stop$"]["POST"] = terra_stop_workspace;
    https_server.resource["^/eclipse/v1/workspaces/([0-9a-fA-F-]+)/adopt$"]["POST"] = terra_adopt_workspace;
    https_server.resource["^/eclipse/v1/sandboxes$"]["GET"] = terra_sandboxes_route;
    https_server.resource["^/eclipse/v1/sandboxes$"]["POST"] = terra_create_sandbox;
    https_server.resource["^/eclipse/v1/sandboxes/([0-9a-fA-F-]+)$"]["GET"] = terra_sandbox_route;
    https_server.resource["^/eclipse/v1/sandboxes/([0-9a-fA-F-]+)$"]["DELETE"] = terra_delete_sandbox;
    https_server.resource["^/eclipse/v1/sandboxes/([0-9a-fA-F-]+)/start$"]["POST"] = terra_start_sandbox;
    https_server.resource["^/eclipse/v1/sandboxes/([0-9a-fA-F-]+)/stop$"]["POST"] = terra_stop_sandbox;
    https_server.resource["^/eclipse/v1/sandboxes/([0-9a-fA-F-]+)/restart$"]["POST"] = terra_restart_sandbox;
    https_server.resource["^/eclipse/v1/sandboxes/([0-9a-fA-F-]+)/adopt$"]["POST"] = terra_adopt_sandbox;
    https_server.resource["^/eclipse/v1/displays$"]["GET"] = terra_displays;
    https_server.resource["^/eclipse/v1/displays/([0-9a-fA-F-]+)$"]["GET"] = terra_display;
    https_server.resource["^/eclipse/v1/displays/([0-9a-fA-F-]+)$"]["PATCH"] = terra_patch_display;
    https_server.resource["^/eclipse/v1/display-topology$"]["GET"] = terra_display_topology_get;
    https_server.resource["^/eclipse/v1/display-topology$"]["PUT"] = terra_display_topology_put;
    https_server.resource["^/eclipse/v1/virtual-displays$"]["GET"] = terra_virtual_displays;
    https_server.resource["^/eclipse/v1/virtual-displays$"]["POST"] = terra_create_virtual_display;
    https_server.resource["^/eclipse/v1/virtual-displays/([0-9a-fA-F-]+)$"]["GET"] = terra_virtual_display;
    https_server.resource["^/eclipse/v1/virtual-displays/([0-9a-fA-F-]+)$"]["PATCH"] = terra_patch_virtual_display;
    https_server.resource["^/eclipse/v1/virtual-displays/([0-9a-fA-F-]+)$"]["DELETE"] = terra_delete_virtual_display;
    https_server.resource["^/eclipse/v1/virtual-displays/([0-9a-fA-F-]+)/attach$"]["POST"] = terra_attach_virtual_display;
    https_server.resource["^/eclipse/v1/virtual-displays/([0-9a-fA-F-]+)/detach$"]["POST"] = terra_detach_virtual_display;
    https_server.resource["^/eclipse/v1/virtual-displays/([0-9a-fA-F-]+)/adopt$"]["POST"] = terra_adopt_virtual_display;
#endif
    https_server.resource["^/eclipse/v1/operations/([0-9a-fA-F-]+)$"]["GET"] = terra_operation;
    https_server.resource["^/eclipse/v1/peripherals$"]["GET"] = terra_peripherals_list;
    https_server.resource["^/eclipse/v1/peripherals$"]["POST"] = terra_peripherals_create;
    https_server.resource["^/eclipse/v1/peripherals/claims$"]["GET"] = terra_peripheral_claims_list;
    https_server.resource["^/eclipse/v1/peripherals/claims$"]["POST"] = terra_peripheral_claims_create;
    https_server.resource["^/eclipse/v1/peripherals/claims/([0-9a-fA-F-]+)$"]["GET"] = terra_peripheral_claim_get;
    https_server.resource["^/eclipse/v1/peripherals/claims/([0-9a-fA-F-]+)$"]["DELETE"] = terra_peripheral_claim_delete;
    https_server.resource["^/eclipse/v1/peripherals/([0-9a-fA-F-]+)$"]["GET"] = terra_peripheral_get;
    https_server.resource["^/eclipse/v1/peripherals/([0-9a-fA-F-]+)$"]["DELETE"] = terra_peripheral_delete;
    https_server.resource["^/eclipse/v1/sessions$"]["GET"] = terra_sessions;
    https_server.resource["^/eclipse/v1/telemetry$"]["GET"] = terra_telemetry;
    https_server.resource["^/eclipse/v1/sessions/([0-9a-fA-F-]+)/telemetry$"]["GET"] = terra_session_telemetry;
    https_server.resource["^/eclipse/v1/sessions/([0-9a-fA-F-]+)$"]["GET"] = terra_session;
    https_server.resource["^/eclipse/v1/sessions/([0-9a-fA-F-]+)/disconnect$"]["POST"] = terra_disconnect_session;
    https_server.resource["^/eclipse/v1/sessions/([0-9a-fA-F-]+)/stop$"]["POST"] = terra_stop_session;

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

    auto accept_and_run = [&](auto *server, const std::function<void()> &start_server) {
      try {
        std::string name = "nvhttp::" + std::to_string(server->config.port);
        platf::set_thread_name(name);
        start_server();
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
    std::jthread ssl {accept_and_run, &https_server, std::function<void()> {[&] {
                        https_server.start([&](const unsigned short port) {
                          if (https_ready) {
                            https_ready(port);
                          }
                        });
                      }}};
    std::jthread tcp {accept_and_run, &http_server, std::function<void()> {[&] {
                        http_server.start();
                      }}};

    std::jthread change_monitor([](const std::stop_token stop) {
      auto catalog_revision = proc::catalog_revision();
      std::set<std::string> expired_clients;
      std::uint64_t monitor_tick = 0;
      std::string host_name_state = config::nvhttp.sol_name;
      std::string capabilities_state;
#ifdef _WIN32
      std::uint64_t display_revision = 0;
      if (const auto snapshot = terra_display_snapshot()) {
        display_revision = snapshot->first;
      }
#endif
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds {250});
        ++monitor_tick;
        std::vector<std::pair<std::string, std::string>> newly_expired;
        {
          std::lock_guard lock {client_auth_mutex};
          for (const auto &client : client_root.named_devices) {
            if (permissions_expired(client.permissions)) {
              if (expired_clients.emplace(client.uuid).second) {
                newly_expired.emplace_back(client.uuid, client.cert);
              }
            } else {
              expired_clients.erase(client.uuid);
            }
          }
          if (!newly_expired.empty()) {
            std::erase_if(verified_clients, [&](const auto &entry) {
              return expired_clients.contains(entry.second.uuid);
            });
            rebuild_client_cert_chain();
          }
        }
        for (const auto &[client_uuid, certificate] : newly_expired) {
          if (terra_event_hub) {
            terra_event_hub->disconnect_client(client_uuid);
          }
          rtsp_stream::terminate_sessions_by_cert(certificate);
          if (terra_peripheral_manager) {
            terra_peripheral_manager->revoke_owner(client_uuid);
          }
#ifdef _WIN32
          if (terra_virtual_display_manager) {
            static_cast<void>(terra_virtual_display_manager->revoke_owner(client_uuid));
          }
          revoke_terra_sandboxes(client_uuid);
          revoke_terra_profiles(client_uuid);
          revoke_terra_workspaces(client_uuid);
#endif
        }
        const auto current_catalog_revision = proc::catalog_revision();
        if (current_catalog_revision != catalog_revision) {
          catalog_revision = current_catalog_revision;
          publish_terra_event({"catalog.changed", std::nullopt, catalog_revision, {{"revision", catalog_revision}}}, "catalog.read");
        }
#ifdef _WIN32
        if (terra_sandbox_manager) {
          const auto status = terra_sandbox_manager->reconcile();
          if (status != terra_sandboxes::status_t::success && status != terra_sandboxes::status_t::unavailable) {
            BOOST_LOG(error) << "Sandbox runtime reconciliation failed";
          }
        }
        if (const auto snapshot = terra_display_snapshot(); snapshot && snapshot->first != display_revision) {
          display_revision = snapshot->first;
          publish_terra_event({"displays.changed", std::nullopt, display_revision, {{"revision", display_revision}}}, "display.read");
        }
#endif
        if (config::nvhttp.sol_name != host_name_state) {
          host_name_state = config::nvhttp.sol_name;
          publish_terra_event({"host.changed", std::nullopt, std::nullopt, {{"name", host_name_state}}});
        }
        if (monitor_tick % 8 == 0) {
          bool sandbox_ok = true;
#ifdef _WIN32
          sandbox_ok = terra::windows::sandbox::health().available;
#endif
          nlohmann::json capabilities = nlohmann::json::array();
          for (const auto capability : terra_api::CAPABILITIES) {
            capabilities.push_back(capability);
          }
          if (sandbox_ok) {
            capabilities.push_back("sandboxes-v1");
          }
          if (terra_peripheral_manager) {
            capabilities.push_back("peripherals-v1");
          }
#ifdef _WIN32
          capabilities.push_back("displays-v1");
#endif
          std::string signature = capabilities.dump();
          if (capabilities_state.empty()) {
            capabilities_state = signature;
          } else if (signature != capabilities_state) {
            capabilities_state = signature;
            publish_terra_event({"capabilities.changed", std::nullopt, std::nullopt, {{"capabilities", std::move(capabilities)}}});
          }
        }
        {
          const auto snapshots = terra_session_snapshots();
          const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
          std::vector<std::pair<terra_events::event_t, std::pair<std::string, std::vector<std::string>>>> pending_events;
          std::vector<terra_session_binding_t> doomed_bindings;
          {
            std::lock_guard lock {terra_session_tracking_mutex};
            std::set<std::string> seen;
            for (const auto &session : snapshots) {
              seen.insert(session.id);
              auto entry = terra_session_tracking.find(session.id);
              if (entry == terra_session_tracking.end()) {
                entry = terra_session_tracking.emplace(session.id, terra_session_tracking_t {}).first;
                entry->second.owner_client_uuid = session.client_uuid;
                entry->second.app_uuid = session.app_uuid;
                entry->second.state = session.state;
                entry->second.updated_at = now_ms;
                entry->second.announced = true;
                ++terra_session_collection_revision;
              }
              entry->second.terminal_since.reset();
              if (entry->second.state != session.state) {
                entry->second.state = session.state;
                ++entry->second.revision;
                entry->second.updated_at = now_ms;
                ++terra_session_collection_revision;
                pending_events.emplace_back(
                  terra_events::event_t {"session.updated", session.id, entry->second.revision, terra_session_json(session, &entry->second)},
                  std::pair {session.client_uuid, std::vector<std::string> {session.app_uuid}}
                );
              }
            }
            for (auto entry = terra_session_tracking.begin(); entry != terra_session_tracking.end();) {
              if (seen.contains(entry->first)) {
                ++entry;
                continue;
              }
              const bool terminal = entry->second.state == "stopped" || entry->second.state == "failed";
              const auto event_apps = entry->second.app_uuid.empty() ? std::vector<std::string> {} : std::vector<std::string> {entry->second.app_uuid};
              const auto &event_owner = entry->second.owner_client_uuid;
              if (!terminal) {
                entry->second.state = "stopped";
                ++entry->second.revision;
                entry->second.updated_at = now_ms;
                entry->second.terminal_since = now_ms;
                ++terra_session_collection_revision;
                pending_events.emplace_back(
                  terra_events::event_t {"session.updated", entry->first, entry->second.revision, {{"id", entry->first}, {"state", "stopped"}, {"stateReason", "terminated"}, {"revision", entry->second.revision}}},
                  std::pair {event_owner, event_apps}
                );
                ++entry;
                continue;
              }
              if (!entry->second.terminal_since) {
                entry->second.terminal_since = now_ms;
                ++entry;
                continue;
              }
              if (now_ms - *entry->second.terminal_since < 60'000) {
                ++entry;
                continue;
              }
              if (entry->second.binding.workspace_id.empty()) {
                doomed_bindings.push_back(entry->second.binding);
              }
              pending_events.emplace_back(
                terra_events::event_t {"session.removed", entry->first, entry->second.revision + 1, {{"id", entry->first}, {"revision", entry->second.revision + 1}}},
                std::pair {event_owner, event_apps}
              );
              entry = terra_session_tracking.erase(entry);
              ++terra_session_collection_revision;
            }
          }
          for (const auto &[event, visibility] : pending_events) {
            const auto &[owner, apps] = visibility;
            publish_terra_event(event, "session.control", owner, apps);
          }
          for (const auto &binding : doomed_bindings) {
            terra_destroy_session_sandbox(binding);
          }
        }
        if (monitor_tick % 4 == 0) {
          for (const auto &recipient : terra_event_recipients("telemetry.read")) {
            if (!terra_event_hub) {
              continue;
            }
            std::string owner_filter;
            {
              std::lock_guard lock {client_auth_mutex};
              const auto found = std::find_if(client_root.named_devices.begin(), client_root.named_devices.end(), [&](const auto &entry) {
                return entry.uuid == recipient;
              });
              if (found != client_root.named_devices.end() && found->permissions.scopes.contains("host.control")) {
                owner_filter.clear();
              } else {
                owner_filter = recipient;
              }
            }
            try {
              terra_event_hub->publish({"telemetry.sample", std::nullopt, std::nullopt, terra_telemetry_document(owner_filter)}, {recipient});
            } catch (const std::exception &exception) {
              BOOST_LOG(error) << "Terra telemetry event publication failed: " << exception.what();
            }
          }
        }
      }
    });

    // Wait for any event
    while (!external_stop && !shutdown_event->peek()) {
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }

    change_monitor.request_stop();
    change_monitor.join();
    publish_terra_event({"host.stopping", std::nullopt, std::nullopt, nlohmann::json::object()});
    std::this_thread::sleep_for(std::chrono::milliseconds {50});
    terra_event_hub->disconnect_all();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
    terra_operation_pool.stop();
    terra_operation_pool.join();
    {
      std::lock_guard stream_lock {terra_event_stream_mutex};
      terra_event_streams.clear();
    }
  }

  void start() {
    run_nvhttp_servers(std::nullopt, std::nullopt, {}, {});
  }

  void erase_all_clients() {
    bool erased = false;
    std::vector<std::string> removed_owners;
    {
      std::lock_guard lock {client_auth_mutex};
      auto previous_clients = client_root;
      for (const auto &client : client_root.named_devices) {
        removed_owners.push_back(client.uuid);
      }
      client_root = {};
      cert_chain.clear();
      verified_clients.clear();
      if (!save_state()) {
        client_root = std::move(previous_clients);
        rebuild_client_cert_chain();
        removed_owners.clear();
      } else {
        erased = true;
      }
    }
    if (erased) {
      if (terra_event_hub) {
        for (const auto &owner : removed_owners) {
          terra_event_hub->disconnect_client(owner);
        }
      }
      rtsp_stream::terminate_sessions();
      if (terra_peripheral_manager) {
        for (const auto &owner : removed_owners) {
          terra_peripheral_manager->revoke_owner(owner);
        }
      }
#ifdef _WIN32
      if (terra_virtual_display_manager) {
        for (const auto &owner : removed_owners) {
          static_cast<void>(terra_virtual_display_manager->revoke_owner(owner));
        }
      }
      for (const auto &owner : removed_owners) {
        revoke_terra_sandboxes(owner);
        revoke_terra_profiles(owner);
        revoke_terra_workspaces(owner);
      }
#endif
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
      if (terra_event_hub) {
        terra_event_hub->disconnect_client(std::string {uuid});
      }
      rtsp_stream::terminate_sessions_by_cert(certificate);
#ifdef _WIN32
      if (terra_virtual_display_manager) {
        static_cast<void>(terra_virtual_display_manager->revoke_owner(std::string {uuid}));
      }
      revoke_terra_sandboxes(std::string {uuid});
      revoke_terra_profiles(std::string {uuid});
      revoke_terra_workspaces(std::string {uuid});
#endif
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
                                 return !terra_api::is_known_scope(scope);
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
    bool revoke_resources = false;
    bool revoke_profile_owner = false;
    bool revoke_sandbox_owner = false;
    bool revoke_workspace_owner = false;
    std::optional<terra_api::client_permissions_t> current_profile_permissions;
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
        revoke_resources = !*update.enabled;
        revoke_profile_owner = !*update.enabled;
        revoke_sandbox_owner = !*update.enabled;
        revoke_workspace_owner = !*update.enabled;
      }
      if (update.permissions) {
        client->permissions = std::move(*update.permissions);
        terminate_sessions = true;
        revoke_resources = !client->permissions.scopes.contains("virtual-display.manage") || permissions_expired(client->permissions);
        current_profile_permissions = client->permissions;
        revoke_sandbox_owner = true;
        revoke_workspace_owner = true;
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
      if (terra_event_hub) {
        terra_event_hub->disconnect_client(std::string {uuid});
      }
      rtsp_stream::terminate_sessions_by_cert(previous_certificate);
    }
#ifdef _WIN32
    if (revoke_resources && terra_virtual_display_manager) {
      static_cast<void>(terra_virtual_display_manager->revoke_owner(std::string {uuid}));
    }
    if (revoke_sandbox_owner) {
      revoke_terra_sandboxes(std::string {uuid});
    }
    if (revoke_profile_owner) {
      revoke_terra_profiles(std::string {uuid});
    } else if (current_profile_permissions) {
      revoke_terra_profiles(std::string {uuid}, current_profile_permissions);
    }
    if (revoke_workspace_owner) {
      revoke_terra_workspaces(std::string {uuid});
    }
#endif
    return true;
  }

  std::optional<terra_api::client_permissions_t> get_client_permissions(const std::string_view uuid) {
    std::lock_guard lock {client_auth_mutex};
    const auto client = std::ranges::find(client_root.named_devices, uuid, &named_cert_t::uuid);
    if (client == client_root.named_devices.end()) {
      return std::nullopt;
    }
    return client->permissions;
  }

  bool set_client_permissions(const std::string_view uuid, terra_api::client_permissions_t permissions) {
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

#ifdef SOL_TESTS
  namespace test_support {
    nlohmann::json capabilities_document(
      const std::string_view client_uuid,
      const std::string_view client_name,
      const terra_api::client_permissions_t &permissions,
      const std::string_view host_uuid,
      const std::string_view host_name,
      const std::string_view host_platform,
      const std::string_view host_version,
      const bool wake_available
    ) {
      return terra_capabilities_document(client_uuid, client_name, permissions, host_uuid, host_name, host_platform, host_version, wake_available);
    }

    nlohmann::json catalog_app_document(const proc::ctx_t &app) {
      return terra_app_json(app);
    }

    std::vector<std::string> event_collections(const terra_api::client_permissions_t &permissions) {
      return terra_event_collections(permissions);
    }

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

    std::optional<terra_api::client_permissions_t> client_permissions(const std::string_view uuid) {
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

    void run_servers(const unsigned short port_http, const unsigned short port_https, const std::function<void(unsigned short)> &https_ready, const std::atomic_bool &external_stop) {
      run_nvhttp_servers(port_http, port_https, https_ready, external_stop);
    }
  }  // namespace test_support
#endif
}  // namespace nvhttp
