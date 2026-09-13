/**
 * @file tests/unit/test_input.cpp
 * @brief Tests for retained stream input and virtual gamepad lifecycle behavior.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// moonlight-common-c includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

// local includes
#include "src/config.h"
#include "src/input.h"
#include "src/platform/virtualhid_input.h"
#include "src/utility.h"

namespace {
  /**
   * @brief Protocol metadata for a fixed-size input packet.
   */
  struct input_packet_spec_t {
    std::uint32_t magic;  ///< Packet type identifier.
    std::size_t size;  ///< Complete fixed packet size.
  };

  /**
   * @brief Create raw input bytes with a caller-selected header and buffer size.
   *
   * @param magic Packet type identifier.
   * @param declared_size Packet size declared after the size field.
   * @param actual_size Number of bytes available in the packet buffer.
   * @return Raw packet bytes.
   */
  std::vector<std::uint8_t> make_input_packet(std::uint32_t magic, std::uint32_t declared_size, std::size_t actual_size) {
    std::vector<std::uint8_t> packet(actual_size);
    const NV_INPUT_HEADER header {
      util::endian::big(declared_size),
      util::endian::little(magic),
    };
    if (!packet.empty()) {
      std::memcpy(packet.data(), &header, std::min(packet.size(), sizeof(header)));
    }
    return packet;
  }

  /**
   * @brief Fixture that installs a fake global virtual input backend.
   */
  class InputGamepadSessionTest: public ::testing::Test {
  protected:
    /**
     * @brief Preserve configuration and install observable fake devices.
     */
    void SetUp() override {
      original_input_ = config::input;
      config::input.controller = true;
      config::input.gamepad = "xseries";

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      context_ = &context;
      runtime_ = context.runtime.get();
      ASSERT_NE(runtime_, nullptr);
      input::testing::set_platform_input(std::move(platform_input));
    }

    /**
     * @brief Destroy retained test sessions and restore configuration.
     */
    void TearDown() override {
      input::terminate_gamepads();
      input::testing::set_platform_input({});
      context_ = nullptr;
      runtime_ = nullptr;
      config::input = std::move(original_input_);
    }

    /**
     * @brief Access the fake runtime installed for the current test.
     *
     * @return Fake libvirtualhid runtime.
     */
    lvh::Runtime &runtime() const {
      return *runtime_;
    }

    /**
     * @brief Access the shared libvirtualhid input context installed for the test.
     *
     * @return Fake input context.
     */
    platf::virtualhid::input_context_t &context() const {
      return *context_;
    }

  private:
    platf::virtualhid::input_context_t *context_ = nullptr;  ///< Fake input context installed in the global backend.
    lvh::Runtime *runtime_ = nullptr;  ///< Fake runtime installed in the global input backend.
    config::input_t original_input_;  ///< Input configuration restored after each test.
  };
}  // namespace

TEST(InputPacketValidationTest, RejectsEveryBufferShorterThanTheHeader) {
  for (std::size_t actual_size = 0; actual_size < sizeof(NV_INPUT_HEADER); ++actual_size) {
    const auto packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), actual_size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(packet)) << "actual_size=" << actual_size;
  }
}

TEST(InputPacketValidationTest, RejectsDeclaredSizesOutsideTheAvailableBuffer) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  for (std::uint32_t declared_size = 0; declared_size < sizeof(std::uint32_t); ++declared_size) {
    const auto packet = make_input_packet(unknown_magic, declared_size, sizeof(NV_INPUT_HEADER));
    EXPECT_FALSE(input::testing::is_valid_input_packet(packet)) << "declared_size=" << declared_size;
  }

  const auto truncated_packet = make_input_packet(unknown_magic, sizeof(std::uint32_t) + 1, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_packet));

  const auto overflowing_size = make_input_packet(
    unknown_magic,
    std::numeric_limits<std::uint32_t>::max(),
    sizeof(NV_INPUT_HEADER)
  );
  EXPECT_FALSE(input::testing::is_valid_input_packet(overflowing_size));
}

