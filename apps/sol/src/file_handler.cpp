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
    if (std::filesystem::exists(destination, error) && !error) {
      const auto permissions = std::filesystem::status(destination, error).permissions();
      if (!error) {
        std::filesystem::permissions(temporary, permissions, error);
      }
    }

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
#endif
    return 0;
  }
}  // namespace file_handler
