// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Teeny Tiny Library contributors

#include "TtlSyncActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>
#include <utility>
#include <variant>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "ttl/TtlSyncCore.h"
#include "ttl/TtlSyncStore.h"

namespace fui = freeink::ui;

namespace {
constexpr int kSync = 0;
constexpr int kServer = 1;
constexpr int kEmail = 2;
constexpr int kPassword = 3;
constexpr int kSignOut = 4;
constexpr int kUpdate = 5;

const char* kLabels[] = {"Sync", "Server", "Email", "Password", "Sign out", "Check for Update"};
}  // namespace

TtlSyncActivity::TtlSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("TtlSync", renderer, mappedInput) {
  static_assert(sizeof(kLabels) / sizeof(kLabels[0]) == kRows, "TTL Sync row labels");
  for (int i = 0; i < kRows; ++i) {
    rowItems_[i].label = kLabels[i];
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

void TtlSyncActivity::onExit() {
  Activity::onExit();
  if (wifiUsed_ && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

bool TtlSyncActivity::wifiConnected() const {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void TtlSyncActivity::showResult(const bool ok, const bool cancelled, const std::string& message) {
  {
    RenderLock lock(*this);
    screen_ = Screen::Result;
    resultOk_ = ok;
    resultCancelled_ = cancelled;
    resultHeading_.clear();
    resultMessage_ = message;
    pending_ = {};
  }
  requestUpdate();
}

void TtlSyncActivity::showNotice(const char* heading, const std::string& message) {
  {
    RenderLock lock(*this);
    screen_ = Screen::Result;
    resultOk_ = false;
    resultCancelled_ = false;
    resultHeading_ = heading == nullptr ? "" : heading;
    resultMessage_ = message;
    pending_ = {};
  }
  requestUpdate();
}

void TtlSyncActivity::backToMenu() {
  {
    RenderLock lock(*this);
    screen_ = Screen::Menu;
    pending_ = {};
    offerVersion_.clear();
    offerSize_.clear();
    offerNotes_.clear();
    resultHeading_.clear();
  }
  requestUpdate();
}

bool TtlSyncActivity::handleCustomInput() {
  switch (screen_) {
    case Screen::Menu:
      return false;
    case Screen::Busy:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.isPressed(MappedInputManager::Button::Back)) {
        cancelRequested_ = true;
      }
      return true;
    case Screen::Offer:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startInstall();
        return true;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) backToMenu();
      return true;
    case Screen::Result:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        backToMenu();
      }
      return true;
  }
  return true;
}

void TtlSyncActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  rowValues_[kSync] = TTL_STORE.hasToken() ? "Signed in" : "Sign in";
  rowValues_[kServer] = TTL_STORE.serverUrl();
  rowValues_[kEmail] = TTL_STORE.email().empty() ? "Not set" : TTL_STORE.email();
  rowValues_[kPassword] = TTL_STORE.hasPassword() ? "Set" : "Not set";
  rowValues_[kSignOut].clear();
  for (int i = 0; i < kRows; ++i) {
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(kRows);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void TtlSyncActivity::activateIndex(const int index) {
  if (screen_ != Screen::Menu) return;
  app.clearTapFlash();

  if (index == kServer || index == kEmail || index == kPassword) {
    const char* title = index == kServer ? "Server" : index == kEmail ? "Email" : "Password";
    std::string prefill;
    InputType input = InputType::Text;
    size_t limit = ttl::kMaxPassword;
    if (index == kServer) {
      prefill = TTL_STORE.serverUrl();
      input = InputType::Url;
      limit = ttl::kMaxServerUrl;
    } else if (index == kEmail) {
      prefill = TTL_STORE.email();
      limit = ttl::kMaxEmail;
    }
    auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, title, prefill, limit, input);
    if (!keyboard) {
      LOG_ERR("TTL", "OOM: keyboard");
      showResult(false, false, "Low memory");
      return;
    }
    startActivityForResult(std::move(keyboard), [this, index](const ActivityResult& result) {
      if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) return;
      const auto& text = std::get<KeyboardResult>(result.data).text;
      std::string error;
      if (index == kServer) {
        std::string url;
        if (!ttl::ttlNormalizeServerUrl(text, url, error)) {
          showResult(false, false, error.empty() ? "Bad URL" : error);
          return;
        }
        if (url == ttl::kDefaultServerUrl) url.clear();
        TTL_STORE.setServerUrl(url);
      } else if (index == kEmail) {
        std::string email;
        if (!ttl::ttlNormalizeEmail(text, email, error)) {
          showResult(false, false, error.empty() ? "Bad email" : error);
          return;
        }
        TTL_STORE.setEmail(email);
      } else {
        TTL_STORE.setPassword(text.size() > ttl::kMaxPassword ? text.substr(0, ttl::kMaxPassword) : text);
      }
      if (!TTL_STORE.saveToFile()) {
        showResult(false, false, "SD error");
        return;
      }
      requestUpdate();
    });
    return;
  }

  if (index == kSignOut) {
    TTL_STORE.clearSecrets();
    if (!TTL_STORE.saveToFile()) showResult(false, false, "SD error");
    requestUpdate();
    return;
  }

  if (index == kSync) startSync();
  if (index == kUpdate) startUpdateCheck();
}

void TtlSyncActivity::connectThen(const NetJob job) {
  wifiUsed_ = true;
  if (wifiConnected()) {
    runNetJob(job);
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("TTL", "OOM: Wi-Fi selection");
    showResult(false, false, "Low memory");
    return;
  }
  startActivityForResult(std::move(wifi), [this, job](const ActivityResult& result) {
    if (result.isCancelled || !wifiConnected()) {
      showResult(false, false, "Wi-Fi needed");
      return;
    }
    runNetJob(job);
  });
}

void TtlSyncActivity::runNetJob(const NetJob job) {
  switch (job) {
    case NetJob::Sync:
      runSync();
      return;
    case NetJob::Update:
      checkForUpdate();
      return;
  }
}

void TtlSyncActivity::startSync() {
  if (!TTL_STORE.hasToken() && (TTL_STORE.email().empty() || !TTL_STORE.hasPassword())) {
    showResult(false, false, "Add email and password");
    return;
  }
  connectThen(NetJob::Sync);
}

void TtlSyncActivity::startUpdateCheck() { connectThen(NetJob::Update); }

void TtlSyncActivity::runSync() {
  {
    RenderLock lock(*this);
    screen_ = Screen::Busy;
    busyJob_ = BusyJob::Sync;
    phase_ = TtlSyncPhase::Login;
    index_ = 0;
    bookTotal_ = 0;
    statusTitle_.clear();
    bytes_ = 0;
    total_ = 0;
    cancelRequested_ = false;
    goHomeRequested_ = false;
    lastPaintMs_ = millis();
  }
  requestUpdateAndWait();

  TtlSyncCallbacks callbacks;
  callbacks.ctx = this;
  callbacks.cancel = &cancelRequested_;
  callbacks.onStatus = [](const TtlSyncStatus& status, void* ctx) {
    auto* self = static_cast<TtlSyncActivity*>(ctx);
    {
      RenderLock lock(*self);
      self->phase_ = status.phase;
      self->index_ = status.index;
      self->bookTotal_ = status.total;
      self->statusTitle_ = status.title;
    }
    self->requestUpdateAndWait();
  };
  callbacks.onProgress = [](const size_t got, const size_t expected, void* ctx) {
    auto* self = static_cast<TtlSyncActivity*>(ctx);
    self->bytes_ = got;
    self->total_ = expected;
  };
  callbacks.pump = [](void* ctx) {
    auto* self = static_cast<TtlSyncActivity*>(ctx);
    self->mappedInput.update(true);
    if (self->mappedInput.isPressed(MappedInputManager::Button::Back) ||
        self->mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      self->cancelRequested_ = true;
    }
    if (self->mappedInput.wasHomeGesture()) {
      self->cancelRequested_ = true;
      self->goHomeRequested_ = true;
    }
    if (millis() - self->lastPaintMs_ >= 5000) {
      self->lastPaintMs_ = millis();
      self->requestUpdate(true);
    }
  };

  const TtlSyncRunResult result = ttlRunSync(callbacks);
  // The pump callback sets goHomeRequested_ on the home gesture. cppcheck does
  // not follow that function pointer, so it treats the flag as still false.
  // cppcheck-suppress knownConditionTrueFalse
  if (goHomeRequested_) {
    onGoHome(HomeMenuItem::TTL_SYNC);
    return;
  }
  showResult(result.ok, result.cancelled, result.message);
}

TtlOtaCallbacks TtlSyncActivity::otaCallbacks() {
  TtlOtaCallbacks callbacks;
  callbacks.ctx = this;
  callbacks.cancel = &cancelRequested_;
  callbacks.onProgress = [](const int percent, void* ctx) {
    auto* self = static_cast<TtlSyncActivity*>(ctx);
    self->progressPercent_ = percent;
    self->requestUpdate(true);
  };
  callbacks.pump = [](void* ctx) {
    auto* self = static_cast<TtlSyncActivity*>(ctx);
    self->mappedInput.update(true);
    if (self->mappedInput.isPressed(MappedInputManager::Button::Back) ||
        self->mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      self->cancelRequested_ = true;
    }
    if (self->mappedInput.wasHomeGesture()) {
      self->cancelRequested_ = true;
      self->goHomeRequested_ = true;
    }
  };
  return callbacks;
}

void TtlSyncActivity::checkForUpdate() {
  {
    RenderLock lock(*this);
    screen_ = Screen::Busy;
    busyJob_ = BusyJob::Check;
    statusTitle_.clear();
    bookTotal_ = 0;
    cancelRequested_ = false;
    goHomeRequested_ = false;
    lastPaintMs_ = millis();
  }
  requestUpdateAndWait();

  const TtlOtaCheckResult result = ttlCheckFirmwareUpdate(otaCallbacks());
  // The pump callback sets goHomeRequested_ on the home gesture. cppcheck does
  // not follow that function pointer, so it treats the flag as still false.
  // cppcheck-suppress knownConditionTrueFalse
  if (goHomeRequested_) {
    onGoHome(HomeMenuItem::TTL_SYNC);
    return;
  }
  switch (result.kind) {
    case TtlOtaCheckKind::UpToDate:
      showNotice("Up to date", ttl::kVersionLabel);
      return;
    case TtlOtaCheckKind::Cancelled:
      showResult(false, true, "Stopped");
      return;
    case TtlOtaCheckKind::Failed:
      showNotice("Update failed", result.message.empty() ? "Network error" : result.message);
      return;
    case TtlOtaCheckKind::Available:
      break;
  }
  {
    RenderLock lock(*this);
    pending_ = result.manifest;
    offerVersion_ = ttl::ttlVersionLabel(pending_.version);
    offerSize_ = ttl::ttlFormatFirmwareSize(pending_.size);
    offerNotes_ = pending_.notes;
    screen_ = Screen::Offer;
  }
  requestUpdate();
}

void TtlSyncActivity::startInstall() {
  const ttl::TtlFirmwareManifest manifest = pending_;
  {
    RenderLock lock(*this);
    screen_ = Screen::Busy;
    busyJob_ = BusyJob::Install;
    statusTitle_.clear();
    bookTotal_ = 0;
    progressPercent_ = 0;
    cancelRequested_ = false;
    goHomeRequested_ = false;
    lastPaintMs_ = millis();
  }
  requestUpdateAndWait();

  const TtlOtaInstallResult result = ttlInstallFirmwareUpdate(manifest, otaCallbacks());
  // cppcheck-suppress knownConditionTrueFalse
  if (goHomeRequested_) {
    onGoHome(HomeMenuItem::TTL_SYNC);
    return;
  }
  switch (result.kind) {
    case TtlOtaInstallKind::Ok:
      showNotice("Update complete. Rebooting...", "");
      requestUpdateAndWait();
      delay(1500);
      ESP.restart();
      return;
    case TtlOtaInstallKind::Cancelled:
      showResult(false, true, "Stopped");
      return;
    case TtlOtaInstallKind::Failed:
      showNotice("Update failed", result.message.empty() ? "Could not flash" : result.message);
      return;
  }
}

void TtlSyncActivity::render(RenderLock&& lock) {
  switch (screen_) {
    case Screen::Menu:
      UiListActivity::render(std::move(lock));
      return;
    case Screen::Busy:
    case Screen::Offer:
    case Screen::Result:
      break;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int centerY = (pageHeight - lineHeight) / 2;
  const int side = metrics.contentSidePadding;
  renderer.clearScreen();
  drawChrome();

  if (screen_ == Screen::Busy) {
    const char* heading = "Syncing";
    switch (busyJob_) {
      case BusyJob::Sync:
        if (phase_ == TtlSyncPhase::Login) heading = "Signing in";
        if (phase_ == TtlSyncPhase::Manifest) heading = "Book list";
        break;
      case BusyJob::Check:
        heading = "Checking";
        break;
      case BusyJob::Install:
        heading = "Installing";
        break;
    }
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight * 2, heading, true, EpdFontFamily::BOLD);
    if (busyJob_ == BusyJob::Sync) {
      if (!statusTitle_.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, centerY, statusTitle_.c_str());
      } else if (bookTotal_ > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d / %d", index_, bookTotal_);
        renderer.drawCenteredText(UI_10_FONT_ID, centerY, buf);
      }
    }
    if (busyJob_ != BusyJob::Check) {
      int percent = 0;
      if (busyJob_ == BusyJob::Install) {
        percent = progressPercent_.load();
      } else {
        const size_t total = total_.load();
        if (total > 0) percent = static_cast<int>((bytes_.load() * 100) / total);
      }
      if (percent > 100) percent = 100;
      GUI.drawProgressBar(
          renderer,
          Rect{side, centerY + lineHeight + metrics.verticalSpacing, pageWidth - side * 2, metrics.progressBarHeight},
          percent, 100);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (screen_ == Screen::Offer) {
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Update available", true, EpdFontFamily::BOLD);
    y += lineHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, offerVersion_.c_str());
    y += lineHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, offerSize_.c_str());
    y += lineHeight + metrics.verticalSpacing;
    const int hintTop = pageHeight - metrics.buttonHintsHeight;
    if (!offerNotes_.empty() && y < hintTop) {
      const Rect notes{side, y, pageWidth - side * 2, hintTop - y};
      UITheme::drawCenteredWrappedText(renderer, notes, UI_10_FONT_ID, offerNotes_.c_str(), 3, true,
                                       EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Install", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (screen_ == Screen::Result) {
    const char* heading = "Failed";
    if (!resultHeading_.empty())
      heading = resultHeading_.c_str();
    else if (resultOk_)
      heading = "Done";
    else if (resultCancelled_)
      heading = "Stopped";
    const Rect head{side, centerY - lineHeight, pageWidth - side * 2, lineHeight * 2};
    UITheme::drawCenteredWrappedText(renderer, head, UI_10_FONT_ID, heading, 2, true, EpdFontFamily::BOLD,
                                     UITheme::TextVerticalAlignment::TOP);
    if (!resultMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, resultMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BACK), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