TEST(InputPacketValidationTest, ValidatesUnicodePacketBoundsWithoutOverflow) {
  const auto empty_packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::testing::is_valid_input_packet(empty_packet));

  constexpr std::uint32_t maximum_declared_size = sizeof(std::uint32_t) + UTF8_TEXT_EVENT_MAX_COUNT;
  const auto maximum_packet = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size,
    sizeof(std::uint32_t) + maximum_declared_size
  );
  EXPECT_TRUE(input::testing::is_valid_input_packet(maximum_packet));

  const auto oversized_text = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size + 1,
    sizeof(std::uint32_t) + maximum_declared_size + 1
  );
  EXPECT_FALSE(input::testing::is_valid_input_packet(oversized_text));

  const auto truncated_text = make_input_packet(UTF8_TEXT_EVENT_MAGIC, maximum_declared_size, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_text));
}

TEST(InputPacketValidationTest, EnforcesEveryFixedPacketSize) {
  constexpr std::array packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {KEY_UP_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_ARRIVAL_MAGIC, sizeof(SS_CONTROLLER_ARRIVAL_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_BATTERY_MAGIC, sizeof(SS_CONTROLLER_BATTERY_PACKET)},
  };

  for (const auto &[magic, size] : packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));
    const auto valid_packet = make_input_packet(magic, declared_size, size);
    EXPECT_TRUE(input::testing::is_valid_input_packet(valid_packet)) << "magic=" << magic;

    const auto trailing_bytes = make_input_packet(magic, declared_size, size + 8);
    EXPECT_TRUE(input::testing::is_valid_input_packet(trailing_bytes)) << "magic=" << magic;

    const auto wrong_declared_size = make_input_packet(magic, declared_size - 1, size);
    EXPECT_FALSE(input::testing::is_valid_input_packet(wrong_declared_size)) << "magic=" << magic;

    const auto oversized_declared_size = make_input_packet(magic, declared_size + 1, size + 1);
    EXPECT_FALSE(input::testing::is_valid_input_packet(oversized_declared_size)) << "magic=" << magic;

    const auto truncated_packet = make_input_packet(magic, declared_size, size - 1);
    EXPECT_FALSE(input::testing::is_valid_input_packet(truncated_packet)) << "magic=" << magic;
  }
}

TEST(InputPacketValidationTest, PreservesUnknownPacketHandlingWithinDeclaredBounds) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  const auto packet = make_input_packet(unknown_magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::testing::is_valid_input_packet(packet));
}

TEST_F(InputGamepadSessionTest, RejectsMalformedBatchablePacketsAtQueueIngress) {
  ASSERT_FALSE(task_pool.running());
  const std::shared_ptr<input::input_t> empty_input;
  EXPECT_EQ(input::testing::queued_input_packet_count(empty_input), 0);

  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "packet-validation-client");
  ASSERT_NE(stream_input, nullptr);

  auto short_header = make_input_packet(SS_PEN_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER) - 1);
  input::passthrough(stream_input, std::move(short_header));
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);

  constexpr std::array batchable_packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET)},
  };

  std::size_t valid_packet_count = 0;
  for (const auto &[magic, size] : batchable_packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));

    auto truncated_packet = make_input_packet(magic, declared_size, size - 1);
    input::passthrough(stream_input, std::move(truncated_packet));
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;

    auto valid_packet = make_input_packet(magic, declared_size, size);
    input::passthrough(stream_input, std::move(valid_packet));
    ++valid_packet_count;
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;

    auto oversized_declared_size = make_input_packet(magic, declared_size + 1, size + 1);
    input::passthrough(stream_input, std::move(oversized_declared_size));
    EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), valid_packet_count) << "magic=" << magic;
  }
}

TEST_F(InputGamepadSessionTest, EnforcesCertificateBoundInputClassesAtQueueIngress) {
  ASSERT_FALSE(task_pool.running());
  terra_api::input_permissions_t permissions;
  permissions.keyboard = false;
  permissions.mouse = false;
  permissions.controller = false;
  permissions.pen = false;
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "restricted-input-client", permissions);
  ASSERT_NE(stream_input, nullptr);

  auto keyboard = make_input_packet(KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET) - sizeof(std::uint32_t), sizeof(NV_KEYBOARD_PACKET));
  input::passthrough(stream_input, std::move(keyboard));
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);

  auto touch = make_input_packet(SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET) - sizeof(std::uint32_t), sizeof(SS_TOUCH_PACKET));
  input::passthrough(stream_input, std::move(touch));
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 1);

  input::terminate_gamepads("restricted-input-client");
}

