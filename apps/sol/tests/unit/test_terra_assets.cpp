/**
 * @file tests/unit/test_terra_assets.cpp
 * @brief Test Terra catalog image inspection.
 */

// standard includes
#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

// local includes
#include <src/terra_assets.h>

namespace {
  /**
   * @brief Temporary asset file deleted when a test exits.
   */
  struct temporary_asset_t {
    std::filesystem::path path;  ///< Temporary file path.

    /**
     * @brief Remove temporary asset.
     */
    ~temporary_asset_t() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  };

  /**
   * @brief Write binary test data under the test executable directory.
   *
   * @param name Unique file name.
   * @param bytes Bytes to write.
   * @return RAII temporary file.
   */
  temporary_asset_t write_asset(const std::string_view name, const std::vector<std::uint8_t> &bytes) {
    temporary_asset_t asset {std::filesystem::path {SOL_TEST_BIN_DIR} / name};
    std::ofstream stream {asset.path, std::ios::binary};
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return asset;
  }

  /**
   * @brief Return a complete valid one-pixel PNG.
   * @return PNG bytes with valid chunk CRCs.
   */
  std::vector<std::uint8_t> valid_png() {
    return {
      0x89,
      0x50,
      0x4E,
      0x47,
      0x0D,
      0x0A,
      0x1A,
      0x0A,
      0x00,
      0x00,
      0x00,
      0x0D,
      'I',
      'H',
      'D',
      'R',
      0x00,
      0x00,
      0x00,
      0x01,
      0x00,
      0x00,
      0x00,
      0x01,
      0x08,
      0x06,
      0x00,
      0x00,
      0x00,
      0x1F,
      0x15,
      0xC4,
      0x89,
      0x00,
      0x00,
      0x00,
      0x0D,
      'I',
      'D',
      'A',
      'T',
      0x08,
      0xD7,
      0x63,
      0xF8,
      0xCF,
      0xC0,
      0xF0,
      0x1F,
      0x00,
      0x05,
      0x00,
      0x01,
      0xFF,
      0x72,
      0x9C,
      0x52,
      0x67,
      0x00,
      0x00,
      0x00,
      0x00,
      'I',
      'E',
      'N',
      'D',
      0xAE,
      0x42,
      0x60,
      0x82,
    };
  }
}  // namespace

TEST(TerraAssetsTest, InspectsPngMetadataAndStableRevision) {
  const auto png = valid_png();
  const auto file = write_asset("terra-asset.png", png);

  const auto first = terra_assets::inspect(file.path, "poster");
  const auto second = terra_assets::inspect(file.path, "poster");

  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->id, "poster");
  EXPECT_EQ(first->media_type, "image/png");
  EXPECT_EQ(first->width, 1);
  EXPECT_EQ(first->height, 1);
  EXPECT_EQ(first->size, png.size());
  EXPECT_EQ(first->revision, second->revision);
  EXPECT_LE(first->revision, 9007199254740991ULL);
  EXPECT_EQ(first->etag, second->etag);
}

TEST(TerraAssetsTest, InspectsJpegMetadata) {
  const std::vector<std::uint8_t> jpeg {
    0xFF,
    0xD8,
    0xFF,
    0xE0,
    0x00,
    0x04,
    0x00,
    0x00,
    0xFF,
    0xC0,
    0x00,
    0x0B,
    0x08,
    0x04,
    0x38,
    0x07,
    0x80,
    0x01,
    0x01,
    0x11,
    0x00,
    0xFF,
    0xDA,
    0x00,
    0x08,
    0x01,
    0x01,
    0x00,
    0x00,
    0x3F,
    0x00,
    0x01,
    0xFF,
    0xD9,
  };
  const auto file = write_asset("terra-asset.jpg", jpeg);
  const auto asset = terra_assets::inspect(file.path, "icon");

  ASSERT_TRUE(asset);
  EXPECT_EQ(asset->media_type, "image/jpeg");
  EXPECT_EQ(asset->width, 1920);
  EXPECT_EQ(asset->height, 1080);
}

TEST(TerraAssetsTest, RejectsMissingEmptyMalformedAndOversizedAssets) {
  EXPECT_FALSE(terra_assets::inspect(std::filesystem::path {SOL_TEST_BIN_DIR} / "missing-terra-asset.png", "poster"));

  const auto empty = write_asset("empty-terra-asset.png", {});
  EXPECT_FALSE(terra_assets::inspect(empty.path, "poster"));

  const auto malformed = write_asset("malformed-terra-asset.png", {0x89, 0x50, 0x4E, 0x47});
  EXPECT_FALSE(terra_assets::inspect(malformed.path, "poster"));

  const auto truncated_png = write_asset("truncated-terra-asset.png", {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R', 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01});
  EXPECT_FALSE(terra_assets::inspect(truncated_png.path, "poster"));

  const auto truncated_jpeg = write_asset("truncated-terra-asset.jpg", {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x11, 0x00});
  EXPECT_FALSE(terra_assets::inspect(truncated_jpeg.path, "poster"));

  auto bad_crc_bytes = valid_png();
  bad_crc_bytes[29] ^= 0x01;
  const auto bad_crc = write_asset("bad-crc-terra-asset.png", bad_crc_bytes);
  EXPECT_FALSE(terra_assets::inspect(bad_crc.path, "poster"));

  const auto scan_before_frame = write_asset("scan-before-frame-terra-asset.jpg", {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0x01, 0xFF, 0xD9});
  EXPECT_FALSE(terra_assets::inspect(scan_before_frame.path, "poster"));

  temporary_asset_t oversized {std::filesystem::path {SOL_TEST_BIN_DIR} / "oversized-terra-asset.png"};
  std::ofstream stream {oversized.path, std::ios::binary};
  stream.seekp(static_cast<std::streamoff>(terra_assets::MAX_ASSET_BYTES));
  stream.put('\0');
  stream.close();
  EXPECT_FALSE(terra_assets::inspect(oversized.path, "poster"));
}
