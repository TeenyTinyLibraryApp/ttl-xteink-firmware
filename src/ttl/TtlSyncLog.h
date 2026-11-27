// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#pragma once

#include <string>

// Appends one line to /TTL/ttl-sync-log.txt. The line must already be a single
// line (see ttlFormatLogLine). Passwords and tokens are never written here.
bool ttlAppendSyncLog(const std::string& line);
