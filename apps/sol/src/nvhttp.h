/**
 * @file src/nvhttp.h
 * @brief Declarations for the nvhttp (GameStream) server.
 */
// macros
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "crypto.h"
#include "terra_api.h"

namespace proc {
  struct ctx_t;
}

/**
 * @brief Contains all the functions and variables related to the nvhttp (GameStream) server.
 */
namespace nvhttp {

  /**
   * @brief The protocol version.
   * @details The version of the GameStream protocol we are mocking.
   * @note The negative 4th number identifies a Sunshine-compatible host to Moonlight.
   */
  constexpr auto VERSION = "7.1.431.-1";

  /**
   * @brief The GFE version we are replicating.
   */
  constexpr auto GFE_VERSION = "3.23.0.74";

  /**
   * @brief The HTTP port, as a difference from the config port.
   */
  constexpr auto PORT_HTTP = 0;

  /**
   * @brief The HTTPS port, as a difference from the config port.
   */
  constexpr auto PORT_HTTPS = -5;

  /**
   * @brief Maximum number of pairing sessions retained at one time.
   */
  constexpr std::size_t MAX_PENDING_PAIRING_SESSIONS = 32;

  /**
   * @brief Number of hexadecimal characters in an operator approval identifier.
   */
  constexpr std::size_t PAIRING_ID_SIZE = 32;

  /**
   * @brief Maximum number of bytes accepted for an operator-supplied paired-client name.
   */
  constexpr std::size_t MAX_PAIRING_CLIENT_NAME_SIZE = 128;

  /**
   * @brief Lifetime of an incomplete pairing session.
   */
  constexpr auto PAIRING_SESSION_TIMEOUT = std::chrono::minutes {5};

  /**
   * @brief Start the nvhttp server.
   * @examples
   * nvhttp::start();
   * @examples_end
   */
  void start();

  /**
   * @brief Setup the nvhttp server.
   * @param pkey
   * @param cert
   */
  void setup(const std::string &pkey, const std::string &cert);

  /**
   * @brief Simple-Web-Server HTTPS backend configured for Sol certificate handling.
   */
  class SolHTTPS: public SimpleWeb::HTTPS {
  public:
    /**
     * @brief Construct an HTTPS connection using Sol's TLS context.
     *
     * @param io_context Boost.Asio context used for network operations.
     * @param ctx TLS context configured with Sol's certificate and key.
     */
    SolHTTPS(boost::asio::io_context &io_context, boost::asio::ssl::context &ctx):
        SimpleWeb::HTTPS(io_context, ctx) {
    }

    virtual ~SolHTTPS() noexcept {
      // Gracefully shutdown the TLS connection, but never block the destructor on
      // a peer that has already stopped responding with TLS close_notify.
      SimpleWeb::error_code ec;
      lowest_layer().non_blocking(true, ec);
      shutdown(ec);
    }
  };

  /**
   * @brief Enumerates supported pAIR PHASE options.
   */
  enum class PAIR_PHASE {
    NONE,  ///< Sol is not in a pairing phase
    GETSERVERCERT,  ///< Sol is in the get server certificate phase
    CLIENTCHALLENGE,  ///< Sol is in the client challenge phase
    SERVERCHALLENGERESP,  ///< Sol is in the server challenge response phase
    CLIENTPAIRINGSECRET  ///< Sol is in the client pairing secret phase
  };

  /**
   * @brief Pairing handshake state exchanged with a Moonlight client.
   */
  struct pair_session_t {
    struct {
      std::string uniqueID = {};
      std::string cert = {};
      std::string name = {};
      std::string platform = {};  ///< Untrusted platform name reported by a Terra pairing client.
      terra_api::client_permissions_t requested_permissions = terra_api::legacy_client_permissions();  ///< Permissions requested during pairing.
      bool requested_terra_permissions = false;  ///< Whether the pairing client explicitly requested Terra permissions.
    } client;  ///< Client object or client certificate data owned by this state..

    std::unique_ptr<crypto::aes_t> cipher_key = {};  ///< Cipher key.
    std::vector<uint8_t> clienthash = {};  ///< Client certificate hash used during pairing.

    std::string serversecret = {};  ///< Server pairing secret.
    std::string serverchallenge = {};  ///< Server challenge sent during pairing.