TEST_F(InputGamepadSessionTest, IsolatesDisplayStreamsAndRevokesAllClientDisplays) {
  ASSERT_FALSE(task_pool.running());
  const auto first = input::alloc(std::make_shared<safe::mail_raw_t>(), "multi-display-client", {}, input::mouse_mode_e::any, "display-a");
  const auto second = input::alloc(std::make_shared<safe::mail_raw_t>(), "multi-display-client", {}, input::mouse_mode_e::any, "display-b");
  const auto other = input::alloc(std::make_shared<safe::mail_raw_t>(), "other-client", {}, input::mouse_mode_e::any, "display-a");
  EXPECT_NE(first, second);
  EXPECT_NE(first, other);
  EXPECT_EQ(first, input::alloc(std::make_shared<safe::mail_raw_t>(), "multi-display-client", {}, input::mouse_mode_e::any, "display-a"));
  EXPECT_EQ(second, input::alloc(std::make_shared<safe::mail_raw_t>(), "multi-display-client", {}, input::mouse_mode_e::any, "display-b"));

  input::terminate_gamepads("multi-display-client");
  EXPECT_NE(first, input::alloc(std::make_shared<safe::mail_raw_t>(), "multi-display-client", {}, input::mouse_mode_e::any, "display-a"));
  EXPECT_NE(second, input::alloc(std::make_shared<safe::mail_raw_t>(), "multi-display-client", {}, input::mouse_mode_e::any, "display-b"));
  EXPECT_EQ(other, input::alloc(std::make_shared<safe::mail_raw_t>(), "other-client", {}, input::mouse_mode_e::any, "display-a"));
}

TEST_F(InputGamepadSessionTest, ReplacesInputPermissionsWhenSessionResumes) {
  ASSERT_FALSE(task_pool.running());
  terra_api::input_permissions_t restricted_permissions;
  restricted_permissions.keyboard = false;
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "permission-resume-client", restricted_permissions);
  ASSERT_NE(stream_input, nullptr);

  auto denied_keyboard = make_input_packet(KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET) - sizeof(std::uint32_t), sizeof(NV_KEYBOARD_PACKET));
  input::passthrough(stream_input, std::move(denied_keyboard));
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0);

  auto resumed = input::alloc(std::make_shared<safe::mail_raw_t>(), "permission-resume-client");
  ASSERT_EQ(resumed, stream_input);
  auto allowed_keyboard = make_input_packet(KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET) - sizeof(std::uint32_t), sizeof(NV_KEYBOARD_PACKET));
  input::passthrough(resumed, std::move(allowed_keyboard));
  EXPECT_EQ(input::testing::queued_input_packet_count(resumed), 1);

  input::terminate_gamepads("permission-resume-client");
}

TEST_F(InputGamepadSessionTest, ReusesGamepadsAcrossPauseAndDestroysThemOnTermination) {
  const std::string session_id = "paired-client-certificate";
  auto first_mail = std::make_shared<safe::mail_raw_t>();
  auto first = input::alloc(first_mail, session_id);
  ASSERT_NE(first, nullptr);
  const auto active_devices_before_gamepad = runtime().active_device_count();

  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};
  const auto original_id = input::testing::alloc_gamepad(first, 0, metadata);
  ASSERT_GE(original_id, 0);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  const std::weak_ptr<input::input_t> paused = first;
  first.reset();
  EXPECT_FALSE(paused.expired());
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  auto resumed_mail = std::make_shared<safe::mail_raw_t>();
  auto resumed = input::alloc(resumed_mail, session_id);
  ASSERT_NE(resumed, nullptr);
  EXPECT_EQ(paused.lock(), resumed);
  EXPECT_EQ(input::testing::gamepad_id(resumed, 0), original_id);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  input::terminate_gamepads(session_id);
  EXPECT_EQ(input::testing::gamepad_id(resumed, 0), -1);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad);

  auto replacement = input::alloc(std::make_shared<safe::mail_raw_t>(), session_id);
  EXPECT_NE(replacement, resumed);
}

