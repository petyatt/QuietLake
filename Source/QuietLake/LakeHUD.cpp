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
    bool bParkedBite = false;
    int32 OnStands = 0;
    for (const FLakeRodSlot& Slot : Fisher->Rods)
    {
        OnStands += Slot.Place == ERodPlace::Stand;
        bParkedBite |= Slot.Place == ERodPlace::Stand && Slot.State == EFishingState::Bite;
    }
    const bool bUrgent = Fisher->FishingState >= EFishingState::Bite || bParkedBite;
    const float Age = Now - StatusTime;
    const float Alpha = bUrgent ? 1.0f : FMath::Clamp((6.0f - Age) / 1.0f, 0.0f, 1.0f);
    const FString Rod = Fisher->HasRodInHands() ? FString::Printf(TEXT("Удилище: %s"), ALakeCharacter::RodName(Fisher->RodKind)) : FString(TEXT("Руки свободны"));
    FString Info = FString::Printf(TEXT("Улов: %d   ·   Наживка: %s   ·   Глубина: %.0f см   ·   %s   ·   Прикормка: %d"), Fisher->CatchCount, LakeFish::BaitName(Fisher->Bait), Fisher->FloatDepth, *Rod, Fisher->GroundbaitLeft);
    if (OnStands > 0) Info += FString::Printf(TEXT("   ·   На рогатках: %d"), OnStands);
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
        const FLinearColor Colour = Fisher->FishingState == EFishingState::Bite || bParkedBite ? FLinearColor(1.0f, 0.82f, 0.25f) : FLinearColor(0.93f, 0.91f, 0.86f, Alpha);
        Text(LastStatus, Colour, X + Pad, Y, 0.95f);
        Y += LineH;
    }
    Text(Info, FLinearColor(0.7f, 0.74f, 0.72f, 0.9f), X + Pad, Y, 0.85f);

    // Line tension while fighting a fish.
    const float BarW = 360 * S, BarH = 8 * S, BarX = (Canvas->ClipX - BarW) / 2, BarY = Canvas->ClipY - 70 * S;
    // The cast is aimed by the eyes: a ring on the water where the rig will be laid. Faint while the
    // rig is in the hand ready to cast, bright while LMB is held.
    if (Fisher->HasRodInHands() && Fisher->FishingState == EFishingState::Idle && (Fisher->bCharging || Fisher->bRigInHand))
    {
        const FVector Aim = Fisher->CastAimPoint();
        if (LakeMap::IsWater(Aim.X, Aim.Y) && FVector::Dist2D(Aim, Fisher->GetActorLocation()) < Fisher->CastReach() + 60)
        {
            const FLinearColor Ring(1.0f, 0.93f, 0.55f, Fisher->bCharging ? 0.95f : 0.35f);
            constexpr int32 Points = 28;
            FVector Last = Canvas->Project(Aim + FVector(32, 0, 0.5f));
            for (int32 I = 1; I <= Points; ++I)
            {
                const float A = 2 * PI * I / Points;
                const FVector Next = Canvas->Project(Aim + FVector(32 * FMath::Cos(A), 32 * FMath::Sin(A), 0.5f));
                if (Last.Z > 0 && Next.Z > 0) DrawLine(Last.X, Last.Y, Next.X, Next.Y, Ring, (Fisher->bCharging ? 2.0f : 1.2f) * S);
                Last = Next;
            }
            if (Fisher->bCharging)
            {
                const FString Label = FString::Printf(TEXT("Заброс на %.1f м — отпустите ЛКМ"), FVector::Dist2D(Aim, Fisher->GetActorLocation()) / 100).Replace(TEXT("."), TEXT(","));
                Text(Label, FLinearColor(0.93f, 0.91f, 0.86f), (Canvas->ClipX - Width(Label, 0.8f)) / 2, BarY - 26 * S, 0.8f);
            }
        }
        else if (Fisher->bCharging)
        {
            const FString Label(TEXT("Отсюда до воды не достать — подойдите ближе к берегу"));
            Text(Label, FLinearColor(0.93f, 0.91f, 0.86f), (Canvas->ClipX - Width(Label, 0.8f)) / 2, BarY - 26 * S, 0.8f);
        }
    }
    if (Fisher->FishingState == EFishingState::Fight)
    {
        // The line's load in kilograms-force against what it stands (the bar's whole length). Green:
        // the hands hold it. Amber, past the notch: the hook's hold wears, ease off. Red: about to break.
        const FLakeTackle Gear = Fisher->Tackle();
        const float Safe = FMath::Min(1.0f, Gear.HookSafe / Gear.LineBreak);
        const float Shown = Fisher->Tension;
        const FLinearColor Fill = Shown > 0.8f ? FLinearColor(0.95f, 0.25f, 0.2f) : Shown > Safe ? FLinearColor(0.96f, 0.72f, 0.2f) : FLinearColor(0.35f, 0.8f, 0.55f);
        DrawRect(FLinearColor(0, 0, 0, 0.4f), BarX, BarY, BarW, BarH);
        DrawRect(Fill, BarX, BarY, BarW * Shown, BarH);
        DrawRect(FLinearColor(0.93f, 0.91f, 0.86f, 0.8f), BarX + BarW * Safe - 1 * S, BarY - 3 * S, 2 * S, BarH + 6 * S);
        const FString Load = FString::Printf(TEXT("%.1f кг"), Fisher->LineLoadGf / 1000).Replace(TEXT("."), TEXT(","));
        const FString Label = Shown > 0.8f ? FString(Fisher->bRodStrained ? TEXT("Удилище на пределе — опустите его к рыбе") : TEXT("Леска на пределе — шагните к рыбе"))
            : Shown > Safe ? FString::Printf(TEXT("Рыба рвётся: %s — крючок может вырвать"), *Load)
            : FString::Printf(TEXT("Тянет %s   ·   до рыбы %.1f м"), *Load, Fisher->FishDistance / 100);
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
        const FString Hint = TEXT("Мышь — взгляд и удилище   ·   ЛКМ держать — прицел, отпустить — заброс в круг   ·   ЛКМ после заброса — поднять снасть   ·   ЛКМ с рыбой — тянуть   ·   ПКМ — подсечка   ·   +/− — глубина   ·   G — прикормка   ·   R — рогатка (держать — на землю)   ·   B — чехол   ·   F — открыть / взять   ·   V — вид   ·   Esc — меню");
        Text(Hint, FLinearColor(0.85f, 0.87f, 0.85f, 0.75f * HintAlpha), Canvas->ClipX - Width(Hint, 0.75f) - 28 * S, WeatherTop - 30 * S, 0.75f);
    }
    DrawInteraction(Fisher, S);
    if (Fisher->IsCaseWindowOpen()) DrawCaseWindow(Fisher, S);
    else DragRod = -1;
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

