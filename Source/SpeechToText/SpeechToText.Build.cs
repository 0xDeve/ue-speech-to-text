using UnrealBuildTool;

public class SpeechToText : ModuleRules
{
	public SpeechToText(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"AudioCaptureCore",
			"AudioMixer",
			"DeveloperSettings",
			"HTTP"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",
			"UMG",
			"Slate",
			"SlateCore",
			"NPCConversation"
		});

		// Platform-specific audio capture implementations
		if (Target.Platform.IsInGroup(UnrealPlatformGroup.Windows) ||
		    Target.Platform == UnrealTargetPlatform.Mac)
		{
			PrivateDependencyModuleNames.Add("AudioCaptureRtAudio");
		}
		else if (Target.Platform == UnrealTargetPlatform.Android)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"AudioCaptureAndroid",
				"AndroidPermission"
			});
		}
	}
}
