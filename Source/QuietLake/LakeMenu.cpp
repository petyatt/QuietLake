#include "LakeMenu.h"
#include "LakeGame.h"
#include "LakeMap.h"
#include "AudioDevice.h"
#include "Blueprint/WidgetTree.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ComboBoxString.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Slider.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SWindow.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"

namespace
{
    const FLinearColor Ink(0.92f, 0.9f, 0.84f);          // warm off-white, matches the misty dawn
    const FLinearColor Muted(0.65f, 0.68f, 0.66f);
    const TCHAR* SettingsSection = TEXT("/Script/QuietLake.LakeSettings");
}

// --- widget ------------------------------------------------------------------------------

UTextBlock* ULakeMenuWidget::Label(const FString& Text, int32 Size, FLinearColor Colour)
{
    UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>();
    Block->SetText(FText::FromString(Text));
    FSlateFontInfo Font = Block->GetFont();
    Font.Size = Size;
    Block->SetFont(Font);
    Block->SetColorAndOpacity(FSlateColor(Colour));
    return Block;
}

UButton* ULakeMenuWidget::MenuButton(const FString& Text, UVerticalBox* Parent, UTextBlock** OutLabel)
{
    UButton* Button = WidgetTree->ConstructWidget<UButton>();
    // Flat buttons: invisible until hovered, a soft light band on hover.
    FButtonStyle Style = Button->GetStyle();
    auto Brush = [](FLinearColor C) { FSlateBrush B; B.TintColor = FSlateColor(C); B.DrawAs = ESlateBrushDrawType::Box; return B; };
    Style.SetNormal(Brush(FLinearColor(1, 1, 1, 0.0f)));
    Style.SetHovered(Brush(FLinearColor(1, 1, 1, 0.12f)));
    Style.SetPressed(Brush(FLinearColor(1, 1, 1, 0.22f)));
    Style.SetNormalPadding(FMargin(14, 8));
    Style.SetPressedPadding(FMargin(14, 9, 14, 7));
    Button->SetStyle(Style);
    UTextBlock* Text_ = Label(Text, 30, Ink);
    Button->AddChild(Text_);
    UVerticalBoxSlot* ButtonSlot = Parent->AddChildToVerticalBox(Button);
    ButtonSlot->SetPadding(FMargin(0, 4));
    ButtonSlot->SetHorizontalAlignment(HAlign_Left);
    if (OutLabel) *OutLabel = Text_;
    return Button;
}

