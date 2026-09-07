/**
 * @file src/platform/windows/terra_sandbox_provider.cpp
 * @brief Fail-closed Windows process provider for Terra sandboxes.
 */

// header include
#include "terra_sandbox_provider.h"

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

// system includes
#include <windows.h>
#include <wtsapi32.h>

// local includes
#include "src/file_handler.h"
#include "src/utility.h"
#include "src/uuid.h"

#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
  /** @brief Windows 10 process-creation attribute for atomic Job Object assignment. */
  #define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

namespace terra::windows::sandbox {
  namespace {
    using json = nlohmann::json;
    constexpr DWORD MINIMUM_WINDOWS_BUILD = 19045;  ///< Windows 10 22H2 minimum supported build.
    constexpr DWORD TERMINATION_WAIT_MS = 5000;  ///< Maximum synchronous Job Object termination wait.
    constexpr std::int64_t MAX_TIMEOUT_MS = LLONG_MAX / 10000;  ///< Largest waitable-timer millisecond duration.

    /**
     * @brief Close an owned Win32 handle.
     */
    struct handle_closer_t {
      /**
       * @brief Close an owned handle.
       * @param handle Handle to close.
       */
      void operator()(void *handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
        }
      }
    };

    /** @brief Unique ownership wrapper for Win32 handles. */
    using unique_handle_t = std::unique_ptr<void, handle_closer_t>;

    /**
     * @brief Construct an owned-handle wrapper.
     * @param handle Raw Win32 handle.
     * @return Owned handle.
     */
    unique_handle_t own_handle(const HANDLE handle) {
      return unique_handle_t {handle};
    }

