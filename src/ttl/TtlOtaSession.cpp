// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#include "ttl/TtlOtaSession.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>

#include "network/FirmwareBoardTag.h"
#include "network/FirmwareFlasher.h"
#include "network/HttpDownloader.h"
#include "network/OtaBootSwitch.h"
#include "ttl/TtlSyncLog.h"
#include "ttl/TtlSyncStore.h"

namespace {

constexpr size_t kChunk = 4096;
constexpr size_t kErase = 64 * 1024;
constexpr uint32_t kManifestSlack = 4096;

void logLine(const std::string& message) { ttlAppendSyncLog(message); }

bool heapForTls(uint32_t extra) {
  return ESP.getFreeHeap() >= HttpDownloader::MIN_TLS_FREE_HEAP + extra &&
         ESP.getMaxAllocHeap() >= HttpDownloader::MIN_TLS_MAX_ALLOC;
}

void pump(const TtlOtaCallbacks& callbacks) {
  if (callbacks.pump != nullptr) callbacks.pump(callbacks.ctx);
}

bool stopRequested(const TtlOtaCallbacks& callbacks) { return callbacks.cancel != nullptr && *callbacks.cancel; }

// Writes the download into the inactive app partition in 4 KiB chunks.
// Padding is 0xFF and is not part of the SHA-256. esp_ota_end() is not used:
// its image verify rejects this firmware on X4 silicon, so otadata is switched
// with ota_boot::switchTo only after the caller checks size and digest.
// cppcheck-suppress unsafeClassCanLeak
class SlotWriter {
 public:
  SlotWriter(const esp_partition_t* dest, size_t expected, const std::string& shaHex, uint8_t* buf)
      : dest_(dest), expected_(expected), shaHex_(&shaHex), buf_(buf) {
    mbedtls_sha256_init(&sha_);
    mbedtls_sha256_starts(&sha_, 0);
  }

  ~SlotWriter() { mbedtls_sha256_free(&sha_); }

  SlotWriter(const SlotWriter&) = delete;
  SlotWriter& operator=(const SlotWriter&) = delete;
  SlotWriter(SlotWriter&&) = delete;
  SlotWriter& operator=(SlotWriter&&) = delete;

  const char* error() const { return error_; }
  size_t written() const { return hashed_; }

  bool take(const uint8_t* data, size_t len) {
    if (error_ != nullptr) return false;
    if (hashed_ > expected_ || len > expected_ - hashed_) {
      error_ = "Size mismatch";
      return false;
    }
    while (len > 0) {
      const size_t n = std::min(kChunk - fill_, len);
      std::memcpy(buf_ + fill_, data, n);
      if (hashed_ < sizeof(hdr_)) {
        const size_t hdrN = std::min(sizeof(hdr_) - hashed_, n);
        std::memcpy(hdr_ + hashed_, data, hdrN);
      }
      mbedtls_sha256_update(&sha_, data, n);
      scanner_.feed(data, n);
      hashed_ += n;
      fill_ += n;
      data += n;
      len -= n;
      if (!noteHeader()) return false;
      if (scanner_.mismatch()) {
        error_ = "Wrong device";
        return false;
      }
      if (fill_ == kChunk && !flushFull()) return false;
    }
    return error_ == nullptr;
  }

  bool seal() {
    if (error_ != nullptr) return false;
    if (fill_ > 0 && !flushPartial()) return false;
    if (!headerChecked_) {
      error_ = "Bad firmware";
      return false;
    }
    if (hashed_ != expected_) {
      error_ = "Size mismatch";
      return false;
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha_, digest);
    if (!ttl::ttlSha256HexMatches(digest, sizeof(digest), *shaHex_)) {
      error_ = "Checksum mismatch";
      return false;
    }
    return true;
  }

 private:
  bool noteHeader() {
    if (headerChecked_ || hashed_ < sizeof(hdr_)) return true;
    headerChecked_ = true;
    if (hdr_[0] != 0xE9) {
      error_ = "Bad firmware";
      return false;
    }
    uint16_t imageChip = 0;
    std::memcpy(&imageChip, hdr_ + 12, sizeof(imageChip));
    const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
    if (deviceChip != 0xFFFF && imageChip != deviceChip) {
      error_ = "Wrong device";
      return false;
    }
    return true;
  }

