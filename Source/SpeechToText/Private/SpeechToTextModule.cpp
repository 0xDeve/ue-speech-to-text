#include "SpeechToTextModule.h"

DEFINE_LOG_CATEGORY(LogSpeechToText);

#define LOCTEXT_NAMESPACE "FSpeechToTextModule"

void FSpeechToTextModule::StartupModule()
{
	UE_LOG(LogSpeechToText, Verbose, TEXT("Module started"));
}

void FSpeechToTextModule::ShutdownModule()
{
	UE_LOG(LogSpeechToText, Verbose, TEXT("Module shut down"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSpeechToTextModule, SpeechToText)
