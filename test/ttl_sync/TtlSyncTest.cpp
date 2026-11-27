#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "ttl/TtlSyncCore.h"

namespace {

std::string readFile(const std::string& path) {
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string repoFile(const std::string& relative) {
  return readFile(std::string(TTL_REPO_ROOT) + "/" + relative);
}

}  // namespace

TEST(TtlVersion, LabelIsTheRecoveryBaseline) {
  EXPECT_STREQ(ttl::kVersionNumber, "0.1.0");
  EXPECT_EQ(std::string(ttl::kVersionLabel), std::string("TTL ") + ttl::kVersionNumber);
  EXPECT_FALSE(ttl::kFirmwareOtaEnabled);
  EXPECT_STREQ(ttl::kDefaultServerUrl, "https://teenytinylibrary.app");
  EXPECT_STREQ(ttl::kBookDirectory, "/TTL");
  EXPECT_STREQ(ttl::kSyncLogPath, "/TTL/ttl-sync-log.txt");
}

TEST(TtlVersion, ReleaseSourcesKeepTheBaseline) {
  const std::string ini = repoFile("platformio.ini");
  EXPECT_NE(ini.find("version = 0.1.0"), std::string::npos);
  EXPECT_NE(ini.find("-DTTL_VERSION_NUMBER=\\\"0.1.0\\\""), std::string::npos);
  EXPECT_NE(ini.find("-DTTL_DISABLE_FIRMWARE_OTA=1"), std::string::npos);
  EXPECT_EQ(ini.find("-DCROSSPOINT_VERSION=\\\"${crosspoint.version}\\\""), std::string::npos);
  EXPECT_NE(ini.find("-DCROSSPOINT_VERSION=\\\"TTL ${crosspoint.version}\\\""), std::string::npos);

  const std::string branch = repoFile("scripts/git_branch.py");
  EXPECT_NE(branch.find("version_string = f'TTL {base_version}'"), std::string::npos);
  EXPECT_EQ(branch.find("-dev-"), std::string::npos);

  const std::string settings = repoFile("src/activities/settings/SettingsActivity.cpp");
  EXPECT_NE(settings.find("#ifndef TTL_DISABLE_FIRMWARE_OTA"), std::string::npos);
  const std::string main = repoFile("src/main.cpp");
  EXPECT_NE(main.find("#ifndef TTL_DISABLE_FIRMWARE_OTA"), std::string::npos);
}

TEST(TtlUrl, NormalizesThePortalRoot) {
  std::string out;
  std::string error;
  EXPECT_TRUE(ttl::ttlNormalizeServerUrl("  ", out, error));
  EXPECT_EQ(out, ttl::kDefaultServerUrl);

  EXPECT_TRUE(ttl::ttlNormalizeServerUrl("https://teenytinylibrary.app/api/sync/login/", out, error));
  EXPECT_EQ(out, "https://teenytinylibrary.app");
  EXPECT_EQ(ttl::ttlLoginUrl(out), "https://teenytinylibrary.app/api/sync/login");
  EXPECT_EQ(ttl::ttlManifestUrl(out), "https://teenytinylibrary.app/api/sync/manifest");

  EXPECT_TRUE(ttl::ttlNormalizeServerUrl("https://example.test/base/api/sync", out, error));
  EXPECT_EQ(out, "https://example.test/base");

  EXPECT_FALSE(ttl::ttlNormalizeServerUrl("https://user:secret@teenytinylibrary.app", out, error));
  EXPECT_TRUE(out.empty());
  EXPECT_FALSE(ttl::ttlNormalizeServerUrl("ftp://teenytinylibrary.app", out, error));
  EXPECT_FALSE(ttl::ttlNormalizeServerUrl("https://teenytinylibrary.app/path?x=1", out, error));
}

TEST(TtlLogin, BuildsEscapedJsonAndParsesTheToken) {
  const std::string body = ttl::ttlBuildLoginJson("a\"b@example.test", "p\\ass\nword");
  EXPECT_EQ(body, "{\"email\":\"a\\\"b@example.test\",\"password\":\"p\\\\ass\\nword\"}");

  const auto ok = ttl::ttlParseLoginResponse(
      200, "{\"ok\":true,\"token\":\"abcdefghijklmnopqrst\",\"token_type\":\"Bearer\"}");
  EXPECT_TRUE(ok.ok);
  EXPECT_EQ(ok.token, "abcdefghijklmnopqrst");

  const auto denied = ttl::ttlParseLoginResponse(
      401, "{\"ok\":false,\"error\":\"invalid_login\",\"message\":\"Email or password is incorrect.\"}");
  EXPECT_FALSE(denied.ok);
  EXPECT_TRUE(denied.token.empty());
  EXPECT_EQ(denied.errorCode, "invalid_login");
  EXPECT_EQ(ttl::ttlSyncErrorText(401, denied.errorCode, denied.message), "Wrong email or password");
  EXPECT_EQ(ttl::ttlSyncErrorText(0, "", ""), "Network error");
  EXPECT_EQ(ttl::ttlSyncErrorText(403, "unconfirmed", "Confirm your email address before signing in."),
            "Confirm email first");
  EXPECT_EQ(ttl::ttlSyncErrorText(401, "unauthorized", ""), "Sign in again");
  EXPECT_EQ(ttl::ttlSyncErrorText(404, "not_found", ""), "Book missing");
  EXPECT_EQ(ttl::ttlSyncErrorText(500, "", std::string(80, 'x')), "Sync failed");
}

TEST(TtlManifest, ParsesThePortalBookArray) {
  const char* json =
      "[{\"id\":\"bk 1\",\"title\":\"Tamil\\nTale\",\"author\":\"A\",\"filename\":\"../Books/Tale.EPUB\","
      "\"file_size\":12,\"updated_at\":\"2026-01-01T00:00:00Z\","
      "\"download_url\":\"https://teenytinylibrary.app/api/sync/download/bk%201\"}]";
  std::vector<ttl::TtlBook> books;
  std::string error;
  ASSERT_TRUE(ttl::ttlParseManifest(json, books, error)) << error;
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].id, "bk 1");
  EXPECT_EQ(books[0].title, "Tamil\nTale");
  EXPECT_EQ(books[0].author, "A");
  EXPECT_EQ(books[0].filename, "../Books/Tale.EPUB");
  EXPECT_TRUE(books[0].hasFileSize);
  EXPECT_EQ(books[0].fileSize, 12u);
  EXPECT_EQ(ttl::ttlBookLabel(books[0]), "Tamil Tale");

  std::string url;
  ASSERT_TRUE(ttl::ttlBookDownloadUrl("https://teenytinylibrary.app", books[0], url, error));
  EXPECT_EQ(url, "https://teenytinylibrary.app/api/sync/download/bk%201");

  books[0].downloadUrl.clear();
  ASSERT_TRUE(ttl::ttlBookDownloadUrl("https://teenytinylibrary.app", books[0], url, error));
  EXPECT_EQ(url, "https://teenytinylibrary.app/api/sync/download/bk%201");

  EXPECT_FALSE(ttl::ttlParseManifest("{\"books\":[]}", books, error));
  EXPECT_FALSE(ttl::ttlParseManifest("[{\"title\":\"No id\"}]", books, error));
  EXPECT_FALSE(ttl::ttlParseManifest("[{\"id\":\"a\",\"file_size\":\"12\"}]", books, error));
}

