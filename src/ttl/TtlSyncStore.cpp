// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#include "ttl/TtlSyncStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>

#include "ttl/TtlSyncCore.h"

namespace {

bool decodeSecret(const char* encoded, size_t maxLen, std::string& out) {
  out.clear();
  if (encoded == nullptr || encoded[0] == '\0') return true;
  bool ok = false;
  bool tooLong = false;
  out = obfuscation::deobfuscateFromBase64(encoded, maxLen, &ok, &tooLong);
  if (!ok || tooLong) {
    out.clear();
    return false;
  }
  return true;
}

}  // namespace

void TtlSyncStore::toJson(JsonDocument& doc) const {
  doc["server_url"] = serverUrl_;
  doc["email"] = email_;
  if (!password_.empty()) doc["password_obf"] = obfuscation::obfuscateToBase64(password_);
  if (!token_.empty()) doc["token_obf"] = obfuscation::obfuscateToBase64(token_);
}

bool TtlSyncStore::fromJson(JsonVariantConst doc) {
  bool resave = false;
  const char* server = doc["server_url"] | "";
  const char* email = doc["email"] | "";
  if (std::strlen(server) > ttl::kMaxServerUrl) {
    serverUrl_.clear();
    resave = true;
  } else {
    serverUrl_ = server;
  }
  if (std::strlen(email) > ttl::kMaxEmail) {
    email_.clear();
    resave = true;
  } else {
    email_ = email;
  }
  if (!decodeSecret(doc["password_obf"] | "", ttl::kMaxPassword, password_)) resave = true;
  if (!decodeSecret(doc["token_obf"] | "", ttl::kMaxToken, token_)) resave = true;
  if (resave) requestResave();
  LOG_DBG("TTL", "Loaded sync settings (signed in: %d)", hasToken() ? 1 : 0);
  return true;
}

std::string TtlSyncStore::serverUrl() const {
  if (!serverUrl_.empty()) return serverUrl_;
  return ttl::kDefaultServerUrl;
}

void TtlSyncStore::setServerUrl(const std::string& url) {
  if (url != serverUrl_) token_.clear();
  serverUrl_ = url;
}

void TtlSyncStore::setEmail(const std::string& email) {
  if (email != email_) token_.clear();
  email_ = email;
}

void TtlSyncStore::setPassword(const std::string& password) {
  if (password != password_) token_.clear();
  password_ = password;
}

void TtlSyncStore::setToken(const std::string& token) { token_ = token; }

void TtlSyncStore::clearSecrets() {
  password_.clear();
  token_.clear();
}
