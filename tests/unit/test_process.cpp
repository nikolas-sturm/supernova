/**
 * @file tests/unit/test_process.cpp
 * @brief Test src/process.* functions.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <filesystem>
#include <fstream>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include <src/process.h>
#include <src/uuid.h>

namespace fs = std::filesystem;

TEST(UUIDValidationTest, AcceptsOnlyCanonicalSyntax) {
  EXPECT_TRUE(uuid_util::is_valid("11111111-1111-1111-1111-111111111111"));
  EXPECT_FALSE(uuid_util::is_valid("11111111-1111-1111-1111-11111111111"));
  EXPECT_FALSE(uuid_util::is_valid("11111111_1111-1111-1111-111111111111"));
  EXPECT_FALSE(uuid_util::is_valid("11111111-1111-1111-1111-11111111111z"));
}

class ProcessPNGTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    // Create test directory
    test_dir = fs::temp_directory_path() / "sunshine_process_png_test";  // NOSONAR(cpp:S5443): safe for tests
    fs::create_directories(test_dir);
  }

  void TearDown() override {
    // Clean up test directory
    if (fs::exists(test_dir)) {
      fs::remove_all(test_dir);
    }
    BaseTest::TearDown();
  }

  // Helper function to create a file with specific content
  void createTestFile(const fs::path &path, const std::vector<unsigned char> &content) const {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(content.data()), content.size());
    file.close();
  }

  fs::path test_dir;
};

// Tests for check_valid_png function
TEST_F(ProcessPNGTest, CheckValidPNG_ValidSignature) {
  // Valid PNG signature
  const std::vector<unsigned char> valid_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,  // PNG signature
    // Add some dummy data to make it more realistic
    0x00,
    0x00,
    0x00,
    0x0D,
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  EXPECT_TRUE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_WrongSignature) {
  // Invalid PNG signature (wrong magic bytes)
  const std::vector<unsigned char> invalid_png_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_TooShort) {
  // File too short (less than 8 bytes)
  const std::vector<unsigned char> short_data = {
    0x89,
    0x50,
    0x4E,
    0x47
  };

  const fs::path test_file = test_dir / "short.png";
  createTestFile(test_file, short_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_EmptyFile) {
  // Empty file
  const std::vector<unsigned char> empty_data = {};

  const fs::path test_file = test_dir / "empty.png";
  createTestFile(test_file, empty_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_NonExistentFile) {
  // File doesn't exist
  const fs::path test_file = test_dir / "nonexistent.png";

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_RealFile) {
  // Test with the actual sunshine.png from the project root

  // Only run this test if the file exists
  if (const fs::path sunshine_png = fs::path(SUNSHINE_SOURCE_DIR) / "sunshine.png"; fs::exists(sunshine_png)) {
    EXPECT_TRUE(proc::check_valid_png(sunshine_png));
  } else {
    GTEST_SKIP() << "sunshine.png not found in project root";
  }
}

TEST_F(ProcessPNGTest, CheckValidPNG_JPEGFile) {
  // JPEG signature (not PNG)
  const std::vector<unsigned char> jpeg_data = {
    0xFF,
    0xD8,
    0xFF,
    0xE0,
    0x00,
    0x10,
    0x4A,
    0x46
  };

  const fs::path test_file = test_dir / "fake.png";
  createTestFile(test_file, jpeg_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

TEST_F(ProcessPNGTest, CheckValidPNG_PartialSignature) {
  // Partial PNG signature (first 4 bytes correct, rest wrong)
  const std::vector<unsigned char> partial_png_data = {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "partial.png";
  createTestFile(test_file, partial_png_data);

  EXPECT_FALSE(proc::check_valid_png(test_file));
}

// Tests for validate_app_image_path function
TEST_F(ProcessPNGTest, ValidateAppImagePath_EmptyPath) {
  // Empty path should return default
  const std::string result = proc::validate_app_image_path("");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonPNGExtension) {
  // Non-PNG extension should return default
  const std::string result = proc::validate_app_image_path("image.jpg");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_CaseInsensitiveExtension) {
  // Test that .PNG (uppercase) is recognized
  // Create a valid PNG file
  const std::vector<unsigned char> valid_png_data = {
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
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "test.PNG";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  // Should accept uppercase .PNG extension
  EXPECT_NE(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_NonExistentFile) {
  // Non-existent PNG file should return default
  const std::string result = proc::validate_app_image_path("/nonexistent/path/image.png");
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_InvalidPNGSignature) {
  // File with .png extension but invalid signature should return default
  const std::vector<unsigned char> invalid_data = {
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00
  };

  const fs::path test_file = test_dir / "invalid.png";
  createTestFile(test_file, invalid_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, DEFAULT_APP_IMAGE_PATH);
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_ValidPNG) {
  // Valid PNG file should return the path
  const std::vector<unsigned char> valid_png_data = {
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
    0x49,
    0x48,
    0x44,
    0x52
  };

  const fs::path test_file = test_dir / "valid.png";
  createTestFile(test_file, valid_png_data);

  const std::string result = proc::validate_app_image_path(test_file.string());
  EXPECT_EQ(result, test_file.string());
}

TEST_F(ProcessPNGTest, ValidateAppImagePath_OldSteamDefault) {
  // Test the special case for old steam image path
  const std::string result = proc::validate_app_image_path("./assets/steam.png");
  EXPECT_EQ(result, SUNSHINE_ASSETS_DIR "/steam.png");
}

TEST_F(ProcessPNGTest, PersistsStableEclipseIdentityAcrossMutableMetadataChanges) {
  const auto catalog = test_dir / "apps.json";
  nlohmann::json source {
    {"env", nlohmann::json::object()},
    {"apps", {{{"name", "Original Name"}, {"cmd", ""}}}},
  };
  {
    std::ofstream file {catalog};
    file << source.dump(2);
  }

  const auto first = proc::parse(catalog.string());
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(first->get_apps().size(), 1);
  const auto original_uuid = first->get_apps().front().uuid;
  const auto original_id = first->get_apps().front().id;
  EXPECT_EQ(original_uuid.size(), 36);
  EXPECT_FALSE(original_id.empty());

  {
    std::ifstream file {catalog};
    source = nlohmann::json::parse(file);
  }
  ASSERT_TRUE(source["apps"][0].contains("x-eclipse"));
  source["apps"][0]["name"] = "Renamed Application";
  source["apps"][0]["image-path"] = "missing-artwork.png";
  {
    std::ofstream file {catalog};
    file << source.dump(2);
  }

  const auto second = proc::parse(catalog.string());
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(second->get_apps().size(), 1);
  EXPECT_EQ(second->get_apps().front().uuid, original_uuid);
  EXPECT_EQ(second->get_apps().front().id, original_id);
  EXPECT_EQ(second->find_app_by_uuid(original_uuid), &second->get_apps().front());
  EXPECT_EQ(second->find_app_by_id(std::stoi(original_id)), &second->get_apps().front());
}

TEST_F(ProcessPNGTest, RepairsDuplicateEclipseIdentitiesAndLegacyIds) {
  const auto catalog = test_dir / "duplicate-apps.json";
  constexpr auto duplicate_uuid = "11111111-1111-1111-1111-111111111111";
  const nlohmann::json source {
    {"env", nlohmann::json::object()},
    {"apps", {
               {{"name", "First"}, {"x-eclipse", {{"schemaVersion", 1}, {"uuid", duplicate_uuid}, {"legacyId", 42}}}},
               {{"name", "Second"}, {"x-eclipse", {{"schemaVersion", 1}, {"uuid", duplicate_uuid}, {"legacyId", 42}}}},
             }},
  };
  {
    std::ofstream file {catalog};
    file << source.dump(2);
  }

  const auto parsed = proc::parse(catalog.string());
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->get_apps().size(), 2);
  EXPECT_NE(parsed->get_apps()[0].uuid, parsed->get_apps()[1].uuid);
  EXPECT_NE(parsed->get_apps()[0].id, parsed->get_apps()[1].id);
}

TEST_F(ProcessPNGTest, RemovesMalformedOptionalEclipseMetadata) {
  const auto catalog = test_dir / "malformed-eclipse-metadata.json";
  const nlohmann::json source {
    {"env", nlohmann::json::object()},
    {"apps", {{{"name", "Malformed"}, {"x-eclipse", {
                                                      {"kind", 42},
                                                      {"tags", nlohmann::json::object()},
                                                      {"installed", "yes"},
                                                      {"classification", {{"source", false}, {"confidence", "high"}}},
                                                      {"assets", nlohmann::json::array()},
                                                    }}}}},
  };
  {
    std::ofstream file {catalog};
    file << source.dump(2);
  }

  const auto parsed = proc::parse(catalog.string());
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->get_apps().size(), 1);
  const auto &metadata = parsed->get_apps().front().eclipse_metadata;
  EXPECT_FALSE(metadata.contains("kind"));
  EXPECT_FALSE(metadata.contains("tags"));
  EXPECT_FALSE(metadata.contains("installed"));
  EXPECT_FALSE(metadata.contains("assets"));
  ASSERT_TRUE(metadata.at("classification").is_object());
  EXPECT_TRUE(metadata.at("classification").empty());
}