  bool eraseThrough(size_t end) {
    while (erased_ < end) {
      if (erased_ >= dest_->size) {
        error_ = "Could not flash";
        return false;
      }
      size_t eraseLen = std::min<size_t>(kErase, static_cast<size_t>(dest_->size) - erased_);
      eraseLen &= ~static_cast<size_t>(4095);
      if (eraseLen == 0) {
        error_ = "Could not flash";
        return false;
      }
      if (esp_partition_erase_range(dest_, erased_, eraseLen) != ESP_OK) {
        LOG_ERR("TTL", "OTA erase failed at %u", static_cast<unsigned>(erased_));
        error_ = "Could not flash";
        return false;
      }
      erased_ += eraseLen;
      delay(1);
    }
    return true;
  }

  bool writeAt(size_t offset, const uint8_t* data, size_t n) {
    if (offset + n > dest_->size) {
      error_ = "Firmware too large";
      return false;
    }
    if (!eraseThrough(offset + n)) return false;
    if (esp_partition_write(dest_, offset, data, n) != ESP_OK) {
      LOG_ERR("TTL", "OTA write failed at %u", static_cast<unsigned>(offset));
      error_ = "Could not flash";
      return false;
    }
    delay(1);
    return true;
  }

  bool flushFull() {
    if (!writeAt(flashed_, buf_, kChunk)) return false;
    flashed_ += kChunk;
    fill_ = 0;
    return true;
  }

  bool flushPartial() {
    const size_t padded = (fill_ + 15) & ~static_cast<size_t>(15);
    std::memset(buf_ + fill_, 0xFF, padded - fill_);
    if (!writeAt(flashed_, buf_, padded)) return false;
    fill_ = 0;
    return true;
  }

  const esp_partition_t* dest_;
  size_t expected_;
  const std::string* shaHex_;
  uint8_t* buf_;
  mbedtls_sha256_context sha_{};
  board_tag::Scanner scanner_;
  uint8_t hdr_[14] = {};
  size_t hashed_ = 0;
  size_t fill_ = 0;
  size_t flashed_ = 0;
  size_t erased_ = 0;
  bool headerChecked_ = false;
  const char* error_ = nullptr;
};

TtlOtaInstallResult failInstall(const char* message) {
  LOG_ERR("TTL", "OTA %s", message);
  logLine(std::string("ota failed ") + message);
  TtlOtaInstallResult result;
  result.kind = TtlOtaInstallKind::Failed;
  result.message = message;
  return result;
}

}  // namespace

