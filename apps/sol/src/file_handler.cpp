/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <filesystem>
#include <fstream>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

// local includes
#include "file_handler.h"
#include "logging.h"

namespace file_handler {
  std::string get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string();
  }

  bool make_directory(const std::string &path) {
    // first, check if the directory already exists
    if (std::filesystem::exists(path)) {
      return true;
    }

    return std::filesystem::create_directories(path);
  }

  std::string read_file(const char *path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
  }

  int write_file(const char *path, const std::string_view &contents) {
    std::ofstream out(path);

    if (!out.is_open()) {
      return -1;
    }

    out << contents;

    return 0;
  }

  int write_file_atomic(const char *path, const std::string_view contents) {
    const std::filesystem::path destination {path};
    auto temporary = destination;
    temporary += ".tmp";

    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) {
        return -1;
      }
      out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
      out.flush();
      if (!out.good()) {
        out.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return -1;
      }
      out.close();
      if (out.fail()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return -1;
      }
    }

    std::error_code error;
    const auto destination_exists = std::filesystem::exists(destination, error);
    if (error) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return -1;
    }
    if (destination_exists) {
      const auto permissions = std::filesystem::status(destination, error).permissions();
      if (!error) {
        std::filesystem::permissions(temporary, permissions, error);
      }
      if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return -1;
      }
    }

#ifndef _WIN32
    const auto temporary_native = temporary.c_str();
    const auto temporary_fd = open(temporary_native, O_RDONLY | O_CLOEXEC);
    if (temporary_fd < 0 || fsync(temporary_fd) != 0) {
      if (temporary_fd >= 0) {
        close(temporary_fd);
      }
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return -1;
    }
    close(temporary_fd);
#endif

#ifdef _WIN32
    const auto temporary_wide = temporary.wstring();
    const auto destination_wide = destination.wstring();
    if (!MoveFileExW(temporary_wide.c_str(), destination_wide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      std::filesystem::remove(temporary, error);
      return -1;
    }
#else
    std::filesystem::rename(temporary, destination, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      return -1;
    }
    const auto parent = destination.parent_path().empty() ? std::filesystem::path {"."} : destination.parent_path();
    const auto parent_fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent_fd < 0) {
      BOOST_LOG(warning) << "Atomic file replacement committed, but parent directory could not be opened for synchronization: " << parent;
      return 0;
    }
    if (fsync(parent_fd) != 0) {
      BOOST_LOG(warning) << "Atomic file replacement committed, but parent directory synchronization failed: " << parent;
    }
    close(parent_fd);
#endif
    return 0;
  }
}  // namespace file_handler
