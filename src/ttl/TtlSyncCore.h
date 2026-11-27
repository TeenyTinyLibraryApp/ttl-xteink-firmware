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

inline constexpr const char kVersionNumber[] = "0.1.1";
inline constexpr const char kVersionLabel[] = "TTL 0.1.1";
inline constexpr const char kDefaultServerUrl[] = "https://teenytinylibrary.app";
inline constexpr const char kBookDirectory[] = "/TTL";
inline constexpr const char kSyncLogPath[] = "/TTL/ttl-sync-log.txt";
// v0.1.1 is the recovery baseline: no firmware update checks or flashing UI.
inline constexpr bool kFirmwareOtaEnabled = false;

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
