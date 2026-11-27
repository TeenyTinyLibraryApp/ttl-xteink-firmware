// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "activities/UiListActivity.h"
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
  enum class Screen { Menu, Busy, Result };
  static constexpr int kRows = 5;

  int listCount() const override { return kRows; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override { return "TTL Sync"; }

  void startSync();
  void runSync();
  void showResult(bool ok, bool cancelled, const std::string& message);
  bool wifiConnected() const;

  Screen screen_ = Screen::Menu;
  TtlSyncPhase phase_ = TtlSyncPhase::Login;
  int index_ = 0;
  int bookTotal_ = 0;
  std::string statusTitle_;
  std::atomic<size_t> bytes_{0};
  std::atomic<size_t> total_{0};
  bool cancelRequested_ = false;
  bool goHomeRequested_ = false;
  uint32_t lastPaintMs_ = 0;
  bool resultOk_ = false;
  bool resultCancelled_ = false;
  std::string resultMessage_;
  bool wifiUsed_ = false;

  std::string rowValues_[kRows];
  freeink::ui::ListItem rowItems_[kRows]{};
};