    struct {
      util::Either<
        std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>,
        std::shared_ptr<typename SimpleWeb::ServerBase<SolHTTPS>::Response>>
        response;
      std::string salt = {};
      std::string id = {};  ///< Unguessable identifier used by the Web UI to approve this session.
      std::string device_name = {};  ///< Untrusted device name reported by the pairing client.
      std::string address = {};  ///< Network address from which the pairing request originated.
      std::chrono::steady_clock::time_point expires_at = std::chrono::steady_clock::time_point::max();  ///< Deadline for completing this pairing session.
    } async_insert_pin;  ///< Async insert pin.

    /**
     * @brief used as a security measure to prevent out of order calls
     */
    PAIR_PHASE last_phase = PAIR_PHASE::NONE;
    bool failed = false;  ///< Whether protocol validation failed and the session must be removed.
  };

  /**
   * @brief Operator-visible context for a pairing request awaiting PIN approval.
   */
  struct pending_pairing_t {
    std::string id;  ///< Unguessable approval identifier.
    std::string name;  ///< Untrusted device name reported by the pairing client.
    std::string address;  ///< Network address from which the request originated.
    std::string platform;  ///< Untrusted client platform reported during pairing.
    std::vector<std::string> requested_scopes;  ///< Terra scopes requested by the pairing client.
    std::vector<std::string> requested_inputs;  ///< Streamed input classes requested by the pairing client.
  };

  /**
   * @brief Result of inserting a new pairing session into bounded pending storage.
   */
  enum class pair_session_insert_e {
    ADDED,  ///< The session was inserted successfully.
    ALREADY_EXISTS,  ///< A session with the same client unique ID is already active.
    FULL  ///< The bounded session store has reached its limit.
  };

  /**
   * @brief Validate an operator approval identifier received from the REST API.
   *
   * @param pairing_id Approval identifier to validate.
   * @return `true` when the value has the exact hexadecimal format Sol generates.
   */
  bool is_valid_pairing_id(std::string_view pairing_id);

  /**
   * @brief Validate a Moonlight pairing PIN received from the REST API.
   *
   * @param pin PIN to validate.
   * @return `true` when the value contains exactly four ASCII digits.
   */
  bool is_valid_pairing_pin(std::string_view pin);

  /**
   * @brief Validate an operator-supplied paired-client name received from the REST API.
   *
   * @param name Client name to validate.
   * @return `true` when the name is non-empty and within the storage limit.
   */
  bool is_valid_pairing_name(std::string_view name);

  /**
   * @brief Insert a newly created pairing session into bounded pending storage.
   *
   * @param sess Pairing session to insert.
   * @param pairing_id Receives the unguessable approval identifier on success.
   * @return Status describing whether the session was inserted.
   */
  pair_session_insert_e insert_pair_session(pair_session_t sess, std::string &pairing_id);