TtlOtaCheckResult ttlCheckFirmwareUpdate(const TtlOtaCallbacks& callbacks) {
  TtlOtaCheckResult result;
  if (!heapForTls(kManifestSlack)) {
    result.message = "Low memory";
    LOG_ERR("TTL", "OTA check skipped: low memory");
    logLine("ota failed Low memory");
    return result;
  }
  pump(callbacks);
  if (stopRequested(callbacks)) {
    result.kind = TtlOtaCheckKind::Cancelled;
    result.message = "Stopped";
    logLine("ota cancelled");
    return result;
  }

  const std::string url = ttl::ttlFirmwareLatestUrl(TTL_STORE.serverUrl());
  std::string body;
  body.reserve(512);
  bool overflow = false;
  bool cancelled = false;
  int status = 0;
  HttpDownloader::FetchOptions options;
  options.accept = "application/json";
  options.statusOut = &status;
  const bool httpOk = HttpDownloader::fetchUrl(
      url,
      [&](const uint8_t* data, size_t len) {
        pump(callbacks);
        if (stopRequested(callbacks)) {
          cancelled = true;
          return false;
        }
        if (body.size() + len > kManifestSlack) {
          overflow = true;
          return false;
        }
        body.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      options);

  if (cancelled || stopRequested(callbacks)) {
    result.kind = TtlOtaCheckKind::Cancelled;
    result.message = "Stopped";
    logLine("ota cancelled");
    return result;
  }
  if (overflow || !httpOk) {
    result.message = overflow ? "Bad update info" : "Network error";
    LOG_ERR("TTL", "OTA check failed (%d)", status);
    logLine(std::string("ota failed ") + result.message);
    return result;
  }

  std::string error;
  if (!ttl::ttlParseFirmwareManifest(body, result.manifest, error)) {
    result.manifest = {};
    result.message = error.empty() ? "Bad update info" : error;
    LOG_ERR("TTL", "OTA manifest rejected");
    logLine(std::string("ota failed ") + result.message);
    return result;
  }
  body.clear();

  if (ttl::compareSemVer(result.manifest.version, ttl::kVersionNumber) <= 0) {
    result.kind = TtlOtaCheckKind::UpToDate;
    result.manifest = {};
    result.message = ttl::kVersionLabel;
    logLine(std::string("ota up to date ") + ttl::kVersionNumber);
    return result;
  }

  result.kind = TtlOtaCheckKind::Available;
  logLine(std::string("ota available ") + result.manifest.version);
  return result;
}

TtlOtaInstallResult ttlInstallFirmwareUpdate(const ttl::TtlFirmwareManifest& manifest,
                                             const TtlOtaCallbacks& callbacks) {
  std::string error;
  if (!ttl::ttlFirmwareManifestUsable(manifest, error)) {
    return failInstall(error.empty() ? "Bad update info" : error.c_str());
  }
  if (!heapForTls(kChunk)) return failInstall("Low memory");

  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (dest == nullptr) return failInstall("Could not flash");

  const size_t expected = static_cast<size_t>(manifest.size);
  const size_t paddedEnd = (expected + 15) & ~static_cast<size_t>(15);
  if (manifest.size != expected || expected > dest->size || paddedEnd > dest->size) {
    return failInstall("Firmware too large");
  }

  auto buffer = makeUniqueNoThrow<uint8_t[]>(kChunk);
  if (!buffer) {
    LOG_ERR("TTL", "OOM: OTA buffer (%u)", static_cast<unsigned>(kChunk));
    return failInstall("Low memory");
  }
  if (!heapForTls(0)) return failInstall("Low memory");

  SlotWriter writer(dest, expected, manifest.sha256, buffer.get());
  bool cancelled = false;
  int lastBucket = -1;
  int status = 0;
  HttpDownloader::FetchOptions options;
  options.statusOut = &status;
  const bool httpOk = HttpDownloader::fetchUrl(
      manifest.downloadUrl,
      [&](const uint8_t* data, size_t len) {
        pump(callbacks);
        if (stopRequested(callbacks)) {
          cancelled = true;
          return false;
        }
        if (!writer.take(data, len)) return false;
        const int pct = static_cast<int>((static_cast<uint64_t>(writer.written()) * 100) / expected);
        const int bucket = pct >= 100 ? 100 : (pct / 5) * 5;
        if (bucket != lastBucket) {
          lastBucket = bucket;
          if (callbacks.onProgress != nullptr) callbacks.onProgress(pct > 100 ? 100 : pct, callbacks.ctx);
        }
        return true;
      },
      options);

  if (cancelled || stopRequested(callbacks)) {
    logLine("ota cancelled");
    TtlOtaInstallResult result;
    result.kind = TtlOtaInstallKind::Cancelled;
    result.message = "Stopped";
    return result;
  }
  if (writer.error() != nullptr) return failInstall(writer.error());
  if (!httpOk) {
    LOG_ERR("TTL", "OTA download failed (%d)", status);
    return failInstall("Network error");
  }
  if (!writer.seal()) return failInstall(writer.error() != nullptr ? writer.error() : "Could not flash");
  if (!ota_boot::switchTo(dest)) return failInstall("Could not flash");

  LOG_INF("TTL", "OTA complete %s", manifest.version.c_str());
  logLine(std::string("ota complete ") + manifest.version);
  TtlOtaInstallResult result;
  result.kind = TtlOtaInstallKind::Ok;
  result.message = "Update complete. Rebooting...";
  return result;
}
