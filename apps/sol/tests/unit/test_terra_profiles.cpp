/**
 * @file tests/unit/test_terra_profiles.cpp
 * @brief Tests for standalone Terra profile storage and validation.
 */

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/terra_profiles.h>

namespace {
  using terra::profiles::actor_t;
  using terra::profiles::callbacks_t;
  using terra::profiles::manager_t;
  using terra::profiles::status_t;
  using nlohmann::json;

  constexpr auto CLIENT = "10000000-0000-4000-8000-000000000001";
  constexpr auto PROFILE = "20000000-0000-4000-8000-000000000002";
  constexpr auto APP = "30000000-0000-4000-8000-000000000003";
  constexpr auto DISPLAY_PROFILE = "40000000-0000-4000-8000-000000000004";

  json stream() {
    return {{"width", 1920}, {"height", 1080}, {"fps", 60}, {"bitrateKbps", 20000}, {"codec", "automatic"}, {"hdr", false}, {"yuv444", false}, {"audioChannels", "stereo"}, {"hostAudio", false}, {"inputMode", "relative"}, {"requiredInputClasses", {"keyboard", "mouse"}}, {"controllerLimit", 4}, {"encryptionRequired", true}, {"gameOptimizations", true}};
  }

  json display() {
    return {{"targetDisplayId", nullptr}, {"topology", {{"unsafe", true}}}, {"modeId", nullptr}, {"scale", 1.0}, {"rotation", 0}, {"hdr", nullptr}, {"primaryPolicy", "preserve"}, {"restorePolicy", "always"}, {"virtualDisplays", {{{"unsafePath", "outside-root"}}}}};
  }

  json launch() {
    return {{"appUuid", APP}, {"displayProfileId", nullptr}, {"streamProfileId", nullptr}, {"sandboxProfileId", nullptr}, {"arguments", json::array()}, {"environment", json::object()}, {"workingDirectory", "outside-root"}, {"elevated", false}, {"preLaunchPolicy", {{{"command", "outside-root/tool"}, {"undoCommand", nullptr}, {"elevated", false}, {"timeoutMs", 0}, {"failurePolicy", "abort"}}}}, {"postExitPolicy", json::array()}, {"cleanupPolicy", "on-stop"}, {"resumePolicy", "deny"}, {"concurrentLaunchPolicy", "deny"}};
  }

  actor_t admin() {
    return {CLIENT, {"catalog.read", "display.read", "display.manage", "sandbox.manage", "host.control"}, {}};
  }

  callbacks_t callbacks(std::string *saved = nullptr) {
    callbacks_t cb;
    cb.uuid = [] {
      return PROFILE;
    };
    cb.save = [saved](const std::string &value) {
      if (saved) {
        *saved = value;
      }
      return true;
    };
    cb.validate_reference = [](const auto &, const auto &) {
      return status_t::success;
    };
    cb.provider_supports = [](const auto &, const auto &) {
      return true;
    };
    return cb;
  }

  TEST(TerraProfiles, CreateGetListPatchDeleteAndRevisions) {
    std::string saved;
    manager_t manager(callbacks(&saved));
    auto created = manager.create(admin(), {{"type", "stream"}, {"name", "Desk"}, {"shared", true}, {"configuration", stream()}});
    ASSERT_EQ(created.status, status_t::success);
    EXPECT_EQ(created.profile->revision, 1);
    EXPECT_EQ(manager.get(admin(), PROFILE).profile->name, "Desk");
    EXPECT_EQ(manager.type(PROFILE), "stream");
    EXPECT_FALSE(manager.type(APP));
    EXPECT_EQ(manager.inspect(PROFILE)->name, "Desk");
    EXPECT_EQ(manager.validate_configuration("stream", stream()), status_t::success);
    EXPECT_EQ(manager.list(admin(), "stream").profiles.size(), 1);
    auto patched = manager.patch(admin(), PROFILE, 1, {{"name", "TV"}});
    ASSERT_EQ(patched.status, status_t::success);
    EXPECT_EQ(patched.profile->revision, 2);
    EXPECT_EQ(manager.patch(admin(), PROFILE, 1, {{"name", "old"}}).status, status_t::conflict);
    EXPECT_EQ(manager.erase(admin(), PROFILE, 2).status, status_t::success);
    EXPECT_EQ(manager.get(admin(), PROFILE).status, status_t::not_found);
    EXPECT_FALSE(saved.empty());
  }

