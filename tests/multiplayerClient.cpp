// Unit tests for the local window-sharing state used by the Multiplayer panel.

#include <gtest/gtest.h>

#include "MultiPlayer/Client.h"

using namespace MultiPlayer;

TEST(WindowShareSettingsTest, DefaultsToPrivate)
{
  Client client(nullptr, false);
  const auto settings = client.windowShare(entt::entity{ 1 });
  EXPECT_FALSE(settings.shared);
  EXPECT_FALSE(settings.allowControl);
  EXPECT_TRUE(client.sharedWindows().empty());
}

TEST(WindowShareSettingsTest, SharingResetsControlPermission)
{
  Client client(nullptr, false);
  const entt::entity entity{ 7 };

  client.setWindowShared(entity, true);
  auto settings = client.windowShare(entity);
  EXPECT_TRUE(settings.shared);
  EXPECT_FALSE(settings.allowControl);

  client.setWindowAllowControl(entity, true);
  settings = client.windowShare(entity);
  EXPECT_TRUE(settings.shared);
  EXPECT_TRUE(settings.allowControl);

  // Re-sharing after unsharing must not silently restore control permission.
  client.setWindowShared(entity, false);
  client.setWindowShared(entity, true);
  settings = client.windowShare(entity);
  EXPECT_TRUE(settings.shared);
  EXPECT_FALSE(settings.allowControl);
}

TEST(WindowShareSettingsTest, ControlRequiresSharing)
{
  Client client(nullptr, false);
  const entt::entity entity{ 3 };

  client.setWindowAllowControl(entity, true);
  auto settings = client.windowShare(entity);
  EXPECT_FALSE(settings.shared);
  EXPECT_FALSE(settings.allowControl);
}

TEST(WindowShareSettingsTest, SharedWindowsListsOnlyPublishedWindows)
{
  Client client(nullptr, false);
  const entt::entity first{ 11 };
  const entt::entity second{ 12 };

  client.setWindowShared(first, true);
  client.setWindowShared(second, true);
  client.setWindowShared(second, false);

  const auto shared = client.sharedWindows();
  ASSERT_EQ(shared.size(), 1u);
  EXPECT_EQ(shared.front(), first);
}

TEST(WindowShareSettingsTest, ClearRemovesEverything)
{
  Client client(nullptr, false);
  client.setWindowShared(entt::entity{ 21 }, true);
  client.clearWindowShares();
  EXPECT_TRUE(client.sharedWindows().empty());
}

TEST(WindowStreamSettingsTest, Defaults)
{
  Client client(nullptr, false);
  EXPECT_EQ(client.windowStreamFps(), 5);
  EXPECT_EQ(client.windowStreamQuality(), 65);
}

TEST(WindowStreamSettingsTest, ClampsFps)
{
  Client client(nullptr, false);
  client.setWindowStreamFps(0);
  EXPECT_EQ(client.windowStreamFps(), 1);
  client.setWindowStreamFps(999);
  EXPECT_EQ(client.windowStreamFps(), 30);
  client.setWindowStreamFps(12);
  EXPECT_EQ(client.windowStreamFps(), 12);
}

TEST(WindowStreamSettingsTest, ClampsQuality)
{
  Client client(nullptr, false);
  client.setWindowStreamQuality(0);
  EXPECT_EQ(client.windowStreamQuality(), 1);
  client.setWindowStreamQuality(999);
  EXPECT_EQ(client.windowStreamQuality(), 100);
  client.setWindowStreamQuality(80);
  EXPECT_EQ(client.windowStreamQuality(), 80);
}