TSharedRef<SWidget> ULakeMenuWidget::RebuildWidget()
{
    if (WidgetTree && !WidgetTree->RootWidget)
    {
        UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
        WidgetTree->RootWidget = Root;
        // A dark translucent band on the left; the lake stays visible on the right.
        UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
        Panel->SetBrushColor(FLinearColor(0.01f, 0.015f, 0.02f, 0.62f));
        Panel->SetPadding(FMargin(80, 0, 60, 0));
        Panel->SetVerticalAlignment(VAlign_Center);
        UCanvasPanelSlot* PanelSlot = Root->AddChildToCanvas(Panel);
        PanelSlot->SetAnchors(FAnchors(0, 0, 0, 1));
        PanelSlot->SetOffsets(FMargin(0, 0, 680, 0));

        Pages = WidgetTree->ConstructWidget<UWidgetSwitcher>();
        Panel->SetContent(Pages);

        // Page 0: title and main buttons.
        UVerticalBox* Main = WidgetTree->ConstructWidget<UVerticalBox>();
        Main->AddChildToVerticalBox(Label(TEXT("ТИХОЕ ОЗЕРО"), 46, Ink));
        Main->AddChildToVerticalBox(Label(TEXT("утро на лесном озере"), 18, Muted))->SetPadding(FMargin(4, 0, 0, 56));
        UTextBlock* PlayText = nullptr;
        MenuButton(TEXT("Играть"), Main, &PlayText)->OnClicked.AddDynamic(this, &ULakeMenuWidget::OnPlay);
        PlayLabel = PlayText;
        MenuButton(TEXT("Настройки"), Main)->OnClicked.AddDynamic(this, &ULakeMenuWidget::OnSettings);
        MenuButton(TEXT("Выход"), Main)->OnClicked.AddDynamic(this, &ULakeMenuWidget::OnExit);
        Pages->AddChild(Main);

        // Page 1: settings.
        UVerticalBox* Settings = WidgetTree->ConstructWidget<UVerticalBox>();
        Settings->AddChildToVerticalBox(Label(TEXT("НАСТРОЙКИ"), 40, Ink))->SetPadding(FMargin(0, 0, 0, 36));
        auto Row = [this, Settings](const FString& Name, UWidget* Control)
        {
            Settings->AddChildToVerticalBox(Label(Name, 18, Muted))->SetPadding(FMargin(0, 10, 0, 6));
            UVerticalBoxSlot* RowSlot = Settings->AddChildToVerticalBox(Control);
            RowSlot->SetHorizontalAlignment(HAlign_Fill);
            RowSlot->SetPadding(FMargin(0, 0, 40, 8));
        };
        Resolution = WidgetTree->ConstructWidget<UComboBoxString>();
        Row(TEXT("Разрешение экрана"), Resolution);
        WindowMode = WidgetTree->ConstructWidget<UComboBoxString>();
        for (const TCHAR* Mode : { TEXT("Полный экран"), TEXT("Окно без рамки"), TEXT("Окно") }) WindowMode->AddOption(Mode);
        Row(TEXT("Режим экрана"), WindowMode);
        UHorizontalBox* VolumeRow = WidgetTree->ConstructWidget<UHorizontalBox>();
        Volume = WidgetTree->ConstructWidget<USlider>();
        Volume->SetMinValue(0);
        Volume->SetMaxValue(1);
        Volume->SetStepSize(0.01f);
        Volume->OnValueChanged.AddDynamic(this, &ULakeMenuWidget::OnVolumeChanged);
        UHorizontalBoxSlot* SliderSlot = VolumeRow->AddChildToHorizontalBox(Volume);
        SliderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        SliderSlot->SetVerticalAlignment(VAlign_Center);
        VolumeValue = Label(TEXT("100%"), 20, Ink);
        VolumeRow->AddChildToHorizontalBox(VolumeValue)->SetPadding(FMargin(16, 0, 0, 0));
        Row(TEXT("Громкость"), VolumeRow);
        Settings->AddChildToVerticalBox(WidgetTree->ConstructWidget<USpacer>())->SetPadding(FMargin(0, 24, 0, 0));
        MenuButton(TEXT("Применить"), Settings)->OnClicked.AddDynamic(this, &ULakeMenuWidget::OnApply);
        MenuButton(TEXT("Назад"), Settings)->OnClicked.AddDynamic(this, &ULakeMenuWidget::OnBack);
        Pages->AddChild(Settings);

        // Page 2: controls and tips (the in-game hints fade after a few seconds, the full list lives here).
        UVerticalBox* Help = WidgetTree->ConstructWidget<UVerticalBox>();
        Help->AddChildToVerticalBox(Label(TEXT("ПОДСКАЗКИ"), 40, Ink))->SetPadding(FMargin(0, 0, 0, 28));
        for (const TPair<const TCHAR*, const TCHAR*>& Line : {
                 TPair<const TCHAR*, const TCHAR*>(TEXT("Мышь"), TEXT("взгляд и удилище")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("ЛКМ — держать"), TEXT("маятниковый мах; отпустить — опустить удилище и снасть")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("ЛКМ после заброса"), TEXT("держать — поднимать удилище, снасть придёт в левую руку; отпустить — удилище остановится")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("ПКМ"), TEXT("подсечка, когда поплавок ушёл под воду, лёг или поехал")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("ЛКМ с рыбой"), TEXT("держать — поднимать удилище и тянуть рыбу, отпускать на рывках")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("G"), TEXT("бросить шар прикормки к поплавку (или куда смотрите, до 12 м); в шар добавляется то, что на крючке; рыба подходит на запах через минуту-другую")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("R"), TEXT("поставить удочку на рогатку / взять; рогаток до трёх, снасть на рогатке ловит сама; поклёвка — подбегите и ПКМ")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("R — держать 2 с"), TEXT("положить удочку на землю: синий контур, ЛКМ — положить, ПКМ — отмена")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("B"), TEXT("снять чехол со спины: синий контур, ЛКМ — положить, ПКМ — отмена")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("F — держать"), TEXT("навести на чехол: «Открыть» или «Надеть на спину» (колесо — выбрать); на удочку на земле — взять")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("Окно чехла"), TEXT("перетаскивать удочки мышью между тремя ячейками и рукой; F или Esc — закрыть")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("V"), TEXT("вид от первого лица / со стороны")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("1 / 2 / 3"), TEXT("наживка: червь, опарыш, хлеб")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("+ / −"), TEXT("глубина наживки (или колесо мыши, если не наведено на чехол): 10–500 см, шаг 1 см, только до заброса")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("WASD"), TEXT("ходьба, в том числе с заброшенной удочкой")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("F3"), TEXT("«Live Fish»: чем заняты рыбы, последние всплески и круги")),
                 TPair<const TCHAR*, const TCHAR*>(TEXT("Esc"), TEXT("меню")) })
        {
            UHorizontalBox* HelpRow = WidgetTree->ConstructWidget<UHorizontalBox>();
            UTextBlock* Key = Label(Line.Key, 17, Ink);
            Key->SetMinDesiredWidth(230);
            HelpRow->AddChildToHorizontalBox(Key);
            UTextBlock* Value = Label(Line.Value, 17, Muted);
            Value->SetAutoWrapText(true);
            HelpRow->AddChildToHorizontalBox(Value)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            Help->AddChildToVerticalBox(HelpRow)->SetPadding(FMargin(0, 3));
        }
        UTextBlock* Tips = Label(TEXT("Окунь любит червя, плотва — опарыша и хлеб, карась держится у дна, щука стоит в засаде у тростника. Плотва ходит в верхних слоях — ставьте мельче; окунь и щука — в средней толще; для карася опускайте наживку до самого дна. Рыбу пугает всплеск и ранняя подсечка. Прикормка (G) за пару минут собирает плотву и карася; с червём в шаре подходит и окунь; перекормите — рыба наестся и станет разборчивой."), 15, Muted);
        Tips->SetAutoWrapText(true);
        Help->AddChildToVerticalBox(Tips)->SetPadding(FMargin(0, 22, 40, 18));
        MenuButton(TEXT("Назад"), Help)->OnClicked.AddDynamic(this, &ULakeMenuWidget::OnBack);
        Pages->AddChild(Help);

        // A quiet "Tips" line at the bottom of the main panel.
        UVerticalBox* Bottom = WidgetTree->ConstructWidget<UVerticalBox>();
        UTextBlock* HelpText = nullptr;
        UButton* HelpButton = MenuButton(TEXT("Подсказки"), Bottom, &HelpText);
        FSlateFontInfo HelpFont = HelpText->GetFont();
        HelpFont.Size = 20;
        HelpText->SetFont(HelpFont);
        HelpText->SetColorAndOpacity(FSlateColor(Muted));
        HelpButton->OnClicked.AddDynamic(this, &ULakeMenuWidget::OnHelp);
        HelpLine = Bottom;
        UCanvasPanelSlot* BottomSlot = Root->AddChildToCanvas(Bottom);
        BottomSlot->SetAnchors(FAnchors(0, 1, 0, 1));
        BottomSlot->SetOffsets(FMargin(66, -90, 300, 60));
        FillSettings();
    }
    return Super::RebuildWidget();
}

