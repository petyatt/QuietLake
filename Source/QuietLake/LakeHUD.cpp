#include "LakeHUD.h"
#include "LakeGame.h"
#include "LakeFish.h"
#include "LakeLife.h"
#include "LakeFood.h"
#include "LakeMap.h"
#include "LakeMenu.h"
#include "LakeWeather.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"

void ALakeHUD::DrawHUD()
{
    Super::DrawHUD();
    auto* Fisher = Cast<ALakeCharacter>(GetOwningPawn());
    if (!Fisher || !Canvas) return;
    if (auto* PC = Cast<ALakePlayerController>(GetOwningPlayerController()); PC && PC->IsMenuOpen()) { PlayStart = -1; return; }
    const float Now = GetWorld()->GetRealTimeSeconds();
    if (PlayStart < 0) PlayStart = Now;
    // Everything scales with the screen height (designed for 1080p) and stays out of the view:
    // a small status card in the bottom-left corner, bars at the bottom centre only when needed.
    const float S = FMath::Max(0.8f, Canvas->ClipY / 1080.0f) * 1.4f;
    UFont* Font = GEngine->GetMediumFont();
    auto Text = [this, Font, S](const FString& Line, FLinearColor Colour, float X, float Y, float Scale)
    {
        DrawText(Line, Colour, X, Y, Font, Scale * S);
    };
    auto Width = [this, Font, S](const FString& Line, float Scale)
    {
        float W = 0, H = 0;
        GetTextSize(Line, W, H, Font, Scale * S);
        return W;
    };

    // Status: a new message shows for 6 s and fades; a bite stays (and is highlighted) while it lasts.
    if (Fisher->Status != LastStatus) { LastStatus = Fisher->Status; StatusTime = Now; }
    const bool bUrgent = Fisher->FishingState >= EFishingState::Bite;
    const float Age = Now - StatusTime;
    const float Alpha = bUrgent ? 1.0f : FMath::Clamp((6.0f - Age) / 1.0f, 0.0f, 1.0f);
    FString Info = FString::Printf(TEXT("Улов: %d   ·   Наживка: %s   ·   Глубина: %.0f см   ·   Удилище: %s   ·   Прикормка: %d"), Fisher->CatchCount, LakeFish::BaitName(Fisher->Bait), Fisher->FloatDepth, ALakeCharacter::RodName(Fisher->RodKind), Fisher->GroundbaitLeft);
    // The name of the fishing spot the angler stands at.
    if (const LakeMap::FSpot* Spot = LakeMap::SpotAt(FVector2D(Fisher->GetActorLocation()))) Info = FString::Printf(TEXT("%s   ·   %s"), Spot->Name, *Info);
    const float Pad = 14 * S, X = 28 * S;
    const float LineH = 26 * S;
    const float CardW = FMath::Max(Width(Info, 0.85f), Alpha > 0 ? Width(LastStatus, 0.95f) : 0.0f) + Pad * 2;
    const float CardH = (Alpha > 0 ? LineH * 2 : LineH) + Pad * 2 - 6 * S;
    const float CardY = Canvas->ClipY - CardH - 28 * S;
    DrawRect(FLinearColor(0.01f, 0.015f, 0.02f, 0.45f), X, CardY, CardW, CardH);
    float Y = CardY + Pad - 4 * S;
    if (Alpha > 0)
    {
        const FLinearColor Colour = Fisher->FishingState == EFishingState::Bite ? FLinearColor(1.0f, 0.82f, 0.25f) : FLinearColor(0.93f, 0.91f, 0.86f, Alpha);
        Text(LastStatus, Colour, X + Pad, Y, 0.95f);
        Y += LineH;
    }
    Text(Info, FLinearColor(0.7f, 0.74f, 0.72f, 0.9f), X + Pad, Y, 0.85f);

    // Line tension while fighting a fish.
    const float BarW = 360 * S, BarH = 8 * S, BarX = (Canvas->ClipX - BarW) / 2, BarY = Canvas->ClipY - 70 * S;
    if (Fisher->bCharging)
    {
        const FString Label(TEXT("Маятниковый мах — отпустите ЛКМ, чтобы опустить снасть"));
        Text(Label, FLinearColor(0.93f, 0.91f, 0.86f), (Canvas->ClipX - Width(Label, 0.8f)) / 2, BarY - 26 * S, 0.8f);
    }
    if (Fisher->FishingState == EFishingState::Fight)
    {
        const FLinearColor Fill = Fisher->Tension > 0.75f ? FLinearColor(0.95f, 0.25f, 0.2f) : FLinearColor(0.35f, 0.8f, 0.55f);
        DrawRect(FLinearColor(0, 0, 0, 0.4f), BarX, BarY, BarW, BarH);
        DrawRect(Fill, BarX, BarY, BarW * Fisher->Tension, BarH);
        const FString Label = Fisher->Tension > 0.75f ? TEXT("Отпустите ЛКМ — леска на пределе")
            : FString::Printf(TEXT("Натяжение %.0f%%   ·   до рыбы %.1f м"), Fisher->Tension * 100, Fisher->FishDistance / 100);
        Text(Label, FLinearColor(0.93f, 0.91f, 0.86f), (Canvas->ClipX - Width(Label, 0.8f)) / 2, BarY - 26 * S, 0.8f);
    }
    // Bottom right: the time of day, the weather and the air temperature.
    float WeatherTop = Canvas->ClipY - 28 * S;
    if (const ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
    {
        const float H = Weather->Hours();
        const FString Clock = FString::Printf(TEXT("%02d:%02d"), (int32)H, (int32)(FMath::Frac(H) * 60));
        const int32 T = FMath::RoundToInt(Weather->TemperatureC());
        const FString Line = Weather->Describe() + FString::Printf(TEXT("  ·  %s%d °C"), T > 0 ? TEXT("+") : TEXT(""), T);
        // Pressure and water temperature: what an angler checks to guess the bite.
        const float Trend = Weather->PressureTrend();
        const int32 Water = FMath::RoundToInt(Weather->WaterTempC());
        const FString Line2 = FString::Printf(TEXT("Давление %d мм%s  ·  вода %s%d °C"), FMath::RoundToInt(Weather->PressureMmHg()),
            Trend < -0.3f ? TEXT(", падает") : Trend > 0.3f ? TEXT(", растёт") : TEXT(""), Water > 0 ? TEXT("+") : TEXT(""), Water);
        const float W = FMath::Max3(Width(Clock, 1.35f), Width(Line, 0.8f), Width(Line2, 0.8f)) + Pad * 2;
        const float CardH2 = 30 * S + LineH * 2 + Pad * 2 - 6 * S;
        const float CX = Canvas->ClipX - W - 28 * S, CY = Canvas->ClipY - CardH2 - 28 * S;
        DrawRect(FLinearColor(0.01f, 0.015f, 0.02f, 0.45f), CX, CY, W, CardH2);
        Text(Clock, FLinearColor(0.95f, 0.94f, 0.9f), CX + W - Pad - Width(Clock, 1.35f), CY + Pad - 6 * S, 1.35f);
        Text(Line, FLinearColor(0.75f, 0.78f, 0.76f, 0.95f), CX + W - Pad - Width(Line, 0.8f), CY + Pad + 30 * S, 0.8f);
        Text(Line2, FLinearColor(0.68f, 0.72f, 0.72f, 0.9f), CX + W - Pad - Width(Line2, 0.8f), CY + Pad + 30 * S + LineH, 0.8f);
        WeatherTop = CY;
    }
    // Key hints for the first seconds of play only (the full list is in the Esc menu).
    const float HintAlpha = FMath::Clamp((15.0f - (Now - PlayStart)) / 2.0f, 0.0f, 1.0f);
    if (HintAlpha > 0)
    {
        const FString Hint = TEXT("Мышь — взгляд и удилище   ·   ЛКМ держать — мах / подъём   ·   отпустить — опустить снасть   ·   ЛКМ с рыбой — держать и отпускать на рывках   ·   ПКМ — подсечка   ·   Q — удилище   ·   +/− — глубина   ·   G — прикормка   ·   R — рогатка   ·   V — вид   ·   Esc — меню");
        Text(Hint, FLinearColor(0.85f, 0.87f, 0.85f, 0.75f * HintAlpha), Canvas->ClipX - Width(Hint, 0.75f) - 28 * S, WeatherTop - 30 * S, 0.75f);
    }
    if (bShowLife) DrawLifePanel(S);
}
void ALakeHUD::DrawLifePanel(float S)
{
    // "Live Fish": what every species is doing now, the latest signs seen on the water, and
    // markers over the fish within 30 m (green: feeding or hunting, blue: resting, grey: cruising).
    UFont* Font = GEngine->GetMediumFont();
    const ALakeLife* Life = ALakeLife::Get(GetWorld());
    const int32 Kinds = LakeFish::NumSpecies();
    TArray<int32> Count, Total, Food;
    Count.Init(0, Kinds * (int32)EFishGoal::Count);
    Total.Init(0, Kinds);
    Food.Init(0, Kinds * 8);   // what the feeding fish are on
    const FLinearColor GoalColour[] = { FLinearColor(0.7f, 0.72f, 0.72f), FLinearColor(0.45f, 0.95f, 0.45f), FLinearColor(0.45f, 0.7f, 1.0f) };
    const APlayerController* PC = GetOwningPlayerController();
    FVector ViewPos = FVector::ZeroVector;
    FRotator ViewRot = FRotator::ZeroRotator;
    if (PC) PC->GetPlayerViewPoint(ViewPos, ViewRot);
    for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
    {
        const ALakeFish* Fish = *It;
        if ((int32)Fish->Species >= Kinds) continue;
        ++Count[(int32)Fish->Species * (int32)EFishGoal::Count + (int32)Fish->GetGoal()];
        ++Total[(int32)Fish->Species];
        if (Fish->GetGoal() == EFishGoal::Feed) ++Food[(int32)Fish->Species * 8 + FMath::Clamp((int32)Fish->GetFeedOn(), 0, 7)];
        const FVector P = Fish->GetActorLocation();
        if (FVector::Dist(P, ViewPos) > 3000 || FVector::DotProduct(P - ViewPos, ViewRot.Vector()) < 50) continue;
        const FVector Screen = Canvas->Project(P);
        if (Screen.X < 0 || Screen.Y < 0 || Screen.X > Canvas->ClipX || Screen.Y > Canvas->ClipY) continue;
        const float Dot = 6 * S;
        DrawRect(GoalColour[(int32)Fish->GetGoal()].CopyWithNewOpacity(0.85f), Screen.X - Dot / 2, Screen.Y - Dot / 2, Dot, Dot);
    }
    TArray<TPair<FString, FLinearColor>> Lines;
    Lines.Add({ TEXT("Live Fish  (F3)"), FLinearColor(0.95f, 0.94f, 0.9f) });
    auto Of = [&](int32 Sp, EFishGoal G) { return Count[Sp * (int32)EFishGoal::Count + (int32)G]; };
    for (int32 Sp = 0; Sp < Kinds; ++Sp)
    {
        const EFishSpecies Species = (EFishSpecies)Sp;
        // The food most of the feeding ones are on.
        int32 Main = 0;
        for (int32 F = 1; F < 8; ++F) if (Food[Sp * 8 + F] > Food[Sp * 8 + Main]) Main = F;
        const FString On = Main > 0 ? FString::Printf(TEXT(" (%s)"), LakeFood::Name((EFishFood)Main)) : FString();
        Lines.Add({ FString::Printf(TEXT("%s %d:  %s %d%s  ·  %s %d  ·  %s %d"), *LakeFish::Info(Species).Name, Total[Sp],
            LakeFish::GoalName(Species, EFishGoal::Feed), Of(Sp, EFishGoal::Feed), *On,
            LakeFish::GoalName(Species, EFishGoal::Rest), Of(Sp, EFishGoal::Rest),
            LakeFish::GoalName(Species, EFishGoal::Cruise), Of(Sp, EFishGoal::Cruise)), FLinearColor(0.8f, 0.83f, 0.8f) });
    }
    if (Life)
    {
        int32 Fry = 0;
        for (const FFryShoal& Shoal : Life->FryShoals()) Fry += Shoal.Count;
        Lines.Add({ FString::Printf(TEXT("Малёк: %d стаек, около %d рыбок"), Life->FryShoals().Num(), Fry), FLinearColor(0.8f, 0.83f, 0.8f) });
        const float Now = GetWorld()->GetRealTimeSeconds();
        for (const ALakeLife::FNote& Note : Life->Notes())
            if (Now - Note.Time < 180)
                Lines.Add({ FString::Printf(TEXT("• %s  (%.0f с назад)"), *Note.Text, Now - Note.Time), FLinearColor(0.95f, 0.85f, 0.55f) });
    }
    Lines.Add({ TEXT("Точки над рыбами: зелёные — кормятся, синие — отдыхают, серые — гуляют"), FLinearColor(0.6f, 0.63f, 0.62f) });
    float W = 0;
    for (const auto& Line : Lines) { float LW, LH; GetTextSize(Line.Key, LW, LH, Font, 0.8f * S); W = FMath::Max(W, LW); }
    const float Pad = 14 * S, LineH = 24 * S, X = 28 * S, Y = 28 * S;
    DrawRect(FLinearColor(0.01f, 0.015f, 0.02f, 0.55f), X, Y, W + Pad * 2, Lines.Num() * LineH + Pad * 2 - 4 * S);
    for (int32 I = 0; I < Lines.Num(); ++I)
        DrawText(Lines[I].Key, Lines[I].Value, X + Pad, Y + Pad - 4 * S + I * LineH, Font, (I == 0 ? 0.9f : 0.8f) * S);
}
