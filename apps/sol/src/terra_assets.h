/**
 * @file src/terra_assets.h
 * @brief Validated Terra catalog image asset metadata.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

/**
 * @brief Terra application asset inspection.
 */
namespace terra_assets {
  constexpr std::uintmax_t MAX_ASSET_BYTES = 8U * 1024U * 1024U;  ///< Largest asset accepted by Terra Catalog V2.

  /**
   * @brief Validated image asset ready for publication.
   */
  struct asset_t {
    std::string id;  ///< Stable application-local asset identifier.
    std::string media_type;  ///< Validated MIME media type.
    std::uint32_t width;  ///< Image width in pixels.
    std::uint32_t height;  ///< Image height in pixels.
    std::uint64_t revision;  ///< Content-derived nonzero revision.
    std::string etag;  ///< Strong content-derived HTTP entity tag.
    std::filesystem::path path;  ///< Validated local asset path.
    std::uintmax_t size;  ///< Asset size in bytes.
  };

  /**
   * @brief Inspect a local PNG or JPEG without decoding pixel data.
   *
   * @param path Candidate image path.
   * @param id Stable application-local asset identifier.
   * @return Validated asset, or no value for missing, malformed, empty, or oversized data.
   */
  std::optional<asset_t> inspect(const std::filesystem::path &path, std::string id);
}  // namespace terra_assets