TEST_F(InputGamepadSessionTest, RefreshesSharedVirtualInputAfterLicenseStateChanges) {
  ASSERT_NE(context().keyboard, nullptr);
  ASSERT_NE(context().mouse, nullptr);
  const auto original_keyboard_id = context().keyboard->device_id();
  const auto original_mouse_id = context().mouse->device_id();
  const auto active_devices = runtime().active_device_count();

  input::refresh_virtual_input();

  ASSERT_NE(context().keyboard, nullptr);
  ASSERT_NE(context().mouse, nullptr);
  EXPECT_NE(context().keyboard->device_id(), original_keyboard_id);
  EXPECT_NE(context().mouse->device_id(), original_mouse_id);
  EXPECT_EQ(runtime().active_device_count(), active_devices);
}

TEST_F(InputGamepadSessionTest, MapsSignedWorkspaceMouseRelativeToVirtualEnvironmentWithoutOrdinaryClamping) {
  ASSERT_FALSE(task_pool.running());
  ASSERT_EQ(runtime().backend_kind(), lvh::BackendKind::fake);
  ASSERT_NE(context().mouse, nullptr);
  config::input.mouse = true;
  const auto generation = input::workspace_mouse_generation();
  const std::vector<input::mouse_viewport_t> viewports {
    {-1920, -1080, 1920, 1080, generation},
    {-3840, -2160, 1920, 1080, generation},
  };
  // Desktop origin is (-3840, -2160); the source offset is (960, 540) at 200% scaling.
  const input::touch_port_t port {{960, 540, 960, 540}, 5760, 3240, 0, 0, 2, 2, 2880, 1620};
  auto ordinary_mail = std::make_shared<safe::mail_raw_t>();
  auto ordinary = input::alloc(ordinary_mail, "ordinary-mouse-client");
  ordinary_mail->event<input::touch_port_t>(mail::touch_port)->raise(port);
  auto workspace_mail = std::make_shared<safe::mail_raw_t>();
  auto workspace = input::alloc(workspace_mail, "workspace-mouse-client", {}, input::mouse_mode_e::absolute, "source", viewports);
  workspace_mail->event<input::touch_port_t>(mail::touch_port)->raise(port);
  auto &mouse = *context().mouse;
  const auto before = mouse.submit_count();

  input::testing::send_mouse_position_packet(ordinary, -480, -270, 960, 540);
  ASSERT_EQ(mouse.submit_count(), before + 1);
  auto event = mouse.last_submitted_event();
  EXPECT_EQ(event.kind, lvh::MouseEventKind::absolute_motion);
  EXPECT_EQ(event.x, 960);
  EXPECT_EQ(event.y, 540);
  EXPECT_EQ(event.width, 2880);
  EXPECT_EQ(event.height, 1620);

  input::testing::send_mouse_position_packet(workspace, -480, -270, 960, 540);
  ASSERT_EQ(mouse.submit_count(), before + 2);
  event = mouse.last_submitted_event();
  EXPECT_EQ(event.kind, lvh::MouseEventKind::absolute_motion);
  EXPECT_EQ(event.x, 480);
  EXPECT_EQ(event.y, 270);
  EXPECT_EQ(event.width, 2880);
  EXPECT_EQ(event.height, 1620);

  input::testing::send_mouse_position_packet(ordinary, 1440, 810, 960, 540);
  ASSERT_EQ(mouse.submit_count(), before + 3);
  event = mouse.last_submitted_event();
  EXPECT_EQ(event.x, 1920);
  EXPECT_EQ(event.y, 1080);
}

