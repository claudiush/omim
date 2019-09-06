#pragma once

#include "platform/http_request.hpp"
#include "platform/safe_callback.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace storage
{
// This interface encapsulates HTTP routines for receiving servers
// URLs and downloading a single map file.
class MapFilesDownloader
{
public:
  // Denotes bytes downloaded and total number of bytes.
  using Progress = std::pair<int64_t, int64_t>;
  using ServersList = std::vector<std::string>;

  using FileDownloadedCallback =
      std::function<void(downloader::HttpRequest::Status status, Progress const & progress)>;
  using DownloadingProgressCallback = std::function<void(Progress const & progress)>;
  using ServersListCallback = platform::SafeCallback<void(ServersList const & serverList)>;

  virtual ~MapFilesDownloader() = default;

  /// Asynchronously downloads a map file, periodically invokes
  /// onProgress callback and finally invokes onDownloaded
  /// callback. Both callbacks will be invoked on the main thread.
  void DownloadMapFile(std::string const & relativeUrl, std::string const & path, int64_t size,
                       FileDownloadedCallback const & onDownloaded,
                       DownloadingProgressCallback const & onProgress);

  /// Returns current downloading progress.
  virtual Progress GetDownloadingProgress() = 0;

  /// Returns true when downloader does not perform any job.
  virtual bool IsIdle() = 0;

  /// Resets downloader to the idle state.
  virtual void Reset() = 0;

  static std::string MakeRelativeUrl(std::string const & fileName, int64_t dataVersion,
                                     uint64_t diffVersion);
  static std::string MakeFullUrlLegacy(string const & baseUrl, string const & fileName,
                                       int64_t dataVersion);

  void SetServersList(ServersList const & serversList);

protected:
  // Synchronously loads list of servers by http client.
  static ServersList LoadServersList();

private:
  /// Default implementation asynchronously receives a list of all servers that can be asked
  /// for a map file and invokes callback on the main thread.
  virtual void GetServersList(ServersListCallback const & callback);
  /// This method must be implemented to asynchronous downloading file
  /// from provided |urls| and saving result into |path|.
  virtual void Download(std::vector<std::string> const & urls, std::string const & path,
                        int64_t size, FileDownloadedCallback const & onDownloaded,
                        DownloadingProgressCallback const & onProgress) = 0;

  ServersList m_serverList;
};
}  // namespace storage
