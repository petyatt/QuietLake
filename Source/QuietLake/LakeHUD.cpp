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
#include "Engine/Texture2D.h"
#include "CanvasItem.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

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
    if (!bShowMap) DrawMinimap(S);
    if (bShowMap) DrawMap(S);
}

void ALakeHUD::BeginPlay()
{
    Super::BeginPlay();
#if !UE_BUILD_SHIPPING
    // -MapPreview: the minimap on its own, then the big map, then exit — a quick look at both.
    if (FParse::Param(FCommandLine::Get(), TEXT("MapPreview")))
    {
        FTimerHandle Shot, Open, Shot2, Exit;
        GetWorldTimerManager().SetTimer(Shot, [this]
        {
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Map_Minimap.png"), true, false);
        }, 12.0f, false);
        GetWorldTimerManager().SetTimer(Open, [this] { bShowMap = true; }, 13.5f, false);
        GetWorldTimerManager().SetTimer(Shot2, [this]
        {
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Map_Full.png"), true, false);
        }, 15.5f, false);
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 16.5f, false);
    }
#endif
}

void ALakeHUD::BuildMapTexture()
{
    if (MapTexture) return;
    // A square picture of the lake and its banks from the same SDF the world is built from: water by
    // depth, forest and meadow on land with a hill-shade, sand at the waterline, the pier and the
    // cleared fishing spots. Everything the angler needs to see where he is.
    const FBox2D Lake = LakeMap::LakeBounds();
    const FVector2D Centre = (Lake.Min + Lake.Max) * 0.5f;
    const FVector2D Extent = Lake.GetSize();
    MapWorldSize = FMath::Max(Extent.X, Extent.Y) + 26000.0f;   // 130 m of bank on each side
    MapOrigin = Centre - FVector2D(MapWorldSize * 0.5f, MapWorldSize * 0.5f);
    const int32 Size = 768;
    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(Size * Size);
    constexpr float Step = 200.0f;
    for (int32 Py = 0; Py < Size; ++Py)
    {
        const float Wy = MapOrigin.Y + (Py + 0.5f) / Size * MapWorldSize;
        for (int32 Px = 0; Px < Size; ++Px)
        {
            const float Wx = MapOrigin.X + (Px + 0.5f) / Size * MapWorldSize;
            FLinearColor C;
            const float Shore = LakeMap::ShoreDistance(Wx, Wy);   // cm, negative in the water
            if (LakeMap::IsWater(Wx, Wy))
            {
                const float Depth = LakeMap::Depth(Wx, Wy);   // cm
                // Sand-gold at the very edge, green in the shallows, then dark water.
                if (Depth < 30.0f) C = FLinearColor(0.50f, 0.53f, 0.36f);
                else if (Depth < 70.0f) C = FLinearColor(0.30f, 0.45f, 0.38f);
                else if (Depth < 130.0f) C = FLinearColor(0.16f, 0.33f, 0.39f);
                else if (Depth < 200.0f) C = FLinearColor(0.09f, 0.23f, 0.33f);
                else C = FLinearColor(0.05f, 0.14f, 0.24f);
                // A pale rim right at the waterline, so the shape reads (shore distance is
                // positive in the water: zero at the bank and growing towards the middle).
                if (Shore < 45.0f) C = FLinearColor(0.63f, 0.59f, 0.42f);
            }
            else
            {
                const FVector2D P(Wx, Wy);
                const float Forest = FMath::Clamp(LakeMap::ForestAmount(P), 0.0f, 1.0f);
                C = FMath::Lerp(FLinearColor(0.31f, 0.34f, 0.19f), FLinearColor(0.11f, 0.19f, 0.12f), Forest);
                // Relief: light from the north-west over the height field, gently.
                const float Hx = LakeMap::Ground(Wx + Step, Wy) - LakeMap::Ground(Wx - Step, Wy);
                const float Hy = LakeMap::Ground(Wx, Wy + Step) - LakeMap::Ground(Wx, Wy - Step);
                const float Shade = FMath::Clamp(1.0f - (Hx + Hy) * 0.0010f, 0.72f, 1.28f);
                C *= Shade;
                // The sandy strip along the water and the clearings of the fishing spots.
                if (Shore > -150.0f) C = FMath::Lerp(C, FLinearColor(0.54f, 0.50f, 0.34f), 1.0f + Shore / 150.0f);
                if (LakeMap::SpotAmount(P) > 0.25f) C = FMath::Lerp(C, FLinearColor(0.56f, 0.49f, 0.33f), 0.55f);
            }
            C.R = FMath::Clamp(C.R, 0.0f, 1.0f);
            C.G = FMath::Clamp(C.G, 0.0f, 1.0f);
            C.B = FMath::Clamp(C.B, 0.0f, 1.0f);
            Pixels[Py * Size + Px] = C.ToFColor(true);
        }
    }
    // The pier: a plank line from the bank into the water.
    const LakeMap::FPier& Pier = LakeMap::Pier();
    for (int32 I = 0; I <= 400; ++I)
    {
        const FVector2D P = Pier.Root + Pier.Dir * (Pier.Length * I / 400.0f);
        const int32 Px = FMath::FloorToInt((P.X - MapOrigin.X) / MapWorldSize * Size);
        const int32 Py = FMath::FloorToInt((P.Y - MapOrigin.Y) / MapWorldSize * Size);
        for (int32 Dy = -1; Dy <= 1; ++Dy)
            for (int32 Dx = -1; Dx <= 1; ++Dx)
                if (Px + Dx >= 0 && Px + Dx < Size && Py + Dy >= 0 && Py + Dy < Size)
                    Pixels[(Py + Dy) * Size + Px + Dx] = FColor(122, 96, 64);
    }
    MapTexture = UTexture2D::CreateTransient(Size, Size, PF_B8G8R8A8);
    if (!MapTexture) return;
    MapTexture->Filter = TF_Bilinear;
    MapTexture->SRGB = true;
    MapTexture->AddressX = TA_Clamp;
    MapTexture->AddressY = TA_Clamp;
    FTexture2DMipMap& Mip = MapTexture->GetPlatformData()->Mips[0];
    void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
    Mip.BulkData.Unlock();
    MapTexture->UpdateResource();
}