TEST_F(InputGamepadSessionTest, RejectsWorkspaceMouseWhenCaptureSourcePixelDimensionsChange) {
  ASSERT_FALSE(task_pool.running());
  ASSERT_EQ(runtime().backend_kind(), lvh::BackendKind::fake);
  ASSERT_NE(context().mouse, nullptr);
  config::input.mouse = true;
  const auto generation = input::workspace_mouse_generation();
  auto stream_mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(stream_mail, "mouse-source-size-client", {}, input::mouse_mode_e::absolute, "source", {
    {0, 0, 1920, 1080, generation},
    {1920, 0, 1920, 1080, generation},
  });
  const input::touch_port_t port {{0, 0, 1000, 600}, 3840, 1080, 20, 30, 2, 1, 0, 0};
  auto port_event = stream_mail->event<input::touch_port_t>(mail::touch_port);
  port_event->raise(port);
  auto &mouse = *context().mouse;
  const auto before = mouse.submit_count();

  // Packet resolution may differ; the letterbox-adjusted physical source must still match.
  input::testing::send_mouse_position_packet(stream_input, 480, 270, 960, 540);
  ASSERT_EQ(mouse.submit_count(), before + 1);
  EXPECT_EQ(mouse.last_submitted_event().x, 960);
  EXPECT_EQ(mouse.last_submitted_event().y, 540);

  for (const bool change_width : {true, false}) {
    auto changed_port = port;
    if (change_width) {
      changed_port.width += 2;
    } else {
      changed_port.height += 2;
    }
    port_event->raise(changed_port);
    input::testing::send_mouse_position_packet(stream_input, 480, 270, 960, 540);
    EXPECT_EQ(mouse.submit_count(), before + 1) << "change_width=" << change_width;
  }

  port_event->raise(port);
  input::testing::send_mouse_position_packet(stream_input, 480, 270, 960, 540);
  EXPECT_EQ(mouse.submit_count(), before + 2);
}

TEST_F(InputGamepadSessionTest, RejectsWorkspaceMouseInViewportGap) {
  ASSERT_FALSE(task_pool.running());
  ASSERT_EQ(runtime().backend_kind(), lvh::BackendKind::fake);
  ASSERT_NE(context().mouse, nullptr);
  config::input.mouse = true;
  const auto generation = input::workspace_mouse_generation();
  auto stream_mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(stream_mail, "mouse-gap-client", {}, input::mouse_mode_e::absolute, "source", {
    {0, 0, 1920, 1080, generation},
    {2400, 0, 1920, 1080, generation},
  });
  stream_mail->event<input::touch_port_t>(mail::touch_port)->raise(input::touch_port_t {
    {0, 0, 1920, 1080}, 4320, 1080, 0, 0, 1, 1, 0, 0,
  });
  auto &mouse = *context().mouse;
  const auto before = mouse.submit_count();

  input::testing::send_mouse_position_packet(stream_input, 2400, 540, 1920, 1080);
  ASSERT_EQ(mouse.submit_count(), before + 1);
  EXPECT_EQ(mouse.last_submitted_event().x, 2400);
  EXPECT_EQ(mouse.last_submitted_event().width, 4320);
  for (const std::int16_t x : {1920, 2160, 2399}) {
    input::testing::send_mouse_position_packet(stream_input, x, 540, 1920, 1080);
    EXPECT_EQ(mouse.submit_count(), before + 1) << "x=" << x;
  }
  input::testing::send_mouse_position_packet(stream_input, 1919, 540, 1920, 1080);
  ASSERT_EQ(mouse.submit_count(), before + 2);
  EXPECT_EQ(mouse.last_submitted_event().x, 1919);
}

