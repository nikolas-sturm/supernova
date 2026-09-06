/**
 * @file src/eclipse_assets.cpp
 * @brief Validated Eclipse catalog image asset metadata implementation.
 */

// header include
#include "eclipse_assets.h"

// standard includes
#include <array>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

// local includes
#include "crypto.h"

namespace eclipse_assets {
  namespace {
    constexpr std::array<std::uint8_t, 8> PNG_SIGNATURE {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    /**
     * @brief Read an unsigned big-endian 16-bit integer.
     *
     * @param bytes Source bytes.
     * @param offset First byte offset.
     * @return Parsed integer.
     */
    std::uint16_t read_be16(const std::vector<std::uint8_t> &bytes, const std::size_t offset) {
      return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1]);
    }

    /**
     * @brief Read an unsigned big-endian 32-bit integer.
     *
     * @param bytes Source bytes.
     * @param offset First byte offset.
     * @return Parsed integer.
     */
    std::uint32_t read_be32(const std::vector<std::uint8_t> &bytes, const std::size_t offset) {
      return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
             (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
             bytes[offset + 3];
    }

    /**
     * @brief Parse PNG dimensions after validating signature and IHDR placement.
     *
     * @param bytes Complete bounded image bytes.
     * @return Width and height, or no value for malformed data.
     */
    std::optional<std::pair<std::uint32_t, std::uint32_t>> png_dimensions(const std::vector<std::uint8_t> &bytes) {
      if (bytes.size() < 24 || !std::equal(PNG_SIGNATURE.begin(), PNG_SIGNATURE.end(), bytes.begin()) || std::string_view {reinterpret_cast<const char *>(bytes.data() + 12), 4} != "IHDR") {
        return std::nullopt;
      }
      const auto width = read_be32(bytes, 16);
      const auto height = read_be32(bytes, 20);
      if (width == 0 || height == 0) {
        return std::nullopt;
      }
      return std::pair {width, height};
    }

    /**
     * @brief Determine whether a JPEG marker carries frame dimensions.
     *
     * @param marker Marker byte after `0xff`.
     * @return `true` for Start Of Frame markers carrying dimensions.
     */
    bool is_start_of_frame(const std::uint8_t marker) {
      return (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) || (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
    }

    /**
     * @brief Parse JPEG dimensions from Start Of Frame metadata.
     *
     * @param bytes Complete bounded image bytes.
     * @return Width and height, or no value for malformed/unsupported data.
     */
    std::optional<std::pair<std::uint32_t, std::uint32_t>> jpeg_dimensions(const std::vector<std::uint8_t> &bytes) {
      if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
        return std::nullopt;
      }
      std::size_t offset = 2;
      while (offset + 3 < bytes.size()) {
        while (offset < bytes.size() && bytes[offset] == 0xFF) {
          ++offset;
        }
        if (offset >= bytes.size()) {
          return std::nullopt;
        }
        const auto marker = bytes[offset++];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
          continue;
        }
        if (marker == 0xD9 || marker == 0xDA || offset + 1 >= bytes.size()) {
          return std::nullopt;
        }
        const auto segment_size = read_be16(bytes, offset);
        if (segment_size < 2 || segment_size > bytes.size() - offset) {
          return std::nullopt;
        }
        if (is_start_of_frame(marker)) {
          if (segment_size < 7) {
            return std::nullopt;
          }
          const auto height = read_be16(bytes, offset + 3);
          const auto width = read_be16(bytes, offset + 5);
          if (width == 0 || height == 0) {
            return std::nullopt;
          }
          return std::pair<std::uint32_t, std::uint32_t> {width, height};
        }
        offset += segment_size;
      }
      return std::nullopt;
    }

    /**
     * @brief Convert SHA-256 bytes to lowercase hexadecimal text.
     *
     * @param digest SHA-256 digest.
     * @return Lowercase hexadecimal digest.
     */
    std::string digest_hex(const crypto::sha256_t &digest) {
      constexpr std::string_view digits = "0123456789abcdef";
      std::string result(digest.size() * 2, '\0');
      for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = digits[digest[index] >> 4U];
        result[index * 2 + 1] = digits[digest[index] & 0x0FU];
      }
      return result;
    }

    /**
     * @brief Derive a nonzero numeric asset revision from SHA-256 bytes.
     *
     * @param digest SHA-256 digest.
     * @return Stable nonzero content revision.
     */
    std::uint64_t digest_revision(const crypto::sha256_t &digest) {
      std::uint64_t revision {};
      for (std::size_t index = 0; index < sizeof(revision); ++index) {
        revision = (revision << 8U) | digest[index];
      }
      return revision == 0 ? 1 : revision;
    }
  }  // namespace

  std::optional<asset_t> inspect(const std::filesystem::path &path, std::string id) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > MAX_ASSET_BYTES || size > std::numeric_limits<std::size_t>::max()) {
      return std::nullopt;
    }

    std::ifstream stream {path, std::ios::binary};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
      return std::nullopt;
    }

    std::string media_type;
    auto dimensions = png_dimensions(bytes);
    if (dimensions) {
      media_type = "image/png";
    } else {
      dimensions = jpeg_dimensions(bytes);
      if (!dimensions) {
        return std::nullopt;
      }
      media_type = "image/jpeg";
    }

    const std::string_view contents {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    const auto digest = crypto::hash(contents);
    return asset_t {
      .id = std::move(id),
      .media_type = std::move(media_type),
      .width = dimensions->first,
      .height = dimensions->second,
      .revision = digest_revision(digest),
      .etag = '"' + digest_hex(digest) + '"',
      .path = path,
      .size = size,
    };
  }
}  // namespace eclipse_assets
