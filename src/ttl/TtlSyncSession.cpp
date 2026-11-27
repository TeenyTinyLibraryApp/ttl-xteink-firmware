// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#include "ttl/TtlSyncSession.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <string>
#include <vector>

#include "network/HttpDownloader.h"
#include "ttl/TtlSyncCore.h"
#include "ttl/TtlSyncLog.h"
#include "ttl/TtlSyncStore.h"

namespace {

void logLine(const std::string& message) {
  std::string clean = message;
  std::replace_if(clean.begin(), clean.end(), [](const char c) { return c == '\n' || c == '\r'; }, ' ');
  ttlAppendSyncLog(clean);
}

struct SignIn {
  bool ok = false;
  std::string error;
};

SignIn signIn(std::string& token) {
  SignIn attempt;
  const std::string email = TTL_STORE.email();
  std::string password = TTL_STORE.password();
  if (email.empty() || password.empty()) {
    attempt.error = "Add email and password";
    return attempt;
  }
  const std::string url = ttl::ttlLoginUrl(TTL_STORE.serverUrl());
  std::string body = ttl::ttlBuildLoginJson(email, password);
  password.clear();
  std::string response;
  int status = 0;
  HttpDownloader::postJson(url, body, response, &status);
  body.assign(body.size(), '\0');
  body.clear();
  const ttl::TtlLoginResponse parsed = ttl::ttlParseLoginResponse(status, response);
  if (!parsed.ok) {
    logLine(std::string("login failed ") + (parsed.errorCode.empty() ? std::to_string(status) : parsed.errorCode));
    attempt.error = ttl::ttlSyncErrorText(status, parsed.errorCode, parsed.message);
    return attempt;
  }
  token = parsed.token;
  TTL_STORE.setToken(token);
  if (!TTL_STORE.saveToFile()) LOG_ERR("TTL", "Could not store device token");
  logLine("login ok");
  attempt.ok = true;
  return attempt;
}

void notify(const TtlSyncCallbacks& callbacks, TtlSyncPhase phase, int index, int total, const std::string& title) {
  if (callbacks.onStatus == nullptr) return;
  TtlSyncStatus status;
  status.phase = phase;
  status.index = index;
  status.total = total;
  status.title = title;
  callbacks.onStatus(status, callbacks.ctx);
}

bool stopRequested(const TtlSyncCallbacks& callbacks) { return callbacks.cancel != nullptr && *callbacks.cancel; }

void pump(const TtlSyncCallbacks& callbacks) {
  if (callbacks.pump != nullptr) callbacks.pump(callbacks.ctx);
}

bool heapReady() {
  return ESP.getFreeHeap() >= HttpDownloader::MIN_TLS_FREE_HEAP &&
         ESP.getMaxAllocHeap() >= HttpDownloader::MIN_TLS_MAX_ALLOC;
}

bool existingSize(const std::string& path, bool& exists, uint64_t& size) {
  exists = false;
  size = 0;
  if (!Storage.exists(path.c_str())) return true;
  HalFile file;
  if (!Storage.openFileForRead("TTL", path.c_str(), file)) return false;
  size = file.fileSize64();
  exists = true;
  file.close();
  return true;
}

TtlSyncRunResult finish(bool ok, bool cancelled, int saved, int skipped, int failed, const std::string& message) {
  logLine(cancelled ? "cancelled" : message);
  TtlSyncRunResult result;
  result.ok = ok;
  result.cancelled = cancelled;
  result.saved = saved;
  result.skipped = skipped;
  result.failed = failed;
  result.message = message;
  return result;
}

enum class ManifestFetch { Ok, Unauthorized, Failed };

ManifestFetch fetchManifest(const std::string& token, std::string& payload, int& status) {
  payload.clear();
  status = 0;
  bool overflow = false;
  HttpDownloader::FetchOptions options;
  options.bearer = token;
  options.accept = "application/json";
  options.captureErrorBody = true;
  options.statusOut = &status;
  const bool httpOk = HttpDownloader::fetchUrl(
      ttl::ttlManifestUrl(TTL_STORE.serverUrl()),
      [&](const uint8_t* data, size_t len) {
        if (payload.size() + len > ttl::kMaxManifestBytes) {
          overflow = true;
          return false;
        }
        payload.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      options);
  if (overflow) return ManifestFetch::Failed;
  if (status == 401 || status == 403) return ManifestFetch::Unauthorized;
  if (!httpOk) return ManifestFetch::Failed;
  return ManifestFetch::Ok;
}

}  // namespace

TtlSyncRunResult ttlRunSync(const TtlSyncCallbacks& callbacks) {
  logLine("start");
  if (!heapReady()) return finish(false, false, 0, 0, 0, "Low memory");

  std::string token = TTL_STORE.token();
  if (token.empty()) {
    notify(callbacks, TtlSyncPhase::Login, 0, 0, "");
    const SignIn attempt = signIn(token);
    if (!attempt.ok) return finish(false, false, 0, 0, 0, attempt.error);
  }

  notify(callbacks, TtlSyncPhase::Manifest, 0, 0, "");
  if (!heapReady()) return finish(false, false, 0, 0, 0, "Low memory");

  std::string payload;
  int status = 0;
  ManifestFetch fetched = fetchManifest(token, payload, status);
  if (fetched == ManifestFetch::Unauthorized && TTL_STORE.hasPassword()) {
    token.clear();
    TTL_STORE.setToken("");
    TTL_STORE.saveToFile();
    notify(callbacks, TtlSyncPhase::Login, 0, 0, "");
    if (!heapReady()) return finish(false, false, 0, 0, 0, "Low memory");
    const SignIn attempt = signIn(token);
    if (!attempt.ok) return finish(false, false, 0, 0, 0, attempt.error.empty() ? "Sign in again" : attempt.error);
    notify(callbacks, TtlSyncPhase::Manifest, 0, 0, "");
    fetched = fetchManifest(token, payload, status);
  }
  if (fetched == ManifestFetch::Unauthorized) {
    TTL_STORE.setToken("");
    TTL_STORE.saveToFile();
    return finish(false, false, 0, 0, 0, "Sign in again");
  }
  if (fetched != ManifestFetch::Ok) {
    return finish(false, false, 0, 0, 0, ttl::ttlSyncErrorText(status, "", ""));
  }

  std::vector<ttl::TtlBook> books;
  std::string parseError;
  if (!ttl::ttlParseManifest(payload, books, parseError)) {
    payload.clear();
    return finish(false, false, 0, 0, 0, parseError.empty() ? "Bad book list" : parseError);
  }
  payload.clear();
  logLine("manifest " + std::to_string(books.size()));
  if (books.empty()) return finish(true, false, 0, 0, 0, "No books");

  if (!Storage.mkdir(ttl::kBookDirectory)) return finish(false, false, 0, 0, 0, "SD error");

  int saved = 0;
  int skipped = 0;
  int failed = 0;
  const int total = static_cast<int>(books.size());
  for (int i = 0; i < total; ++i) {
    pump(callbacks);
    if (stopRequested(callbacks)) return finish(false, true, saved, skipped, failed, "Cancelled");
    if (!heapReady()) return finish(false, false, saved, skipped, failed, "Low memory");

    const ttl::TtlBook& book = books[static_cast<size_t>(i)];
    const std::string label = ttl::ttlBookLabel(book);
    const std::string dest = std::string(ttl::kBookDirectory) + "/" + ttl::ttlSanitizeFilename(book.filename, book.id);
    bool exists = false;
    uint64_t size = 0;
    if (!existingSize(dest, exists, size)) {
      ++failed;
      logLine("error " + label);
      continue;
    }
    if (ttl::ttlPlanDownload(exists, size, book.hasFileSize, book.fileSize) == ttl::TtlBookAction::Skip) {
      ++skipped;
      logLine("skip " + label);
      continue;
    }

    std::string url;
    std::string urlError;
    if (!ttl::ttlBookDownloadUrl(TTL_STORE.serverUrl(), book, url, urlError)) {
      ++failed;
      logLine("error " + label);
      continue;
    }

    notify(callbacks, TtlSyncPhase::Download, i + 1, total, label);
    if (callbacks.onProgress != nullptr) {
      const size_t expected = book.hasFileSize ? static_cast<size_t>(book.fileSize) : 0;
      callbacks.onProgress(0, expected, callbacks.ctx);
    }

    std::vector<HttpDownloader::Header> headers;
    headers.emplace_back("Authorization", "Bearer " + token);
    const auto downloaded = HttpDownloader::downloadToFile(
        url, dest,
        [&](size_t got, size_t expected) {
          if (callbacks.onProgress != nullptr) callbacks.onProgress(got, expected, callbacks.ctx);
          pump(callbacks);
        },
        callbacks.cancel, "", "", headers, false);

    if (downloaded == HttpDownloader::ABORTED || stopRequested(callbacks)) {
      return finish(false, true, saved, skipped, failed, "Cancelled");
    }
    if (downloaded == HttpDownloader::UNAUTHORIZED) {
      TTL_STORE.setToken("");
      TTL_STORE.saveToFile();
      ++failed;
      logLine("error " + label);
      return finish(false, false, saved, skipped, failed, "Sign in again");
    }
    if (downloaded != HttpDownloader::OK) {
      ++failed;
      logLine("error " + label);
      continue;
    }

    bool wrote = false;
    uint64_t written = 0;
    if (!existingSize(dest, wrote, written) || !wrote || (book.hasFileSize && written != book.fileSize)) {
      Storage.remove(dest.c_str());
      ++failed;
      logLine("error " + label);
      continue;
    }
    ++saved;
    logLine("saved " + label);
  }

  const std::string summary = ttl::ttlSyncSummary(saved, skipped, failed);
  const bool ok = failed == 0;
  return finish(ok, false, saved, skipped, failed, summary);
}