  TEST(TerraProfiles, RejectsUnknownInvalidAndUnsupportedConfiguration) {
    auto cb = callbacks();
    cb.provider_supports = [](const auto &, const auto &) {
      return false;
    };
    manager_t manager(std::move(cb));
    auto config = stream();
    config["surprise"] = true;
    EXPECT_EQ(manager.create(admin(), {{"type", "stream"}, {"name", "x"}, {"shared", true}, {"configuration", config}}).status, status_t::invalid);
    config.erase("surprise");
    EXPECT_EQ(manager.create(admin(), {{"type", "stream"}, {"name", "x"}, {"shared", true}, {"configuration", config}}).status, status_t::unsupported);
    EXPECT_EQ(manager.create(admin(), {{"type", "stream"}, {"name", "x"}, {"shared", true}}).status, status_t::invalid);
  }

  TEST(TerraProfiles, RequiresDomainScopeEvenForHostAdministrator) {
    manager_t manager(callbacks());
    auto actor = admin();
    actor.scopes.erase("display.manage");
    EXPECT_EQ(manager.create(actor, {{"type", "display"}, {"name", "x"}, {"shared", false}, {"configuration", display()}}).status, status_t::forbidden);
    actor.scopes.erase("sandbox.manage");
    EXPECT_EQ(manager.create(actor, {{"type", "sandbox"}, {"name", "x"}, {"shared", false}, {"configuration", json::object()}}).status, status_t::forbidden);
  }

  TEST(TerraProfiles, DisplayModeIdIsOpaqueNonEmptyText) {
    manager_t manager(callbacks());
    auto config = display();
    config["modeId"] = "1920x1080@60000/1000";
    EXPECT_EQ(manager.create(admin(), {{"type", "display"}, {"name", "x"}, {"shared", true}, {"configuration", config}}).status, status_t::success);
  }

  TEST(TerraProfiles, LaunchReferencesRequireMatchingVisibleProfileType) {
    auto cb = callbacks();
    unsigned generated = 0;
    cb.uuid = [&generated] {
      return generated++ == 0 ? std::string {DISPLAY_PROFILE} : std::string {PROFILE};
    };
    manager_t manager(std::move(cb));
    ASSERT_EQ(manager.create(admin(), {{"type", "display"}, {"name", "display"}, {"shared", true}, {"configuration", display()}}).status, status_t::success);
    auto config = launch();
    config["displayProfileId"] = DISPLAY_PROFILE;
    EXPECT_EQ(manager.create(admin(), {{"type", "launch"}, {"name", "launch"}, {"shared", true}, {"configuration", config}}).status, status_t::success);
    config["streamProfileId"] = DISPLAY_PROFILE;
    EXPECT_EQ(manager.create(admin(), {{"type", "launch"}, {"name", "bad"}, {"shared", true}, {"configuration", config}}).status, status_t::not_found);
  }

  TEST(TerraProfiles, FailedWriteRollsBackAndBusyBlocksDelete) {
    auto cb = callbacks();
    cb.save = [](const auto &) {
      return false;
    };
    manager_t manager(std::move(cb));
    EXPECT_EQ(manager.create(admin(), {{"type", "stream"}, {"name", "x"}, {"shared", true}, {"configuration", stream()}}).status, status_t::persistence);
    EXPECT_EQ(manager.get(admin(), PROFILE).status, status_t::not_found);
  }

  TEST(TerraProfiles, RecoversValidRecordsAndIsolatesMalformedOptionalRecord) {
    auto cb = callbacks();
    cb.load = [] {
      return std::optional<std::string>(json({{"version", 1}, {"revision", 7}, {"profiles", {json({{"bad", true}}), json({{"id", PROFILE}, {"type", "stream"}, {"name", "ok"}, {"ownerClientUuid", CLIENT}, {"shared", true}, {"revision", 3}, {"configuration", stream()}})}}}).dump());
    };
    manager_t manager(std::move(cb));
    EXPECT_EQ(manager.availability(), status_t::success);
    EXPECT_EQ(manager.list(admin()).profiles.size(), 1);
    EXPECT_EQ(manager.list(admin()).collection_revision, 7);
  }

