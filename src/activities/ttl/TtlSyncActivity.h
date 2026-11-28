// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "activities/UiListActivity.h"
#include "ttl/TtlOtaSession.h"
#include "ttl/TtlSyncSession.h"

// Full-page TTL Sync. Replacing Home frees the reader heap before any TLS call.
class TtlSyncActivity final : public UiListActivity {
 public:
  explicit TtlSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  bool preventAutoSleep() override { return screen_ == Screen::Busy; }
  bool skipLoopDelay() override { return screen_ == Screen::Busy; }
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen { Menu, Busy, Result, Offer };
  enum class BusyJob { Sync, Check, Install };
  enum class NetJob { Sync, Update };
  static constexpr int kRows = 6;

  int listCount() const override { return kRows; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override { return "TTL Sync"; }

  void startSync();
  void runSync();
  void startUpdateCheck();
  void checkForUpdate();
  void startInstall();
  void connectThen(NetJob job);
  void runNetJob(NetJob job);
  void backToMenu();
  void showResult(bool ok, bool cancelled, const std::string& message);
  void showNotice(const char* heading, const std::string& message);
  TtlOtaCallbacks otaCallbacks();
  bool wifiConnected() const;

  Screen screen_ = Screen::Menu;
  BusyJob busyJob_ = BusyJob::Sync;
  TtlSyncPhase phase_ = TtlSyncPhase::Login;
  int index_ = 0;
  int bookTotal_ = 0;
  std::string statusTitle_;
  std::atomic<size_t> bytes_{0};
  std::atomic<size_t> total_{0};
  std::atomic<int> progressPercent_{0};
  bool cancelRequested_ = false;
  bool goHomeRequested_ = false;
  uint32_t lastPaintMs_ = 0;
  bool resultOk_ = false;
  bool resultCancelled_ = false;
  std::string resultHeading_;
  std::string resultMessage_;
  bool wifiUsed_ = false;
  ttl::TtlFirmwareManifest pending_;
  std::string offerVersion_;
  std::string offerSize_;
  std::string offerNotes_;

  std::string rowValues_[kRows];
  freeink::ui::ListItem rowItems_[kRows]{};
};
