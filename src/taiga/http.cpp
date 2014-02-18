/*
** Taiga
** Copyright (C) 2010-2014, Eren Okka
** 
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
** 
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
** 
** You should have received a copy of the GNU General Public License
** along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "base/logger.h"
#include "base/string.h"
#include "library/resource.h"
#include "sync/manager.h"
#include "taiga/announce.h"
#include "taiga/http.h"
#include "taiga/settings.h"
#include "taiga/stats.h"
#include "taiga/taiga.h"
#include "taiga/version.h"
#include "ui/ui.h"

taiga::HttpManager ConnectionManager;

namespace taiga {

HttpClient::HttpClient()
    : mode_(kHttpSilent) {
  // We will handle redirections ourselves
  set_auto_redirect(false);

  // The default header (e.g. "User-Agent: Taiga/1.0") will be used, unless
  // another value is specified in the request header
  set_user_agent(
      TAIGA_APP_NAME L"/" +
      ToWstr(TAIGA_VERSION_MAJOR) + L"." + ToWstr(TAIGA_VERSION_MINOR));

  // Make sure all new clients use the proxy settings
  set_proxy(Settings[kApp_Connection_ProxyHost],
            Settings[kApp_Connection_ProxyUsername],
            Settings[kApp_Connection_ProxyPassword]);
}

HttpClientMode HttpClient::mode() const {
  return mode_;
}

void HttpClient::set_mode(HttpClientMode mode) {
  mode_ = mode;
}

////////////////////////////////////////////////////////////////////////////////

void HttpClient::OnError(DWORD error) {
  std::wstring error_text = L"HTTP error #" + ToWstr(error) + L": " +
                            Logger::FormatError(error, L"winhttp.dll");
  TrimRight(error_text, L"\r\n");

  LOG(LevelError, error_text);
  LOG(LevelError, L"Connection mode: " + ToWstr(mode_));

  ui::OnHttpError(*this, error_text);

  Stats.connections_failed++;

  ConnectionManager.HandleError(response_, error_text);
}

bool HttpClient::OnHeadersAvailable() {
  ui::OnHttpHeadersAvailable(*this);
  return false;
}

bool HttpClient::OnRedirect(const std::wstring& address) {
  LOG(LevelDebug, L"Redirecting... (" + address + L")");

  switch (mode()) {
    case kHttpTaigaUpdateDownload: {
      std::wstring file = address.substr(address.find_last_of(L"/") + 1);
      download_path_ = GetPathOnly(download_path_) + file;
      Taiga.Updater.SetDownloadPath(download_path_);
      break;
    }
  }

  return false;
}

bool HttpClient::OnReadData() {
  ui::OnHttpProgress(*this);
  return false;
}

bool HttpClient::OnReadComplete() {
  ui::OnHttpReadComplete(*this);

  Stats.connections_succeeded++;

  ConnectionManager.HandleResponse(response_);

  return false;
}

////////////////////////////////////////////////////////////////////////////////

HttpClient& HttpManager::GetNewClient(const base::uuid_t& uuid) {
  return clients_[uuid];
}

void HttpManager::CancelRequest(base::uuid_t uuid) {
  if (clients_.count(uuid))
    clients_[uuid].Cleanup();
}

void HttpManager::MakeRequest(HttpRequest& request, HttpClientMode mode) {
  if (clients_.count(request.uuid)) {
    LOG(LevelWarning, L"HttpClient already exists. ID: " + request.uuid);
    clients_[request.uuid].Cleanup();
  }

  HttpClient& client = clients_[request.uuid];
  client.set_mode(mode);
  client.MakeRequest(request);
}

void HttpManager::MakeRequest(HttpClient& client, HttpRequest& request,
                              HttpClientMode mode) {
  client.set_mode(mode);
  client.MakeRequest(request);
}

void HttpManager::HandleError(HttpResponse& response, const string_t& error) {
  HttpClient& client = clients_[response.uuid];

  switch (client.mode()) {
    case kHttpServiceAuthenticateUser:
    case kHttpServiceGetMetadataById:
    case kHttpServiceSearchTitle:
    case kHttpServiceAddLibraryEntry:
    case kHttpServiceDeleteLibraryEntry:
    case kHttpServiceGetLibraryEntries:
    case kHttpServiceUpdateLibraryEntry:
      ServiceManager.HandleHttpError(client.response_, error);
      break;
  }
}

void HttpManager::HandleResponse(HttpResponse& response) {
  HttpClient& client = clients_[response.uuid];

  switch (client.mode()) {
    case kHttpServiceAuthenticateUser:
    case kHttpServiceGetMetadataById:
    case kHttpServiceSearchTitle:
    case kHttpServiceAddLibraryEntry:
    case kHttpServiceDeleteLibraryEntry:
    case kHttpServiceGetLibraryEntries:
    case kHttpServiceUpdateLibraryEntry:
      ServiceManager.HandleHttpResponse(response);
      break;

    case kHttpGetLibraryEntryImage: {
      int anime_id = static_cast<int>(response.parameter);
      if (ImageDatabase.Load(anime_id, true, false))
        ui::OnLibraryEntryImageChange(anime_id);
      break;
    }

    case kHttpFeedCheck:
    case kHttpFeedCheckAuto: {
      Feed* feed = reinterpret_cast<Feed*>(response.parameter);
      if (feed) {
        bool automatic = client.mode() == kHttpFeedCheckAuto;
        Aggregator.HandleFeedCheck(*feed, automatic);
      }
      break;
    }
    case kHttpFeedDownload:
    case kHttpFeedDownloadAll: {
      auto feed = reinterpret_cast<Feed*>(response.parameter);
      if (feed) {
        bool download_all = client.mode() == kHttpFeedDownloadAll;
        Aggregator.HandleFeedDownload(*feed, download_all);
      }
      break;
    }

    case kHttpTwitterRequest:
    case kHttpTwitterAuth:
    case kHttpTwitterPost:
      ::Twitter.HandleHttpResponse(client.mode(), response);
      break;

    case kHttpTaigaUpdateCheck:
      if (Taiga.Updater.ParseData(response.body))
        if (Taiga.Updater.IsDownloadAllowed())
          break;
      ui::OnUpdateFinished();
      break;
    case kHttpTaigaUpdateDownload:
      Taiga.Updater.RunInstaller();
      ui::OnUpdateFinished();
      break;
  }
}

////////////////////////////////////////////////////////////////////////////////

void HttpManager::FreeMemory() {
  for (auto it = clients_.cbegin(); it != clients_.cend(); ) {
    if (it->second.response().code > 0) {
      clients_.erase(it++);
    } else {
      ++it;
    }
  }
}

}  // namespace taiga