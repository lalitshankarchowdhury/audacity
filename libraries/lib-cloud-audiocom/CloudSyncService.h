/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  CloudSyncService.h

  Dmitry Vedenko

**********************************************************************/
#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "NetworkUtils.h"

class AudacityProject;

namespace cloud::audiocom
{
namespace sync
{
class LocalProjectSnapshot;
class PaginatedProjectsResponse;
class ProjectInfo;
class SnapshotInfo;
class RemoteProjectSnapshot;

struct ProjectSyncResult final
{
   enum class StatusCode
   {
      Succeeded,
      Blocked,
      Failed,
   };

   StatusCode Status {};
   ResponseResult Result;
   std::string ProjectPath;
}; // struct ProjectSyncResult

using ProgressCallback = std::function<bool(double)>;

using GetProjectsResult =
   std::variant<sync::PaginatedProjectsResponse, ResponseResult>;
} // namespace sync

//! CloudSyncService is responsible for saving and loading projects from the
//! cloud
class CLOUD_AUDIOCOM_API CloudSyncService final
{
   CloudSyncService()  = default;
   ~CloudSyncService() = default;

   CloudSyncService(const CloudSyncService&)            = delete;
   CloudSyncService(CloudSyncService&&)                 = delete;
   CloudSyncService& operator=(const CloudSyncService&) = delete;
   CloudSyncService& operator=(CloudSyncService&&)      = delete;

public:
   static CloudSyncService& Get();

   using SyncPromise = std::promise<sync::ProjectSyncResult>;
   using SyncFuture  = std::future<sync::ProjectSyncResult>;

   using GetProjectsPromise = std::promise<sync::GetProjectsResult>;
   using GetProjectsFuture  = std::future<sync::GetProjectsResult>;

   enum class SyncMode
   {
      Normal,
      ForceOverwrite,
      ForceNew
   };

   //! Retrieve the list of projects from the cloud
   GetProjectsFuture GetProjects(
      std::shared_ptr<CancellationContext> context, int page, int pageSize,
      std::string searchString);

   //! Open the project from the cloud. This operation is asynchronous.
   [[nodiscard]] SyncFuture OpenFromCloud(
      std::string projectId, std::string snapshotId, SyncMode mode,
      sync::ProgressCallback callback);

   [[nodiscard]] SyncFuture SyncProject(
      AudacityProject& project, const std::string& path, bool forceSync,
      sync::ProgressCallback callback);

   static bool IsCloudProject(const std::string& path);

private:
   void FailSync(ResponseResult responseResult);

   void CompleteSync(std::string path);

   void CompleteSync(sync::ProjectSyncResult result);

   void SyncCloudSnapshot(
      const sync::ProjectInfo& projectInfo,
      const sync::SnapshotInfo& snapshotInfo, SyncMode mode);

   void UpdateDowloadProgress(double downloadProgress);

   std::vector<std::shared_ptr<sync::LocalProjectSnapshot>> mLocalSnapshots;
   std::shared_ptr<sync::RemoteProjectSnapshot> mRemoteSnapshot;

   SyncPromise mSyncPromise;
   sync::ProgressCallback mProgressCallback;

   std::atomic<double> mDownloadProgress { 0.0 };
   std::atomic<bool> mProgressUpdateQueued { false };

   std::atomic<bool> mSyncInProcess { false };
};
} // namespace cloud::audiocom
