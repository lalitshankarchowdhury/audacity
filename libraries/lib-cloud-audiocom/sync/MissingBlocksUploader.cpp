/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  MissingBlocksUploader.cpp

  Dmitry Vedenko

**********************************************************************/

#include "MissingBlocksUploader.h"

#include "DataUploader.h"

#include "WavPackCompressor.h"

namespace cloud::audiocom::sync
{

MissingBlocksUploader::MissingBlocksUploader(
   const ServiceConfig& serviceConfig, std::vector<BlockUploadTask> uploadTasks,
   MissingBlocksUploadProgressCallback progress)
    : mServiceConfig { serviceConfig }
    , mUploadTasks { std::move(uploadTasks) }
    , mProgressCallback { std::move(progress) }
{
   mProgressData.TotalBlocks = mUploadTasks.size();

   for (auto& thread : mProducerThread)
      thread = std::thread([this] { ProducerThread(); });

   mConsumerThread = std::thread([this] { ConsumerThread(); });

   if (!mProgressCallback)
      mProgressCallback = [](auto...) {};
}

MissingBlocksUploader::~MissingBlocksUploader()
{
   mIsRunning.store(false, std::memory_order_release);

   mRingBufferNotEmpty.notify_all();
   mRingBufferNotFull.notify_all();
   mUploadsNotFull.notify_all();

   for (auto& thread : mProducerThread)
      thread.join();

   mConsumerThread.join();

   // mProgressMutex can be held by the consumer thread, so we need to wait
   // until it's released.
   std::lock_guard lock(mProgressDataMutex);
}

MissingBlocksUploader::ProducedItem MissingBlocksUploader::ProduceBlock()
{
   const auto index = mFirstUnprocessedBlockIndex++;
   const auto& task = mUploadTasks[index];

   return { task, CompressBlock(task.Block) };
}

void MissingBlocksUploader::ConsumeBlock(ProducedItem item)
{
   {
      std::unique_lock<std::mutex> lock(mUploadsMutex);
      mUploadsNotFull.wait(
         lock,
         [this]
         {
            return mConcurrentUploads < NUM_UPLOADERS ||
                   !mIsRunning.load(std::memory_order_consume);
         });

      if (!mIsRunning.load(std::memory_order_relaxed))
         return;

      ++mConcurrentUploads;
   }

   DataUploader::Get().Upload(
      mServiceConfig, item.Task.BlockUrls, std::move(item.CompressedData),
      [this, task = item.Task](ResponseResult result)
      {
         if (result.Code != ResponseResultCode::Success)
            HandleFailedBlock(result, task);
         else
            ConfirmBlock(task);
      });
}

void MissingBlocksUploader::PushBlockToQueue(ProducedItem item)
{
   std::unique_lock<std::mutex> lock(mRingBufferMutex);
   mRingBufferNotFull.wait(
      lock,
      [this]
      {
         return ((mRingBufferWriteIndex + 1) % RING_BUFFER_SIZE) !=
                   mRingBufferReadIndex ||
                !mIsRunning.load(std::memory_order_consume);
      });

   if (!mIsRunning.load(std::memory_order_relaxed))
      return;

   mRingBuffer[mRingBufferWriteIndex] = std::move(item);
   mRingBufferWriteIndex = (mRingBufferWriteIndex + 1) % RING_BUFFER_SIZE;

   mRingBufferNotEmpty.notify_one();
}

MissingBlocksUploader::ProducedItem MissingBlocksUploader::PopBlockFromQueue()
{
   std::unique_lock<std::mutex> lock(mRingBufferMutex);
   mRingBufferNotEmpty.wait(
      lock,
      [this]
      {
         return mRingBufferWriteIndex != mRingBufferReadIndex ||
                !mIsRunning.load(std::memory_order_consume);
      });

   if (!mIsRunning.load(std::memory_order_relaxed))
      return {};

   auto item = std::move(mRingBuffer[mRingBufferReadIndex]);
   mRingBufferReadIndex = (mRingBufferReadIndex + 1) % RING_BUFFER_SIZE;

   mRingBufferNotFull.notify_one();

   return std::move(item);
}

void MissingBlocksUploader::ConfirmBlock(BlockUploadTask item)
{
   {
      std::lock_guard<std::mutex> lock(mProgressDataMutex);
      mProgressData.UploadedBlocks++;

      mProgressCallback(mProgressData, item.Block, {});
   }

   {
      std::lock_guard<std::mutex> lock(mUploadsMutex);
      --mConcurrentUploads;
      mUploadsNotFull.notify_one();
   }
}

void MissingBlocksUploader::HandleFailedBlock(
   const ResponseResult& result, BlockUploadTask task)
{
   {
      std::lock_guard<std::mutex> lock(mProgressDataMutex);

      mProgressData.FailedBlocks++;
      mProgressData.UploadErrors.push_back(result);
      mProgressCallback(mProgressData, task.Block, result);
   }

   {
      std::lock_guard<std::mutex> lock(mUploadsMutex);
      --mConcurrentUploads;
      mUploadsNotFull.notify_one();
   }
}

void MissingBlocksUploader::ProducerThread()
{
   while (mIsRunning.load(std::memory_order_consume))
   {
      std::lock_guard<std::mutex> lock(mBlocksMutex);

      if (mFirstUnprocessedBlockIndex >= mUploadTasks.size())
         return;

      auto item = ProduceBlock();

      if (item.CompressedData.empty())
      {
         std::lock_guard<std::mutex> lock(mProgressDataMutex);
         mProgressData.FailedBlocks++;
         mProgressCallback(
            mProgressData, item.Task.Block,
            { ResponseResultCode::InternalClientError, {} });

         continue;
      }

      PushBlockToQueue(std::move(item));
   }
}

void MissingBlocksUploader::ConsumerThread()
{
   while (mIsRunning.load (std::memory_order_consume))
   {
      auto item = PopBlockFromQueue();
      ConsumeBlock(std::move(item));
   }
}

} // namespace cloud::audiocom::sync
