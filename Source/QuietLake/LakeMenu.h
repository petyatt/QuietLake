#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/PlayerController.h"
#include "LakeMenu.generated.h"

/**
 * Main / pause menu, built entirely in C++ (no widget blueprint).
 * Page 0: Play (Continue once in game), Settings, Exit.
 * Page 1: screen resolution, window mode, master volume.
 * Page 2: controls and tips, opened from the "Подсказки" line at the bottom.
 */
UCLASS()
class QUIETLAKE_API ULakeMenuWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetInGame(bool bInGame);
    void ShowPage(int32 Page);

protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual FReply NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

private:
    class UTextBlock* Label(const FString& Text, int32 Size, FLinearColor Colour = FLinearColor::White);
    class UButton* MenuButton(const FString& Text, class UVerticalBox* Parent, class UTextBlock** OutLabel = nullptr);
    void FillSettings();

    UFUNCTION() void OnPlay();
    UFUNCTION() void OnSettings();
    UFUNCTION() void OnExit();
    UFUNCTION() void OnApply();
    UFUNCTION() void OnBack();
    UFUNCTION() void OnHelp();
    UFUNCTION() void OnVolumeChanged(float Value);

    UPROPERTY() TObjectPtr<class UWidgetSwitcher> Pages;
    UPROPERTY() TObjectPtr<class UTextBlock> PlayLabel;
    UPROPERTY() TObjectPtr<class UComboBoxString> Resolution;
    UPROPERTY() TObjectPtr<class UComboBoxString> WindowMode;
    UPROPERTY() TObjectPtr<class USlider> Volume;
    UPROPERTY() TObjectPtr<class UTextBlock> VolumeValue;
    UPROPERTY() TObjectPtr<class UWidget> HelpLine;
    TArray<FIntPoint> Resolutions;
    bool bInGame = false;
};

/**
 * Shows the menu over a slow camera flight around the lake before play, and again on Esc.
 * While the menu is open the game input is off and the mouse cursor is visible.
 */
UCLASS()
class QUIETLAKE_API ALakePlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    ALakePlayerController();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupInputComponent() override;

    void OpenMenu();
    void CloseMenu();
    bool IsMenuOpen() const { return bMenuOpen; }

    /** Master volume 0..1, stored in GameUserSettings.ini. */
    static float LoadVolume();
    static void SaveVolume(float Volume);
    void ApplyVolume(float Volume);

private:
    void ToggleMenu();
    UPROPERTY() TObjectPtr<ULakeMenuWidget> Menu;
    UPROPERTY() TObjectPtr<class ACameraActor> MenuCamera;
    bool bMenuOpen = false;
    bool bStarted = false;
    float OrbitAngle = -2.4f;
};
