#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "SpeechToTextTypes.h"
#include "SpeechToTextWidget.generated.h"

class UTextBlock;
class UVerticalBox;
class USpeechToTextComponent;

/**
 * Widget that displays speech-to-text transcription results.
 * Place in VR world space via a WidgetComponent on any Actor.
 *
 * Bind to a USpeechToTextComponent to auto-update, or call
 * UpdateTranscription/UpdateState manually from Blueprint.
 */
UCLASS(BlueprintType, Blueprintable)
class SPEECHTOTEXT_API USpeechToTextWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Bind this widget to a SpeechToTextComponent for automatic updates. */
	UFUNCTION(BlueprintCallable, Category = "Speech To Text|Widget")
	void BindToComponent(USpeechToTextComponent* Component);

	/** Manually update the displayed transcription text. */
	UFUNCTION(BlueprintCallable, Category = "Speech To Text|Widget")
	void UpdateTranscription(const FSpeechToTextResult& Result);

	/** Manually update the displayed state. */
	UFUNCTION(BlueprintCallable, Category = "Speech To Text|Widget")
	void UpdateState(ESpeechToTextState NewState);

	/** Clear all transcription history. */
	UFUNCTION(BlueprintCallable, Category = "Speech To Text|Widget")
	void ClearHistory();

	/** Maximum number of transcription lines to keep visible. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Speech To Text|Widget")
	int32 MaxHistoryLines = 5;

protected:
	virtual void NativeConstruct() override;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidget), Category = "Speech To Text|Widget")
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional), Category = "Speech To Text|Widget")
	TObjectPtr<UTextBlock> TranscriptionText;

private:
	void OnTranscriptionComplete(const FSpeechToTextResult& Result);
	void OnStateChanged(ESpeechToTextState NewState);

	TArray<FString> TranscriptionHistory;

	UPROPERTY()
	TObjectPtr<USpeechToTextComponent> BoundComponent;
};