TEST_F(InputGamepadSessionTest, WorkspaceGenerationInvalidationRejectsMovesAndPressesAndCancelsHeldButton) {
  ASSERT_FALSE(task_pool.running());
  ASSERT_EQ(runtime().backend_kind(), lvh::BackendKind::fake);
  ASSERT_NE(context().mouse, nullptr);
  config::input.mouse = true;
  const auto generation = input::workspace_mouse_generation();
  auto stream_mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(stream_mail, "mouse-generation-client", {}, input::mouse_mode_e::absolute, "source", {
    {0, 0, 1920, 1080, generation},
    {1920, 0, 1920, 1080, generation},
  });
  stream_mail->event<input::touch_port_t>(mail::touch_port)->raise(input::touch_port_t {
    {0, 0, 1920, 1080}, 3840, 1080, 0, 0, 1, 1, 0, 0,
  });
  auto &mouse = *context().mouse;
  const auto before = mouse.submit_count();
  input::testing::send_mouse_position_packet(stream_input, 2400, 540, 1920, 1080);
  ASSERT_EQ(mouse.submit_count(), before + 1);
  input::peripheral_forward_mouse_button(stream_input, 2, false);
  const auto pressed = mouse.last_submitted_event();

  // Invalidation is synchronous with the task pool stopped and also cleans up the held button.
  input::invalidate_workspace_mouse();
  const auto after_invalidation = mouse.submit_count();
  const auto released = mouse.last_submitted_event();
  // Clean up even if cancellation regresses, before any fatal assertion can end the test.
  input::peripheral_forward_mouse_button(stream_input, 2, true);
  EXPECT_EQ(input::workspace_mouse_generation(), generation + 1);
  ASSERT_EQ(after_invalidation, before + 3);
  ASSERT_EQ(mouse.submit_count(), before + 3);
  EXPECT_EQ(pressed.kind, lvh::MouseEventKind::button);
  EXPECT_EQ(pressed.button, lvh::MouseButton::middle);
  EXPECT_TRUE(pressed.pressed);
  EXPECT_EQ(released.kind, lvh::MouseEventKind::button);
  EXPECT_EQ(released.button, lvh::MouseButton::middle);
  EXPECT_FALSE(released.pressed);

  input::testing::send_mouse_position_packet(stream_input, 2400, 540, 1920, 1080);
  EXPECT_EQ(mouse.submit_count(), before + 3);
  input::peripheral_forward_mouse_button(stream_input, 2, false);
  const auto after_denied_press = mouse.submit_count();
  input::peripheral_forward_mouse_button(stream_input, 2, true);
  EXPECT_EQ(after_denied_press, before + 3);
  EXPECT_EQ(mouse.submit_count(), before + 3);
}

TEST_F(InputGamepadSessionTest, ResumedMouseClearsObsoleteWorkspaceAndRequiresFreshTouchPort) {
  ASSERT_FALSE(task_pool.running());
  ASSERT_EQ(runtime().backend_kind(), lvh::BackendKind::fake);
  ASSERT_NE(context().mouse, nullptr);
  config::input.mouse = true;
  const auto generation = input::workspace_mouse_generation();
  auto old_mail = std::make_shared<safe::mail_raw_t>();
  auto stream_input = input::alloc(old_mail, "mouse-resume-client", {}, input::mouse_mode_e::absolute, "source", {
    {0, 0, 1920, 1080, generation},
    {-1920, 0, 1920, 1080, generation},
  });
  const input::touch_port_t old_port {{1920, 0, 1920, 1080}, 3840, 1080, 0, 0, 1, 1, 0, 0};
  auto old_port_event = old_mail->event<input::touch_port_t>(mail::touch_port);
  old_port_event->raise(old_port);
  auto &mouse = *context().mouse;
  const auto before = mouse.submit_count();
  input::testing::send_mouse_position_packet(stream_input, -480, 270, 960, 540);
  ASSERT_EQ(mouse.submit_count(), before + 1);
  EXPECT_EQ(mouse.last_submitted_event().x, 960);
  input::invalidate_workspace_mouse();

  auto resumed_mail = std::make_shared<safe::mail_raw_t>();
  auto resumed = input::alloc(resumed_mail, "mouse-resume-client", {}, input::mouse_mode_e::absolute, "source");
  ASSERT_EQ(resumed, stream_input);
  input::testing::send_mouse_position_packet(resumed, -480, 270, 960, 540);
  EXPECT_EQ(mouse.submit_count(), before + 1);
  old_port_event->raise(old_port);
  input::testing::send_mouse_position_packet(resumed, -480, 270, 960, 540);
  EXPECT_EQ(mouse.submit_count(), before + 1);

  resumed_mail->event<input::touch_port_t>(mail::touch_port)->raise(input::touch_port_t {
    {100, 50, 1280, 720}, 2560, 1440, 0, 0, 1, 1, 0, 0,
  });
  input::testing::send_mouse_position_packet(resumed, -480, 270, 960, 540);
  ASSERT_EQ(mouse.submit_count(), before + 2);
  const auto event = mouse.last_submitted_event();
  EXPECT_EQ(event.kind, lvh::MouseEventKind::absolute_motion);
  EXPECT_EQ(event.x, 100);
  EXPECT_EQ(event.y, 410);
  EXPECT_EQ(event.width, 2560);
  EXPECT_EQ(event.height, 1440);
}
