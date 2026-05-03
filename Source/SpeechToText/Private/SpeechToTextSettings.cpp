#include "SpeechToTextSettings.h"

USpeechToTextSettings::USpeechToTextSettings()
{
}

const USpeechToTextSettings* USpeechToTextSettings::Get()
{
	return GetDefault<USpeechToTextSettings>();
}

FName USpeechToTextSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}
