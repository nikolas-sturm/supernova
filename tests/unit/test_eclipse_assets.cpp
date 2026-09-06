/**
 * @file tests/unit/test_eclipse_assets.cpp
 * @brief Test Eclipse catalog image inspection.
 */

// standard includes
#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

// local includes
#include <src/eclipse_assets.h>

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
    temporary_asset_t asset {std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / name};
    std::ofstream stream {asset.path, std::ios::binary};
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return asset;
  }
}  // namespace

TEST(EclipseAssetsTest, InspectsPngMetadataAndStableRevision) {
  const std::vector<std::uint8_t> png {
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
    0x02,
    0x80,
    0x00,
    0x00,
    0x01,
    0xE0,
  };
  const auto file = write_asset("eclipse-asset.png", png);

  const auto first = eclipse_assets::inspect(file.path, "poster");
  const auto second = eclipse_assets::inspect(file.path, "poster");

  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->id, "poster");
  EXPECT_EQ(first->media_type, "image/png");
  EXPECT_EQ(first->width, 640);
  EXPECT_EQ(first->height, 480);
  EXPECT_EQ(first->size, png.size());
  EXPECT_EQ(first->revision, second->revision);
  EXPECT_EQ(first->etag, second->etag);
}

TEST(EclipseAssetsTest, InspectsJpegMetadata) {
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
    0x07,
    0x08,
    0x04,
    0x38,
    0x07,
    0x80,
  };
  const auto file = write_asset("eclipse-asset.jpg", jpeg);
  const auto asset = eclipse_assets::inspect(file.path, "icon");

  ASSERT_TRUE(asset);
  EXPECT_EQ(asset->media_type, "image/jpeg");
  EXPECT_EQ(asset->width, 1920);
  EXPECT_EQ(asset->height, 1080);
}

TEST(EclipseAssetsTest, RejectsMissingEmptyMalformedAndOversizedAssets) {
  EXPECT_FALSE(eclipse_assets::inspect(std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "missing-eclipse-asset.png", "poster"));

  const auto empty = write_asset("empty-eclipse-asset.png", {});
  EXPECT_FALSE(eclipse_assets::inspect(empty.path, "poster"));

  const auto malformed = write_asset("malformed-eclipse-asset.png", {0x89, 0x50, 0x4E, 0x47});
  EXPECT_FALSE(eclipse_assets::inspect(malformed.path, "poster"));

  temporary_asset_t oversized {std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "oversized-eclipse-asset.png"};
  std::ofstream stream {oversized.path, std::ios::binary};
  stream.seekp(static_cast<std::streamoff>(eclipse_assets::MAX_ASSET_BYTES));
  stream.put('\0');
  stream.close();
  EXPECT_FALSE(eclipse_assets::inspect(oversized.path, "poster"));
}
