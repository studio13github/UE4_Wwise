/*******************************************************************************
The content of this file includes portions of the proprietary AUDIOKINETIC Wwise
Technology released in source code form as part of the game integration package.
The content of this file may not be used without valid licenses to the
AUDIOKINETIC Wwise Technology.
Note that the use of the game engine is subject to the Unreal(R) Engine End User
License Agreement at https://www.unrealengine.com/en-US/eula/unreal
 
License Usage
 
Licensees holding valid licenses to the AUDIOKINETIC Wwise Technology may use
this file in accordance with the end user license agreement provided with the
software or, alternatively, in accordance with the terms contained
in a written agreement between you and Audiokinetic Inc.
Copyright (c) 2022 Audiokinetic Inc.
*******************************************************************************/

#include "Wwise/WwiseFileCache.h"

#include "Wwise/WwiseFileHandlerModule.h"
#include "Wwise/Stats/AsyncStats.h"
#include "Wwise/Stats/FileHandler.h"
#include "AkUEFeatures.h"

#include "Async/Async.h"
#include "Async/AsyncFileHandle.h"
#if UE_5_0_OR_LATER
#include "HAL/PlatformFileManager.h"
#else
#include "HAL/PlatformFilemanager.h"
#endif

#include <inttypes.h>

FWwiseFileCache* FWwiseFileCache::Get()
{
	if (auto* Module = IWwiseFileHandlerModule::GetModule())
	{
		if (auto* FileCache = Module->GetFileCache())
		{
			return FileCache;
		}
	}
	return nullptr;
}

FWwiseFileCache::FWwiseFileCache()
{
	if (!FileCacheThreadPool)
	{
		InitializeFileCacheThreadPool();
	}
	if (!ExecutionQueue)
	{
		ExecutionQueue = new FWwiseExecutionQueue(FileCacheThreadPool);
	}
}

FWwiseFileCache::~FWwiseFileCache()
{
	delete ExecutionQueue; ExecutionQueue = nullptr;
	delete FileCacheThreadPool; FileCacheThreadPool = nullptr;
}

void FWwiseFileCache::CreateFileCacheHandle(
	FWwiseFileCacheHandle*& OutHandle,
	const FString& Pathname,
	FWwiseFileOperationDone&& OnDone)
{
	OutHandle = new FWwiseFileCacheHandle(Pathname);
	if (UNLIKELY(!OutHandle))
	{
		OnDone(false);
	}
	OutHandle->Open(MoveTemp(OnDone));
}

void FWwiseFileCache::InitializeFileCacheThreadPool()
{
	static constexpr int32 StackSize = 128 * 1024;
	static constexpr int32 NumThreadsInThreadPool = 1;

	FileCacheThreadPool = FQueuedThreadPool::Allocate();
	verify(FileCacheThreadPool->Create(NumThreadsInThreadPool, StackSize, TPri_Normal, TEXT("Wwise FileCache Pool")));
}

FWwiseFileCacheHandle::FWwiseFileCacheHandle(const FString& InPathname) :
	Pathname { InPathname },
	FileHandle { nullptr },
	FileSize { 0 },
	InitializationStat { nullptr }
{
}

FWwiseFileCacheHandle::~FWwiseFileCacheHandle()
{
	const auto NumReadLeftToProcess = ReadDataInProcess.Load();
	UE_CLOG(UNLIKELY(NumReadLeftToProcess), LogWwiseFileHandler, Warning, TEXT("FWwiseFileCacheHandle: Closing %s with %" PRIi32 " read left to process."), *Pathname, NumReadLeftToProcess);
	UE_CLOG(LIKELY(!NumReadLeftToProcess), LogWwiseFileHandler, Verbose, TEXT("FWwiseFileCacheHandle: Closing %s."), *Pathname);

	auto FileCache = FWwiseFileCache::Get();
	if (UNLIKELY(!FileCache))
	{
		// We are probably exiting, so we simply housekeep
		delete FileHandle;
		ASYNC_DEC_DWORD_STAT(STAT_WwiseFileHandlerOpenedStreams);
	}
	FileCache->ExecutionQueue->AsyncWait([FileHandle = FileHandle]
	{
		delete FileHandle;
		ASYNC_DEC_DWORD_STAT(STAT_WwiseFileHandlerOpenedStreams);
	});
	FileHandle = nullptr;
}

