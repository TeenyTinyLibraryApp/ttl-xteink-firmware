// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors
//
// Independent community fork of CrossPoint Reader. Not affiliated with Xteink.
// The combined firmware executable is GPL-3.0-or-later because it includes
// wolfSSL. See NOTICE and licenses/README.md.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Host-testable TTL sync rules: version label, portal JSON, filenames, and
// which books to download. Device I/O lives in TtlSyncSession.

namespace ttl {

inline constexpr const char kVersionNumber[] = "0.2.0";
inline constexpr const char kVersionLabel[] = "TTL 0.2.0";
inline constexpr const char kDefaultServerUrl[] = "https://teenytinylibrary.app";
inline constexpr const char kBookDirectory[] = "/TTL";
inline constexpr const char kSyncLogPath[] = "/TTL/ttl-sync-log.txt";
// Portal OTA is offered from the TTL Sync screen. The GitHub updater stays
// behind TTL_DISABLE_FIRMWARE_OTA so that path adds no flash.
inline constexpr bool kFirmwareOtaEnabled = true;

inline constexpr size_t kMaxServerUrl = 200;
inline constexpr size_t kMaxEmail = 128;
inline constexpr size_t kMaxPassword = 128;
inline constexpr size_t kMaxToken = 256;
inline constexpr size_t kMaxManifestBytes = 256 * 1024;
inline constexpr size_t kMaxBooks = 500;

struct TtlBook {
  std::string id;
  std::string title;
  std::string author;
  std::string filename;
  std::string updatedAt;
  std::string downloadUrl;
  uint64_t fileSize = 0;
  bool hasFileSize = false;
};

enum class TtlBookAction { Download, Skip };

struct TtlLoginResponse {
  bool ok = false;
  std::string token;
  std::string errorCode;
  std::string message;
};

// Empty or whitespace input becomes kDefaultServerUrl. A pasted /api/sync
// path is stripped back to the server root. Rejects missing schemes, userinfo,
// queries, fragments, and whitespace inside the URL.
bool ttlNormalizeServerUrl(const std::string& input, std::string& out, std::string& error);

// Empty input clears the address (out empty, returns true). A non-empty value
// must contain '@' and no spaces.
bool ttlNormalizeEmail(const std::string& input, std::string& out, std::string& error);

std::string ttlLoginUrl(const std::string& server);
std::string ttlManifestUrl(const std::string& server);

// Absolute http(s) download_url wins. Otherwise GET {server}/api/sync/download/{id}.
bool ttlBookDownloadUrl(const std::string& server, const TtlBook& book, std::string& out, std::string& error);

// JSON object for POST /api/sync/login. Escapes quotes, backslashes, and controls.
std::string ttlBuildLoginJson(const std::string& email, const std::string& password);

// Accepts a JSON array of book objects. Unknown fields are ignored.
bool ttlParseManifest(const std::string& json, std::vector<TtlBook>& books, std::string& error);

struct TtlFirmwareManifest {
  std::string device;
  std::string version;
  std::string filename;
  std::string downloadUrl;
  uint64_t size = 0;
  std::string sha256;
  std::string notes;
};

// Negative when a < b, zero when equal, positive when a > b.
// Strips a leading "TTL " and a single leading v, then compares MAJOR.MINOR.PATCH.
int compareSemVer(const std::string& a, const std::string& b);

std::string ttlFirmwareLatestUrl(const std::string& server);

// Parses one /api/firmware/latest object and rejects an unusable manifest.
bool ttlParseFirmwareManifest(const std::string& json, TtlFirmwareManifest& manifest, std::string& error);

// version non-empty, download URL http(s), size > 0, sha256 64 hex digits.
bool ttlFirmwareManifestUsable(const TtlFirmwareManifest& manifest, std::string& error);

// "0.2.0" and "v0.2.0" become "TTL 0.2.0". A value that already starts with "TTL " is kept.
std::string ttlVersionLabel(const std::string& version);

// Rounded half-up to a tenth of a mebibyte, e.g. 5757440 -> "5.5 MB".
std::string ttlFormatFirmwareSize(uint64_t bytes);

// One line, clipped on a UTF-8 boundary. Adds "..." when it does not fit.
std::string ttlClipNote(const std::string& notes, size_t maxBytes);

// Case-insensitive match of a raw SHA-256 digest against a 64-digit hex string.
bool ttlSha256HexMatches(const uint8_t* digest, size_t digestLen, const std::string& hex);

// httpStatus 0 means the request never completed. Token characters are limited
// to the portal's base64url alphabet.
TtlLoginResponse ttlParseLoginResponse(int httpStatus, const std::string& body);

// Short on-screen text. Known portal error codes win over the server message.
std::string ttlSyncErrorText(int httpStatus, const std::string& errorCode, const std::string& message);

TtlBookAction ttlPlanDownload(bool exists, uint64_t existingSize, bool hasFileSize, uint64_t fileSize);

// Single path segment ending in .epub. Drops directories and reserved names.
std::string ttlSanitizeFilename(const std::string& filename, const std::string& idFallback);

std::string ttlBookLabel(const TtlBook& book);

std::string ttlSyncSummary(int saved, int skipped, int failed);

// One physical log line. Newlines in the message become spaces.
std::string ttlFormatLogLine(const std::string& stamp, const std::string& message);

}  // namespace ttl
