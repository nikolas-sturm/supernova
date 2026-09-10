#include "stream_worker_process.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "stream_worker_protocol.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace terra {
namespace {
constexpr std::uint32_t kMaxFrameBytes = 1024 * 1024;

#ifdef _WIN32
using NativeHandle = HANDLE;
constexpr NativeHandle kInvalidHandle = nullptr;

bool readExact(NativeHandle handle, void *data, std::size_t size) {
  auto *bytes = static_cast<unsigned char *>(data);
  while (size > 0) {
    DWORD read = 0;
    if (!ReadFile(handle, bytes, static_cast<DWORD>(size), &read, nullptr) ||
        read == 0)
      return false;
    bytes += read;
    size -= read;
  }
  return true;
}

bool writeExact(NativeHandle handle, const void *data, std::size_t size) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  while (size > 0) {
    DWORD written = 0;
    if (!WriteFile(handle, bytes, static_cast<DWORD>(size), &written,
                   nullptr) ||
        written == 0)
      return false;
    bytes += written;
    size -= written;
  }
  return true;
}
#else
using NativeHandle = int;
constexpr NativeHandle kInvalidHandle = -1;

bool readExact(NativeHandle handle, void *data, std::size_t size) {
  auto *bytes = static_cast<unsigned char *>(data);
  while (size > 0) {
    const auto count = read(handle, bytes, size);
    if (count == 0)
      return false;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    bytes += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

bool writeExact(NativeHandle handle, const void *data, std::size_t size) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  while (size > 0) {
    const auto count = write(handle, bytes, size);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    bytes += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}
#endif

bool writeFrame(NativeHandle handle, const nlohmann::json &value) {
  const auto payload = value.dump();
  if (payload.empty() || payload.size() > kMaxFrameBytes)
    return false;
  const auto size = static_cast<std::uint32_t>(payload.size());
  const std::array<unsigned char, 4> header{
      static_cast<unsigned char>(size >> 24U),
      static_cast<unsigned char>(size >> 16U),
      static_cast<unsigned char>(size >> 8U), static_cast<unsigned char>(size)};
  return writeExact(handle, header.data(), header.size()) &&
         writeExact(handle, payload.data(), payload.size());
}

std::optional<nlohmann::json> readFrame(NativeHandle handle) {
  std::array<unsigned char, 4> header{};
  if (!readExact(handle, header.data(), header.size()))
    return std::nullopt;
  const auto size = (static_cast<std::uint32_t>(header[0]) << 24U) |
                    (static_cast<std::uint32_t>(header[1]) << 16U) |
                    (static_cast<std::uint32_t>(header[2]) << 8U) | header[3];
  if (size == 0 || size > kMaxFrameBytes)
    return std::nullopt;
  std::string payload(size, '\0');
  if (!readExact(handle, payload.data(), payload.size()))
    return std::nullopt;
  return nlohmann::json::parse(payload, nullptr, false);
}
} // namespace

struct StreamWorkerProcess::Impl {
  explicit Impl(Listener callback) : listener(std::move(callback)) {}

  Listener listener;
  std::mutex mutex;
  std::condition_variable condition;
  bool connected = false;
  bool failed = false;
  bool stopping = false;
  NativeHandle input = kInvalidHandle;
  NativeHandle output = kInvalidHandle;
  std::jthread reader;
#ifdef _WIN32
  PROCESS_INFORMATION process{};
#else
  pid_t process = -1;
#endif

  void readMessages() {
    while (const auto message = readFrame(output)) {
      if (message->is_discarded())
        break;
      const auto type = message->value("type", "");
      {
        std::lock_guard lock{mutex};
        if (type == "status" && message->value("state", "") == "connected")
          connected = true;
        if (type == "error" || type == "disconnected")
          failed = true;
      }
      condition.notify_all();
      try {
        if (listener)
          listener(*message);
      } catch (...) {
      }
    }
    bool unexpected = false;
    {
      std::lock_guard lock{mutex};
      unexpected = !stopping;
      failed = true;
    }
    condition.notify_all();
    if (unexpected) {
      try {
        if (listener) {
          listener({{"type", "error"},
                    {"message", "Stream worker exited unexpectedly."}});
        }
      } catch (...) {
      }
    }
  }
};

StreamWorkerProcess::StreamWorkerProcess(Listener listener)
    : impl_(std::make_unique<Impl>(std::move(listener))) {}

StreamWorkerProcess::~StreamWorkerProcess() { stop(); }

void StreamWorkerProcess::start(const StreamSessionConfig &config) {
  if (impl_->input != kInvalidHandle)
    throw std::runtime_error("Stream worker already started.");
#ifdef _WIN32
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE childInput = nullptr;
  HANDLE parentOutput = nullptr;
  const auto closePipes = [&]() {
    if (childInput)
      CloseHandle(std::exchange(childInput, nullptr));
    if (parentOutput)
      CloseHandle(std::exchange(parentOutput, nullptr));
    if (impl_->input)
      CloseHandle(std::exchange(impl_->input, kInvalidHandle));
    if (impl_->output)
      CloseHandle(std::exchange(impl_->output, kInvalidHandle));
  };
  if (!CreatePipe(&childInput, &impl_->input, &security, 0) ||
      !SetHandleInformation(impl_->input, HANDLE_FLAG_INHERIT, 0) ||
      !CreatePipe(&impl_->output, &parentOutput, &security, 0) ||
      !SetHandleInformation(impl_->output, HANDLE_FLAG_INHERIT, 0)) {
    closePipes();
    throw std::runtime_error("Failed to create stream worker pipes.");
  }
  std::array<wchar_t, 32768> executable{};
  if (GetModuleFileNameW(nullptr, executable.data(),
                         static_cast<DWORD>(executable.size())) == 0) {
    closePipes();
    throw std::runtime_error("Failed to resolve terra-core executable.");
  }
  std::wstring command =
      L"\"" + std::wstring{executable.data()} + L"\" --stream-worker";
  STARTUPINFOW startup{sizeof(STARTUPINFOW)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = childInput;
  startup.hStdOutput = parentOutput;
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &impl_->process)) {
    closePipes();
    throw std::runtime_error("Failed to start stream worker process.");
  }
  CloseHandle(childInput);
  CloseHandle(parentOutput);
#else
  static std::once_flag ignoreSigpipe;
  std::call_once(ignoreSigpipe, [] { std::signal(SIGPIPE, SIG_IGN); });
  int inputPipe[2]{};
  int outputPipe[2]{};
  if (pipe(inputPipe))
    throw std::runtime_error("Failed to create stream worker pipes.");
  if (pipe(outputPipe)) {
    close(inputPipe[0]);
    close(inputPipe[1]);
    throw std::runtime_error("Failed to create stream worker pipes.");
  }
  impl_->process = fork();
  if (impl_->process == 0) {
    dup2(inputPipe[0], STDIN_FILENO);
    dup2(outputPipe[1], STDOUT_FILENO);
    close(inputPipe[0]);
    close(inputPipe[1]);
    close(outputPipe[0]);
    close(outputPipe[1]);
    std::array<char, 4096> executable{};
    const auto length =
        readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    if (length <= 0)
      _exit(127);
    executable[static_cast<std::size_t>(length)] = '\0';
    execl(executable.data(), executable.data(), "--stream-worker", nullptr);
    _exit(127);
  }
  close(inputPipe[0]);
  close(outputPipe[1]);
  if (impl_->process < 0) {
    close(inputPipe[1]);
    close(outputPipe[0]);
    throw std::runtime_error("Failed to start stream worker process.");
  }
  impl_->input = inputPipe[1];
  impl_->output = outputPipe[0];
#endif
  if (!writeFrame(impl_->input, streamWorkerConfigJson(config))) {
    stop();
    throw std::runtime_error("Failed to configure stream worker process.");
  }
  impl_->reader = std::jthread{[this] { impl_->readMessages(); }};
}