void ULakeMenuWidget::FillSettings()
{
    UGameUserSettings* User = UGameUserSettings::GetGameUserSettings();
    Resolutions.Reset();
    if (!UKismetSystemLibrary::GetSupportedFullscreenResolutions(Resolutions) || Resolutions.Num() == 0)
        UKismetSystemLibrary::GetConvenientWindowedResolutions(Resolutions);
    const FIntPoint Current = User->GetScreenResolution();
    Resolutions.AddUnique(Current);
    Resolutions.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X * A.Y > B.X * B.Y; });
    Resolution->ClearOptions();
    for (const FIntPoint& R : Resolutions) Resolution->AddOption(FString::Printf(TEXT("%d × %d"), R.X, R.Y));
    Resolution->SetSelectedOption(FString::Printf(TEXT("%d × %d"), Current.X, Current.Y));
    // Show the mode the window is actually in (the command line can differ from the saved one).
    EWindowMode::Type Mode = User->GetFullscreenMode();
    if (GEngine && GEngine->GameViewport && GEngine->GameViewport->GetWindow().IsValid())
        Mode = GEngine->GameViewport->GetWindow()->GetWindowMode();
    WindowMode->SetSelectedIndex(Mode == EWindowMode::Fullscreen ? 0 : Mode == EWindowMode::WindowedFullscreen ? 1 : 2);
    const float V = ALakePlayerController::LoadVolume();
    Volume->SetValue(V);
    VolumeValue->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(V * 100))));
}

void ULakeMenuWidget::SetInGame(bool bInGameNow)
{
    bInGame = bInGameNow;
    if (PlayLabel) PlayLabel->SetText(FText::FromString(bInGame ? TEXT("Продолжить") : TEXT("Играть")));
}