namespace
{
    /** Where a world point lands on the map picture (0..1 of its edge, y down). */
    FVector2D WorldToMap(const FVector2D& World, const FVector2D& Origin, float WorldSize)
    {
        return FVector2D((World.X - Origin.X) / WorldSize, (World.Y - Origin.Y) / WorldSize);
    }
}

void ALakeHUD::DrawMinimap(float S)
{
    if (!Canvas) return;
    BuildMapTexture();
    if (!MapTexture) return;
    const float Size = FMath::Clamp(230.0f * S, 140.0f, 320.0f);
    const float Margin = 28.0f * S;
    const float X = Canvas->ClipX - Size - Margin;
    const float Y = Margin;
    DrawRect(FLinearColor(0.01f, 0.015f, 0.02f, 0.55f), X - 5, Y - 5, Size + 10, Size + 10);
    FCanvasTileItem Tile(FVector2D(X, Y), MapTexture->GetResource(), FVector2D(Size, Size), FLinearColor(1, 1, 1, 0.95f));
    Tile.BlendMode = SE_BLEND_Translucent;
    Canvas->DrawItem(Tile);
    // A thin border, then the angler: a dot with a line for the facing.
    const FLinearColor Edge(0.75f, 0.78f, 0.74f, 0.35f);
    DrawRect(Edge, X, Y, Size, 2); DrawRect(Edge, X, Y + Size - 2, Size, 2);
    DrawRect(Edge, X, Y, 2, Size); DrawRect(Edge, X + Size - 2, Y, 2, Size);
    if (const APawn* Pawn = GetOwningPawn())
    {
        const FVector2D At = WorldToMap(FVector2D(Pawn->GetActorLocation()), MapOrigin, MapWorldSize) * Size + FVector2D(X, Y);
        const float Yaw = FMath::DegreesToRadians(Pawn->GetActorRotation().Yaw);
        const FVector2D Dir(FMath::Cos(Yaw), FMath::Sin(Yaw));
        DrawLine(At.X, At.Y, At.X + Dir.X * 11.0f * S, At.Y + Dir.Y * 11.0f * S, FLinearColor(1.0f, 0.86f, 0.35f), 2.0f * S);
        DrawRect(FLinearColor(1.0f, 0.86f, 0.35f), At.X - 2.5f * S, At.Y - 2.5f * S, 5.0f * S, 5.0f * S);
    }
}