// --- the rod case ------------------------------------------------------------------------------

namespace
{
    const FLinearColor RodColour[] = { FLinearColor(0.55f, 0.58f, 0.56f), FLinearColor(0.50f, 0.36f, 0.20f), FLinearColor(0.84f, 0.64f, 0.18f) };
    const FLinearColor Cork(0.70f, 0.52f, 0.31f);
}

int32 ALakeHUD::SlotUnderMouse() const
{
    const APlayerController* PC = GetOwningPlayerController();
    float X = 0, Y = 0;
    if (!PC || !PC->GetMousePosition(X, Y)) return -1;
    for (int32 I = 0; I < 4; ++I)
        if (SlotRects[I].bIsValid && SlotRects[I].IsInside(FVector2D(X, Y))) return I;
    return -1;
}

void ALakeHUD::CaseWindowPress()
{
    const ALakeCharacter* Fisher = Cast<ALakeCharacter>(GetOwningPawn());
    const int32 Slot = SlotUnderMouse();
    DragRod = -1;
    if (!Fisher || Slot < 0) return;
    for (int32 Rod = 0; Rod < Fisher->Rods.Num(); ++Rod)
    {
        const FLakeRodSlot& R = Fisher->Rods[Rod];
        if ((Slot < 3 && R.Place == ERodPlace::Case && R.CaseSlot == Slot) || (Slot == 3 && R.Place == ERodPlace::Hands)) DragRod = Rod;
    }
}