  TEST(TerraProfiles, PersistedRecordsRequireExactTypedFieldsAndFirstDuplicateWins) {
    auto valid = json({{"id", PROFILE}, {"type", "stream"}, {"name", "first"}, {"ownerClientUuid", CLIENT}, {"shared", true}, {"revision", 3}, {"configuration", stream()}});
    auto duplicate = valid;
    duplicate["name"] = "second";
    auto extra = valid;
    extra["id"] = "40000000-0000-4000-8000-000000000004";
    extra["extra"] = true;
    auto empty_name = valid;
    empty_name["id"] = "50000000-0000-4000-8000-000000000005";
    empty_name["name"] = "";
    auto wrong_shared = valid;
    wrong_shared["id"] = "60000000-0000-4000-8000-000000000006";
    wrong_shared["shared"] = 1;
    auto zero_revision = valid;
    zero_revision["id"] = "70000000-0000-4000-8000-000000000007";
    zero_revision["revision"] = 0;
    auto cb = callbacks();
    cb.load = [records = json::array({valid, duplicate, extra, empty_name, wrong_shared, zero_revision})] {
      return std::optional<std::string>(json({{"version", 1}, {"revision", 9}, {"profiles", records}}).dump());
    };
    manager_t manager(std::move(cb));
    const auto listed = manager.list(admin());
    ASSERT_EQ(listed.profiles.size(), 1);
    EXPECT_EQ(listed.profiles.front().name, "first");
  }

  TEST(TerraProfiles, CanonicalSandboxNormalizerRejectsUnlistedEnvironmentValue) {
    manager_t manager(callbacks());
    json configuration = {{"environment", {{"allowedNames", json::array()}, {"values", {{"TOKEN", "plaintext"}}}}}};
    EXPECT_EQ(manager.create(admin(), {{"type", "sandbox"}, {"name", "safe"}, {"shared", true}, {"configuration", configuration}}).status, status_t::invalid);
  }

  TEST(TerraProfiles, ProviderCanRejectDisplayTopologyAndVirtualDisplays) {
    auto cb = callbacks();
    cb.provider_supports = [](const std::string &type, const json &) {
      return type != "display";
    };
    manager_t manager(std::move(cb));
    EXPECT_EQ(manager.create(admin(), {{"type", "display"}, {"name", "unsafe"}, {"shared", true}, {"configuration", display()}}).status, status_t::unsupported);
    EXPECT_TRUE(manager.list(admin(), "display").profiles.empty());
  }

  TEST(TerraProfiles, ProviderCanRejectLaunchPathsAndActions) {
    auto cb = callbacks();
    cb.provider_supports = [](const std::string &type, const json &) {
      return type != "launch";
    };
    manager_t manager(std::move(cb));
    EXPECT_EQ(manager.create(admin(), {{"type", "launch"}, {"name", "unsafe"}, {"shared", true}, {"configuration", launch()}}).status, status_t::unsupported);
    EXPECT_TRUE(manager.list(admin(), "launch").profiles.empty());
  }

  TEST(TerraProfiles, SandboxDefaultsAreReturnedAndSecretsRemainOpaque) {
    manager_t manager(callbacks());
    json configuration = {{"environment", {{"allowedNames", {"TOKEN"}}, {"values", {{"TOKEN", {{"secretRef", "vault:item"}}}}}}}};
    auto result = manager.create(admin(), {{"type", "sandbox"}, {"name", "safe"}, {"shared", true}, {"configuration", configuration}});
    ASSERT_EQ(result.status, status_t::success);
    EXPECT_EQ(result.profile->configuration["network"]["mode"], "none");
    EXPECT_EQ(result.profile->configuration["environment"]["values"]["TOKEN"]["secretRef"], "vault:item");
  }

  TEST(TerraProfiles, SandboxAllowedApplicationsMustResolve) {
    auto cb = callbacks();
    cb.validate_reference = [](const std::string &kind, const std::string &) {
      return kind == "application" ? status_t::not_found : status_t::success;
    };
    manager_t manager(std::move(cb));
    const auto result = manager.create(admin(), {{"type", "sandbox"}, {"name", "safe"}, {"shared", true}, {"configuration", {{"allowedAppUuids", {APP}}}}});
    EXPECT_EQ(result.status, status_t::not_found);
  }

  TEST(TerraProfiles, VisibilityAndAdoptionEnforceOwnership) {
    auto cb = callbacks();
    cb.load = [] {
      return std::optional<std::string>(json({{"version", 1}, {"revision", 1}, {"profiles", {json({{"id", PROFILE}, {"type", "stream"}, {"name", "orphan"}, {"ownerClientUuid", nullptr}, {"shared", true}, {"revision", 1}, {"configuration", stream()}})}}}).dump());
    };
    manager_t manager(std::move(cb));
    EXPECT_EQ(manager.get(admin(), PROFILE).status, status_t::not_found);
    EXPECT_EQ(manager.adopt(admin(), PROFILE, 1).status, status_t::success);
    EXPECT_EQ(manager.get(admin(), PROFILE).status, status_t::success);
    EXPECT_EQ(manager.adopt(admin(), PROFILE, 2).status, status_t::resource_busy);
  }

