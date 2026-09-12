/**
 * @file src/platform/windows/sol_vdd.cpp
 * @brief Client implementation for the SolVDD topology control protocol.
 */

// header include
#include "sol_vdd.h"

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>

// system includes
#include <devguid.h>
#include <setupapi.h>

// local includes
#include "src/utility.h"

namespace sol_vdd {
  namespace {
    constexpr std::chrono::milliseconds PIPE_TIMEOUT {2000};  ///< Named-pipe connect and transfer timeout.
    constexpr std::chrono::seconds COMMAND_TIMEOUT {30};  ///< Complete topology transaction timeout.
    constexpr std::chrono::seconds INVENTORY_TIMEOUT {45};  ///< Monitor PnP inventory convergence timeout.
    constexpr std::chrono::milliseconds INVENTORY_POLL_INTERVAL {250};  ///< Monitor PnP inventory polling interval.
    std::mutex pipe_mutex;  ///< Serializes SolVDD's effectively single-client command server.
    using handle_t = util::safe_ptr_v2<void, BOOL, CloseHandle>;  ///< Owning Win32 handle.
    using registry_key_t = util::safe_ptr_v2<HKEY__, LONG, RegCloseKey>;  ///< Owning registry key.

    /**
     * @brief Wait for overlapped pipe I/O and cancel it after the given timeout.
     *
     * @param pipe Open named-pipe handle.
     * @param operation Pending overlapped operation.
     * @param transferred Receives completed byte count.
     * @param timeout Maximum wait duration.
     * @return True when operation completed successfully before timeout.
     */
    bool complete_io(const HANDLE pipe, OVERLAPPED &operation, DWORD &transferred, const std::chrono::milliseconds timeout) {
      const auto wait = WaitForSingleObject(operation.hEvent, static_cast<DWORD>(timeout.count()));
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
     * @brief Execute bounded overlapped I/O on an SolVDD pipe.
     *
     * @param pipe Open named-pipe handle.
     * @param timeout Maximum wait duration after the operation starts.
     * @tparam Start Callable that starts I/O with byte-count and operation pointers.
     * @param transferred Receives completed byte count.
     * @param start Callable that invokes `ReadFile` or `WriteFile`.
     * @return True when complete operation succeeded before timeout.
     */
    template<class Start>
    bool pipe_io(const HANDLE pipe, const std::chrono::milliseconds timeout, DWORD &transferred, Start &&start) {
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
      return complete_io(pipe, operation, transferred, timeout);
    }

    /**
     * @brief Result from one request on SolVDD's one-command pipe connection.
     */
    struct transaction_t {
      bool connected;  ///< Whether the named pipe was opened.
      bool written;  ///< Whether the complete command was written.
      std::string error;  ///< Human-readable Win32 failure description.
      std::array<std::byte, sol_vdd::MAX_MESSAGE_SIZE> response {};  ///< Response message bytes.
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
     * @brief Execute one request using SolVDD's one-command connection semantics.
     *
     * @param command Serialized request message.
     * @param read_response Whether to wait for one response message.
     * @return Connection, write, and optional response state.
     */
    transaction_t transact(const std::vector<std::byte> &command, const bool read_response) {
      std::lock_guard lock {pipe_mutex};
      transaction_t result {};
      if (command.empty() || command.size() > sol_vdd::MAX_MESSAGE_SIZE) {
        result.error = "Command exceeds SolVDD protocol limits";
        return result;
      }
      if (!WaitNamedPipeW(sol_vdd::PIPE_NAME, static_cast<DWORD>(PIPE_TIMEOUT.count()))) {
        result.error = last_error();
        return result;
      }

      const auto raw_handle = CreateFileW(sol_vdd::PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
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

      const auto command_bytes = static_cast<DWORD>(command.size());
      DWORD bytes_written {};
      if (!pipe_io(pipe.get(), PIPE_TIMEOUT, bytes_written, [&](auto *transferred, auto *operation) {
            return WriteFile(pipe.get(), command.data(), command_bytes, transferred, operation);
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
      if (!pipe_io(pipe.get(), COMMAND_TIMEOUT, bytes_read, [&](auto *transferred, auto *operation) {
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
     * @brief Read one device's EDID from its driver key.
     *
     * @param device_set Present monitor device set.
     * @param device Monitor device info.
     * @return EDID bytes, or no value when unavailable.
     */
    std::optional<std::vector<std::byte>> device_edid(HDEVINFO device_set, SP_DEVINFO_DATA &device) {
      registry_key_t key {SetupDiOpenDevRegKey(device_set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ)};
      if (!key || key.get() == INVALID_HANDLE_VALUE) {
        return std::nullopt;
      }
      DWORD type = 0;
      DWORD size = 0;
      if (RegQueryValueExW(static_cast<HKEY>(key.get()), L"EDID", nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_BINARY || size == 0 || size > 4096) {
        return std::nullopt;
      }
      std::vector<std::byte> edid(size);
      if (RegQueryValueExW(static_cast<HKEY>(key.get()), L"EDID", nullptr, nullptr, reinterpret_cast<LPBYTE>(edid.data()), &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      edid.resize(size);
      return edid;
    }

    /**
     * @brief Compare two connector records exactly.
     *
     * @param left First connector.
     * @param right Second connector.
     * @return True when every field matches.
     */
    bool same_connector(const connector_t &left, const connector_t &right) {
      return left.slot == right.slot && left.width == right.width && left.height == right.height && left.refresh_numerator == right.refresh_numerator && left.refresh_denominator == right.refresh_denominator && left.bit_depth == right.bit_depth && left.hdr == right.hdr;
    }

    /**
     * @brief Convert a protocol record to the Sol connector type.
     *
     * @param state Protocol connector record.
     * @return Sol connector.
     */
    connector_t to_connector(const sol_vdd::connector_state_t &state) {
      return {state.slot, state.width, state.height, state.refresh_numerator, state.refresh_denominator, state.bit_depth, state.hdr != 0};
    }
  }  // namespace

  bool valid_connector(const connector_t &connector) {
    return connector.slot < MAX_DISPLAY_COUNT && connector.width > 0 && connector.height > 0 && connector.width <= 16384 && connector.height <= 16384 && connector.refresh_numerator > 0 && connector.refresh_denominator > 0 && (connector.bit_depth == 8 || connector.bit_depth == 10);
  }

  std::vector<std::byte> encode_query_request(const std::uint32_t request_id) {
    const sol_vdd::request_header_t header {
      sol_vdd::PROTOCOL_MAGIC,
      sol_vdd::PROTOCOL_VERSION,
      sizeof(sol_vdd::request_header_t),
      request_id,
      static_cast<std::uint32_t>(sol_vdd::command_t::query_state),
      0,
      0,
    };
    std::vector<std::byte> message(sizeof(header));
    std::memcpy(message.data(), &header, sizeof(header));
    return message;
  }

  std::vector<std::byte> encode_apply_request(const std::uint32_t request_id, const std::vector<connector_t> &topology) {
    if (topology.size() > MAX_DISPLAY_COUNT) {
      return {};
    }
    const sol_vdd::request_header_t header {
      sol_vdd::PROTOCOL_MAGIC,
      sol_vdd::PROTOCOL_VERSION,
      sizeof(sol_vdd::request_header_t),
      request_id,
      static_cast<std::uint32_t>(sol_vdd::command_t::apply_topology),
      static_cast<std::uint32_t>(topology.size()),
      0,
    };
    std::vector<std::byte> message(sizeof(header) + topology.size() * sizeof(sol_vdd::connector_state_t));
    std::memcpy(message.data(), &header, sizeof(header));
    for (std::size_t index = 0; index < topology.size(); ++index) {
      const auto &connector = topology[index];
      const sol_vdd::connector_state_t state {
        connector.slot,
        connector.width,
        connector.height,
        connector.refresh_numerator,
        connector.refresh_denominator,
        connector.bit_depth,
        connector.hdr ? 1U : 0U,
        0,
      };
      std::memcpy(message.data() + sizeof(header) + index * sizeof(state), &state, sizeof(state));
    }
    return message;
  }

  std::optional<response_t> decode_response(const std::span<const std::byte> response, const std::uint32_t expected_request_id) {
    if (response.size() < sizeof(sol_vdd::response_header_t)) {
      return std::nullopt;
    }
    sol_vdd::response_header_t header {};
    std::memcpy(&header, response.data(), sizeof(header));
    if (header.magic != sol_vdd::PROTOCOL_MAGIC || header.version != sol_vdd::PROTOCOL_VERSION || header.header_size != sizeof(sol_vdd::response_header_t) || header.request_id != expected_request_id || header.reserved != 0 || header.connector_count > MAX_DISPLAY_COUNT || response.size() != sizeof(header) + header.connector_count * sizeof(sol_vdd::connector_state_t)) {
      return std::nullopt;
    }
    switch (static_cast<sol_vdd::status_t>(header.status)) {
      case sol_vdd::status_t::success:
      case sol_vdd::status_t::invalid_request:
      case sol_vdd::status_t::invalid_state:
      case sol_vdd::status_t::unsupported_version:
      case sol_vdd::status_t::internal_error:
      case sol_vdd::status_t::rollback_failed:
        break;
      default:
        return std::nullopt;
    }

    response_t decoded {static_cast<sol_vdd::status_t>(header.status), {}};
    decoded.connectors.reserve(header.connector_count);
    for (std::uint32_t index = 0; index < header.connector_count; ++index) {
      sol_vdd::connector_state_t state {};
      std::memcpy(&state, response.data() + sizeof(header) + index * sizeof(state), sizeof(state));
      if (!sol_vdd::valid(state) || std::ranges::any_of(decoded.connectors, [&](const auto &existing) {
            return existing.slot == state.slot;
          })) {
        return std::nullopt;
      }
      decoded.connectors.push_back(to_connector(state));
    }
    return decoded;
  }

  std::optional<std::uint32_t> monitor_slot_from_edid(const std::span<const std::byte> edid) {
    if (edid.size() < 128) {
      return std::nullopt;
    }
    const auto serial = static_cast<std::uint32_t>(std::to_integer<unsigned char>(edid[12])) | (static_cast<std::uint32_t>(std::to_integer<unsigned char>(edid[13])) << 8) | (static_cast<std::uint32_t>(std::to_integer<unsigned char>(edid[14])) << 16) | (static_cast<std::uint32_t>(std::to_integer<unsigned char>(edid[15])) << 24);
    if (serial < sol_vdd::EDID_SERIAL_BASE || serial >= sol_vdd::EDID_SERIAL_BASE + MAX_DISPLAY_COUNT) {
      return std::nullopt;
    }
    return serial - sol_vdd::EDID_SERIAL_BASE;
  }

  bool is_monitor_id(const std::wstring_view id) {
    std::wstring uppercase {id};
    std::ranges::transform(uppercase, uppercase.begin(), [](const wchar_t character) {
      return static_cast<wchar_t>(std::towupper(character));
    });
    return uppercase.find(L"\\SLV1337\\") != std::wstring::npos;
  }

  std::optional<std::string> canonical_monitor_id(const std::string_view id) {
    std::string uppercase {id};
    std::ranges::transform(uppercase, uppercase.begin(), [](const unsigned char character) {
      return static_cast<char>(std::toupper(character));
    });
    auto model = uppercase.find("\\SLV1337\\");
    if (model == std::string::npos) {
      model = uppercase.find("#SLV1337#");
    }
    if (model == std::string::npos) {
      return std::nullopt;
    }
    auto uid = uppercase.find("UID", model + 9);
    while (uid != std::string::npos && uid > 0 && uppercase[uid - 1] != '&' && uppercase[uid - 1] != '\\' && uppercase[uid - 1] != '#') {
      uid = uppercase.find("UID", uid + 3);
    }
    if (uid == std::string::npos) {
      return std::nullopt;
    }
    auto end = uid + 3;
    while (end < uppercase.size() && std::isdigit(static_cast<unsigned char>(uppercase[end]))) {
      ++end;
    }
    if (end == uid + 3 || (end < uppercase.size() && std::isalnum(static_cast<unsigned char>(uppercase[end])))) {
      return std::nullopt;
    }
    return "DISPLAY\\SLV1337\\" + uppercase.substr(uid, end - uid);
  }

  bool monitor_id_matches_path(const std::string_view instance_id, const std::string_view interface_path) {
    const auto instance = canonical_monitor_id(instance_id);
    const auto monitor_interface = canonical_monitor_id(interface_path);
    return instance && monitor_interface && *instance == *monitor_interface;
  }

  std::optional<std::vector<display_connector_t>> display_inventory() {
    const auto raw_set = SetupDiGetClassDevsW(&GUID_DEVCLASS_MONITOR, nullptr, nullptr, DIGCF_PRESENT);
    if (raw_set == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }
    using device_set_t = util::safe_ptr_v2<void, BOOL, SetupDiDestroyDeviceInfoList>;
    device_set_t device_set {raw_set};
    std::vector<display_connector_t> result;
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
      const auto canonical = canonical_monitor_id(utf8(instance_id));
      const auto edid = device_edid(device_set.get(), device);
      const auto slot = edid ? monitor_slot_from_edid(*edid) : std::nullopt;
      if (!canonical || !slot) {
        return std::nullopt;
      }
      result.push_back({*slot, *canonical});
    }
    std::ranges::sort(result, {}, &display_connector_t::slot);
    if (std::ranges::adjacent_find(result, [](const auto &left, const auto &right) {
          return left.slot == right.slot || left.id == right.id;
        }) != result.end()) {
      return std::nullopt;
    }
    return result;
  }

  probe_status_t probe() {
    static_assert(sizeof(wchar_t) == 2, "SolVDD protocol requires UTF-16 wchar_t");
    constexpr std::uint32_t request_id = 1;
    const auto transaction = transact(encode_query_request(request_id), true);
    if (!transaction.connected) {
      return {false, "provider_not_installed", "SolVDD control pipe is unavailable: " + transaction.error};
    }
    if (!transaction.written || transaction.response_size == 0) {
      return {false, "provider_incompatible", "SolVDD control pipe did not return a compatible response"};
    }
    const auto decoded = decode_response(std::span(transaction.response.data(), transaction.response_size), request_id);
    if (!decoded || decoded->status != sol_vdd::status_t::success) {
      return {false, "provider_incompatible", "SolVDD control pipe returned an unknown protocol response"};
    }
    return {true, {}, {}};
  }

  std::optional<std::vector<connector_t>> query_topology() {
    constexpr std::uint32_t request_id = 2;
    const auto transaction = transact(encode_query_request(request_id), true);
    if (!transaction.connected || !transaction.written || transaction.response_size == 0) {
      return std::nullopt;
    }
    const auto decoded = decode_response(std::span(transaction.response.data(), transaction.response_size), request_id);
    if (!decoded || decoded->status != sol_vdd::status_t::success) {
      return std::nullopt;
    }
    return decoded->connectors;
  }

  bool apply_topology_and_wait(const std::vector<connector_t> &topology) {
    if (topology.size() > MAX_DISPLAY_COUNT || !std::ranges::all_of(topology, valid_connector)) {
      return false;
    }
    const auto request = encode_apply_request(3, topology);
    if (request.empty()) {
      return false;
    }
    const auto transaction = transact(request, true);
    if (!transaction.connected || !transaction.written || transaction.response_size == 0) {
      return false;
    }
    const auto decoded = decode_response(std::span(transaction.response.data(), transaction.response_size), 3);
    if (!decoded || decoded->status != sol_vdd::status_t::success || decoded->connectors.size() != topology.size()) {
      return false;
    }
    const auto committed_matches = std::ranges::all_of(topology, [&](const auto &requested) {
      const auto committed = std::ranges::find(decoded->connectors, requested.slot, &connector_t::slot);
      return committed != decoded->connectors.end() && same_connector(requested, *committed);
    });
    if (!committed_matches) {
      return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + INVENTORY_TIMEOUT;
    do {
      const auto inventory = display_inventory();
      if (inventory && inventory->size() == topology.size() && std::ranges::all_of(topology, [&](const auto &requested) {
            const auto present = std::ranges::find(*inventory, requested.slot, &display_connector_t::slot);
            return present != inventory->end();
          })) {
        return true;
      }
      std::this_thread::sleep_for(INVENTORY_POLL_INTERVAL);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }
}  // namespace sol_vdd