void ULakeMenuWidget::ShowPage(int32 Page)
{
    if (!Pages) return;
    if (Page == 1) FillSettings();
    Pages->SetActiveWidgetIndex(Page);
    if (HelpLine) HelpLine->SetVisibility(Page == 0 ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

FReply ULakeMenuWidget::NativeOnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
    // Esc: back from the settings, or back into the game.
    if (KeyEvent.GetKey() == EKeys::Escape)
    {
        if (Pages && Pages->GetActiveWidgetIndex() != 0) OnBack();
        else if (bInGame) OnPlay();
        return FReply::Handled();
    }
    return Super::NativeOnKeyDown(Geometry, KeyEvent);
}

void ULakeMenuWidget::OnPlay()
{
    if (auto* PC = Cast<ALakePlayerController>(GetOwningPlayer())) PC->CloseMenu();
}
void ULakeMenuWidget::OnSettings() { ShowPage(1); }
void ULakeMenuWidget::OnHelp() { ShowPage(2); }
void ULakeMenuWidget::OnBack() { ShowPage(0); }
void ULakeMenuWidget::OnExit()
{
    UKismetSystemLibrary::QuitGame(this, GetOwningPlayer(), EQuitPreference::Quit, false);
}
void ULakeMenuWidget::OnVolumeChanged(float Value)
{
    // Volume changes are heard immediately and saved right away.
    VolumeValue->SetText(FText::FromString(FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Value * 100))));
    if (auto* PC = Cast<ALakePlayerController>(GetOwningPlayer())) PC->ApplyVolume(Value);
    ALakePlayerController::SaveVolume(Value);
}
void ULakeMenuWidget::OnApply()
{
    UGameUserSettings* User = UGameUserSettings::GetGameUserSettings();
    const int32 Index = Resolution->GetSelectedIndex();
    if (Resolutions.IsValidIndex(Index)) User->SetScreenResolution(Resolutions[Index]);
    const int32 Mode = WindowMode->GetSelectedIndex();
    User->SetFullscreenMode(Mode == 0 ? EWindowMode::Fullscreen : Mode == 1 ? EWindowMode::WindowedFullscreen : EWindowMode::Windowed);
    User->ApplySettings(false);
    User->SaveSettings();
}

// --- player controller ---------------------------------------------------------------------

ALakePlayerController::ALakePlayerController()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bTickEvenWhenPaused = true;
}

float ALakePlayerController::LoadVolume()
{
    float Volume = 1.0f;
    GConfig->GetFloat(SettingsSection, TEXT("MasterVolume"), Volume, GGameUserSettingsIni);
    return FMath::Clamp(Volume, 0.0f, 1.0f);
}
void ALakePlayerController::SaveVolume(float Volume)
{
    GConfig->SetFloat(SettingsSection, TEXT("MasterVolume"), FMath::Clamp(Volume, 0.0f, 1.0f), GGameUserSettingsIni);
    GConfig->Flush(false, GGameUserSettingsIni);
}
void ALakePlayerController::ApplyVolume(float Volume)
{
    if (UWorld* World = GetWorld())
        if (FAudioDeviceHandle Device = World->GetAudioDevice()) Device->SetTransientPrimaryVolume(Volume);
}

