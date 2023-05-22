// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"

#if !UE_SERVER
#include "Misc/CoreDelegates.h"
#include "Misc/ConfigCacheIni.h"
#include "AkUEFeatures.h"

#if UE_5_1_OR_LATER
#include "Wwise/WwiseAudioLinkRuntimeModule.h"
#if WITH_EDITOR
#include "Wwise/WwiseAudioLinkEditorModule.h"
#endif
#endif
#endif

class FWwiseModule : public IModuleInterface
{
#if !UE_SERVER

#if UE_5_1_OR_LATER
	TUniquePtr<FWwiseAudioLinkRuntimeModule> WwiseAudioLinkRuntimeModule;
#if WITH_EDITOR
	TUniquePtr<FWwiseAudioLinkEditorModule> WwiseAudioLinkEditorModule;
#endif
#endif

public:
	virtual void StartupModule() override
	{
		UE_LOG(LogLoad, Log, TEXT("WwiseModule: Loading AkAudio"));
		FModuleManager::Get().LoadModule(TEXT("AkAudio"));

		FCoreDelegates::OnPostEngineInit.AddRaw(this, &FWwiseModule::OnPostEngineInit);

		// Loading optional modules
		bool bAkAudioMixerEnabled = false;
		GConfig->GetBool(TEXT("/Script/AkAudio.AkSettings"), TEXT("bAkAudioMixerEnabled"), bAkAudioMixerEnabled, GGameIni);
		if (bAkAudioMixerEnabled)
		{
			UE_LOG(LogLoad, Log, TEXT("WwiseModule: Loading AkAudioMixer"));
			FModuleManager::Get().LoadModule(TEXT("AkAudioMixer"));
		}

		bool bWwiseAudioLinkEnabled = false;
		GConfig->GetBool(TEXT("/Script/AkAudio.AkSettings"), TEXT("bWwiseAudioLinkEnabled"), bWwiseAudioLinkEnabled, GGameIni);
		if (bWwiseAudioLinkEnabled)
		{
#if UE_5_1_OR_LATER
			UE_LOG(LogLoad, Log, TEXT("WwiseModule: Loading WwiseAudioLink"));
			WwiseAudioLinkRuntimeModule = MakeUnique<FWwiseAudioLinkRuntimeModule>();
#if WITH_EDITOR
			WwiseAudioLinkEditorModule = MakeUnique<FWwiseAudioLinkEditorModule>();
#endif
#else
			UE_LOG(LogLoad, Error, TEXT("WwiseModule: AudioLink is not available in Unreal versions prior to 5.1. Ignoring."));
#endif
		}
	}
	
	void OnPostEngineInit()
	{
#if WITH_EDITOR
		UE_LOG(LogLoad, Log, TEXT("WwiseModule: Loading AudiokineticTools"));
		FModuleManager::Get().LoadModule(TEXT("AudiokineticTools"));
#endif
	}

	virtual void ShutdownModule() override
	{
#if UE_5_1_OR_LATER
#if WITH_EDITOR
		WwiseAudioLinkEditorModule.Reset();
#endif
		WwiseAudioLinkRuntimeModule.Reset();
#endif
	}

#endif
};

IMPLEMENT_MODULE(FWwiseModule, Wwise);
