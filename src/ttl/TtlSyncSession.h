// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class TtlSyncPhase { Login, Manifest, Download };

struct TtlSyncStatus {
  TtlSyncPhase phase = TtlSyncPhase::Login;
  int index = 0;
  int total = 0;
  std::string title;
};

struct TtlSyncRunResult {
  bool ok = false;
  bool cancelled = false;
  int saved = 0;
  int skipped = 0;
  int failed = 0;
  std::string message;
};

struct TtlSyncCallbacks {
  void (*onStatus)(const TtlSyncStatus& status, void* ctx) = nullptr;
  void (*onProgress)(size_t bytes, size_t total, void* ctx) = nullptr;
  void (*pump)(void* ctx) = nullptr;
  void* ctx = nullptr;
  bool* cancel = nullptr;
};

// Full-page sync against ttl-portal v0.1.1:
//   POST /api/sync/login
//   GET  /api/sync/manifest
//   GET  /api/sync/download/:id
// Home is not on the activity stack while this runs, so TLS has the heap.
TtlSyncRunResult ttlRunSync(const TtlSyncCallbacks& callbacks);
