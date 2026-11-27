// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#pragma once

#include <string>

#include "ttl/TtlSyncCore.h"

// Device half of portal OTA. Parsing and version compare live in TtlSyncCore.

enum class TtlOtaCheckKind { UpToDate, Available, Failed, Cancelled };

struct TtlOtaCheckResult {
  TtlOtaCheckKind kind = TtlOtaCheckKind::Failed;
  std::string message;
  ttl::TtlFirmwareManifest manifest;
};

enum class TtlOtaInstallKind { Ok, Failed, Cancelled };

struct TtlOtaInstallResult {
  TtlOtaInstallKind kind = TtlOtaInstallKind::Failed;
  std::string message;
};

struct TtlOtaCallbacks {
  void (*onProgress)(int percent, void* ctx) = nullptr;
  void (*pump)(void* ctx) = nullptr;
  void* ctx = nullptr;
  bool* cancel = nullptr;
};

// GET {server}/api/firmware/latest. Public: no login token.
TtlOtaCheckResult ttlCheckFirmwareUpdate(const TtlOtaCallbacks& callbacks);

// Stream manifest.downloadUrl into the inactive app slot. otadata is updated
// only after the byte count and SHA-256 both match.
TtlOtaInstallResult ttlInstallFirmwareUpdate(const ttl::TtlFirmwareManifest& manifest,
                                             const TtlOtaCallbacks& callbacks);