void ALakePlayerController::BeginPlay()
{
    Super::BeginPlay();
    if (!IsLocalController()) return;
    ApplyVolume(LoadVolume());
    // Automated preview runs go straight into the game.
    FString ScenePreview;
    if (FParse::Value(FCommandLine::Get(), TEXT("ScenePreview="), ScenePreview)) { bStarted = true; return; }
    if (FParse::Param(FCommandLine::Get(), TEXT("LakePreview")) || FParse::Param(FCommandLine::Get(), TEXT("WeatherPreview")) || FParse::Param(FCommandLine::Get(), TEXT("NightPreview")) || FParse::Param(FCommandLine::Get(), TEXT("CloudPreview")) || FParse::Param(FCommandLine::Get(), TEXT("FrontPreview")) || FParse::Param(FCommandLine::Get(), TEXT("MapPreview")) || FParse::Param(FCommandLine::Get(), TEXT("FisherPreview")) || FParse::Param(FCommandLine::Get(), TEXT("LifePreview")) || FParse::Param(FCommandLine::Get(), TEXT("FishPreview")) || FParse::Param(FCommandLine::Get(), TEXT("WaterPreview")) || FParse::Param(FCommandLine::Get(), TEXT("BitePreview")) || FParse::Param(FCommandLine::Get(), TEXT("LandPreview")) || FParse::Param(FCommandLine::Get(), TEXT("CastPreview")) || FParse::Param(FCommandLine::Get(), TEXT("LinePreview")) || FParse::Param(FCommandLine::Get(), TEXT("RodPreview")) || FParse::Param(FCommandLine::Get(), TEXT("HandsPreview")) || FParse::Param(FCommandLine::Get(), TEXT("LeaderPreview")) || FParse::Param(FCommandLine::Get(), TEXT("RetrievePreview")) || FParse::Param(FCommandLine::Get(), TEXT("GroundbaitPreview")) || FParse::Param(FCommandLine::Get(), TEXT("FightPreview")) || FParse::Param(FCommandLine::Get(), TEXT("MouthPreview")) || FParse::Param(FCommandLine::Get(), TEXT("CasePreview")) || FParse::Param(FCommandLine::Get(), TEXT("PlantPreview"))) { bStarted = true; return; }
    // A slow flight around the lake behind the menu.
    MenuCamera = GetWorld()->SpawnActor<ACameraActor>();
    if (MenuCamera) MenuCamera->GetCameraComponent()->SetFieldOfView(60);
    OpenMenu();
#if !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("MenuPreview")))
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        At(20.0f, [] { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Menu_Main.png"), true, false); });
        At(21.0f, [this] { if (Menu) Menu->ShowPage(1); });
        At(22.0f, [] { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Menu_Settings.png"), true, false); });
        At(22.5f, [this] { if (Menu) Menu->ShowPage(2); });
        At(22.9f, [] { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Menu_Help.png"), true, false); });
        At(23.0f, [this] { CloseMenu(); });   // same as pressing Play
        At(25.0f, [] { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Menu_AfterPlay.png"), true, false); });
        At(42.0f, [] { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Menu_HudLater.png"), true, false); });
        At(44.0f, [] { FPlatformMisc::RequestExit(false); });
    }
#endif
}

void ALakePlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    InputComponent->BindAction("Menu", IE_Pressed, this, &ALakePlayerController::ToggleMenu).bExecuteWhenPaused = true;
}

void ALakePlayerController::ToggleMenu()
{
    // Esc first closes the case window, or drops the ghost of what was being laid down.
    if (!bMenuOpen)
        if (ALakeCharacter* Fisher = Cast<ALakeCharacter>(GetPawn()))
        {
            if (Fisher->IsCaseWindowOpen()) { Fisher->CloseCaseWindow(); return; }
            if (Fisher->Placing != ALakeCharacter::EPlacing::None) { Fisher->Placing = ALakeCharacter::EPlacing::None; return; }
        }
    if (bMenuOpen) { if (bStarted) CloseMenu(); }
    else OpenMenu();
}

void ALakePlayerController::OpenMenu()
{
    if (!Menu)
    {
        Menu = CreateWidget<ULakeMenuWidget>(this, ULakeMenuWidget::StaticClass());
        if (!Menu) return;
        Menu->SetIsFocusable(true);   // keyboard focus, so Esc works inside the menu
    }
    Menu->SetInGame(bStarted);
    Menu->ShowPage(0);
    if (!Menu->IsInViewport()) Menu->AddToViewport(10);
    bMenuOpen = true;
    SetShowMouseCursor(true);
    FInputModeUIOnly Mode;
    Mode.SetWidgetToFocus(Menu->TakeWidget());
    SetInputMode(Mode);
}

void ALakePlayerController::CloseMenu()
{
    if (Menu) Menu->RemoveFromParent();
    bMenuOpen = false;
    bStarted = true;
    SetShowMouseCursor(false);
    FInputModeGameOnly Mode;
    Mode.SetConsumeCaptureMouseDown(false);
    SetInputMode(Mode);
    if (UGameViewportClient* Viewport = GetWorld()->GetGameViewport())
    {
        Viewport->SetMouseCaptureMode(EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown);
        Viewport->SetMouseLockMode(EMouseLockMode::LockOnCapture);
    }
    if (GetPawn()) SetViewTarget(GetPawn());
}

void ALakePlayerController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    // Before the first Play the camera circles the lake; possession would otherwise grab it.
    if (bMenuOpen && !bStarted && MenuCamera)
    {
        OrbitAngle += DeltaSeconds * 0.025f;
        const FVector Centre(LakeMap::Centre, LakeMap::SurfaceZ);
        // A slow circle over the middle reach: the island, the wooded north bank and the long water.
        const FVector Eye = Centre + FVector(FMath::Cos(OrbitAngle) * 7000, FMath::Sin(OrbitAngle) * 4500, 650);
        MenuCamera->SetActorLocationAndRotation(Eye, (Centre + FVector(0, 0, 150) - Eye).Rotation());
        if (GetViewTarget() != MenuCamera) SetViewTarget(MenuCamera);
    }
}
