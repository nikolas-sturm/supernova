/**
 * @file src/platform/windows/mttvdd.cpp
 * @brief Client implementation for MikeTheTech Virtual Display Driver.
 */

// header include
#include "mttvdd.h"

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <windows.h>

// system includes
#include <devguid.h>
#include <setupapi.h>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

// local includes
#include "src/utility.h"

namespace mttvdd {
  namespace {
    constexpr auto PIPE_NAME = LR"(\\.\pipe\MTTVirtualDisplayPipe)";
    constexpr auto SETTINGS_PATH = LR"(C:\VirtualDisplayDriver\vdd_settings.xml)";
    constexpr std::chrono::milliseconds PIPE_TIMEOUT {2000};
    constexpr std::chrono::seconds INVENTORY_TIMEOUT {45};
    constexpr std::chrono::milliseconds INVENTORY_POLL_INTERVAL {250};
    constexpr std::chrono::seconds RELOAD_COOLDOWN {3};
    constexpr std::size_t RESPONSE_BYTES = 512;
    std::mutex pipe_mutex;  ///< Serializes MttVDD's effectively single-client command server.
    std::mutex reload_mutex;  ///< Serializes reload commands and their cooldown periods.
    std::chrono::steady_clock::time_point last_reload_finished {};  ///< Last completed reload transaction.
    using handle_t = util::safe_ptr_v2<void, BOOL, CloseHandle>;  ///< Owning Win32 handle.

    /**
     * @brief Wait for overlapped pipe I/O and cancel it after protocol timeout.
     *
     * @param pipe Open named-pipe handle.
     * @param operation Pending overlapped operation.
     * @param transferred Receives completed byte count.
     * @return True when operation completed successfully before timeout.
     */
    bool complete_io(const HANDLE pipe, OVERLAPPED &operation, DWORD &transferred) {
      const auto wait = WaitForSingleObject(operation.hEvent, static_cast<DWORD>(PIPE_TIMEOUT.count()));
      if (wait == WAIT_OBJECT_0) {
        return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
      }

      const auto error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
      static_cast<void>(CancelIoEx(pipe, &operation));
      static_cast<void>(GetOverlappedResult(pipe, &operation, &transferred, TRUE));
      SetLastError(error);
      return false;
    }

    /**
     * @brief Execute bounded overlapped I/O on an MttVDD pipe.
     *
     * @param pipe Open named-pipe handle.
     * @tparam Start Callable that starts I/O with byte-count and operation pointers.
     * @param transferred Receives completed byte count.
     * @param start Callable that invokes `ReadFile` or `WriteFile`.
     * @return True when complete operation succeeded before timeout.
     */
    template<class Start>
    bool pipe_io(const HANDLE pipe, DWORD &transferred, Start &&start) {
      handle_t event {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
      if (!event) {
        return false;
      }
      OVERLAPPED operation {};
      operation.hEvent = event.get();
      if (start(&transferred, &operation)) {
        return true;
      }
      if (GetLastError() != ERROR_IO_PENDING) {
        return false;
      }
      return complete_io(pipe, operation, transferred);
    }

    /**
     * @brief Result from one request on MttVDD's one-command pipe connection.
     */
    struct transaction_t {
      bool connected;  ///< Whether the named pipe was opened.
      bool written;  ///< Whether the complete command was written.
      std::string error;  ///< Human-readable Win32 failure description.
      std::array<std::byte, RESPONSE_BYTES> response {};  ///< Optional response bytes.
      std::size_t response_size {};  ///< Number of bytes present in `response`.
    };

    /**
     * @brief Format the last Win32 error without leaking request data.
     *
     * @return Numeric Win32 error text.
     */
    std::string last_error() {
      return "Win32 error " + std::to_string(GetLastError());
    }

    /**
     * @brief Execute one command using MttVDD's one-command connection semantics.
     *
     * @param command UTF-16 command without terminator or framing.
     * @param read_response Whether to wait for one response message.
     * @return Connection, write, and optional response state.
     */
    transaction_t transact(const std::wstring_view command, const bool read_response) {
      std::lock_guard lock {pipe_mutex};
      transaction_t result {};
      if (command.empty() || command.size() > 127) {
        result.error = "Command exceeds MttVDD protocol limits";
        return result;
      }
      if (!WaitNamedPipeW(PIPE_NAME, static_cast<DWORD>(PIPE_TIMEOUT.count()))) {
        result.error = last_error();
        return result;
      }

      const auto raw_handle = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (raw_handle == INVALID_HANDLE_VALUE) {
        result.error = last_error();
        return result;
      }
      handle_t pipe {raw_handle};
      result.connected = true;

      DWORD mode = PIPE_READMODE_MESSAGE;
      if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
        result.error = last_error();
        return result;
      }

      const auto command_bytes = command.size() * sizeof(wchar_t);
      DWORD bytes_written {};
      if (!pipe_io(pipe.get(), bytes_written, [&](auto *transferred, auto *operation) {
            return WriteFile(pipe.get(), command.data(), static_cast<DWORD>(command_bytes), transferred, operation);
          }) ||
          bytes_written != command_bytes) {
        result.error = last_error();
        return result;
      }
      result.written = true;
      if (!read_response) {
        return result;
      }

      DWORD bytes_read {};
      if (!pipe_io(pipe.get(), bytes_read, [&](auto *transferred, auto *operation) {
            return ReadFile(pipe.get(), result.response.data(), static_cast<DWORD>(result.response.size()), transferred, operation);
          })) {
        result.error = last_error();
        return result;
      }
      result.response_size = bytes_read;
      return result;
    }

