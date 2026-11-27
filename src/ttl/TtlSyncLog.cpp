// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#include "ttl/TtlSyncLog.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <ctime>

#include "ttl/TtlSyncCore.h"

namespace {

std::string nowStamp() {
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm parts{};
    gmtime_r(&now, &parts);
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &parts) > 0) return buf;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "uptime-%lu", static_cast<unsigned long>(millis()));
  return buf;
}

}  // namespace

bool ttlAppendSyncLog(const std::string& line) {
  constexpr uint64_t kMaxBytes = 64 * 1024;
  constexpr uint64_t kKeepBytes = 32 * 1024;
  if (!Storage.exists(ttl::kBookDirectory)) Storage.mkdir(ttl::kBookDirectory);

  std::string body;
  HalFile input;
  if (Storage.openFileForRead("TTL", ttl::kSyncLogPath, input)) {
    const uint64_t size = input.fileSize64();
    uint64_t start = 0;
    if (size > kMaxBytes) start = size - kKeepBytes;
    if (start > 0 && !input.seek64(start)) {
      input.close();
      LOG_ERR("TTL", "Could not seek sync log");
      return false;
    }
    const uint64_t want = size - start;
    if (want > 0 && want < 1024 * 1024) {
      body.resize(static_cast<size_t>(want));
      const int got = input.read(body.data(), body.size());
      if (got < 0) {
        body.clear();
      } else {
        body.resize(static_cast<size_t>(got));
      }
    }
    input.close();
    if (start > 0) {
      const auto nl = body.find('\n');
      if (nl != std::string::npos) body.erase(0, nl + 1);
    }
  }

  body += nowStamp();
  body.push_back(' ');
  body += line;
  if (body.empty() || body.back() != '\n') body.push_back('\n');

  HalFile output;
  if (!Storage.openFileForWrite("TTL", ttl::kSyncLogPath, output)) {
    LOG_ERR("TTL", "Could not write sync log");
    return false;
  }
  const size_t wrote = output.write(body.data(), body.size());
  output.close();
  return wrote == body.size();
}
