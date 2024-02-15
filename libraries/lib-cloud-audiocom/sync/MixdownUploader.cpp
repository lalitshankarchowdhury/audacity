/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  MixdownUploader.cpp

  Dmitry Vedenko

**********************************************************************/

#include "MixdownUploader.h"

#include <rapidjson/document.h>

#include "WaveTrack.h"

#include "CodeConversions.h"

#include "ServiceConfig.h"

#include "Export.h"
#include "ExportPluginRegistry.h"

#include "ProjectRate.h"

#include "DataUploader.h"
#include "UploadService.h"

#include "BasicUI.h"

namespace cloud::audiocom::sync
{
namespace
{
std::string GenerateTempPath(FileExtension extension)
{
   const auto tempPath = GetUploadTempPath();

   wxFileName fileName(
      tempPath,
      wxString::Format(
         "%lld", std::chrono::system_clock::now().time_since_epoch().count()),
      extension);

   fileName.Mkdir(0700, wxPATH_MKDIR_FULL);

   if (fileName.Exists())
   {
      if (!wxRemoveFile(fileName.GetFullPath()))
         return {};
   }

   return audacity::ToUTF8(fileName.GetFullPath());
}

int CalculateChannels(const TrackList& trackList)
{
   auto range = trackList.Any<const WaveTrack>();
   return std::all_of(
             range.begin(), range.end(),
             [](const WaveTrack* track)
             { return IsMono(*track) && track->GetPan() == 0; }) ?
             1 :
             2;
}
} // namespace

class MixdownUploader::DataExporter final : public ExportProcessorDelegate
{
public:
   DataExporter(MixdownUploader& parent, ExportTask task)
       : mParent { parent }
       , mTask { std::move(task) }
       , mExportThread { [this] { ExportTread(); } }
   {
   }

   ~DataExporter() override
   {
      mExportThread.join();
   }

   void Cancel()
   {
      mCancelled.store(true, std::memory_order_release);
   }

   ExportResult GetResult() const
   {
      return mResult;
   }

   void OnComplete(ExportResult result)
   {
      mResult = result;

      if (result == ExportResult::Success)
      {
         mParent.UploadMixdown();
      }
      else
      {
         mParent.ReportProgress(
            result == ExportResult::Error ? MixdownState::Failed :
                                            MixdownState::Cancelled,
            1.0, {});
      }
   }

   void SetStatusString(const TranslatableString& str) override
   {
   }

   bool IsCancelled() const override
   {
      return mCancelled.load(std::memory_order_acquire);
   }

   bool IsStopped() const override
   {
      return false;
   }

   void OnProgress(double value) override
   {
      mParent.ReportProgress(MixdownState::Exporting, value, {});
   }

   void ExportTread()
   {
      try
      {
         auto future = mTask.get_future();
         mTask(*this);
         const auto result = future.get();

         BasicUI::CallAfter([this, result] { OnComplete(result); });
      }
      catch (const ExportDiskFullError& error)
      {
         HandleExportDiskFullError(error);
      }
      catch (const ExportErrorException& error)
      {
         HandleExportError(error);
      }
      catch (const ExportException& error)
      {
         HandleExportException(error);
      }
      catch (...)
      {
         HandleUnkonwnException();
      }
   }

   void HandleExportDiskFullError(const ExportDiskFullError& error)
   {
      BasicUI::CallAfter(
         [this, fileName = error.GetFileName()]
         {
            ShowDiskFullExportErrorDialog(fileName);
            mParent.ReportProgress(MixdownState::Failed, 1.0, {});
         });
   }

   void HandleExportError(const ExportErrorException& error)
   {
      BasicUI::CallAfter(
         [this, message = error.GetMessage(), helpPage = error.GetHelpPageId()]
         {
            ShowExportErrorDialog(message, XO("Export failed"), helpPage, true);
            mParent.ReportProgress(MixdownState::Failed, 1.0, {});
         });
   }

   void HandleExportException(const ExportException& error)
   {
      BasicUI::CallAfter(
         [this, message = error.What()]
         {
            ShowExportErrorDialog(Verbatim(message), XO("Export failed"), true);
            mParent.ReportProgress(MixdownState::Failed, 1.0, {});
         });
   }

   void HandleUnkonwnException()
   {
      BasicUI::CallAfter([] { BasicUI::ShowMessageBox(XO("Export error")); });
   }

private:
   MixdownUploader& mParent;
   ExportTask mTask;
   std::thread mExportThread;