  TEST(TerraProfiles, MutationsMaskProfilesOwnedByAnotherClient) {
    manager_t manager(callbacks());
    ASSERT_EQ(manager.create(admin(), {{"type", "stream"}, {"name", "shared"}, {"shared", true}, {"configuration", stream()}}).status, status_t::success);
    auto other = admin();
    other.client_uuid = "80000000-0000-4000-8000-000000000008";
    other.scopes.erase("host.control");
    EXPECT_EQ(manager.get(other, PROFILE).status, status_t::success);
    EXPECT_EQ(manager.patch(other, PROFILE, 1, {{"name", "stolen"}}).status, status_t::not_found);
    EXPECT_EQ(manager.erase(other, PROFILE, 1).status, status_t::not_found);
  }

  TEST(TerraProfiles, RevocationOrphansOwnedProfilesAtomically) {
    manager_t manager(callbacks());
    ASSERT_EQ(manager.create(admin(), {{"type", "stream"}, {"name", "shared"}, {"shared", true}, {"configuration", stream()}}).status, status_t::success);
    const auto revoked = manager.revoke_owner(CLIENT);
    ASSERT_EQ(revoked.status, status_t::success);
    ASSERT_EQ(revoked.profiles.size(), 1);
    EXPECT_FALSE(revoked.profiles.front().owner_client_uuid);
    EXPECT_EQ(revoked.profiles.front().revision, 2);
    EXPECT_EQ(manager.get(admin(), PROFILE).status, status_t::not_found);
  }

  TEST(TerraProfiles, PolicyReplacementOrphansOnlyUnauthorizedProfiles) {
    auto cb = callbacks();
    unsigned generated = 0;
    cb.uuid = [&generated] {
      return generated++ == 0 ? std::string {DISPLAY_PROFILE} : std::string {PROFILE};
    };
    manager_t manager(std::move(cb));
    ASSERT_EQ(manager.create(admin(), {{"type", "display"}, {"name", "display"}, {"shared", false}, {"configuration", display()}}).status, status_t::success);
    ASSERT_EQ(manager.create(admin(), {{"type", "stream"}, {"name", "stream"}, {"shared", false}, {"configuration", stream()}}).status, status_t::success);
    auto current = admin();
    current.scopes.erase("display.manage");
    const auto revoked = manager.revoke_unauthorized(current);
    ASSERT_EQ(revoked.status, status_t::success);
    ASSERT_EQ(revoked.profiles.size(), 1);
    EXPECT_EQ(revoked.profiles.front().id, DISPLAY_PROFILE);
    EXPECT_TRUE(manager.inspect(PROFILE)->owner_client_uuid);
  }

  TEST(TerraProfiles, LaunchProfileReferencesScanEveryStoredProfile) {
    auto cb = callbacks();
    cb.uuid = [counter = 0]() mutable {
      return std::format("20000000-0000-4000-8000-{:012}", ++counter);
    };
    manager_t manager(cb);

    const json stream_configuration = stream();
    ASSERT_EQ(manager.create(admin(), {{"type", "stream"}, {"name", "Stream"}, {"shared", true}, {"configuration", stream_configuration}}).status, status_t::success);
    const auto stream_id = manager.list(admin(), "stream").profiles.front().id;

    auto launch_profile = launch();
    launch_profile["streamProfileId"] = stream_id;
    ASSERT_EQ(manager.create(admin(), {{"type", "launch"}, {"name", "Launch"}, {"shared", true}, {"configuration", launch_profile}}).status, status_t::success);

    EXPECT_TRUE(manager.launch_profile_references(stream_id));
    EXPECT_FALSE(manager.launch_profile_references(CLIENT));

    launch_profile["streamProfileId"] = nullptr;
    ASSERT_EQ(manager.patch(admin(), manager.list(admin(), "launch").profiles.front().id, 1, {{"configuration", launch_profile}}).status, status_t::success);
    EXPECT_FALSE(manager.launch_profile_references(stream_id));
  }
}  // namespace