  /**
   * @brief Remove pairing sessions whose completion deadline has passed.
   *
   * @param now Monotonic time used to evaluate session deadlines.
   */
  void expire_pair_sessions(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  /**
   * @brief List pairing requests that are waiting for operator PIN approval.
   *
   * @return Pending requests ordered from oldest to newest.
   */
  std::vector<pending_pairing_t> get_pending_pairings();

  /**
   * @brief Cancel a pairing request that is waiting for operator approval.
   *
   * @param pairing_id Unguessable approval identifier returned by @ref get_pending_pairings.
   * @return `true` if the pending request was found and cancelled.
   */
  bool cancel_pairing(std::string_view pairing_id);

  /**
   * @brief removes the temporary pairing session
   * @param sess
   */
  void remove_session(const pair_session_t &sess);

  /**
   * @brief Pair, phase 1
   *
   * Moonlight will send a salt and client certificate, we'll also need the user provided pin.
   *
   * PIN and SALT will be used to derive a shared AES key that needs to be stored
   * in order to be used to decrypt_symmetric in the next phases.
   *
   * At this stage we only have to send back our public certificate.
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param pin PIN supplied by the client during pairing.
   */
  void getservercert(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &pin);

  /**
   * @brief Pair, phase 2
   *
   * Using the AES key that we generated in phase 1 we have to decrypt the client challenge,
   *
   * We generate a SHA256 hash with the following:
   *  - Decrypted challenge
   *  - Server certificate signature
   *  - Server secret: a randomly generated secret
   *
   * The hash + server_challenge will then be AES encrypted and sent as the `challengeresponse` in the returned XML
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param challenge Client challenge bytes from the pairing request.
   */
  void clientchallenge(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &challenge);

  /**
   * @brief Pair, phase 3
   *
   * Moonlight will send back a `serverchallengeresp`: an AES encrypted client hash,
   * we have to send back the `pairingsecret`:
   * using our private key we have to sign the certificate_signature + server_secret (generated in phase 2)
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param encrypted_response Encrypted response.
   */
  void serverchallengeresp(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &encrypted_response);

  /**
   * @brief Pair, phase 4 (final)
   *
   * We now have to use everything we exchanged before in order to verify and finally pair the clients
   *
   * We'll check the client_hash obtained at phase 3, it should contain the following:
   *   - The original server_challenge
   *   - The signature of the X509 client_cert
   *   - The unencrypted client_pairing_secret
   * We'll check that SHA256(server_challenge + client_public_cert_signature + client_secret) == client_hash
   *
   * Then using the client certificate public key we should be able to verify that
   * the client secret has been signed by Moonlight
   * @param sess Pairing session that owns the request state.
   * @param tree XML property tree used for the response body.
   * @param client_pairing_secret Client pairing secret.
   */
  void clientpairingsecret(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &client_pairing_secret);

  /**
   * @brief Apply the user supplied PIN to the explicitly selected pairing request.
   * @param pairing_id Unguessable identifier of the pairing request to approve.
   * @param pin The user supplied pin.
   * @param name The user supplied name.
   * @return `true` if the pin is correct, `false` otherwise.
   * @examples
   * bool pin_status = nvhttp::pin("0123456789abcdef0123456789abcdef", "1234", "laptop");
   * @examples_end
   */
  bool pin(std::string_view pairing_id, std::string pin, std::string name);

  /**
   * @brief Remove single client.
   * @param uuid The UUID of the client to remove.
   * @examples
   * nvhttp::unpair_client("4D7BB2DD-5704-A405-B41C-891A022932E1");
   * @examples_end
   *
   * @return True when the client entry was found and removed.
   */
  bool unpair_client(std::string_view uuid);

  /**
   * @brief Enable or disable a client.
   * @param uuid The UUID of the client.
   * @param enabled Whether the client should be enabled.
   * @return true if the client was found and updated.
   */
  bool set_client_enabled(std::string_view uuid, bool enabled);

  /**
   * @brief Optional paired-client fields changed by one atomic update.
   */
  struct client_update_t {
    std::optional<bool> enabled;  ///< Replacement enabled state, or no change.
    std::optional<terra_api::client_permissions_t> permissions;  ///< Replacement permission policy, or no change.
    std::optional<std::string> certificate;  ///< Replacement PEM certificate, or no change.
  };

  /**
   * @brief Atomically update paired-client state and invalidate active authorization.
   *
   * @param uuid Persistent paired-client UUID.
   * @param update Validated fields to replace.
   * @return `true` when the client exists and every supplied field is valid.
   */
  bool update_client(std::string_view uuid, client_update_t update);

  /**
   * @brief Return certificate-bound permissions for one paired client.
   *
   * @param uuid Persistent paired-client UUID.
   * @return Stored policy, or no value when the client is unknown.
   */
  std::optional<terra_api::client_permissions_t> get_client_permissions(std::string_view uuid);

  /**
   * @brief Replace certificate-bound permissions for one paired client.
   *
   * @param uuid Persistent paired-client UUID.
   * @param permissions Validated permissions to store.
   * @return `true` when the client was found and updated.
   */
  bool set_client_permissions(std::string_view uuid, terra_api::client_permissions_t permissions);
  /**
   * @brief Replace paired certificate while preserving client identity and policy.
   *
   * @param uuid Persistent paired-client UUID.
   * @param cert New PEM-encoded client certificate.
   * @return `true` when certificate is valid and client was updated.
   */
  bool rotate_client_certificate(std::string_view uuid, std::string cert);
  /**
   * @brief Get cert by UUID.
   *
   * @param uuid Client UUID being looked up or removed.
   * @return PEM certificate for the paired client, or an empty string when unknown.
   */
  std::string get_cert_by_uuid(std::string_view uuid);

  /**
   * @brief Get all paired clients.
   * @return The list of all paired clients.
   * @examples
   * nlohmann::json clients = nvhttp::get_all_clients();
   * @examples_end
   */
  nlohmann::json get_all_clients();

  /**
   * @brief Remove all paired clients.
   * @examples
   * nvhttp::erase_all_clients();
   * @examples_end
   */
  void erase_all_clients();

  /**
   * @brief Compute the RFC 6455 handshake accept key for a client key.
   *
   * @param key Raw `Sec-WebSocket-Key` header value.
   * @return Base64 accept key.
   */
  std::string websocket_accept_key(std::string_view key);

  /**
   * @brief Encode one unmasked server WebSocket frame.
   *
   * @param opcode Frame opcode, for example 0x1 for text or 0x8 for close.
   * @param payload Frame payload bytes.
   * @return Complete encoded frame.
   */
  std::vector<std::uint8_t> websocket_frame(int opcode, std::string_view payload);

  /**
   * @brief Decode one complete masked client WebSocket frame.
   *
   * @param frame Complete frame bytes including header, mask, and payload.
   * @return Opcode and unmasked payload, or no value for malformed input.
   */
  std::optional<std::pair<int, std::string>> websocket_decode_frame(const std::vector<std::uint8_t> &frame);

#ifdef SOL_TESTS
  /**
   * @brief Test-only accessors for paired-client authorization state.
   */
  namespace test_support {
    /**
     * @brief Build a capability document through production serialization logic.
     *
     * @param client_uuid Stable paired-client UUID.
     * @param client_name Paired-client friendly name.
     * @param permissions Certificate-bound client policy.
     * @param host_uuid Stable Sol host UUID.
     * @param host_name Sol host name.
     * @param host_platform Sol platform identifier.
     * @param host_version Sol build version.
     * @param wake_available Whether usable Wake-on-LAN information exists.
     * @return Complete capability document without response `schemaVersion`.
     */
    nlohmann::json capabilities_document(
      std::string_view client_uuid,
      std::string_view client_name,
      const terra_api::client_permissions_t &permissions,
      std::string_view host_uuid,
      std::string_view host_name,
      std::string_view host_platform,
      std::string_view host_version,
      bool wake_available
    );

    /**
     * @brief Build one Catalog V2 application through production serialization logic.
     *
     * @param app Runtime application context.
     * @return Catalog V2 application resource.
     */
    nlohmann::json catalog_app_document(const proc::ctx_t &app);

    /**
     * @brief Project event resynchronization collections through client scopes.
     *
     * @param permissions Certificate-bound client policy.
     * @return Collection names visible to event stream.
     */
    std::vector<std::string> event_collections(const terra_api::client_permissions_t &permissions);

    /**
     * @brief Clear in-memory paired-client records without changing persisted state.
     */
    void reset_client_state();

    /**
     * @brief Add a paired-client record for authorization tests.
     *
     * @param name Human-readable client name.
     * @param cert PEM-encoded client certificate.
     * @param enabled Whether the client may connect.
     * @return Persistent UUID for the added client, or an empty string when invalid.
     */
    std::string add_client(const std::string &name, std::string cert, bool enabled);
    /**
     * @brief Return persisted permissions for one test client.
     *
     * @param uuid Persistent paired-client UUID.
     * @return Client permissions, or no value when unknown.
     */
    std::optional<terra_api::client_permissions_t> client_permissions(std::string_view uuid);

    /**
     * @brief Run the production certificate authorization checks against PEM input.
     *
     * @param cert PEM-encoded certificate presented by a client.
     * @return `true` when the exact certificate belongs to one enabled paired client.
     */
    bool authorize_client_certificate(std::string_view cert);

    /**
     * @brief Reload paired-client authorization state from the configured state file.
     */
    void reload_client_state();

    /**
     * @brief Run the production nvhttp servers with explicit ports until stopped.
     *
     * Behaves exactly like `nvhttp::start()`, including Terra manager creation,
     * route registration, and the periodic change monitor, but uses caller-supplied
     * ports and a cooperative stop flag instead of the global shutdown event.
     * Blocks the calling thread until `external_stop` is set or shutdown is signaled.
     *
     * @param port_http Discovery HTTP port, zero selects an ephemeral port.
     * @param port_https Paired HTTPS port, zero selects an ephemeral port.
     * @param https_ready Receives the bound HTTPS port after listening starts.
     * @param external_stop Set to `true` to request server shutdown.
     */
    void run_servers(
      unsigned short port_http,
      unsigned short port_https,
      const std::function<void(unsigned short)> &https_ready,
      const std::atomic_bool &external_stop
    );
  }  // namespace test_support
#endif
}  // namespace nvhttp