void FWwiseFileCacheHandle::Open(FWwiseFileOperationDone&& OnDone)
{
	check(!InitializationStat);
	check(!FileHandle);

	InitializationStat = new FWwiseAsyncCycleCounter(GET_STATID(STAT_WwiseFileHandlerFileOperationLatency));
	InitializationDone = MoveTemp(OnDone);

	UE_LOG(LogWwiseFileHandler, Verbose, TEXT("FWwiseFileCacheHandle: Opening %s."), *Pathname);
	ASYNC_INC_DWORD_STAT(STAT_WwiseFileHandlerOpenedStreams);
	FWwiseAsyncCycleCounter Stat(GET_STATID(STAT_WwiseFileHandlerFileOperationLatency));

	FileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenAsyncRead(*Pathname);
	if (UNLIKELY(!FileHandle))
	{
		UE_LOG(LogWwiseFileHandler, Verbose, TEXT("FWwiseFileCacheHandle: OpenAsyncRead %s failed instantiating."), *Pathname);
		CallDone(false, MoveTemp(InitializationDone));
		delete InitializationStat; InitializationStat = nullptr;
		return;
	}

	FAsyncFileCallBack SizeCallbackFunction = [this](bool bWasCancelled, IAsyncReadRequest* Request) mutable
	{
		OnSizeRequestDone(bWasCancelled, Request);
	};
	auto* Request = FileHandle->SizeRequest(&SizeCallbackFunction);
	if (UNLIKELY(!Request))
	{
		UE_LOG(LogWwiseFileHandler, Verbose, TEXT("FWwiseFileCacheHandle: SizeRequest %s failed instantiating."), *Pathname);
		CallDone(false, MoveTemp(InitializationDone));
		delete InitializationStat; InitializationStat = nullptr;
	}
}

void FWwiseFileCacheHandle::OnSizeRequestDone(bool bWasCancelled, IAsyncReadRequest* Request)
{
	FileSize = Request->GetSizeResults();

	if (auto FileCache = FWwiseFileCache::Get())
	{
		FileCache->ExecutionQueue->AsyncAlways([Pathname = Pathname, Request]
		{
			constexpr const auto MaxDelay = 1.f;
			auto Result = Request->WaitCompletion(MaxDelay);
			UE_CLOG(LIKELY(Result), LogWwiseFileHandler, VeryVerbose, TEXT("FWwiseFileCacheHandle: Size request for %s deleted."), *Pathname);
			UE_CLOG(UNLIKELY(!Result), LogWwiseFileHandler, Warning, TEXT("FWwiseFileCacheHandle: Size request for %s failed to complete in MaxDelay. Leaking."), *Pathname);
			if (Result)
			{
				delete Request;
			}
		});
	}

	if (UNLIKELY(FileSize <= 0))
	{
		UE_LOG(LogWwiseFileHandler, Log, TEXT("FWwiseFileCacheHandle: Streamed file \"%s\" could not be opened."), *Pathname);
		CallDone(false, MoveTemp(InitializationDone));
		delete InitializationStat; InitializationStat = nullptr;
		return;
	}

	UE_LOG(LogWwiseFileHandler, VeryVerbose, TEXT("FWwiseFileCacheHandle: Initializing %s succeeded."), *Pathname);
	CallDone(true, MoveTemp(InitializationDone));
	delete InitializationStat; InitializationStat = nullptr;
}

void FWwiseFileCacheHandle::CallDone(bool bResult, FWwiseFileOperationDone&& OnDone)
{
	if (auto* FileCache = FWwiseFileCache::Get())
	{
		if (auto* ThreadPool = FileCache->FileCacheThreadPool)
		{
			AsyncPool(*ThreadPool, [bResult, OnDone = MoveTemp(OnDone)]
			{
				OnDone(bResult);
			});
			return;
		}
	}

	OnDone(bResult);
}

