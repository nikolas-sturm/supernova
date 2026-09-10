/**
 * @file tests/unit/test_terra_api_primitives.cpp
 * @brief Tests for dependency-light Terra API request primitives.
 */

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/terra_api.h>

TEST(TerraApiPrimitivesTest, RequiresUnsignedIntegerSchemaVersionOne) {
  EXPECT_TRUE(terra_api::valid_request_schema(nlohmann::json::parse(R"({"schemaVersion":1})")));
  EXPECT_FALSE(terra_api::valid_request_schema(nlohmann::json::parse(R"({})")));
  EXPECT_FALSE(terra_api::valid_request_schema(nlohmann::json::parse(R"({"schemaVersion":1.0})")));
  EXPECT_FALSE(terra_api::valid_request_schema(nlohmann::json::parse(R"({"schemaVersion":"1"})")));
  EXPECT_FALSE(terra_api::valid_request_schema(nlohmann::json::parse(R"({"schemaVersion":true})")));
  EXPECT_FALSE(terra_api::valid_request_schema(nlohmann::json::parse(R"({"schemaVersion":-1})")));
  EXPECT_FALSE(terra_api::valid_request_schema(nlohmann::json::parse(R"({"schemaVersion":null})")));
  EXPECT_FALSE(terra_api::valid_request_schema(nlohmann::json::array({1})));
}
