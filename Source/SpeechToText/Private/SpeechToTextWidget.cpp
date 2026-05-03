#include "SpeechToTextWidget.h"
#include "SpeechToTextComponent.h"
#include "Components/TextBlock.h"

void USpeechToTextWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (StatusText)
	{
		StatusText->SetText(FText::FromString(TEXT("Ready")));
	}

	if (TranscriptionText)
	{
		TranscriptionText->SetText(FText::GetEmpty());
	}
}

void USpeechToTextWidget::BindToComponent(USpeechToTextComponent* Component)
{
	if (!Component)
	{
		UE_LOG(LogTemp, Warning, TEXT("SpeechToTextWidget: Cannot bind to null component"));
		return;
	}

	// Unbind previous if any
	if (BoundComponent)
	{
		BoundComponent->OnTranscriptionComplete.RemoveDynamic(this, &USpeechToTextWidget::OnTranscriptionComplete);
		BoundComponent->OnRecordingStateChanged.RemoveDynamic(this, &USpeechToTextWidget::OnStateChanged);
	}

	BoundComponent = Component;
	Component->OnTranscriptionComplete.AddDynamic(this, &USpeechToTextWidget::OnTranscriptionComplete);
	Component->OnRecordingStateChanged.AddDynamic(this, &USpeechToTextWidget::OnStateChanged);

	UpdateState(Component->GetCurrentState());
}

void USpeechToTextWidget::OnTranscriptionComplete(const FSpeechToTextResult& Result)
{
	UpdateTranscription(Result);
}

void USpeechToTextWidget::OnStateChanged(ESpeechToTextState NewState)
{
	UpdateState(NewState);
}

void USpeechToTextWidget::UpdateTranscription(const FSpeechToTextResult& Result)
{
	if (Result.bSuccess && !Result.TranscribedText.IsEmpty())
	{
		TranscriptionHistory.Add(Result.TranscribedText);

		// Trim history to max lines
		while (TranscriptionHistory.Num() > MaxHistoryLines)
		{
			TranscriptionHistory.RemoveAt(0);
		}
	}
	else if (!Result.bSuccess)
	{
		TranscriptionHistory.Add(FString::Printf(TEXT("[Error: %s]"), *Result.ErrorMessage));

		while (TranscriptionHistory.Num() > MaxHistoryLines)
		{
			TranscriptionHistory.RemoveAt(0);
		}
	}

	if (TranscriptionText)
	{
		const FString Combined = FString::Join(TranscriptionHistory, TEXT("\n"));
		TranscriptionText->SetText(FText::FromString(Combined));
	}
}

void USpeechToTextWidget::UpdateState(ESpeechToTextState NewState)
{
	if (!StatusText)
	{
		return;
	}

	switch (NewState)
	{
	case ESpeechToTextState::Idle:
		StatusText->SetText(FText::FromString(TEXT("Ready")));
		StatusText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		break;
	case ESpeechToTextState::Recording:
		StatusText->SetText(FText::FromString(TEXT("Recording...")));
		StatusText->SetColorAndOpacity(FSlateColor(FLinearColor::Red));
		break;
	case ESpeechToTextState::Processing:
		StatusText->SetText(FText::FromString(TEXT("Transcribing...")));
		StatusText->SetColorAndOpacity(FSlateColor(FLinearColor::Yellow));
		break;
	case ESpeechToTextState::Error:
		StatusText->SetText(FText::FromString(TEXT("Error")));
		StatusText->SetColorAndOpacity(FSlateColor(FLinearColor::Red));
		break;
	}
}

void USpeechToTextWidget::ClearHistory()
{
	TranscriptionHistory.Empty();

	if (TranscriptionText)
	{
		TranscriptionText->SetText(FText::GetEmpty());
	}
}