bool ALakeHUD::CaseWindowRelease(int32& OutRod, int32& OutTarget)
{
    const ALakeCharacter* Fisher = Cast<ALakeCharacter>(GetOwningPawn());
    const int32 Rod = DragRod, Slot = SlotUnderMouse();
    DragRod = -1;
    if (!Fisher || Rod < 0 || Slot < 0 || !Fisher->Rods.IsValidIndex(Rod)) return false;
    const FLakeRodSlot& R = Fisher->Rods[Rod];
    const int32 From = R.Place == ERodPlace::Hands ? 3 : R.CaseSlot;
    if (From == Slot) return false;
    OutRod = Rod;
    OutTarget = Slot;
    return true;
}

void ALakeHUD::DrawCaseWindow(ALakeCharacter* Fisher, float S)
{
    // Three tall compartments side by side, the hand apart on the right; the rods stand in them.
    UFont* Font = GEngine->GetMediumFont();
    const float U = S / 1.4f;
    const float ColW = 96 * U, ColH = 330 * U, Gap = 10 * U, Sep = 26 * U, Pad = 18 * U;
    const float W = Pad * 2 + ColW * 4 + Gap * 2 + Sep * 2, H = Pad * 2 + 44 * U + ColH + 34 * U + 30 * U;
    const float X0 = (Canvas->ClipX - W) / 2, Y0 = (Canvas->ClipY - H) / 2;
    DrawRect(FLinearColor(0.09f, 0.11f, 0.10f, 0.96f), X0, Y0, W, H);
    DrawText(TEXT("Чехол для удочек"), FLinearColor(0.93f, 0.92f, 0.88f), X0 + Pad, Y0 + Pad - 4 * U, Font, 1.0f * S);
    const float ColY = Y0 + Pad + 44 * U;
    const int32 Hover = DragRod >= 0 ? SlotUnderMouse() : -1;
    auto Outline = [this](float X, float Y, float Wd, float Ht, const FLinearColor& C, float T)
    {
        DrawRect(C, X, Y, Wd, T); DrawRect(C, X, Y + Ht - T, Wd, T);
        DrawRect(C, X, Y, T, Ht); DrawRect(C, X + Wd - T, Y, T, Ht);
    };
    // A folded rod standing: the blank tapering up from the cork handle at the bottom.
    auto DrawRod = [&](int32 Rod, float CX, float Top, float Bottom, float Alpha)
    {
        const float Handle = 56 * U;
        const FLinearColor C = RodColour[FMath::Clamp(Rod, 0, 2)].CopyWithNewOpacity(Alpha);
        for (int32 I = 0; I < 12; ++I)
        {
            const float T0 = I / 12.0f;
            const float Wd = FMath::Lerp(3.0f, 7.0f, T0) * U;
            const float Y = FMath::Lerp(Top, Bottom - Handle, T0);
            DrawRect(C, CX - Wd / 2, Y, Wd, (Bottom - Handle - Top) / 12 + 1);
        }
        DrawRect(Cork.CopyWithNewOpacity(Alpha), CX - 6 * U, Bottom - Handle, 12 * U, Handle);
    };
    for (int32 Slot = 0; Slot < 4; ++Slot)
    {
        const float X = X0 + Pad + Slot * (ColW + Gap) + (Slot == 3 ? Sep * 2 - Gap : 0);
        SlotRects[Slot] = FBox2D(FVector2D(X, ColY), FVector2D(X + ColW, ColY + ColH));
        DrawRect(Hover == Slot ? FLinearColor(0.25f, 0.45f, 0.75f, 0.35f) : FLinearColor(1, 1, 1, 0.04f), X, ColY, ColW, ColH);
        Outline(X, ColY, ColW, ColH, Slot == 3 ? FLinearColor(0.91f, 0.77f, 0.35f, 0.6f) : FLinearColor(1, 1, 1, 0.2f), 1.5f * U);
        const FString No = Slot == 3 ? FString(TEXT("рука")) : FString::FromInt(Slot + 1);
        float TW = 0, TH = 0;
        GetTextSize(No, TW, TH, Font, 0.7f * S);
        DrawText(No, FLinearColor(0.6f, 0.65f, 0.62f), X + (ColW - TW) / 2, ColY + 6 * U, Font, 0.7f * S);
        int32 Rod = -1;
        for (int32 R = 0; R < Fisher->Rods.Num(); ++R)
        {
            const FLakeRodSlot& RS = Fisher->Rods[R];
            if ((Slot < 3 && RS.Place == ERodPlace::Case && RS.CaseSlot == Slot) || (Slot == 3 && RS.Place == ERodPlace::Hands)) Rod = R;
        }
        if (Rod < 0)
        {
            GetTextSize(TEXT("пусто"), TW, TH, Font, 0.7f * S);
            DrawText(TEXT("пусто"), FLinearColor(0.6f, 0.65f, 0.62f), X + (ColW - TW) / 2, ColY + ColH / 2 - TH / 2, Font, 0.7f * S);
            continue;
        }
        const bool bDragged = Rod == DragRod;
        DrawRod(Rod, X + ColW / 2, ColY + 34 * U, ColY + ColH - 62 * U, bDragged ? 0.3f : 1.0f);
        static const TCHAR* ShortName[] = { TEXT("Лёгкое"), TEXT("Среднее"), TEXT("Тяжёлое") };
        const FString Short = ShortName[FMath::Clamp(Rod, 0, 2)];
        const FString Grams = FString::Printf(TEXT("%.0f г"), ALakeCharacter::RodGrams((ERodKind)Rod));
        GetTextSize(Short, TW, TH, Font, 0.72f * S);
        DrawText(Short, FLinearColor(0.92f, 0.92f, 0.88f), X + (ColW - TW) / 2, ColY + ColH - 54 * U, Font, 0.72f * S);
        GetTextSize(Grams, TW, TH, Font, 0.62f * S);
        DrawText(Grams, FLinearColor(0.6f, 0.65f, 0.62f), X + (ColW - TW) / 2, ColY + ColH - 30 * U, Font, 0.62f * S);
    }
    // The divider between the case and the hand, and the group labels.
    DrawRect(FLinearColor(1, 1, 1, 0.14f), X0 + Pad + 3 * ColW + 2 * Gap + Sep - 0.5f * U, ColY, 1.5f * U, ColH);
    float TW = 0, TH = 0;
    GetTextSize(TEXT("В чехле"), TW, TH, Font, 0.7f * S);
    DrawText(TEXT("В чехле"), FLinearColor(0.6f, 0.65f, 0.62f), X0 + Pad + (3 * ColW + 2 * Gap - TW) / 2, ColY + ColH + 8 * U, Font, 0.7f * S);
    GetTextSize(TEXT("В руках"), TW, TH, Font, 0.7f * S);
    DrawText(TEXT("В руках"), FLinearColor(0.6f, 0.65f, 0.62f), SlotRects[3].Min.X + (ColW - TW) / 2, ColY + ColH + 8 * U, Font, 0.7f * S);
    const FString Hint = TEXT("Перетащите удочку в руку или обратно   ·   F или Esc — закрыть");
    GetTextSize(Hint, TW, TH, Font, 0.66f * S);
    DrawText(Hint, FLinearColor(0.6f, 0.65f, 0.62f), X0 + (W - TW) / 2, Y0 + H - Pad - TH + 4 * U, Font, 0.66f * S);
    // The rod being dragged follows the mouse.
    float MX = 0, MY = 0;
    if (DragRod >= 0 && GetOwningPlayerController() && GetOwningPlayerController()->GetMousePosition(MX, MY))
        DrawRod(DragRod, MX, MY - ColH * 0.45f, MY + ColH * 0.35f, 0.9f);
}