bool StreamWorkerProcess::waitConnected(
    const std::chrono::milliseconds timeout) {
  std::unique_lock lock{impl_->mutex};
  impl_->condition.wait_for(lock, timeout,
                            [&] { return impl_->connected || impl_->failed; });
  return impl_->connected && !impl_->failed;
}

void StreamWorkerProcess::stop() noexcept {
  if (impl_->input == kInvalidHandle)
    return;
  {
    std::lock_guard lock{impl_->mutex};
    impl_->stopping = true;
  }
#ifdef _WIN32
  CloseHandle(impl_->input);
  impl_->input = kInvalidHandle;
  if (WaitForSingleObject(impl_->process.hProcess, 5000) == WAIT_TIMEOUT) {
    TerminateProcess(impl_->process.hProcess, 1);
  }
  if (impl_->reader.joinable())
    impl_->reader.join();
  CloseHandle(impl_->output);
  CloseHandle(impl_->process.hThread);
  CloseHandle(impl_->process.hProcess);
  impl_->output = kInvalidHandle;
  impl_->process = {};
#else
  close(impl_->input);
  impl_->input = kInvalidHandle;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (waitpid(impl_->process, nullptr, WNOHANG) == impl_->process)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  if (waitpid(impl_->process, nullptr, WNOHANG) == 0) {
    kill(impl_->process, SIGTERM);
    waitpid(impl_->process, nullptr, 0);
  }
  if (impl_->reader.joinable())
    impl_->reader.join();
  close(impl_->output);
  impl_->output = kInvalidHandle;
  impl_->process = -1;
#endif
}

} // namespace terra