   std::atomic<bool> mCancelled { false };
   ExportResult mResult { ExportResult::Stopped };
};

MixdownUploader::MixdownUploader(
   Tag, const ServiceConfig& config, const AudacityProject& project,
   MixdownProgressCallback progressCallback)
    : mServiceConfig { config }
    , mProject { project }
    , mProgressCallback { std::move(progressCallback) }
{
   ExportProject();
}

MixdownUploader::~MixdownUploader()
{
   if (wxFileExists(mExportedFilePath))
      wxRemoveFile(mExportedFilePath);
}

std::shared_ptr<MixdownUploader> MixdownUploader::Upload(
   const ServiceConfig& config, const AudacityProject& project,
   MixdownProgressCallback progressCallback)
{
   if (!progressCallback)
      progressCallback = [](auto...) { return true; };

   return std::make_shared<MixdownUploader>(
      Tag {}, config, project, std::move(progressCallback));
}

void MixdownUploader::SetUrls(const UploadUrls& urls)
{
   auto lock = std::lock_guard { mUploadUrlsMutex };

   assert(!mUploadUrls);
   mUploadUrls = urls;

   mUploadUrlsSet.notify_one();
}

void MixdownUploader::Cancel()
{
   if (!mDataExporter)
      return;

   // To be on a safe side, we cancel both operations
   mDataExporter->Cancel();
   mUploadCancelled.store(true, std::memory_order_release);
   // And ensure that WaitingForUrls is interrupted too
   mUploadUrlsSet.notify_all();
}

std::future<MixdownResult> MixdownUploader::GetResultFuture()
{
   return mPromise.get_future();
}

void MixdownUploader::ReportProgress(
   MixdownState state, double progress, ResponseResult uploadResult)
{
   mProgress.store(progress);

   if (BasicUI::IsUiThread())
   {
      mProgressUpdateQueued = false;
      if (!mProgressCallback(progress))
         Cancel();
   }
   else if (!mProgressUpdateQueued)
   {
      mProgressUpdateQueued = true;

      BasicUI::CallAfter(
         [weakThis = weak_from_this(), this]
         {
            auto lock = weakThis.lock();

            if (!lock)
               return;

            if (mFinished.load())
               return;

            if (!mProgressCallback(mProgress.load()))
               Cancel();

            mProgressUpdateQueued = false;
         });
   }

   if (state == MixdownState::Succeeded || state == MixdownState::Failed ||
       state == MixdownState::Cancelled)
   {
      mFinished.store(true);
      mPromise.set_value({ state, uploadResult });
   }
}

void MixdownUploader::ExportProject()
{
   auto& tracks = TrackList::Get(mProject);

   const double t0 = 0.0;
   const double t1 = tracks.GetEndTime();

   const int nChannels = CalculateChannels(tracks);

   auto hasMimeType = [](const auto&& mimeTypes, const std::string& mimeType)
   {
      return std::find(mimeTypes.begin(), mimeTypes.end(), mimeType) !=
             mimeTypes.end();
   };

   const auto& registry = ExportPluginRegistry::Get();

   for (const auto& preferredMimeType :
        GetServiceConfig().GetPreferredAudioFormats(false))
   {
      auto config = GetServiceConfig().GetExportConfig(preferredMimeType);

      ExportProcessor::Parameters parameters;
      auto pluginIt = std::find_if(
         registry.begin(), registry.end(),
         [&](auto t)
         {
            auto [plugin, formatIndex] = t;
            parameters.clear();
            return hasMimeType(
                      plugin->GetMimeTypes(formatIndex), preferredMimeType) &&
                   plugin->ParseConfig(formatIndex, config, parameters);
         });

      if (pluginIt == registry.end())
         continue;

      const auto [plugin, formatIndex] = *pluginIt;

      const auto formatInfo = plugin->GetFormatInfo(formatIndex);
      const auto path       = GenerateTempPath(formatInfo.extensions[0]);

      if (path.empty())
         continue;

      auto builder = ExportTaskBuilder {}
                        .SetParameters(parameters)
                        .SetNumChannels(nChannels)
                        .SetSampleRate(ProjectRate::Get(mProject).GetRate())
                        .SetPlugin(plugin)
                        .SetFileName(audacity::ToWXString(path))
                        .SetRange(t0, t1, false);

      mExportedFilePath = path;

      mDataExporter = std::make_unique<DataExporter>(
         *this, builder.Build(const_cast<AudacityProject&>(mProject)));

      return;
   }

   if (!mDataExporter)
   {
      mFinished.store(true);
      mPromise.set_value({ MixdownState::Failed });
   }
}

void MixdownUploader::UploadMixdown()
{
   ReportProgress(MixdownState::WaitingForUrls, 0.0, {});

   {
      auto lock = std::unique_lock { mUploadUrlsMutex };
      mUploadUrlsSet.wait(
         lock, [this] { return mUploadCancelled.load() || !!mUploadUrls; });
   }

   if (mUploadCancelled.load(std::memory_order_acquire))
   {
      ReportProgress(MixdownState::Cancelled, 0.0, {});
      return;
   }

   ReportProgress(MixdownState::Uploading, 0.0, {});

   DataUploader::Get().Upload(
      mServiceConfig, *mUploadUrls, mExportedFilePath,
      [this, strongThis = shared_from_this()](ResponseResult result)
      {
         const auto state = [code = result.Code]
         {
            if (code == ResponseResultCode::Success)
               return MixdownState::Succeeded;
            else if (code == ResponseResultCode::Cancelled)
               return MixdownState::Cancelled;
            else
               return MixdownState::Failed;
         }();

         ReportProgress(state, 1.0, result);
      },
      [this, strongThis = shared_from_this()](double progress)
      {
         ReportProgress(MixdownState::Uploading, progress, {});
         return !mUploadCancelled.load(std::memory_order_acquire);
      });
}
} // namespace cloud::audiocom::sync