    /**
     * @brief Convert strict UTF-8 to UTF-16.
     * @param value UTF-8 input.
     * @param output Converted UTF-16 output.
     * @return `true` when input is valid UTF-8.
     */
    bool to_utf16(const std::string &value, std::wstring &output) {
      output.clear();
      if (value.empty()) {
        return true;
      }
      if (value.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
      }
      const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
      if (size <= 0) {
        return false;
      }
      output.resize(static_cast<std::size_t>(size));
      return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size) == size;
    }

    /**
     * @brief Return whether current machine meets provider OS and architecture requirements.
     * @return `true` on x64 Windows 10 22H2 or newer.
     */
    bool supported_platform() {
#if !defined(__x86_64__) && !defined(_M_X64)
      return false;
#else
      using rtl_get_version_t = LONG(WINAPI *)(OSVERSIONINFOW *);
      const auto module = GetModuleHandleW(L"ntdll.dll");
      const auto get_version = module == nullptr ? nullptr : reinterpret_cast<rtl_get_version_t>(GetProcAddress(module, "RtlGetVersion"));
      OSVERSIONINFOW version {.dwOSVersionInfoSize = sizeof(version)};
      return get_version != nullptr && get_version(&version) >= 0 && version.dwMajorVersion == 10 && version.dwBuildNumber >= MINIMUM_WINDOWS_BUILD;
#endif
    }

    /**
     * @brief Compare environment names using Windows case-insensitive semantics.
     */
    struct environment_less_t {
      /**
       * @brief Compare two environment names.
       * @param left Left name.
       * @param right Right name.
       * @return `true` when left sorts before right.
       */
      bool operator()(const std::wstring &left, const std::wstring &right) const {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
      }
    };

    /**
     * @brief Set fail-closed capability reason.
     * @param reason Output diagnostic.
     * @param value Stable diagnostic value.
     * @return Always `false`.
     */
    bool reject(std::string &reason, const char *value) {
      reason = value;
      return false;
    }

    /**
     * @brief Validate provider-specific launch metadata.
     * @param application Resolved application.
     * @param reason Failure diagnostic.
     * @return `true` when metadata can be launched exactly.
     */
    bool validate_application(const terra_sandboxes::application_t &application, std::string &reason) {
      std::wstring executable;
      if (!to_utf16(application.executable, executable) || executable.empty() || !std::filesystem::path(executable).is_absolute()) {
        return reject(reason, "Executable must be an absolute UTF-8 path");
      }
      const auto attributes = GetFileAttributesW(executable.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return reject(reason, "Executable path is not an existing file");
      }
      const auto &data = application.launch_data;
      if (data.is_null()) {
        return true;
      }
      if (!data.is_object()) {
        return reject(reason, "Launch data must be an object");
      }
      for (const auto &[key, value] : data.items()) {
        static_cast<void>(value);
        if (key != "arguments" && key != "workingDirectory" && key != "environment") {
          return reject(reason, "Launch data contains an unsupported field");
        }
      }
      if (data.contains("arguments")) {
        const auto &arguments = data.at("arguments");
        if (!arguments.is_array() || arguments.size() > 1024) {
          return reject(reason, "Launch arguments must be a bounded string array");
        }
        std::size_t maximum_command_size = executable.size() * 2 + 3;
        for (const auto &argument : arguments) {
          std::wstring converted;
          if (!argument.is_string() || !to_utf16(argument.get_ref<const std::string &>(), converted) || converted.find(L'\0') != std::wstring::npos) {
            return reject(reason, "Launch argument is not valid text");
          }
          maximum_command_size += converted.size() * 2 + 3;
          if (maximum_command_size >= 32767) {
            return reject(reason, "Windows command line exceeds platform limit");
          }
        }
      }
      if (data.contains("workingDirectory") && !data.at("workingDirectory").is_null()) {
        std::wstring directory;
        if (!data.at("workingDirectory").is_string() || !to_utf16(data.at("workingDirectory").get_ref<const std::string &>(), directory) || directory.empty() || !std::filesystem::path(directory).is_absolute()) {
          return reject(reason, "Working directory must be an absolute UTF-8 path");
        }
        const auto directory_attributes = GetFileAttributesW(directory.c_str());
        if (directory_attributes == INVALID_FILE_ATTRIBUTES || (directory_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
          return reject(reason, "Working directory is not an existing directory");
        }
      }
      if (data.contains("environment")) {
        const auto &environment = data.at("environment");
        if (!environment.is_object()) {
          return reject(reason, "Launch environment must be an object");
        }
        for (const auto &[name, value] : environment.items()) {
          std::wstring converted_name;
          if (!to_utf16(name, converted_name) || converted_name.empty() || converted_name.find(L'\0') != std::wstring::npos || name.find('=') != std::string::npos || (!value.is_string() && (!value.is_object() || value.size() != 1 || !value.contains("secretRef") || !value.at("secretRef").is_string() || value.at("secretRef").get_ref<const std::string &>().empty()))) {
            return reject(reason, "Launch environment contains an invalid value");
          }
        }
      }
      return true;
    }

    /**
     * @brief Validate exact Windows provider policy support.
     * @param request Capability request.
     * @param has_secret_resolver Whether secret references can be resolved.
     * @param reason Failure diagnostic.
     * @return `true` only when every policy field is enforceable.
     */
    bool capable(const terra_sandboxes::launch_request_t &request, const bool has_secret_resolver, std::string &reason) {
      reason.clear();
      if (!supported_platform()) {
        return reject(reason, "Windows sandbox provider requires Windows 10 22H2 or newer on x64");
      }
      json normalized;
      if (!terra_sandboxes::normalize_policy(request.effective_policy, normalized) || normalized != request.effective_policy) {
        return reject(reason, "Policy is not complete and normalized");
      }
      const auto &policy = request.effective_policy;
      if (policy.at("executablePolicy") != "configured-only") {
        return reject(reason, "Executable allowlist policy cannot be enforced");
      }
      const auto &filesystem = policy.at("filesystem");
      if (filesystem.at("denyOther").get<bool>() || !filesystem.at("readOnlyRoots").empty() || !filesystem.at("writableRoots").empty()) {
        return reject(reason, "Filesystem isolation cannot be enforced");
      }
      const auto &network = policy.at("network");
      if (network.at("mode") != "full" || !network.at("allowedHosts").empty() || !network.at("allowedPorts").empty()) {
        return reject(reason, "Network filtering requires unavailable WFP enforcement");
      }
      const auto &resources = policy.at("resources");
      if (!resources.at("storageBytes").is_null()) {
        return reject(reason, "Storage quotas cannot be enforced");
      }
      if (policy.at("timeoutMs").get<std::int64_t>() > MAX_TIMEOUT_MS) {
        return reject(reason, "Timeout exceeds Windows waitable-timer range");
      }
      const auto &gpu = policy.at("gpu");
      if (gpu.at("mode") != "full" || !gpu.at("encoder").get<bool>()) {
        return reject(reason, "GPU mode or encoder isolation cannot be enforced");
      }
      const auto &displays = policy.at("displays");
      if (displays.at("virtualOnly").get<bool>() || !displays.at("allowedIds").empty()) {
        return reject(reason, "Display isolation cannot be enforced");
      }
      if (!policy.at("input").at("classes").empty()) {
        return reject(reason, "Input-class isolation cannot be enforced");
      }
      const auto &peripherals = policy.at("peripherals");
      if (!peripherals.at("deviceIds").empty() || !peripherals.at("classes").empty()) {
        return reject(reason, "Peripheral or device isolation cannot be enforced");
      }
      if (policy.at("clipboard") != "bidirectional") {
        return reject(reason, "Clipboard isolation cannot be enforced");
      }
      if (policy.at("hostIntegration") != "none") {
        return reject(reason, "Restricted host integration cannot be enforced");
      }
      if (policy.at("elevation") != "deny") {
        return reject(reason, "Elevation is never supported");
      }
      const auto &persistent = policy.at("persistentData");
      if (persistent.at("enabled").get<bool>() || !persistent.at("name").is_null()) {
        return reject(reason, "Persistent sandbox data cannot be enforced");
      }
      if (policy.at("cleanupPolicy") != "delete") {
        return reject(reason, "Only delete cleanup is supported without persistent data");
      }

      std::set<std::wstring, environment_less_t> names;
      for (const auto &name : policy.at("environment").at("allowedNames")) {
        std::wstring converted;
        if (!to_utf16(name.get_ref<const std::string &>(), converted) || converted.empty() || !names.emplace(std::move(converted)).second) {
          return reject(reason, "Environment names must be unique under Windows semantics");
        }
      }
      for (const auto &[name, value] : policy.at("environment").at("values").items()) {
        static_cast<void>(name);
        if (value.is_object() && !has_secret_resolver) {
          return reject(reason, "Environment secret resolver is unavailable");
        }
      }
      if (request.application.launch_data.is_object() && request.application.launch_data.contains("environment")) {
        std::set<std::wstring, environment_less_t> launch_names;
        for (const auto &[name, value] : request.application.launch_data.at("environment").items()) {
          std::wstring converted;
          if (!to_utf16(name, converted) || !names.contains(converted) || !launch_names.emplace(converted).second) {
            return reject(reason, "Launch environment name is not allowed by sandbox policy");
          }
          if (value.is_object() && !has_secret_resolver) {
            return reject(reason, "Environment secret resolver is unavailable");
          }
        }
      }
      if (!request.application.executable.empty() && !validate_application(request.application, reason)) {
        return false;
      }
      return true;
    }

    /**
     * @brief Escape one argument according to CommandLineToArgvW-compatible rules.
     * @param argument Unescaped argument.
     * @return Escaped command-line argument.
     */
    std::wstring quote_argument(const std::wstring &argument) {
      if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
      }
      std::wstring result {L'"'};
      std::size_t backslashes = 0;
      for (const auto character : argument) {
        if (character == L'\\') {
          ++backslashes;
        } else {
          if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
          } else {
            result.append(backslashes, L'\\');
          }
          backslashes = 0;
          result.push_back(character);
        }
      }
      result.append(backslashes * 2, L'\\');
      result.push_back(L'"');
      return result;
    }

    /**
     * @brief Return whether token user is LocalSystem.
     * @param token Token to inspect.
     * @return `true` for LocalSystem.
     */
    bool local_system_token(const HANDLE token) {
      DWORD size = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      std::vector<std::byte> storage(size);
      if (size == 0 || !GetTokenInformation(token, TokenUser, storage.data(), size, &size)) {
        return false;
      }
      std::vector<std::byte> sid_storage(SECURITY_MAX_SID_SIZE);
      DWORD sid_size = static_cast<DWORD>(sid_storage.size());
      return CreateWellKnownSid(WinLocalSystemSid, nullptr, sid_storage.data(), &sid_size) && EqualSid(reinterpret_cast<TOKEN_USER *>(storage.data())->User.Sid, sid_storage.data());
    }

    /**
     * @brief Acquire current interactive user's primary token.
     * @return Owned primary token, or null on failure.
     */
    unique_handle_t acquire_user_token() {
      HANDLE process_token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &process_token)) {
        return {};
      }
      auto source = own_handle(process_token);
      if (local_system_token(source.get())) {
        const auto session = WTSGetActiveConsoleSessionId();
        HANDLE user_token = nullptr;
        if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &user_token)) {
          return {};
        }
        source = own_handle(user_token);
      }
      return source;
    }

    /**
     * @brief Create and verify a non-elevated restricted primary token.
     * @return Owned restricted token, or null when restriction cannot be proven.
     */
    unique_handle_t create_restricted_primary_token() {
      auto source = acquire_user_token();
      if (!source) {
        return {};
      }
      DWORD user_size = 0;
      GetTokenInformation(source.get(), TokenUser, nullptr, 0, &user_size);
      std::vector<std::byte> user_storage(user_size);
      if (user_size == 0 || !GetTokenInformation(source.get(), TokenUser, user_storage.data(), user_size, &user_size)) {
        return {};
      }
      DWORD groups_size = 0;
      GetTokenInformation(source.get(), TokenGroups, nullptr, 0, &groups_size);
      std::vector<std::byte> groups_storage(groups_size);
      if (groups_size == 0 || !GetTokenInformation(source.get(), TokenGroups, groups_storage.data(), groups_size, &groups_size)) {
        return {};
      }
      std::vector<SID_AND_ATTRIBUTES> restricting_sids {{reinterpret_cast<TOKEN_USER *>(user_storage.data())->User.Sid, 0}};
      const auto groups = reinterpret_cast<TOKEN_GROUPS *>(groups_storage.data());
      for (DWORD index = 0; index < groups->GroupCount; ++index) {
        const auto &group = groups->Groups[index];
        if ((group.Attributes & SE_GROUP_ENABLED) != 0 && (group.Attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0) {
          restricting_sids.push_back({group.Sid, 0});
        }
      }
      HANDLE raw_restricted = nullptr;
      if (!CreateRestrictedToken(source.get(), DISABLE_MAX_PRIVILEGE | LUA_TOKEN, 0, nullptr, 0, nullptr, static_cast<DWORD>(restricting_sids.size()), restricting_sids.data(), &raw_restricted)) {
        return {};
      }
      auto restricted = own_handle(raw_restricted);

      SID_IDENTIFIER_AUTHORITY mandatory_authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
      PSID medium_sid = nullptr;
      if (!AllocateAndInitializeSid(&mandatory_authority, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0, 0, 0, 0, 0, &medium_sid)) {
        return {};
      }
      const TOKEN_MANDATORY_LABEL label {{medium_sid, SE_GROUP_INTEGRITY}};
      const auto label_size = static_cast<DWORD>(sizeof(label) + GetLengthSid(medium_sid));
      const bool integrity_set = SetTokenInformation(restricted.get(), TokenIntegrityLevel, const_cast<TOKEN_MANDATORY_LABEL *>(&label), label_size);
      FreeSid(medium_sid);
      if (!integrity_set) {
        return {};
      }

      TOKEN_ELEVATION elevation {};
      DWORD returned = 0;
      if (!GetTokenInformation(restricted.get(), TokenElevation, &elevation, sizeof(elevation), &returned) || elevation.TokenIsElevated || !IsTokenRestricted(restricted.get())) {
        return {};
      }
      SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
      PSID administrators_sid = nullptr;
      if (!AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators_sid)) {
        return {};
      }
      HANDLE raw_impersonation = nullptr;
      if (!DuplicateToken(restricted.get(), SecurityIdentification, &raw_impersonation)) {
        FreeSid(administrators_sid);
        return {};
      }
      auto impersonation = own_handle(raw_impersonation);
      BOOL administrator = FALSE;
      const bool membership_checked = CheckTokenMembership(impersonation.get(), administrators_sid, &administrator);
      FreeSid(administrators_sid);
      if (!membership_checked || administrator) {
        return {};
      }
      return restricted;
    }

    /**
     * @brief Build strict sorted Windows environment block.
     * @param policy Effective environment policy.
     * @param resolve_secret Secret resolver.
     * @param launch_values Launch-profile environment values applied over policy defaults.
     * @param output Resulting double-null-terminated environment block.
     * @return `true` when all values resolve and convert safely.
     */
    bool build_environment(const json &policy, const json &launch_values, const secret_resolver_t &resolve_secret, std::vector<wchar_t> &output) {
      std::map<std::wstring, std::wstring, environment_less_t> entries;
      const auto add_values = [&](const json &values) {
        for (const auto &[name, configured_value] : values.items()) {
          std::string value;
          if (configured_value.is_string()) {
            value = configured_value.get<std::string>();
          } else {
            if (!resolve_secret) {
              return false;
            }
            const auto resolved = resolve_secret(configured_value.at("secretRef").get<std::string>());
            if (!resolved) {
              return false;
            }
            value = *resolved;
          }
          std::wstring wide_name;
          std::wstring wide_value;
          if (!to_utf16(name, wide_name) || !to_utf16(value, wide_value) || wide_value.find(L'\0') != std::wstring::npos) {
            return false;
          }
          entries[std::move(wide_name)] = std::move(wide_value);
        }
        return true;
      };
      if (!add_values(policy.at("values")) || !add_values(launch_values)) {
        return false;
      }
      std::size_t size = 1;
      for (const auto &[name, value] : entries) {
        size += name.size() + value.size() + 2;
        if (size >= 32767) {
          return false;
        }
      }
      if (entries.empty()) {
        ++size;
      }
      output.assign(size, L'\0');
      std::size_t offset = 0;
      for (const auto &[name, value] : entries) {
        std::copy(name.begin(), name.end(), output.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += name.size();
        output[offset++] = L'=';
        std::copy(value.begin(), value.end(), output.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += value.size() + 1;
      }
      return true;
    }

    /**
     * @brief Configure kill-on-close and requested Job Object limits.
     * @param job Job Object handle.
     * @param policy Effective policy.
     * @return `true` when every requested limit was installed.
     */
    bool configure_job(const HANDLE job, const json &policy) {
      const auto &resources = policy.at("resources");
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
      if (!resources.at("memoryBytes").is_null()) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = static_cast<SIZE_T>(resources.at("memoryBytes").get<std::uint64_t>());
      }
      if (!resources.at("processCount").is_null()) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = resources.at("processCount").get<DWORD>();
      }
      if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return false;
      }
      if (!resources.at("cpuPercent").is_null()) {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu {};
        cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        cpu.CpuRate = resources.at("cpuPercent").get<DWORD>() * 100;
        if (!SetInformationJobObject(job, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu))) {
          return false;
        }
      }
      return true;
    }

    /**
     * @brief Live provider-owned process and Job Object state.
     */
    struct runtime_t {
      unique_handle_t job;  ///< Kill-on-close Job Object.
      unique_handle_t process;  ///< Root process handle.
      unique_handle_t deadline;  ///< Optional wall-clock deadline timer.
      unique_handle_t stop_deadline;  ///< Deadline-thread stop event.
      std::thread deadline_thread;  ///< Wall-clock enforcement thread.

      /** @brief Stop watchdog, kill remaining job members by handle close, and release state. */
      ~runtime_t() {
        stop_watchdog();
      }

      runtime_t() = default;  ///< Construct empty runtime state.
      runtime_t(const runtime_t &) = delete;  ///< Runtime handles cannot be copied.
      runtime_t &operator=(const runtime_t &) = delete;  ///< Runtime handles cannot be copy-assigned.
      runtime_t(runtime_t &&) = delete;  ///< Runtime state remains at a stable watchdog address.
      runtime_t &operator=(runtime_t &&) = delete;  ///< Runtime state remains at a stable watchdog address.

      /** @brief Stop and join wall-clock watchdog. */
      void stop_watchdog() noexcept {
        if (stop_deadline) {
          SetEvent(stop_deadline.get());
        }
        if (deadline_thread.joinable()) {
          deadline_thread.join();
        }
      }
    };

    /**
     * @brief Query active process count for a Job Object.
     * @param job Job Object handle.
     * @param active Receives active process count.
     * @return `true` when query succeeds.
     */
    bool active_processes(const HANDLE job, DWORD &active) {
      JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting {};
      if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
        return false;
      }
      active = accounting.ActiveProcesses;
      return true;
    }

    /**
     * @brief Read root-process exit code when available.
     * @param process Process handle.
     * @return Exit code, or no value while active or on failure.
     */
    std::optional<int> process_exit_code(const HANDLE process) {
      DWORD code = STILL_ACTIVE;
      if (!GetExitCodeProcess(process, &code) || code == STILL_ACTIVE) {
        return std::nullopt;
      }
      return static_cast<int>(code);
    }

    /**
     * @brief Shared implementation retained by generated callbacks.
     */
    struct provider_t {
      configuration_t configuration;  ///< Provider dependencies.
      std::mutex mutex;  ///< Serializes runtime map access.
      std::map<std::string, std::shared_ptr<runtime_t>, std::less<>> runtimes;  ///< Live runtimes by opaque ID.
      std::atomic_uint64_t generation {0};  ///< Runtime-ID uniqueness counter.

      /**
       * @brief Construct provider implementation.
       * @param value Provider dependencies.
       */
      explicit provider_t(configuration_t value):
          configuration(std::move(value)) {
      }

      /**
       * @brief Launch application with restricted token and atomic Job Object assignment.
       * @param request Validated launch request.
       * @param error Receives safe failure.
       * @return Provider runtime result, or no value.
       */
      std::optional<terra_sandboxes::launch_result_t> launch(const terra_sandboxes::launch_request_t &request, terra_sandboxes::error_t &error) {
        std::string reason;
        if (!capable(request, static_cast<bool>(configuration.resolve_secret), reason)) {
          error = {"unsupported_configuration", reason};
          return std::nullopt;
        }

        std::wstring executable;
        to_utf16(request.application.executable, executable);
        std::wstring command_line = quote_argument(executable);
        const auto &data = request.application.launch_data;
        if (data.is_object() && data.contains("arguments")) {
          for (const auto &argument : data.at("arguments")) {
            std::wstring converted;
            to_utf16(argument.get_ref<const std::string &>(), converted);
            command_line += L' ';
            command_line += quote_argument(converted);
          }
        }
        if (command_line.size() >= 32767) {
          error = {"launch_data_invalid", "Windows command line exceeds platform limit"};
          return std::nullopt;
        }
        std::wstring working_directory = std::filesystem::path(executable).parent_path().wstring();
        if (data.is_object() && data.contains("workingDirectory") && !data.at("workingDirectory").is_null()) {
          to_utf16(data.at("workingDirectory").get_ref<const std::string &>(), working_directory);
        }
        std::vector<wchar_t> environment;
        const auto launch_environment = data.is_object() && data.contains("environment") ? data.at("environment") : json::object();
        if (!build_environment(request.effective_policy.at("environment"), launch_environment, configuration.resolve_secret, environment)) {
          error = {"environment_failed", "Explicit environment could not be constructed"};
          return std::nullopt;
        }

        auto token = create_restricted_primary_token();
        if (!token) {
          error = {"restricted_token_failed", "Non-elevated restricted primary token could not be established"};
          return std::nullopt;
        }
        auto runtime = std::make_shared<runtime_t>();
        runtime->job = own_handle(CreateJobObjectW(nullptr, nullptr));
        if (!runtime->job || !configure_job(runtime->job.get(), request.effective_policy)) {
          error = {"job_configuration_failed", "Sandbox Job Object limits could not be established"};
          return std::nullopt;
        }

        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        auto attribute_storage = std::make_unique_for_overwrite<std::byte[]>(attribute_size);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.get());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
          error = {"job_assignment_failed", "Process attribute list could not be initialized"};
          return std::nullopt;
        }
        const auto delete_attributes = util::fail_guard([attributes]() {
          DeleteProcThreadAttributeList(attributes);
        });
        HANDLE job_handle = runtime->job.get();
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_handle, sizeof(job_handle), nullptr, nullptr)) {
          error = {"job_assignment_failed", "Atomic Job Object assignment could not be configured"};
          return std::nullopt;
        }

        STARTUPINFOEXW startup {};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process {};
        std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
        mutable_command.push_back(L'\0');
        constexpr DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_BREAKAWAY_FROM_JOB;
        if (!CreateProcessAsUserW(token.get(), executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, flags, environment.data(), working_directory.c_str(), &startup.StartupInfo, &process)) {
          error = {"process_creation_failed", "Restricted suspended process could not be created"};
          return std::nullopt;
        }
        runtime->process = own_handle(process.hProcess);
        auto thread = own_handle(process.hThread);

        const auto timeout = request.effective_policy.at("timeoutMs").get<std::int64_t>();
        if (timeout > 0) {
          runtime->deadline = own_handle(CreateWaitableTimerW(nullptr, TRUE, nullptr));
          runtime->stop_deadline = own_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
          LARGE_INTEGER due {.QuadPart = -timeout * 10000};
          if (!runtime->deadline || !runtime->stop_deadline || !SetWaitableTimer(runtime->deadline.get(), &due, 0, nullptr, nullptr, FALSE)) {
            TerminateJobObject(runtime->job.get(), ERROR_PROCESS_ABORTED);
            error = {"timeout_configuration_failed", "Wall-clock deadline could not be established"};
            return std::nullopt;
          }
        }

        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
          TerminateJobObject(runtime->job.get(), ERROR_PROCESS_ABORTED);
          error = {"process_resume_failed", "Atomically assigned process could not be resumed"};
          return std::nullopt;
        }
        if (timeout > 0) {
          const HANDLE stop = runtime->stop_deadline.get();
          const HANDLE deadline = runtime->deadline.get();
          const HANDLE job = runtime->job.get();
          runtime->deadline_thread = std::thread([stop, deadline, job]() {
            const HANDLE waits[] {stop, deadline};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
              TerminateJobObject(job, ERROR_TIMEOUT);
            }
          });
        }

        const auto runtime_id = request.sandbox_id + ":" + std::to_string(process.dwProcessId) + ":" + std::to_string(++generation);
        {
          std::scoped_lock lock {mutex};
          if (!runtimes.emplace(runtime_id, runtime).second) {
            TerminateJobObject(runtime->job.get(), ERROR_PROCESS_ABORTED);
            error = {"runtime_conflict", "Unique runtime state could not be recorded"};
            return std::nullopt;
          }
        }
        return terra_sandboxes::launch_result_t {runtime_id, std::nullopt, {}, {}};
      }

      /**
       * @brief Terminate all processes in one provider runtime.
       * @param runtime_id Opaque runtime ID.
       * @param force Requested force mode; Job Object termination is always authoritative.
       * @return Confirmed termination result.
       */
      terra_sandboxes::termination_t terminate(const std::string &runtime_id, const bool force) {
        static_cast<void>(force);
        std::shared_ptr<runtime_t> runtime;
        {
          std::scoped_lock lock {mutex};
          const auto found = runtimes.find(runtime_id);
          if (found == runtimes.end()) {
            return {true, std::nullopt, std::nullopt};
          }
          runtime = found->second;
        }
        runtime->stop_watchdog();
        DWORD active = 0;
        if (!active_processes(runtime->job.get(), active)) {
          return {false, std::nullopt, terra_sandboxes::error_t {"job_query_failed", "Sandbox Job Object state could not be queried"}};
        }
        if (active != 0 && !TerminateJobObject(runtime->job.get(), ERROR_PROCESS_ABORTED)) {
          return {false, std::nullopt, terra_sandboxes::error_t {"termination_failed", "Sandbox Job Object could not be terminated"}};
        }
        const auto deadline = GetTickCount64() + TERMINATION_WAIT_MS;
        do {
          if (!active_processes(runtime->job.get(), active)) {
            return {false, std::nullopt, terra_sandboxes::error_t {"job_query_failed", "Sandbox termination could not be confirmed"}};
          }
          if (active == 0) {
            break;
          }
          Sleep(10);
        } while (GetTickCount64() < deadline);
        if (active != 0) {
          return {false, std::nullopt, terra_sandboxes::error_t {"termination_timeout", "Sandbox processes did not terminate in time"}};
        }
        WaitForSingleObject(runtime->process.get(), TERMINATION_WAIT_MS);
        const auto exit_code = process_exit_code(runtime->process.get());
        {
          std::scoped_lock lock {mutex};
          runtimes.erase(runtime_id);
        }
        return {true, exit_code, std::nullopt};
      }

      /**
       * @brief Observe one provider runtime.
       * @param runtime_id Opaque runtime ID.
       * @return Current runtime status.
       */
      terra_sandboxes::reconciliation_t reconcile(const std::string &runtime_id) {
        std::shared_ptr<runtime_t> runtime;
        {
          std::scoped_lock lock {mutex};
          const auto found = runtimes.find(runtime_id);
          if (found == runtimes.end()) {
            return {terra_sandboxes::reconciliation_t::status_t::missing, std::nullopt, std::nullopt};
          }
          runtime = found->second;
        }
        DWORD active = 0;
        if (!active_processes(runtime->job.get(), active)) {
          return {terra_sandboxes::reconciliation_t::status_t::error, std::nullopt, terra_sandboxes::error_t {"job_query_failed", "Sandbox Job Object state could not be queried"}};
        }
        if (active != 0) {
          return {terra_sandboxes::reconciliation_t::status_t::running, std::nullopt, std::nullopt};
        }
        runtime->stop_watchdog();
        const auto exit_code = process_exit_code(runtime->process.get());
        {
          std::scoped_lock lock {mutex};
          const auto found = runtimes.find(runtime_id);
          if (found != runtimes.end() && found->second == runtime) {
            runtimes.erase(found);
          }
        }
        return {terra_sandboxes::reconciliation_t::status_t::exited, exit_code, std::nullopt};
      }
    };
  }  // namespace

  health_t health() {
    if (!create_restricted_primary_token()) {
      return {false, "restricted_token_unavailable", "Non-elevated restricted primary token could not be established"};
    }
    const auto job = own_handle(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
      return {false, "job_unavailable", "Sandbox Job Object could not be created"};
    }
    const json probe_policy {{"resources", {{"cpuPercent", nullptr}, {"memoryBytes", nullptr}, {"processCount", nullptr}}}};
    if (!configure_job(job.get(), probe_policy)) {
      return {false, "job_configuration_failed", "Sandbox Job Object limits could not be established"};
    }
    return {true, {}, {}};
  }

  terra_sandboxes::callbacks_t make_callbacks(configuration_t configuration) {
    auto provider = std::make_shared<provider_t>(std::move(configuration));
    const auto persistence_path = provider->configuration.persistence_path;
    return {
      [persistence_path]() -> std::optional<std::string> {
        if (!std::filesystem::exists(persistence_path)) {
          return std::nullopt;
        }
        return file_handler::read_file(persistence_path.string().c_str());
      },
      [persistence_path](const std::string &document) {
        return file_handler::write_file_atomic(persistence_path.string().c_str(), document) == 0;
      },
      []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      },
      []() {
        return uuid_util::uuid_t::generate().string();
      },
      provider->configuration.resolve_application,
      [provider](const terra_sandboxes::launch_request_t &request, std::string &reason) {
        return capable(request, static_cast<bool>(provider->configuration.resolve_secret), reason);
      },
      [provider](const terra_sandboxes::launch_request_t &request, terra_sandboxes::error_t &error) {
        return provider->launch(request, error);
      },
      [provider](const std::string &runtime_id, const bool force) {
        return provider->terminate(runtime_id, force);
      },
      [provider](const std::string &runtime_id) {
        return provider->reconcile(runtime_id);
      },
      [](const std::string &sandbox_id, const json &policy) {
        static_cast<void>(sandbox_id);
        static_cast<void>(policy);
        return true;
      },
    };
  }
}  // namespace terra::windows::sandbox