void FWwiseFileCacheHandle::ReadData(uint8* OutBuffer, int64 Offset, int64 BytesToRead,
	EAsyncIOPriorityAndFlags Priority, FWwiseFileOperationDone&& OnDone)
{
	FWwiseAsyncCycleCounter Stat(GET_STATID(STAT_WwiseFileHandlerFileOperationLatency));
	++ReadDataInProcess;
	if (UNLIKELY(!FileHandle))
	{
		UE_LOG(LogWwiseFileHandler, Error, TEXT("FWwiseFileCacheHandle::ReadData: Trying to read in file %s while it was not properly initialized."), *Pathname);
		OnReadDataDone(false, MoveTemp(OnDone));
		return;
	}

	UE_LOG(LogWwiseFileHandler, VeryVerbose, TEXT("FWwiseFileCacheHandle::ReadData: %" PRIi64 "@%" PRIi64 " in %s"), BytesToRead, Offset, *Pathname);
	FAsyncFileCallBack ReadCallbackFunction = [this, OnDone = new FWwiseFileOperationDone(MoveTemp(OnDone)), BytesToRead, Stat = MoveTemp(Stat)](bool bWasCancelled, IAsyncReadRequest* Request) mutable
	{
		if (!bWasCancelled && Request)	// Do not add Request->GetReadResults() since it will break subsequent results retrievals.
		{
			ASYNC_INC_FLOAT_STAT_BY(STAT_WwiseFileHandlerTotalStreamedMB, static_cast<float>(BytesToRead) / 1024 / 1024);
		}
		OnReadDataDone(bWasCancelled, Request, MoveTemp(*OnDone));
		delete OnDone;

		if (auto FileCache = FWwiseFileCache::Get())
		{
			FileCache->ExecutionQueue->AsyncAlways([Pathname = Pathname, Request]
			{
				constexpr const auto MaxDelay = 1.f;
				auto Result = Request->WaitCompletion(MaxDelay);
				UE_CLOG(LIKELY(Result), LogWwiseFileHandler, VeryVerbose, TEXT("FWwiseFileCacheHandle: Read request for %s deleted."), *Pathname);
				UE_CLOG(UNLIKELY(!Result), LogWwiseFileHandler, Warning, TEXT("FWwiseFileCacheHandle: Read request for %s failed to complete in MaxDelay. Leaking."), *Pathname);
				if (Result)
				{
					delete Request;
				}
			});
		}
	};
	ASYNC_INC_FLOAT_STAT_BY(STAT_WwiseFileHandlerStreamingKB, static_cast<float>(BytesToRead) / 1024);
	check(BytesToRead > 0);
	auto* Request = FileHandle->ReadRequest(Offset, BytesToRead, Priority, &ReadCallbackFunction, OutBuffer);
	if (UNLIKELY(!Request))
	{
		UE_LOG(LogWwiseFileHandler, Verbose, TEXT("FWwiseFileCacheHandle::ReadData: ReadRequest %s failed instantiating."), *Pathname);
		ReadCallbackFunction(true, nullptr);
	}
}

void FWwiseFileCacheHandle::ReadAkData(uint8* OutBuffer, int64 Offset, int64 BytesToRead, int8 AkPriority, FWwiseFileOperationDone&& OnDone)
{
	EAsyncIOPriorityAndFlags Priority;
	if (LIKELY(AkPriority == AK_DEFAULT_PRIORITY))
	{
		Priority = AIOP_Normal;
	}
	else if (AkPriority <= AK_MIN_PRIORITY)
	{
		Priority = AIOP_MIN;
	}
	else if (AkPriority >= AK_MAX_PRIORITY)
	{
		Priority = AIOP_MAX;
	}
	else if (AkPriority < 50)
	{
		Priority = AIOP_Low;
	}
	else
	{
		Priority = AIOP_High;
	}
	ReadData(OutBuffer, Offset, BytesToRead, Priority, MoveTemp(OnDone));
}

void FWwiseFileCacheHandle::ReadAkData(const AkIoHeuristics& Heuristics, AkAsyncIOTransferInfo& TransferInfo,
	FWwiseAkFileOperationDone&& Callback)
{
	ReadAkData(
		static_cast<uint8*>(TransferInfo.pBuffer),
		static_cast<int64>(TransferInfo.uFilePosition),
		static_cast<int64>(TransferInfo.uRequestedSize),
		Heuristics.priority,
		[TransferInfo = &TransferInfo, FileOpDoneCallback = MoveTemp(Callback)](bool bResult)
		{
			FileOpDoneCallback(TransferInfo, bResult ? AK_Success : AK_UnknownFileError);
		});
}


void FWwiseFileCacheHandle::OnReadDataDone(bool bWasCancelled, IAsyncReadRequest* Request,
	FWwiseFileOperationDone&& OnDone)
{
	OnReadDataDone(!bWasCancelled && Request && Request->GetReadResults(), MoveTemp(OnDone));
}

void FWwiseFileCacheHandle::OnReadDataDone(bool bResult, FWwiseFileOperationDone&& OnDone)
{
	--ReadDataInProcess;
	CallDone(bResult, MoveTemp(OnDone));
}