    /**
     * @brief Convert Windows UTF-16 text to UTF-8.
     *
     * @param value UTF-16 input.
     * @return UTF-8 text, or empty text after conversion failure.
     */
    std::string utf8(const std::wstring_view value) {
      if (value.empty()) {
        return {};
      }
      const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
      if (size <= 0) {
        return {};
      }
      std::string result(static_cast<std::size_t>(size), '\0');
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size) {
        return {};
      }
      return result;
    }

    /**
     * @brief Normalize Windows device identity for cross-API comparison.
     *
     * @param value Device identity text.
     * @return Lowercase ASCII alphanumeric identity.
     */
    std::string normalized_id(const std::string_view value) {
      std::string result;
      result.reserve(value.size());
      for (const unsigned char character : value) {
        if (std::isalnum(character)) {
          result.push_back(static_cast<char>(std::tolower(character)));
        }
      }
      return result;
    }
  }  // namespace

  std::optional<std::wstring> display_count_command(const std::uint32_t count) {
    if (count > MAX_DISPLAY_COUNT) {
      return std::nullopt;
    }
    return L"SETDISPLAYCOUNT " + std::to_wstring(count);
  }

  bool is_settings_response(std::wstring_view response) {
    while (!response.empty() && response.back() == L'\0') {
      response.remove_suffix(1);
    }
    return response.starts_with(L"SETTINGS ") && response.find(L"DEBUG=") != std::wstring_view::npos && response.find(L"LOG=") != std::wstring_view::npos;
  }

  std::optional<std::uint32_t> parse_display_count(const std::string_view xml) {
    try {
      std::stringstream stream;
      stream.write(xml.data(), static_cast<std::streamsize>(xml.size()));
      boost::property_tree::ptree settings;
      boost::property_tree::read_xml(stream, settings);
      const auto count = settings.get<std::int64_t>("vdd_settings.monitors.count");
      if (count < 0 || count > MAX_DISPLAY_COUNT) {
        return std::nullopt;
      }
      return static_cast<std::uint32_t>(count);
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }

  std::optional<std::uint32_t> configured_display_count() {
    std::ifstream stream {SETTINGS_PATH, std::ios::binary};
    if (!stream) {
      return std::nullopt;
    }
    const std::string xml {std::istreambuf_iterator<char> {stream}, std::istreambuf_iterator<char> {}};
    return parse_display_count(xml);
  }

  bool is_monitor_id(const std::wstring_view id) {
    std::wstring uppercase {id};
    std::ranges::transform(uppercase, uppercase.begin(), [](const wchar_t character) {
      return static_cast<wchar_t>(std::towupper(character));
    });
    return uppercase.find(L"\\MTT1337\\") != std::wstring::npos;
  }

  bool monitor_id_matches_path(const std::string_view instance_id, const std::string_view interface_path) {
    const auto instance = normalized_id(instance_id);
    return !instance.empty() && normalized_id(interface_path).find(instance) != std::string::npos;
  }

  std::optional<std::vector<std::string>> display_inventory() {
    const auto raw_set = SetupDiGetClassDevsW(&GUID_DEVCLASS_MONITOR, nullptr, nullptr, DIGCF_PRESENT);
    if (raw_set == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }
    using device_set_t = util::safe_ptr_v2<void, BOOL, SetupDiDestroyDeviceInfoList>;
    device_set_t device_set {raw_set};
    std::vector<std::string> result;
    for (DWORD index = 0;; ++index) {
      SP_DEVINFO_DATA device {.cbSize = sizeof(SP_DEVINFO_DATA)};
      if (!SetupDiEnumDeviceInfo(device_set.get(), index, &device)) {
        if (GetLastError() == ERROR_NO_MORE_ITEMS) {
          break;
        }
        return std::nullopt;
      }
      DWORD required = 0;
      SetupDiGetDeviceInstanceIdW(device_set.get(), &device, nullptr, 0, &required);
      if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::nullopt;
      }
      std::wstring instance_id(required, L'\0');
      if (!SetupDiGetDeviceInstanceIdW(device_set.get(), &device, instance_id.data(), required, nullptr)) {
        return std::nullopt;
      }
      instance_id.resize(std::wcslen(instance_id.c_str()));
      if (!is_monitor_id(instance_id)) {
        continue;
      }
      auto converted = utf8(instance_id);
      if (converted.empty()) {
        return std::nullopt;
      }
      result.push_back(std::move(converted));
    }
    std::ranges::sort(result);
    if (std::ranges::adjacent_find(result) != result.end()) {
      return std::nullopt;
    }
    return result;
  }

  status_t probe() {
    static_assert(sizeof(wchar_t) == 2, "MttVDD protocol requires UTF-16 wchar_t");
    const auto transaction = transact(L"GETSETTINGS", true);
    if (!transaction.connected) {
      return {false, "provider_not_installed", "MttVDD control pipe is unavailable: " + transaction.error};
    }
    if (!transaction.written || transaction.response_size == 0 || transaction.response_size % sizeof(wchar_t) != 0) {
      return {false, "provider_incompatible", "MttVDD control pipe did not return a compatible response"};
    }

    std::wstring response(transaction.response_size / sizeof(wchar_t), L'\0');
    std::memcpy(response.data(), transaction.response.data(), transaction.response_size);
    if (!is_settings_response(response)) {
      return {false, "provider_incompatible", "MttVDD control pipe returned an unknown protocol response"};
    }
    return {true, {}, {}};
  }

  bool set_display_count(const std::uint32_t count) {
    const auto command = display_count_command(count);
    return command && transact(*command, false).written;
  }

  bool set_display_count_and_wait(const std::uint32_t count) {
    std::lock_guard reload_lock {reload_mutex};
    if (count > MAX_DISPLAY_COUNT) {
      return false;
    }
    const auto earliest_reload = last_reload_finished + RELOAD_COOLDOWN;
    if (std::chrono::steady_clock::now() < earliest_reload) {
      std::this_thread::sleep_until(earliest_reload);
    }
    if (!set_display_count(count)) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + INVENTORY_TIMEOUT;
    do {
      const auto configured = configured_display_count();
      const auto inventory = display_inventory();
      if (configured && *configured == count && inventory && inventory->size() == count && probe().available) {
        last_reload_finished = std::chrono::steady_clock::now();
        return true;
      }
      std::this_thread::sleep_for(INVENTORY_POLL_INTERVAL);
    } while (std::chrono::steady_clock::now() < deadline);
    last_reload_finished = std::chrono::steady_clock::now();
    return false;
  }
}  // namespace mttvdd