void ALakeHUD::DrawInteraction(ALakeCharacter* Fisher, float S)
{
    UFont* Font = GEngine->GetMediumFont();
    const float U = S / 1.4f;
    const float Now = GetWorld()->GetRealTimeSeconds();
    const float CX = Canvas->ClipX / 2, CY = Canvas->ClipY / 2;
    auto Centred = [&](const FString& Line, float Y, const FLinearColor& C, float Scale)
    {
        float W = 0, H = 0;
        GetTextSize(Line, W, H, Font, Scale * S);
        DrawText(Line, C, CX - W / 2, Y, Font, Scale * S);
        return H;
    };
    // The ghost: what LMB and RMB do, or why it cannot go there.
    if (Fisher->Placing != ALakeCharacter::EPlacing::None)
    {
        const FString What = Fisher->Placing == ALakeCharacter::EPlacing::Case ? TEXT("Чехол") : TEXT("Удочка");
        const FString Line = Fisher->bPlaceValid ? What + TEXT(": ЛКМ — положить сюда   ·   ПКМ — отмена")
            : Fisher->PlaceProblem + TEXT("   ·   ПКМ — отмена");
        Centred(Line, Canvas->ClipY - 150 * U, Fisher->bPlaceValid ? FLinearColor(0.75f, 0.86f, 1.0f) : FLinearColor(1.0f, 0.55f, 0.45f), 0.8f);
        return;
    }
    // R held: how long until the rod's ghost shows.
    if (Fisher->LayDown >= 0 && !Fisher->bLayDone && Fisher->CanPutRodAway() && Now - Fisher->LayDown > 0.25f)
    {
        const float Fill = FMath::Clamp((Now - Fisher->LayDown) / ALakeCharacter::LayHold, 0.0f, 1.0f);
        const float BW = 220 * U, BH = 5 * U, BY = Canvas->ClipY - 150 * U;
        Centred(TEXT("Держите R — положить удочку на землю"), BY - 30 * U, FLinearColor(0.9f, 0.9f, 0.86f), 0.75f);
        DrawRect(FLinearColor(1, 1, 1, 0.15f), CX - BW / 2, BY, BW, BH);
        DrawRect(FLinearColor(0.91f, 0.77f, 0.35f), CX - BW / 2, BY, BW * Fill, BH);
    }
    if (Fisher->AimOptions.Num() == 0 || Fisher->IsCaseWindowOpen()) return;
    // The crosshair, and the lines of the prompt under it; the wheel picks one, F held does it.
    DrawRect(FLinearColor(0.95f, 0.95f, 0.92f, 0.85f), CX - 2.5f * U, CY - 2.5f * U, 5 * U, 5 * U);
    float W = 0;
    for (const FString& Option : Fisher->AimOptions) { float LW = 0, LH = 0; GetTextSize(Option, LW, LH, Font, 0.85f * S); W = FMath::Max(W, LW); }
    const float LineH = 30 * U, Pad = 10 * U;
    const float BoxW = W + Pad * 2 + 24 * U, BoxH = Fisher->AimOptions.Num() * LineH + Pad * 2 + 10 * U;
    const float BX = CX - BoxW / 2, BY = CY + 26 * U;
    DrawRect(FLinearColor(0.03f, 0.05f, 0.04f, 0.6f), BX, BY, BoxW, BoxH);
    for (int32 I = 0; I < Fisher->AimOptions.Num(); ++I)
    {
        const bool bSel = I == Fisher->AimChoice;
        const float Y = BY + Pad + I * LineH;
        if (bSel) DrawRect(FLinearColor(1, 1, 1, 0.09f), BX + 4 * U, Y - 2 * U, BoxW - 8 * U, LineH - 2 * U);
        DrawText((bSel ? TEXT("> ") : TEXT("   ")) + Fisher->AimOptions[I], bSel ? FLinearColor(0.96f, 0.95f, 0.9f) : FLinearColor(0.62f, 0.67f, 0.64f),
            BX + Pad, Y, Font, 0.85f * S);
    }
    const float Fill = Fisher->InteractDown >= 0 ? FMath::Clamp((Now - Fisher->InteractDown) / ALakeCharacter::InteractHold, 0.0f, 1.0f) : 0.0f;
    DrawRect(FLinearColor(1, 1, 1, 0.12f), BX + Pad, BY + BoxH - Pad - 4 * U, BoxW - Pad * 2, 4 * U);
    DrawRect(FLinearColor(0.91f, 0.77f, 0.35f), BX + Pad, BY + BoxH - Pad - 4 * U, (BoxW - Pad * 2) * Fill, 4 * U);
    Centred(Fisher->AimOptions.Num() > 1 ? TEXT("колесо — выбрать   ·   зажмите F") : TEXT("зажмите F"), BY + BoxH + 6 * U, FLinearColor(0.75f, 0.78f, 0.75f, 0.85f), 0.68f);
}
