/**
 * @file src/platform/windows/publish.cpp
 * @brief Definitions for Windows mDNS service registration.
 */
// platform includes
// WinSock2.h must be included before Windows.h
// clang-format off
#include <WinSock2.h>
#include <Windows.h>
// clang-format on
#include <WinDNS.h>
#include <winerror.h>

// standard includes
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "utf_utils.h"

/**
 * @def _FN(x, ret, args)
 * @brief Macro for FN.
 */
#define _FN(x, ret, args) \
  /** \
   * @brief Function pointer type for the dynamically loaded DNS-SD entry point. \
   */ \
  typedef ret(*x##_fn) args; \
  /** \
   * @brief Loaded DNS-SD entry point pointer. \
   */ \
  static x##_fn x

using namespace std::literals;

/**
 * @def __SV(quote)
 * @brief Macro for SV.
 */
#define __SV(quote) L##quote##sv
/**
 * @def SV(quote)
 * @brief Macro for SV.
 */
#define SV(quote) __SV(quote)

extern "C" {
#ifndef __MINGW32__
  constexpr auto DNS_REQUEST_PENDING = 9506L;  ///< Windows DNS API constant for request pending.
  constexpr auto DNS_QUERY_REQUEST_VERSION1 = 0x1;  ///< Windows DNS API constant for query request version1.
  constexpr auto DNS_QUERY_RESULTS_VERSION1 = 0x1;  ///< Windows DNS API constant for query results version1.
#endif

  constexpr auto SERVICE_DOMAIN = "local";  ///< Protocol or platform constant for service domain.
  const auto SERVICE_TYPE_DOMAIN = std::format("{}.{}"sv, platf::SERVICE_TYPE, SERVICE_DOMAIN);  ///< Protocol or platform constant for service type domain.

#ifndef __MINGW32__
  /**
   * @brief Windows DNS-SD service instance registration data.
   */
  typedef struct _DNS_SERVICE_INSTANCE {
    LPWSTR pszInstanceName;  ///< DNS-SD service instance name.
    LPWSTR pszHostName;  ///< Hostname advertising the DNS-SD service.

    IP4_ADDRESS *ip4Address;  ///< Optional IPv4 address advertised with the service.
    IP6_ADDRESS *ip6Address;  ///< Optional IPv6 address advertised with the service.

    WORD wPort;  ///< TCP or UDP port advertised for the service.
    WORD wPriority;  ///< DNS-SD priority value.
    WORD wWeight;  ///< DNS-SD weight value.

    // Property list
    DWORD dwPropertyCount;  ///< Number of TXT record key/value pairs.

    PWSTR *keys;  ///< DNS TXT record keys.
    PWSTR *values;  ///< DNS TXT record values paired with `keys`.

    DWORD dwInterfaceIndex;  ///< Network interface index used for registration.
  } DNS_SERVICE_INSTANCE, *PDNS_SERVICE_INSTANCE;  ///< Alias for DNS SERVICE INSTANCE.
#endif

  /**
   * @brief DNS service registration completion callback.
   *
   * @param Status Registration status.
   * @param pQueryContext User query context.
   * @param pInstance DNS service instance.
   */
  typedef VOID WINAPI
    DNS_SERVICE_REGISTER_COMPLETE(
      _In_ DWORD Status,
      _In_ PVOID pQueryContext,
      _In_ PDNS_SERVICE_INSTANCE pInstance
    );

  /**
   * @brief Pointer to the Windows DNS-SD registration completion callback.
   */
  typedef DNS_SERVICE_REGISTER_COMPLETE *PDNS_SERVICE_REGISTER_COMPLETE;

#ifndef __MINGW32__
  /**
   * @brief Windows DNS-SD cancellation request data.
   */
  typedef struct _DNS_SERVICE_CANCEL {
    PVOID reserved;  ///< Reserved by the Windows DNS-SD API and left null.
  } DNS_SERVICE_CANCEL, *PDNS_SERVICE_CANCEL;  ///< Alias for DNS SERVICE CANCEL.

  /**
   * @brief Windows DNS-SD service registration request data.
   */
  typedef struct _DNS_SERVICE_REGISTER_REQUEST {
    ULONG Version;  ///< Windows DNS-SD request structure version.
    ULONG InterfaceIndex;  ///< Network interface index used for registration.
    PDNS_SERVICE_INSTANCE pServiceInstance;  ///< Service instance being registered.
    PDNS_SERVICE_REGISTER_COMPLETE pRegisterCompletionCallback;  ///< Callback invoked when registration completes.
    PVOID pQueryContext;  ///< Caller-provided context passed to the completion callback.
    HANDLE hCredentials;  ///< Optional credentials handle supplied to Windows DNS-SD.
    BOOL unicastEnabled;  ///< Whether the DNS-SD registration is unicast-only.
  } DNS_SERVICE_REGISTER_REQUEST, *PDNS_SERVICE_REGISTER_REQUEST;  ///< Alias for DNS SERVICE REGISTER REQUEST.
#endif

  _FN(_DnsServiceFreeInstance, VOID, (_In_ PDNS_SERVICE_INSTANCE pInstance));
  _FN(_DnsServiceDeRegister, DWORD, (_In_ PDNS_SERVICE_REGISTER_REQUEST pRequest, _Inout_opt_ PDNS_SERVICE_CANCEL pCancel));
  _FN(_DnsServiceRegister, DWORD, (_In_ PDNS_SERVICE_REGISTER_REQUEST pRequest, _Inout_opt_ PDNS_SERVICE_CANCEL pCancel));
  _FN(_DnsServiceRegisterCancel, DWORD, (_In_ PDNS_SERVICE_CANCEL pCancel));
} /* extern "C" */

namespace platf::publish {
  namespace {
    constexpr auto SERVICE_REQUEST_TIMEOUT = std::chrono::seconds {5};  ///< Maximum wait for a Windows DNS-SD request callback.

    /**
     * @brief Stable storage and completion state for one asynchronous Windows DNS-SD request.
     */
    class service_request_t {
    public:
      /** @brief Native action required when publication ownership ends. */
      enum class stop_action_t {
        none,  ///< Registration already failed.
        cancel,  ///< Registration callback is still pending.
        deregister,  ///< Registration completed successfully.
      };

      /**
       * @brief Store asynchronous request completion.
       *
       * @param completion_status Native callback status.
       * @param registration Whether this callback completed registration.
       * @return True when a late successful registration must be removed.
       */
      bool complete(const DWORD completion_status, const bool registration) {
        std::lock_guard lock {mutex};
        status = completion_status;
        completed = true;
        condition.notify_one();
        if (!registration) {
          if (status != ERROR_SUCCESS) {
            set_registration_state(state_t::unavailable, "deregistration_failed", "Windows DNS-SD service deregistration failed");
          }
          return false;
        }
        if (stopping) {
          return status == ERROR_SUCCESS;
        }
        if (status == ERROR_SUCCESS) {
          set_registration_state(state_t::available);
        } else {
          set_registration_state(state_t::unavailable, "registration_failed", "Windows DNS-SD service registration failed");
        }
        return false;
      }

      /**
       * @brief Wait for bounded deregistration request completion.
       *
       * @return True when callback completed before timeout.
       */
      bool wait() {
        std::unique_lock lock {mutex};
        return condition.wait_for(lock, SERVICE_REQUEST_TIMEOUT, [&] {
          return completed;
        });
      }

      /**
       * @brief Return native callback status.
       *
       * @return Windows DNS status supplied to callback.
       */
      DWORD completion_status() const {
        std::lock_guard lock {mutex};
        return status;
      }

      /**
       * @brief Prepare original request for one deregistration attempt.
       *
       * @return True for first attempt; otherwise false.
       */
      bool begin_deregistration() {
        std::lock_guard lock {mutex};
        if (deregistration_started) {
          return false;
        }
        deregistration_started = true;
        completed = false;
        status = ERROR_SUCCESS;
        return true;
      }

      /**
       * @brief Mark publication stopped and select required native cleanup.
       *
       * @return Cancel for pending registration, deregister for successful registration, or none.
       */
      stop_action_t begin_stop() {
        std::lock_guard lock {mutex};
        stopping = true;
        set_registration_state(state_t::stopped);
        if (!completed) {
          return stop_action_t::cancel;
        }
        return status == ERROR_SUCCESS ? stop_action_t::deregister : stop_action_t::none;
      }

      mutable std::mutex mutex;  ///< Protects asynchronous completion state.
      std::condition_variable condition;  ///< Signals callback completion.
      bool completed {};  ///< Whether callback has run.
      bool stopping {};  ///< Whether publication ownership has ended.
      bool deregistration_started {};  ///< Whether deregistration was already requested.
      DWORD status {};  ///< Native callback status.
      std::wstring name;  ///< Stable registration instance-name storage.
      std::wstring host;  ///< Stable registration hostname storage.
      std::array<PWCHAR, 1> keys {nullptr};  ///< Stable empty TXT key storage.
      std::array<PWCHAR, 1> values {nullptr};  ///< Stable empty TXT value storage.
      DNS_SERVICE_INSTANCE instance {};  ///< Stable native registration instance request.
      DNS_SERVICE_REGISTER_REQUEST request {};  ///< Stable native asynchronous request.
      DNS_SERVICE_CANCEL cancel {};  ///< Native registration cancellation handle.
    };

    /**
     * @brief Callback-owned reference to a pending DNS-SD request.
     */
    struct callback_context_t {
      std::shared_ptr<service_request_t> operation;  ///< Stable request storage.
      bool registration {};  ///< Whether callback completes registration.
    };

    /**
     * @brief Deregister using the original stable registration request.
     *
     * @param operation Registration request retained for its full native lifetime.
     * @param wait Whether to wait for bounded callback completion.
     * @return True when deregistration started and, when requested, completed successfully.
     */
    bool deregister_service(const std::shared_ptr<service_request_t> &operation, bool wait);
  }  // namespace

  /**
   * @brief Handle completion of a Windows DNS-SD registration request.
   *
   * @param status Native status code returned by the platform API.
   * @param pQueryContext Shared request state retained until callback completion.
   * @param pInstance Registered DNS-SD service instance returned by Windows.
   * @return Callback has no return value; completion is stored in shared request state.
   */
  VOID WINAPI register_cb(DWORD status, PVOID pQueryContext, PDNS_SERVICE_INSTANCE pInstance) {
    std::unique_ptr<callback_context_t> context {
      static_cast<callback_context_t *>(pQueryContext),
    };

    if (status) {
      print_status("register_cb()"sv, status);
    }
    if (pInstance) {
      _DnsServiceFreeInstance(pInstance);
    }

    const bool remove_late_registration = context->operation->complete(status, context->registration);
    if (context->registration && status == ERROR_SUCCESS && !remove_late_registration) {
      BOOST_LOG(info) << "Registered Sol mDNS service"sv;
    }
    if (remove_late_registration) {
      static_cast<void>(deregister_service(context->operation, false));
    }
  }

  /**
   * @brief Begin asynchronous Windows DNS-SD registration.
   *
   * @return Stable original request while registration is active, or null on failure.
   */
  static std::shared_ptr<service_request_t> register_service() {
    auto operation = std::make_shared<service_request_t>();
    const auto domain = utf_utils::from_utf8(SERVICE_TYPE_DOMAIN);
    const auto hostname = platf::get_host_name();
    operation->name = utf_utils::from_utf8(net::mdns_instance_name(hostname) + '.') + domain;
    operation->host = utf_utils::from_utf8(hostname + ".local");
    operation->instance.pszInstanceName = operation->name.data();
    operation->instance.wPort = net::map_port(nvhttp::PORT_HTTP);
    operation->instance.pszHostName = operation->host.data();

    // Windows otherwise emits an invalid zero-string TXT record rejected by Apple resolvers.
    operation->instance.dwPropertyCount = 1;
    operation->instance.keys = operation->keys.data();
    operation->instance.values = operation->values.data();

    operation->request.Version = DNS_QUERY_REQUEST_VERSION1;
    operation->request.pQueryContext = new callback_context_t {operation, true};
    operation->request.pServiceInstance = &operation->instance;
    operation->request.pRegisterCompletionCallback = register_cb;

    const auto status = _DnsServiceRegister(&operation->request, &operation->cancel);
    if (status != DNS_REQUEST_PENDING) {
      delete static_cast<callback_context_t *>(operation->request.pQueryContext);
      operation->request.pQueryContext = nullptr;
      print_status("DnsServiceRegister()"sv, status);
      return nullptr;
    }

    return operation;
  }

  namespace {
    bool deregister_service(const std::shared_ptr<service_request_t> &operation, const bool wait) {
      if (!operation->begin_deregistration()) {
        return true;
      }

      operation->request.pQueryContext = new callback_context_t {operation, false};
      const auto status = _DnsServiceDeRegister(&operation->request, nullptr);
      if (status != DNS_REQUEST_PENDING) {
        delete static_cast<callback_context_t *>(operation->request.pQueryContext);
        operation->request.pQueryContext = nullptr;
        print_status("DnsServiceDeRegister()"sv, status);
        return false;
      }
      if (!wait) {
        return true;
      }
      if (!operation->wait()) {
        BOOST_LOG(error) << "Windows DNS-SD deregistration timed out"sv;
        return false;
      }
      return operation->completion_status() == ERROR_SUCCESS;
    }
  }  // namespace

  /**
   * @brief Windows DNS-SD registration lifetime for the advertised Sol service.
   */
  class mdns_registration_t: public ::platf::deinit_t {
  public:
    mdns_registration_t():
        operation(register_service()) {
      if (!operation) {
        BOOST_LOG(error) << "Unable to register Sol mDNS service"sv;
        set_registration_state(state_t::unavailable, "registration_failed", "Windows DNS-SD service registration failed");
        return;
      }

      BOOST_LOG(info) << "Requested Sol mDNS service registration"sv;
    }

    ~mdns_registration_t() override {
      if (operation) {
        switch (operation->begin_stop()) {
          case service_request_t::stop_action_t::cancel:
            {
              const auto status = _DnsServiceRegisterCancel(&operation->cancel);
              if (status != ERROR_SUCCESS && status != ERROR_CANCELLED) {
                print_status("DnsServiceRegisterCancel()"sv, status);
              }
              break;
            }
          case service_request_t::stop_action_t::deregister:
            if (!deregister_service(operation, true)) {
              BOOST_LOG(error) << "Unable to unregister Sol mDNS service"sv;
              set_registration_state(state_t::unavailable, "deregistration_failed", "Windows DNS-SD service deregistration failed");
              return;
            }
            BOOST_LOG(info) << "Unregistered Sol mDNS service"sv;
            break;
          case service_request_t::stop_action_t::none:
            break;
        }
      }
    }

  private:
    std::shared_ptr<service_request_t> operation;  ///< Original request retained for native deregistration.
  };

  /**
   * @brief Resolve required function pointers from the native library.
   *
   * @param handle Native library or object handle used by the operation.
   * @return 0 when all required DNS-SD functions are resolved; nonzero otherwise.
   */
  int load_funcs(HMODULE handle) {
    auto fg = util::fail_guard([handle]() {
      FreeLibrary(handle);
    });

    _DnsServiceFreeInstance = (_DnsServiceFreeInstance_fn) GetProcAddress(handle, "DnsServiceFreeInstance");
    _DnsServiceDeRegister = (_DnsServiceDeRegister_fn) GetProcAddress(handle, "DnsServiceDeRegister");
    _DnsServiceRegister = (_DnsServiceRegister_fn) GetProcAddress(handle, "DnsServiceRegister");
    _DnsServiceRegisterCancel = (_DnsServiceRegisterCancel_fn) GetProcAddress(handle, "DnsServiceRegisterCancel");

    if (!(_DnsServiceFreeInstance && _DnsServiceDeRegister && _DnsServiceRegister && _DnsServiceRegisterCancel)) {
      BOOST_LOG(error) << "mDNS service not available in dnsapi.dll"sv;
      return -1;
    }

    fg.disable();
    return 0;
  }

  std::unique_ptr<::platf::deinit_t> start() {
    set_registration_state(state_t::starting);
    HMODULE handle = LoadLibrary("dnsapi.dll");

    if (!handle || load_funcs(handle)) {
      BOOST_LOG(error) << "Couldn't load dnsapi.dll, You'll need to add PC manually from Terra"sv;
      set_registration_state(state_t::unavailable, "provider_unavailable", "Windows DNS-SD API is unavailable");
      return nullptr;
    }

    auto registration = std::make_unique<mdns_registration_t>();
    return registration;
  }
}  // namespace platf::publish