void ALakeHUD::DrawMap(float S)
{
    if (!Canvas) return;
    BuildMapTexture();
    if (!MapTexture) return;
    // About 70% of the shorter side, centred: the lake, the places and the angler, not the whole screen.
    const float Side = FMath::Min(Canvas->ClipX, Canvas->ClipY) * 0.70f;
    const float X = (Canvas->ClipX - Side) * 0.5f, Y = (Canvas->ClipY - Side) * 0.5f;
    DrawRect(FLinearColor(0.01f, 0.015f, 0.02f, 0.75f), X - 14, Y - 14, Side + 28, Side + 28);
    FCanvasTileItem Tile(FVector2D(X, Y), MapTexture->GetResource(), FVector2D(Side, Side), FLinearColor::White);
    Tile.BlendMode = SE_BLEND_Opaque;
    Canvas->DrawItem(Tile);
    const FLinearColor Edge(0.78f, 0.80f, 0.76f, 0.5f);
    DrawRect(Edge, X, Y, Side, 2); DrawRect(Edge, X, Y + Side - 2, Side, 2);
    DrawRect(Edge, X, Y, 2, Side); DrawRect(Edge, X + Side - 2, Y, 2, Side);
    auto At = [&](const FVector2D& World) { return WorldToMap(World, MapOrigin, MapWorldSize) * Side + FVector2D(X, Y); };
    auto Label = [this, S](const FString& Line, const FVector2D& P, const FLinearColor& Colour, float Scale)
    {
        float W = 0, H = 0;
        GetTextSize(Line, W, H, GEngine->GetMediumFont(), Scale * S);
        DrawText(Line, Colour, P.X - W * 0.5f, P.Y - H - 3.0f * S, GEngine->GetMediumFont(), Scale * S);
    };
    // The fishing places: a dot and the name.
    for (const LakeMap::FSpot& Spot : LakeMap::Spots())
    {
        const FVector2D P = At(Spot.Location);
        DrawRect(FLinearColor(1.0f, 0.92f, 0.6f, 0.95f), P.X - 3.0f * S, P.Y - 3.0f * S, 6.0f * S, 6.0f * S);
        Label(Spot.Name, P, FLinearColor(0.95f, 0.93f, 0.86f, 0.92f), 0.62f);
    }
    // The camp and the pier, then the angler.
    const FVector2D Camp = At(LakeMap::StartSpot());
    DrawRect(FLinearColor(0.95f, 0.62f, 0.25f), Camp.X - 4.0f * S, Camp.Y - 4.0f * S, 8.0f * S, 8.0f * S);
    Label(TEXT("Лагерь"), Camp, FLinearColor(1.0f, 0.85f, 0.55f), 0.62f);
    const LakeMap::FPier& Pier = LakeMap::Pier();
    const FVector2D PierA = At(Pier.Root), PierB = At(Pier.Root + Pier.Dir * Pier.Length);
    DrawLine(PierA.X, PierA.Y, PierB.X, PierB.Y, FLinearColor(0.85f, 0.7f, 0.45f), 2.0f * S);
    if (const APawn* Pawn = GetOwningPawn())
    {
        const FVector2D P = At(FVector2D(Pawn->GetActorLocation()));
        const float Yaw = FMath::DegreesToRadians(Pawn->GetActorRotation().Yaw);
        const FVector2D Dir(FMath::Cos(Yaw), FMath::Sin(Yaw));
        DrawLine(P.X, P.Y, P.X + Dir.X * 16.0f * S, P.Y + Dir.Y * 16.0f * S, FLinearColor(1.0f, 0.86f, 0.35f), 3.0f * S);
        DrawRect(FLinearColor(1.0f, 0.86f, 0.35f), P.X - 3.5f * S, P.Y - 3.5f * S, 7.0f * S, 7.0f * S);
        Label(TEXT("Вы"), P, FLinearColor(1.0f, 0.9f, 0.5f), 0.6f);
    }
    // Title and the key hint.
    UFont* Font = GEngine->GetMediumFont();
    DrawText(TEXT("Озеро"), FLinearColor(0.95f, 0.93f, 0.86f, 0.95f), X, Y - 34.0f * S, Font, 1.15f * S);
    const FString Hint = TEXT("M — закрыть карту");
    float W = 0, H = 0;
    GetTextSize(Hint, W, H, Font, 0.75f * S);
    DrawText(Hint, FLinearColor(0.8f, 0.82f, 0.78f, 0.75f), X + Side - W, Y - 30.0f * S, Font, 0.75f * S);
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
