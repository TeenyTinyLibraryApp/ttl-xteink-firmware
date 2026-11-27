// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors
//
// Account secrets are XOR-obfuscated with the device MAC before they are
// written to the SD card, the same scheme as Wi-Fi passwords. That stops
// casual reading and ties the file to this device. It is not disk encryption.
// Nothing in this store is compiled into the firmware.

#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

class TtlSyncStore : public PersistableStore<TtlSyncStore> {
 private:
  std::string serverUrl_;
  std::string email_;
  std::string password_;
  std::string token_;

  TtlSyncStore() = default;
  ~TtlSyncStore() = default;
  friend class PersistableStore<TtlSyncStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/ttl_sync.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  std::string serverUrl() const;
  const std::string& email() const { return email_; }
  const std::string& password() const { return password_; }
  const std::string& token() const { return token_; }
  bool hasPassword() const { return !password_.empty(); }
  bool hasToken() const { return !token_.empty(); }

  // Changing the server, email, or password drops the device token.
  void setServerUrl(const std::string& url);
  void setEmail(const std::string& email);
  void setPassword(const std::string& password);
  void setToken(const std::string& token);
  void clearSecrets();
};

#define TTL_STORE TtlSyncStore::getInstance()
