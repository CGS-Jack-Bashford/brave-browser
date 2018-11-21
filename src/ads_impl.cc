/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <fstream>
#include <algorithm>

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "ads_impl.h"
#include "bat/ads/ads_client.h"
#include "bat/ads/notification_info.h"
#include "logging.h"
#include "search_providers.h"
#include "math_helper.h"
#include "string_helper.h"
#include "locale_helper.h"
#include "time_helper.h"
#include "bat/ads/url_components.h"
#include "static_values.h"

using namespace std::placeholders;

namespace ads {

AdsImpl::AdsImpl(AdsClient* ads_client) :
    is_first_run_(true),
    is_initialized_(false),
    is_foreground_(false),
    last_shown_tab_url_(""),
    last_shown_notification_info_(NotificationInfo()),
    last_page_classification_(""),
    page_score_cache_({}),
    collect_activity_timer_id_(0),
    sustained_ad_interaction_timer_id_(0),
    media_playing_({}),
    next_easter_egg_(0),
    ads_client_(ads_client),
    client_(std::make_unique<Client>(this, ads_client_)),
    bundle_(std::make_unique<Bundle>(ads_client_)),
    ads_serve_(std::make_unique<AdsServe>(this, ads_client_, bundle_.get())) {
}

AdsImpl::~AdsImpl() = default;

void AdsImpl::GenerateAdReportingNotificationShownEvent(
    const NotificationInfo& info) {
  if (is_first_run_) {
    is_first_run_ = false;

    GenerateAdReportingRestartEvent();
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("notify");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("notificationType");
  writer.String("generated");

  writer.String("notificationClassification");
  writer.StartArray();
  std::vector<std::string> classifications;
  helper::String::Split(info.category, '-', &classifications);
  for (const auto& classification : classifications) {
    writer.String(classification.c_str());
  }
  writer.EndArray();

  writer.String("notificationCatalog");
  if (info.creative_set_id.empty()) {
    writer.String("sample-catalog");
  } else {
    writer.String(info.creative_set_id.c_str());
  }

  writer.String("notificationUrl");
  writer.String(info.url.c_str());

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

void AdsImpl::GenerateAdReportingNotificationResultEvent(
    const NotificationInfo& info,
    const NotificationResultInfoResultType type) {
  if (is_first_run_) {
    is_first_run_ = false;

    GenerateAdReportingRestartEvent();
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("notify");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("notificationType");
  switch (type) {
    case NotificationResultInfoResultType::CLICKED: {
      writer.String("clicked");
      client_->UpdateAdsUUIDSeen(info.uuid, 1);

      StartSustainingAdInteraction(kSustainAdInteractionAfterSeconds);
      break;
    }

    case NotificationResultInfoResultType::DISMISSED: {
      writer.String("dismissed");
      client_->UpdateAdsUUIDSeen(info.uuid, 1);
      break;
    }

    case NotificationResultInfoResultType::TIMEOUT: {
      writer.String("timeout");
      break;
    }
  }

  writer.String("notificationClassification");
  writer.StartArray();
  std::vector<std::string> classifications;
  helper::String::Split(info.category, '-', &classifications);
  for (const auto& classification : classifications) {
    writer.String(classification.c_str());
  }
  writer.EndArray();

  writer.String("notificationCatalog");
  if (info.creative_set_id.empty()) {
    writer.String("sample-catalog");
  } else {
    writer.String(info.creative_set_id.c_str());
  }

  writer.String("notificationUrl");
  writer.String(info.url.c_str());

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

void AdsImpl::Initialize() {
  if (IsInitialized()) {
    LOG(LogLevel::WARNING) << "Already initialized";
    return;
  }

  if (!ads_client_->IsAdsEnabled()) {
    LOG(LogLevel::INFO) << "Deinitializing as Ads are disabled";

    Deinitialize();
    return;
  }

  if (IsInitialized()) {
    return;
  }

  client_->LoadState();
}

void AdsImpl::InitializeStep2() {
  ProcessLocales(ads_client_->GetLocales());

  LoadUserModel();
}

void AdsImpl::InitializeStep3() {
  is_initialized_ = true;

  LOG(LogLevel::INFO) << "Successfully initialized";

  ads_client_->SetIdleThreshold(kIdleThresholdInSeconds);

  NotificationAllowedCheck(false);

  RetrieveSSID();

  ConfirmAdUUIDIfAdEnabled();

  ads_serve_->DownloadCatalog();
}

bool AdsImpl::IsInitialized() {
  if (!is_initialized_ ||
      !ads_client_->IsAdsEnabled() ||
      !user_model_->IsInitialized()) {
    return false;
  }

  return true;
}

void AdsImpl::OnForeground() {
  is_foreground_ = true;
  GenerateAdReportingForegroundEvent();
}

void AdsImpl::OnBackground() {
  is_foreground_ = false;
  GenerateAdReportingBackgroundEvent();
}

bool AdsImpl::IsForeground() const {
  return is_foreground_;
}

void AdsImpl::OnIdle() {
  // TODO(Terry Mancey): Implement Log (#44)
  // 'Idle state changed', { idleState: action.get('idleState') }
}

void AdsImpl::OnUnIdle() {
  // TODO(Terry Mancey): Implement Log (#44)
  // 'Idle state changed', { idleState: action.get('idleState') }

  client_->UpdateLastUserIdleStopTime();

  NotificationAllowedCheck(true);
}

void AdsImpl::OnMediaPlaying(const int32_t tab_id) {
  auto tab = media_playing_.find(tab_id);
  if (tab != media_playing_.end()) {
    // Media is already playing for this tab
    return;
  }

  media_playing_.insert({tab_id, true});
}

void AdsImpl::OnMediaStopped(const int32_t tab_id) {
  auto tab = media_playing_.find(tab_id);
  if (tab == media_playing_.end()) {
    // Media is not playing for this tab
    return;
  }

  media_playing_.erase(tab_id);
}

bool AdsImpl::IsMediaPlaying() const {
  if (media_playing_.empty()) {
    return false;
  }

  return true;
}

void AdsImpl::TabUpdated(
    const int32_t tab_id,
    const std::string& url,
    const bool is_active,
    const bool is_incognito) {
  if (is_incognito) {
    return;
  }

  client_->UpdateLastUserActivity();

  LoadInfo load_info;
  load_info.tab_id = tab_id;
  load_info.tab_url = url;
  GenerateAdReportingLoadEvent(load_info);

  if (is_active) {
    last_shown_tab_url_ = url;

    TestShoppingData(url);
    TestSearchState(url);

    FocusInfo focus_info;
    focus_info.tab_id = tab_id;
    GenerateAdReportingFocusEvent(focus_info);
  } else {
    BlurInfo blur_info;
    blur_info.tab_id = tab_id;
    GenerateAdReportingBlurEvent(blur_info);
  }
}

void AdsImpl::TabClosed(const int32_t tab_id) {
  OnMediaStopped(tab_id);

  DestroyInfo destroy_info;
  destroy_info.tab_id = tab_id;
  GenerateAdReportingDestroyEvent(destroy_info);
}

void AdsImpl::RemoveAllHistory() {
  client_->RemoveAllHistory();

  ConfirmAdUUIDIfAdEnabled();
}

void AdsImpl::SaveCachedInfo() {
  if (!ads_client_->IsAdsEnabled()) {
    client_->RemoveAllHistory();
  }

  client_->SaveState();
}

void AdsImpl::ClassifyPage(const std::string& url, const std::string& html) {
  if (!IsInitialized()) {
    return;
  }

  TestShoppingData(url);
  TestSearchState(url);

  auto page_score = user_model_->classifyPage(html);
  client_->AppendPageScoreToPageScoreHistory(page_score);

  last_page_classification_ = GetWinningCategory(page_score);

  // TODO(Terry Mancey): Implement Log (#44)
  // 'Site visited', { url, immediateWinner, winnerOverTime }
}

void AdsImpl::ChangeLocale(const std::string& locale) {
  if (!IsInitialized()) {
    return;
  }

  auto locales = ads_client_->GetLocales();

  if (std::find(locales.begin(), locales.end(), locale) != locales.end()) {
    client_->SetLocale(locale);
  } else {
    std::string closest_match_for_locale = "";

    std::vector<std::string> locale_components;
    helper::String::Split(locale, '_', &locale_components);

    auto language_code = locale_components.front();
    if (std::find(locales.begin(), locales.end(),
        language_code) != locales.end()) {
      closest_match_for_locale = language_code;
    } else {
      closest_match_for_locale = kDefaultLanguage;
    }

    client_->SetLocale(closest_match_for_locale);
  }

  LoadUserModel();
}

void AdsImpl::NotificationAllowedCheck(const bool serve) {
  auto ok = ads_client_->IsNotificationsAvailable();

  // TODO(Terry Mancey): Implement Log (#44)
  // appConstants.APP_ON_NATIVE_NOTIFICATION_AVAILABLE_CHECK, {err, result}

  auto previous = client_->GetAvailable();

  if (ok != previous) {
    client_->SetAvailable(ok);
  }

  if (!serve || ok != previous) {
    GenerateAdReportingSettingsEvent();
  }

  if (!serve) {
    return;
  }

  if (!ok) {
    // TODO(Terry Mancey): Implement Log (#44)
    // 'Ad not served', { reason: 'notifications not presently allowed' }

    return;
  }

  CheckReadyAdServe();
}

void AdsImpl::CheckReadyAdServe(const bool forced) {
  if (!IsInitialized()) {
    return;
  }

  if (!forced) {
    if (!IsForeground()) {
      // TODO(Terry Mancey): Implement Log (#44)
      // 'Notification not made', { reason: 'not in foreground' }
      return;
    }

    if (IsMediaPlaying()) {
      // TODO(Terry Mancey): Implement Log (#44)
      // 'Notification not made', { reason: 'media playing in browser' }
      return;
    }

    if (!IsAllowedToShowAds()) {
      // TODO(Terry Mancey): Implement Log (#44)
      // 'Notification not made', { reason: 'not allowed based on history' }
      return;
    }
  }

  auto category = GetWinnerOverTimeCategory();
  ServeAdFromCategory(category);
}

void AdsImpl::ServeSampleAd() {
  if (!IsInitialized()) {
    return;
  }

  auto callback = std::bind(&AdsImpl::OnLoadSampleBundle, this, _1, _2);
  ads_client_->LoadSampleBundle(callback);
}

void AdsImpl::StartCollectingActivity(const uint64_t start_timer_in) {
  StopCollectingActivity();

  collect_activity_timer_id_ = ads_client_->SetTimer(start_timer_in);
  if (collect_activity_timer_id_ == 0) {
    LOG(LogLevel::ERROR) <<
      "Failed to start collecting activity due to an invalid timer";
    return;
  }

  LOG(LogLevel::INFO) <<
    "Start collecting activity in " << start_timer_in << " seconds";
}

void AdsImpl::StopCollectingActivity() {
  if (!IsCollectingActivity()) {
    return;
  }

  LOG(LogLevel::INFO) << "Stopped collecting activity";

  ads_client_->KillTimer(collect_activity_timer_id_);
  collect_activity_timer_id_ = 0;
}

void AdsImpl::OnTimer(const uint32_t timer_id) {
  if (timer_id == collect_activity_timer_id_) {
    CollectActivity();
  } else if (timer_id == sustained_ad_interaction_timer_id_) {
    SustainAdInteraction();
  }
}

//////////////////////////////////////////////////////////////////////////////

void AdsImpl::Deinitialize() {
  if (!IsInitialized()) {
    LOG(LogLevel::WARNING) <<
      "Failed to deinitialize as not initialized";
    return;
  }

  ads_serve_->Reset();

  StopSustainingAdInteraction();

  RemoveAllHistory();

  bundle_->Reset();
  user_model_.reset();

  last_shown_notification_info_ = NotificationInfo();

  last_page_classification_ = "";
  page_score_cache_.clear();

  is_first_run_ = true;
  is_initialized_ = false;
  is_foreground_ = false;
}

void AdsImpl::LoadUserModel() {
  auto locale = client_->GetLocale();
  auto callback = std::bind(&AdsImpl::OnUserModelLoaded, this, _1, _2);
  ads_client_->LoadUserModelForLocale(locale, callback);
}

void AdsImpl::OnUserModelLoaded(const Result result, const std::string& json) {
  if (result == Result::FAILED) {
    LOG(LogLevel::ERROR) << "Failed to load user model";

    // TODO(Terry Mancey): If the user model fails to load, we need to notify
    // the Client to decide what action to take otherwise ads will not work
    return;
  }

  LOG(LogLevel::INFO) << "Successfully loaded user model";

  InitializeUserModel(json);

  if (!IsInitialized()) {
    InitializeStep3();
  }
}

void AdsImpl::InitializeUserModel(const std::string& json) {
  LOG(LogLevel::INFO) << "Initializing user model";

  user_model_.reset(usermodel::UserModel::CreateInstance());
  user_model_->initializePageClassifier(json);
}

std::string AdsImpl::GetWinningCategory(const std::vector<double>& page_score) {
  auto category = user_model_->winningCategory(page_score);
  return category;
}

std::string AdsImpl::GetWinnerOverTimeCategory() {
  auto page_score_history = client_->GetPageScoreHistory();
  if (page_score_history.size() == 0) {
    return "";
  }

  uint64_t count = page_score_history.front().size();

  std::vector<double> winner_over_time_page_scores(count);
  std::fill(winner_over_time_page_scores.begin(),
    winner_over_time_page_scores.end(), 0);

  for (const auto& page_scores : page_score_history) {
    if (page_scores.size() != count) {
      return "";
    }

    for (size_t i = 0; i < page_scores.size(); i++) {
      winner_over_time_page_scores[i] += page_scores[i];
    }
  }

  auto category = user_model_->winningCategory(winner_over_time_page_scores);

  // TODO(Terry Mancey): Implement Log (#44)
  // 'Site visited', { url, immediateWinner, winnerOverTime }
  return category;
}

void AdsImpl::CachePageScore(
    const std::string& url,
    const std::vector<double>& page_score) {
  auto cached_page_score = page_score_cache_.find(url);

  if (cached_page_score == page_score_cache_.end()) {
    page_score_cache_.insert({url, page_score});
  } else {
    cached_page_score->second = page_score;
  }
}

void AdsImpl::OnGetAds(
    const Result result,
    const std::string& region,
    const std::string& category,
    const std::vector<AdInfo>& ads) {
  if (result == Result::FAILED) {
    auto pos = category.find_last_of('-');
    if (pos != std::string::npos) {
      std::string new_category = category.substr(0, pos);

      LOG(LogLevel::WARNING) << "No ads found for \""
        << category << "\" category, trying again with \"" << new_category <<
        "\" category";

      auto callback = std::bind(&AdsImpl::OnGetAds, this, _1, _2, _3, _4);
      ads_client_->GetAds(region, new_category, callback);

      return;
    }

    if (ads.empty()) {
      // TODO(Terry Mancey): Implement Log (#44)
      // 'Notification not made', { reason: 'no ads for category', category }

      LOG(LogLevel::WARNING) << "No ads found for \""
        << category << "\" category";

      return;
    }
  }

  auto ads_unseen = GetUnseenAds(ads);
  if (ads_unseen.empty()) {
    // TODO(Terry Mancey): Implement Log (#44)
    // 'Ad round-robin', { category, adsSeen, adsNotSeen }

    client_->ResetAdsUUIDSeen(ads);

    ads_unseen = GetUnseenAds(ads);
    if (ads_unseen.empty()) {
      // TODO(Terry Mancey): Implement Log (#44)
      // 'Notification not made', { reason: 'no ads for category', category }

      return;
    }
  }

  auto rand = helper::Math::Random(ads_unseen.size() - 1);
  auto ad = ads_unseen.at(rand);
  ShowAd(ad, category);
}

void AdsImpl::OnLoadSampleBundle(
    const Result result,
    const std::string& json) {
  if (result == Result::FAILED) {
    LOG(LogLevel::ERROR) << "Failed to load sample bundle";
    return;
  }

  BundleState sample_bundle_state;
  if (!sample_bundle_state.LoadFromJson(json,
      ads_client_->LoadJsonSchema(_bundle_schema_name))) {
    LOG(LogLevel::ERROR) << "Failed to parse sample bundle: " << json;
    return;
  }

  // TODO(Terry Mancey): Sample bundle state should be persisted on the Client
  // in a database so that sample ads can be fetched from the database rather
  // than parsing the JSON each time, and be consistent with GetAds, therefore
  // the below code should be abstracted into GetAdForSampleCategory once the
  // necessary changes have been made in Brave Core by Brian Johnson

  auto categories = sample_bundle_state.categories.begin();
  auto categories_count = sample_bundle_state.categories.size();
  if (categories_count == 0) {
    // TODO(Terry Mancey): Implement Log (#44)
    // 'Notification not made', { reason: 'no categories' }
    LOG(LogLevel::WARNING) << "Sample bundle does not contain any categories";

    return;
  }

  auto category_rand = helper::Math::Random(categories_count - 1);
  std::advance(categories, static_cast<long>(category_rand));

  auto category = categories->first;
  auto ads = categories->second;

  auto ads_count = ads.size();
  if (ads_count == 0) {
    // TODO(Terry Mancey): Implement Log (#44)
    // 'Notification not made', { reason: 'no ads for category', category }
    LOG(LogLevel::WARNING) << "No ads found for \""
      << category << "\" sample category";

    return;
  }

  auto ad_rand = helper::Math::Random(ads_count - 1);
  auto ad = ads.at(ad_rand);

  ShowAd(ad, category);
}

void AdsImpl::CollectActivity() {
  if (!IsInitialized()) {
    return;
  }

  LOG(LogLevel::INFO) << "Collect activity";

  ads_serve_->DownloadCatalog();
}

bool AdsImpl::IsCollectingActivity() const {
  if (collect_activity_timer_id_ == 0) {
    return false;
  }

  return true;
}

void AdsImpl::StartSustainingAdInteraction(const uint64_t start_timer_in) {
  StopSustainingAdInteraction();

  sustained_ad_interaction_timer_id_ = ads_client_->SetTimer(start_timer_in);
  if (sustained_ad_interaction_timer_id_ == 0) {
    LOG(LogLevel::ERROR) <<
      "Failed to start sustaining ad interaction due to an invalid timer";
    return;
  }

  LOG(LogLevel::INFO) <<
    "Start sustaining ad interaction in " << start_timer_in << " seconds";
}

void AdsImpl::SustainAdInteraction() {
  if (!IsStillViewingAd()) {
    return;
  }

  GenerateAdReportingSustainEvent(last_shown_notification_info_);

  StartSustainingAdInteraction(kSustainAdInteractionAfterSeconds);
}

bool AdsImpl::IsStillViewingAd() const {
  if (last_shown_notification_info_.url != last_shown_tab_url_) {
    return false;
  }

  return true;
}

void AdsImpl::StopSustainingAdInteraction() {
  if (!IsSustainingAdInteraction()) {
    return;
  }

  LOG(LogLevel::INFO) << "Stopped sustaining ad interaction";

  ads_client_->KillTimer(sustained_ad_interaction_timer_id_);
  sustained_ad_interaction_timer_id_ = 0;
}

bool AdsImpl::IsSustainingAdInteraction() const {
  if (sustained_ad_interaction_timer_id_ == 0) {
    return false;
  }

  return true;
}

void AdsImpl::ConfirmAdUUIDIfAdEnabled() {
  if (!ads_client_->IsAdsEnabled()) {
    StopCollectingActivity();
    return;
  }

  client_->UpdateAdUUID();

  if (_is_debug) {
    StartCollectingActivity(kDebugOneHourInSeconds);
  } else {
    StartCollectingActivity(kOneHourInSeconds);
  }
}

void AdsImpl::RetrieveSSID() {
  std::string ssid = ads_client_->GetSSID();
  if (ssid.empty()) {
    ssid = kUnknownSSID;
  }

  client_->SetCurrentSSID(ssid);
}

void AdsImpl::TestShoppingData(const std::string& url) {
  if (!IsInitialized()) {
    return;
  }


  UrlComponents components;
  if (!ads_client_->GetUrlComponents(url, &components)) {
    return;
  }

  // TODO(Terry Mancey): Confirm with product if this list should be expanded
  // to include amazon.co.uk and other territories
  if (components.hostname == "www.amazon.com") {
    client_->FlagShoppingState(url, 1.0);
  } else {
    client_->UnflagShoppingState();
  }
}

void AdsImpl::TestSearchState(const std::string& url) {
  if (!IsInitialized()) {
    return;
  }

  UrlComponents components;
  if (!ads_client_->GetUrlComponents(url, &components)) {
    return;
  }

  if (SearchProviders::IsSearchEngine(components)) {
    client_->FlagSearchState(url, 1.0);
  } else {
    client_->UnflagSearchState(url);
  }
}

void AdsImpl::ProcessLocales(const std::vector<std::string>& locales) {
  if (locales.empty()) {
    return;
  }

  client_->SetLocales(locales);
}

void AdsImpl::ServeAdFromCategory(const std::string& category) {
  std::string catalog_id = bundle_->GetCatalogId();
  if (catalog_id.empty()) {
    // TODO(Terry Mancey): Implement Log (#44)
    // 'Notification not made', { reason: 'no ad catalog' }
    return;
  }

  if (category.empty()) {
    // TODO(Terry Mancey): Implement Log (#44)
    // 'Notification not made', { reason: 'no ad (or permitted ad) for
    // winnerOverTime', category, winnerOverTime, arbitraryKey }
    return;
  }

  auto locale = ads_client_->GetAdsLocale();
  auto region = helper::Locale::GetCountryCode(locale);

  auto callback = std::bind(&AdsImpl::OnGetAds, this, _1, _2, _3, _4);
  ads_client_->GetAds(region, category, callback);
}

std::vector<AdInfo> AdsImpl::GetUnseenAds(
    const std::vector<AdInfo>& ads) {
  auto ads_unseen = ads;
  auto ads_seen = client_->GetAdsUUIDSeen();

  auto iterator = std::remove_if(
    ads_unseen.begin(),
    ads_unseen.end(),
    [&](AdInfo &info) {
      return ads_seen.find(info.uuid) != ads_seen.end();
    });

  ads_unseen.erase(iterator, ads_unseen.end());

  return ads_unseen;
}

bool AdsImpl::IsAllowedToShowAds() {
  auto hour_window = kOneHourInSeconds;
  auto hour_allowed = ads_client_->GetAdsPerHour();
  auto respects_hour_limit = AdsShownHistoryRespectsRollingTimeConstraint(
    hour_window, hour_allowed);

  auto day_window = kOneHourInSeconds;
  auto day_allowed = ads_client_->GetAdsPerDay();
  auto respects_day_limit = AdsShownHistoryRespectsRollingTimeConstraint(
    day_window, day_allowed);

  auto minimum_wait_time = hour_window / hour_allowed;
  bool respects_minimum_wait_time =
    AdsShownHistoryRespectsRollingTimeConstraint(
    minimum_wait_time, 0);

  return respects_hour_limit &&
    respects_day_limit &&
    respects_minimum_wait_time;
}

bool AdsImpl::IsAdValid(const AdInfo& ad_info) {
  if (ad_info.advertiser.empty() ||
      ad_info.notification_text.empty() ||
      ad_info.notification_url.empty()) {
    // TODO(Terry Mancey): Implement Log (#44)
    // 'Notification not made', { reason: 'incomplete ad information',
    // category, winnerOverTime, arbitraryKey, notificationUrl,
    // notificationText, advertiser
    return false;
  }

  return true;
}

bool AdsImpl::ShowAd(
    const AdInfo& ad_info,
    const std::string& category) {
  if (!IsAdValid(ad_info)) {
    return false;
  }

  auto notification_info = std::make_unique<NotificationInfo>();
  notification_info->advertiser = ad_info.advertiser;
  notification_info->category = category;
  notification_info->text = ad_info.notification_text;
  notification_info->url = ad_info.notification_url;
  notification_info->creative_set_id = ad_info.creative_set_id;
  notification_info->uuid = ad_info.uuid;

  last_shown_notification_info_ = NotificationInfo(*notification_info);

  ads_client_->ShowNotification(std::move(notification_info));

  // TODO(Terry Mancey): Implement Log (#44)
  // 'Notification shown', {category, winnerOverTime, arbitraryKey,
  // notificationUrl, notificationText, advertiser, uuid, hierarchy}

  client_->AppendCurrentTimeToAdsShownHistory();

  return true;
}

bool AdsImpl::AdsShownHistoryRespectsRollingTimeConstraint(
    const uint64_t seconds_window,
    const uint64_t allowable_ad_count) const {
  auto ads_shown_history = client_->GetAdsShownHistory();

  uint64_t recent_count = 0;
  auto now = helper::Time::Now();

  for (auto ad_shown : ads_shown_history) {
    if (now - ad_shown < seconds_window) {
      recent_count++;
    }
  }

  if (recent_count <= allowable_ad_count) {
    return true;
  }

  return false;
}

void AdsImpl::GenerateAdReportingSustainEvent(
    const NotificationInfo& info) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("sustain");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("notificationId");
  writer.String(info.uuid.c_str());

  writer.String("notificationType");
  writer.String("viewed");

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

void AdsImpl::GenerateAdReportingLoadEvent(
    const LoadInfo& info) {
  UrlComponents components;

  if (ads_client_->GetUrlComponents(info.tab_url, &components) ||
      (components.scheme != "http" && components.scheme != "https")) {
    return;
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("load");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("tabId");
  writer.Int(info.tab_id);

  writer.String("tabType");
  if (client_->GetSearchState()) {
    writer.String("search");
  } else {
    writer.String("click");
  }

  writer.String("tabUrl");
  writer.String(info.tab_url.c_str());

  writer.String("tabClassification");
  writer.StartArray();
  std::vector<std::string> classifications;
  helper::String::Split(last_page_classification_, '-', &classifications);
  for (const auto& classification : classifications) {
    writer.String(classification.c_str());
  }
  writer.EndArray();

  auto cached_page_score = page_score_cache_.find(info.tab_url);
  if (cached_page_score != page_score_cache_.end()) {
    writer.String("pageScore");
    writer.StartArray();
    for (const auto& page_score : cached_page_score->second) {
      writer.Double(page_score);
    }
    writer.EndArray();
  }

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);

  auto now = helper::Time::Now();
  if (_is_testing && info.tab_url == "https://www.iab.com/"
      && next_easter_egg_ < now) {
    next_easter_egg_ = now + kNextEasterEggStartsInSeconds;

    CheckReadyAdServe(true);
  }
}

void AdsImpl::GenerateAdReportingBackgroundEvent() {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("background");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("place");
  auto place = client_->GetCurrentPlace();
  writer.String(place.c_str());

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

void AdsImpl::GenerateAdReportingForegroundEvent() {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("foreground");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("place");
  auto place = client_->GetCurrentPlace();
  writer.String(place.c_str());

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

void AdsImpl::GenerateAdReportingBlurEvent(
    const BlurInfo& info) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("blur");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("tabId");
  writer.Int(info.tab_id);

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

void AdsImpl::GenerateAdReportingDestroyEvent(
    const DestroyInfo& info) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("destroy");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("tabId");
  writer.Int(info.tab_id);

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

void AdsImpl::GenerateAdReportingFocusEvent(
    const FocusInfo& info) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("focus");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("tabId");
  writer.Int(info.tab_id);

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

void AdsImpl::GenerateAdReportingRestartEvent() {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("restart");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("place");
  auto place = client_->GetCurrentPlace();
  writer.String(place.c_str());

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

void AdsImpl::GenerateAdReportingSettingsEvent() {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  writer.String("data");
  writer.StartObject();

  writer.String("type");
  writer.String("settings");

  writer.String("stamp");
  std::string time_stamp = helper::Time::TimeStamp();
  writer.String(time_stamp.c_str());

  writer.String("settings");
  writer.StartObject();

  writer.String("notifications");
  writer.StartObject();

  writer.String("available");
  auto configured = ads_client_->IsNotificationsAvailable();
  writer.Bool(configured);

  writer.EndObject();

  writer.String("place");
  auto place = client_->GetCurrentPlace();
  writer.String(place.c_str());

  writer.String("locale");
  auto locale = client_->GetLocale();
  writer.String(locale.c_str());

  writer.String("adsPerDay");
  auto ads_per_day = ads_client_->GetAdsPerDay();
  writer.Uint64(ads_per_day);

  writer.String("adsPerHour");
  auto ads_per_hour = ads_client_->GetAdsPerHour();
  writer.Uint64(ads_per_hour);

  writer.EndObject();

  writer.EndObject();

  auto json = buffer.GetString();
  ads_client_->EventLog(json);
}

}  // namespace ads
