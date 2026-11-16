#include "LakeWaves.h"
#include "LakeFish.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyComponent.h"
#include "WaterSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
    // The longest stretch of open water the wind blows over (m): it limits how big the waves grow.
    constexpr float FetchM = 500.0f;
    constexpr int32 WaveCount = 24;
    constexpr float ShortestCm = 110.0f, LongestCm = 420.0f;
    // Short wind waves feel the bottom where the water is shallower than about half their length:
    // the engine fades them as 1 - exp(-2 depth / this).
    constexpr float WaveMaskDepthCm = 60.0f;
}

float ULakeWaveGenerator::SignificantHeight(float WindSpeed)
{
    // Fetch-limited growth (JONSWAP): Hs = 0.0016 U sqrt(F / g), and half of that here: the banks
    // and the forest shelter a village pond, and waves of the full height looked too big for it.
    // Below a light breeze the lake stays smooth.
    const float U = FMath::Max(0.0f, WindSpeed);
    return 100.0f * 0.0008f * U * FMath::Sqrt(FetchM / 9.81f) * FMath::SmoothStep(1.0f, 3.0f, U);
}

float ULakeWaveGenerator::PeakWavelength(float WindSpeed)
{
    // Peak period Tp = 0.286 (g F / U^2)^(1/3) U / g, deep-water wavelength g Tp^2 / 2 pi.
    const float U = FMath::Max(2.0f, WindSpeed);
    const float Tp = 0.286f * FMath::Pow(9.81f * FetchM / (U * U), 1.0f / 3.0f) * U / 9.81f;
    return FMath::Clamp(100.0f * 9.81f * Tp * Tp / (2 * PI), 130.0f, 380.0f);
}

float ULakeWaveGenerator::Ripple(float WindSpeed)
{
    // Cat's paws show at about 1 m/s; by a fresh breeze the whole lake is rippled.
    return FMath::SmoothStep(0.6f, 5.0f, WindSpeed);
}

void ULakeWaveGenerator::MakeWaves(float WindSpeed, float WindYaw, TArray<FGerstnerWave>& OutWaves)
{
    const float Hs = SignificantHeight(WindSpeed);
    const float LogPeak = FMath::Loge(PeakWavelength(WindSpeed));
    const float Wind = FMath::DegreesToRadians(WindYaw);
    float Weights[WaveCount];
    float Sum = 0;
    for (int32 I = 0; I < WaveCount; ++I)
    {
        // Directions by the golden angle, wavelengths by the golden ratio: quasi-random and fixed.
        const float Angle = FMath::DegreesToRadians(FMath::Fmod(I * 137.508f, 360.0f));
        const float Length = ShortestCm * FMath::Pow(LongestCm / ShortestCm, FMath::Frac(I * 0.618034f + 0.31f));
        const float Ahead = FMath::Max(0.0f, FMath::Cos(Angle - Wind));
        Weights[I] = FMath::Exp(-FMath::Square(FMath::Loge(Length) - LogPeak) / (2 * 0.45f * 0.45f)) * Ahead * Ahead;
        Sum += Weights[I];
        FGerstnerWave& W = OutWaves.AddDefaulted_GetRef();
        W.WaveLength = Length;
        W.Direction = FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0);
    }
    for (int32 I = 0; I < WaveCount; ++I)
    {
        FGerstnerWave& W = OutWaves[OutWaves.Num() - WaveCount + I];
        // The heights share the energy of the sea state: sum of a^2 = Hs^2 / 8. (Never exactly zero:
        // the engine divides by the amplitude.)
        W.Amplitude = FMath::Max(0.001f, Hs / FMath::Sqrt(8.0f) * FMath::Sqrt(Sum > 0 ? Weights[I] / Sum : 0.0f));
        // Sharper crests as a wave grows steep; a faint one stays a smooth swell.
        W.Steepness = FMath::Clamp(1.2f * W.Amplitude * 2 * PI / W.WaveLength, 0.0f, 0.5f);
    }
}

void ULakeWaveGenerator::GenerateGerstnerWaves_Implementation(TArray<FGerstnerWave>& OutWaves) const
{
    MakeWaves(WindSpeed, WindYaw, OutWaves);
}

ULakeWaves* ULakeWaves::Get(const UWorld* World)
{
    return World ? World->GetSubsystem<ULakeWaves>() : nullptr;
}

float ULakeWaves::SurfaceAt(const UWorld* World, const FVector2D& P, FVector* OutNormal)
{
    const ULakeWaves* Self = Get(World);
    if (!Self)
    {
        if (OutNormal) *OutNormal = FVector::UpVector;
        return LakeFish::SurfaceZ;
    }
    return LakeFish::SurfaceZ + Self->WaveAt(P, OutNormal);
}

float ULakeWaves::WaveAt(const FVector2D& P, FVector* OutNormal) const
{
    if (OutNormal) *OutNormal = FVector::UpVector;
    const UWaterBodyComponent* Body = Lake.Get();
    if (!Body || !Waves || Built <= 0) return 0.0f;
    const float Depth = LakeFish::SurfaceZ - LakeMap::Ground(P.X, P.Y);
    if (Depth <= 0) return 0.0f;
    FWaveInfo Info;
    if (!Body->GetWaveInfoAtPosition(FVector(P, LakeFish::SurfaceZ), Depth, OutNormal == nullptr, Info)) return 0.0f;
    if (OutNormal) *OutNormal = Info.Normal;
    return Info.Height;
}

