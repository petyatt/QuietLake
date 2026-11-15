#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GerstnerWaterWaves.h"
#include "LakeWaves.generated.h"

class UWaterBodyComponent;

/**
 * Wind waves of the lake as engine Water Gerstner waves. The set of waves is fixed (wavelengths
 * 1.1-4.2 m, directions all round), only their heights follow the wind: the waves running with it
 * and near the fetch-limited peak wavelength of a small lake grow, the rest fade. Changing the wind
 * never shifts a wave's phase, so the surface does not jump.
 */
UCLASS()
class QUIETLAKE_API ULakeWaveGenerator : public UGerstnerWaterWaveGeneratorBase
{
    GENERATED_BODY()
public:
    /** The wind the waves are built for: speed in m/s, the direction it blows towards in degrees. */
    UPROPERTY() float WindSpeed = 0.0f;
    UPROPERTY() float WindYaw = 0.0f;

    virtual void GenerateGerstnerWaves_Implementation(TArray<FGerstnerWave>& OutWaves) const override;

    /** The waves for a wind (heights, wavelengths in cm). */
    static void MakeWaves(float WindSpeed, float WindYaw, TArray<FGerstnerWave>& OutWaves);
    /** Significant wave height (cm, trough to crest of the highest third) a wind raises on the lake. */
    static float SignificantHeight(float WindSpeed);
    /** Wavelength (cm) of the most energetic waves for that wind. */
    static float PeakWavelength(float WindSpeed);
};

/**
 * Keeps the lake's Water waves in step with the weather's wind on every machine (server and clients:
 * the waves depend only on time, and every machine runs the waves on the server's clock), and answers
 * where the water surface is: the float, the line and the fish on the hook ride the same waves that
 * are drawn. Without a Water lake (the procedural world, the tests) the surface is flat.
 */
UCLASS()
class QUIETLAKE_API ULakeWaves : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    static ULakeWaves* Get(const UWorld* World);
    /** The lake surface at P (world Z, cm): the still level plus the wind waves. */
    static float SurfaceAt(const UWorld* World, const FVector2D& P, FVector* OutNormal = nullptr);
    /** The waves alone at P (cm above the still level); the surface normal if asked. */
    float WaveAt(const FVector2D& P, FVector* OutNormal = nullptr) const;
    /** The wind (m/s) the current waves were built for. */
    float BuiltWind() const { return Built; }

    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override
    {
        return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
    }

private:
    void Setup();
    void Rebuild(float Speed, float Yaw);

    TWeakObjectPtr<UWaterBodyComponent> Lake;
    UPROPERTY() TObjectPtr<UGerstnerWaterWaves> Waves;
    UPROPERTY() TObjectPtr<ULakeWaveGenerator> Generator;
    float SmoothWind = -1.0f, SmoothYaw = 0.0f;
    float Built = -1.0f, BuiltYaw = 0.0f;
    double NextSetup = 0, NextRebuild = 0, NextLog = 0;
};