TEST(TtlPlan, SkipsOnlyAMatchingSize) {
  EXPECT_EQ(ttl::ttlPlanDownload(true, 12, true, 12), ttl::TtlBookAction::Skip);
  EXPECT_EQ(ttl::ttlPlanDownload(true, 0, true, 0), ttl::TtlBookAction::Skip);
  EXPECT_EQ(ttl::ttlPlanDownload(true, 11, true, 12), ttl::TtlBookAction::Download);
  EXPECT_EQ(ttl::ttlPlanDownload(false, 12, true, 12), ttl::TtlBookAction::Download);
  EXPECT_EQ(ttl::ttlPlanDownload(true, 12, false, 0), ttl::TtlBookAction::Download);
}

TEST(TtlFilename, StaysASingleEpubSegment) {
  EXPECT_EQ(ttl::ttlSanitizeFilename("../../Books/Tale.EPUB", "id"), "Tale.epub");
  EXPECT_EQ(ttl::ttlSanitizeFilename("CON", "id"), "_CON.epub");
  EXPECT_EQ(ttl::ttlSanitizeFilename("com1.epub", "id"), "_com1.epub");
  EXPECT_EQ(ttl::ttlSanitizeFilename("???", ""), "___.epub");
  EXPECT_EQ(ttl::ttlSanitizeFilename("", ""), "book.epub");
  EXPECT_EQ(ttl::ttlSanitizeFilename("", "a/b"), "b.epub");
  EXPECT_EQ(ttl::ttlSanitizeFilename("தமிழ்.epub", "id"), "தமிழ்.epub");

  const std::string longName(200, 'a');
  const std::string safe = ttl::ttlSanitizeFilename(longName + "\u0BA4", "id");
  EXPECT_LE(safe.size(), 120u);
  EXPECT_EQ(safe.substr(safe.size() - 5), ".epub");
  EXPECT_EQ(static_cast<unsigned char>(safe[safe.size() - 6]), 'a');
}

TEST(TtlLog, SummaryAndLinesStaySingleLine) {
  EXPECT_EQ(ttl::ttlSyncSummary(2, 3, 1), "Saved 2, skipped 3, failed 1");
  EXPECT_EQ(ttl::ttlFormatLogLine("2026-01-01T00:00:00Z", "saved\nTamil"), "2026-01-01T00:00:00Z saved Tamil");
}