void ULakeWaves::Setup()
{
    AWaterBodyLake* LakeActor = nullptr;
    for (TActorIterator<AWaterBodyLake> It(GetWorld()); It; ++It) { LakeActor = *It; break; }
    if (!LakeActor) return;
    UWaterBodyComponent* Body = LakeActor->GetWaterBodyComponent();
    if (!Body) return;
    Body->TargetWaveMaskDepth = WaveMaskDepthCm;
    Waves = NewObject<UGerstnerWaterWaves>(this);
    Generator = NewObject<ULakeWaveGenerator>(Waves);
    Waves->GerstnerWaveGenerator = Generator;
    // Introduced at the strongest wind, so the water body's bounds and tiles cover the highest
    // waves; then built for the wind there is.
    Generator->WindSpeed = 12.0f;
    Waves->RecomputeWaves(false);
    LakeActor->SetWaterWaves(Waves);
    Lake = Body;
    Built = -1.0f;
}

void ULakeWaves::Rebuild(float Speed, float Yaw)
{
    Generator->WindSpeed = Speed;
    Generator->WindYaw = Yaw;
    Waves->RecomputeWaves(false);
    Built = Speed;
    BuiltYaw = Yaw;
}

void ULakeWaves::ApplyRipple(float Speed)
{
    // The lake's fine ripple is M_VillageWater's (the engine's pans one normal map four ways at a
    // fixed strength: a stamped pattern on a still lake, and running against the wind as much as
    // with it). Only a faint slow swell of it is kept, the same in any wind: a mirror in a calm,
    // and in a wind the waves themselves with nothing laid over them (a fine ripple on top of the
    // waves looked unnatural). It drifts downwind; the drift is summed in Tick, so a change of
    // wind turns its course without the pattern jumping.
    UWaterBodyComponent* Body = Lake.Get();
    UMaterialInstanceDynamic* Material = Body ? Body->GetWaterMaterialInstance() : nullptr;
    if (!Material) return;
    const float Ripple = ULakeWaveGenerator::Ripple(Speed);
    Material->SetScalarParameterValue(TEXT("WindRipple"), 0.012f);
    Material->SetVectorParameterValue(TEXT("RippleDrift"), FLinearColor(FineDrift.X, FineDrift.Y, MidDrift.X, MidDrift.Y));
    ShownRipple = Ripple;
}

void ULakeWaves::Tick(float DeltaTime)
{
    UWorld* World = GetWorld();
    if (!World) return;
    const double Now = World->GetTimeSeconds();
    // One clock for the waves everywhere: the server's.
    if (UWaterSubsystem* Water = UWaterSubsystem::GetWaterSubsystem(World))
    {
        const AGameStateBase* GameState = World->GetGameState();
        Water->SetOverrideSmoothedWorldTimeSeconds(GameState ? GameState->GetServerWorldTimeSeconds() : Now);
        Water->SetShouldOverrideSmoothedWorldTimeSeconds(true);
    }
    if (!Lake.IsValid())
    {
        if (Now < NextSetup) return;
        NextSetup = Now + 2.0;
        Setup();
        if (!Lake.IsValid()) return;
    }
    const ALakeWeather* Weather = ALakeWeather::Get(World);
    const float Speed = Weather ? Weather->WindSpeed() : 0.0f;
    const float Yaw = Weather ? Weather->Current().WindYaw : 0.0f;
    // Waves take time to build up and die down: follow the wind over several seconds.
    if (SmoothWind < 0)
    {
        SmoothWind = Speed;
        SmoothYaw = Yaw;
    }
    const float Blend = 1 - FMath::Exp(-DeltaTime / 6.0f);
    SmoothWind += (Speed - SmoothWind) * Blend;
    SmoothYaw += FMath::FindDeltaAngleDegrees(SmoothYaw, Yaw) * Blend;
    if (Built < 0 || (Now >= NextRebuild && (FMath::Abs(SmoothWind - Built) > 0.15f || FMath::Abs(FMath::FindDeltaAngleDegrees(BuiltYaw, SmoothYaw)) > 4.0f)))
    {
        Rebuild(SmoothWind, SmoothYaw);
        NextRebuild = Now + 1.0;
    }
    // The fine ripple runs downwind at about the pace of short wind waves, the two layers a little
    // apart in course and pace so that they never lock into one moving picture.
    {
        const float Pace = FMath::Lerp(12.0f, 55.0f, ULakeWaveGenerator::Ripple(SmoothWind)) * DeltaTime;
        const float Course = FMath::DegreesToRadians(SmoothYaw);
        FineDrift += FVector2D(FMath::Cos(Course - 0.14f), FMath::Sin(Course - 0.14f)) * Pace;
        MidDrift += FVector2D(FMath::Cos(Course + 0.3f), FMath::Sin(Course + 0.3f)) * Pace * 1.5f;
        ApplyRipple(SmoothWind);
    }
    if (Now >= NextLog)
    {
        NextLog = Now + 30.0;
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_WAVES wind %.1f m/s towards %.0f deg: significant height %.1f cm, peak wavelength %.0f cm, max wave %.1f cm, fine ripple %.2f"),
            Built, BuiltYaw, ULakeWaveGenerator::SignificantHeight(Built), ULakeWaveGenerator::PeakWavelength(Built), Waves ? Waves->GetMaxWaveHeight() : 0.0f, ShownRipple);
    }
}

TStatId ULakeWaves::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(ULakeWaves, STATGROUP_Tickables);
}
