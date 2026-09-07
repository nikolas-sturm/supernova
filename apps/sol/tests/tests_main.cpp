/**
 * @file tests/tests_main.cpp
 * @brief Entry point definition.
 */

// test includes
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new SolEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new SolEventListener);
  return RUN_ALL_TESTS();
}
