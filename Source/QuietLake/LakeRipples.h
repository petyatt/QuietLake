#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Tasks/Task.h"
#include "LakeRipples.generated.h"

/**
 * Height field of the water near the viewer: a damped wave equation on a square grid that follows
 * the camera. Whatever pushes the water (a fish just under the surface, a bobbing float, a rain
 * drop) adds vertical velocity; waves spread, cross each other, reflect from the banks and fade.
 * A source faster than the waves leaves a V wake behind it. Units: cm and seconds. CPU only, so it
 * runs (and is tested) without rendering.
 */
struct QUIETLAKE_API FLakeRippleSim
{
    int32 N = 0;                    // cells per side
    float Cell = 4.0f;              // cm per cell
    FIntPoint Corner = FIntPoint::ZeroValue;   // world cell index of cell (0, 0)
    float WaveSpeed = 32.0f;        // cm/s
    float Damping = 0.35f;          // per second: how fast all waves die out
    float Viscosity = 1.5f;         // cm^2/s: short waves die out faster (a drop ring lasts a second or two)
    TArray<float> H, V;             // height (cm) and its rate (cm/s)
    TArray<uint8> Wet;              // 1 water, 0 land (held flat: waves reflect there)

    using FIsWater = TFunctionRef<bool(float X, float Y)>;
    void Init(int32 InN, float InCell, FVector2D Centre, FIsWater IsWater);
    /** Moves the window so that Centre is near its middle; waves keep their world positions. */
    void Recentre(FVector2D Centre, FIsWater IsWater);
    /** Advances by Dt in fixed steps of 1/60 s (at most four per call). */
    void Advance(float Dt);
    void Step(float Dt);
    /** Adds vertical velocity Amount (cm/s at the centre, gaussian of the given radius in cm). */
    void Push(FVector2D At, float Radius, float Amount);
    /** A drop or a plop: pushes down at the centre and up around it, keeping the water's volume. */
    void Drop(FVector2D At, float Radius, float Amount);
    float HeightAt(FVector2D P) const;
    FVector2D Origin() const { return FVector2D(Corner) * Cell; }
    float Size() const { return N * Cell; }
    bool Contains(FVector2D P, float Margin = 0) const;
    float Energy() const;

private:
    float Pending = 0;
    TArray<float> Sponge;           // per cell: extra damping towards the window edge
    TArray<float> Scratch;          // the new rates while a step is computed
    TArray<float> Decay;            // per cell: what a fixed step keeps of the rate
    float DecayDamping = -1;
    TArray<float> ShiftH, ShiftV;   // reused when the window moves
    TArray<uint8> ShiftWet;
    void BuildMask(FIsWater IsWater);
    void BuildSponge();
};

/**
 * Shows FLakeRippleSim on the lake (every machine that renders; nothing is replicated): gathers
 * the sources from what this machine shows (fish near the surface, the floats, the rain), steps
 * the simulation and hands its heights to the water material as a texture.
 */
UCLASS()
class QUIETLAKE_API ALakeRipples : public AActor
{
    GENERATED_BODY()
public:
    ALakeRipples();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

    static ALakeRipples* Get(const UWorld* World);
    /**
     * A drop or a splash at a point, now or after Delay seconds (spray falling back): Amount in cm/s
     * of the water surface (negative heaves the water up instead), Radius in cm. Queued; applied on
     * the next frame.
     */
    void Splash(FVector At, float Radius, float Amount, float Delay = 0);
    /** Inside the live window (as last shown), with a margin: here the water itself shows the sign. */
    bool Covers(FVector P, float Margin = 150) const;
    const FLakeRippleSim& Sim() const { FinishStep(); return Simulation; }
    /** Water height (cm above the rest surface) at a world point; 0 outside the window. */
    float HeightAt(FVector P) const { FinishStep(); return Simulation.HeightAt(FVector2D(P)); }
    /**
     * The same from the heights now shown (the last finished step), without waiting for the
     * worker: for the float and anything else that rides the waves during the frame.
     */
    float ShownHeightAt(FVector2D P) const;
    /** The slope of the shown surface (dz/dx, dz/dy). */
    FVector2D ShownSlopeAt(FVector2D P) const;

private:
    /** The simulation steps on a worker while the frame goes on; everything else waits for it here. */
    void FinishStep() const;
    void FindWater();
    void Upload();
    void PackHeights();
    void AddFishSources(float DeltaSeconds);
    void AddFloatSources(float DeltaSeconds);
    void AddRain(float DeltaSeconds);
    void AddLineSources(float DeltaSeconds);
    void AddWaders(float DeltaSeconds);
    void ApplyQueue();
    bool ViewPoint(FVector& OutLocation, FVector& OutForward) const;

    FLakeRippleSim Simulation;
    mutable UE::Tasks::FTask Stepping;
    FFloat16* Ready = nullptr;       // heights packed for the texture by the last step
    TArray<float> FrontH, BackH;     // heights shown (read on the game thread) and written by the worker
    FVector2D FrontOrigin = FVector2D::ZeroVector, BackOrigin = FVector2D::ZeroVector;
    bool bBackReady = false;
    TMap<TWeakObjectPtr<AActor>, float> LastWater;            // the water under a float last frame
    FVector2D ReadyOrigin = FVector2D::ZeroVector;
    FRandomStream Random;
    UPROPERTY() TObjectPtr<class UTexture2D> Texture;
    UPROPERTY() TArray<TObjectPtr<class UMaterialInstanceDynamic>> Materials;
    TMap<TWeakObjectPtr<AActor>, FVector> LastSeen;
    TMap<TWeakObjectPtr<AActor>, TArray<FVector>> LastLine;   // a rod's line, shot and hook last frame
    TMap<TWeakObjectPtr<AActor>, float> NextStep;              // a wading angler's next footfall
    TMap<TWeakObjectPtr<AActor>, int32> StepFoot;              // and which foot it is
    struct FQueued { FVector2D At; float Radius, Amount, Time; };
    TArray<FQueued> Queue;
    FVector2D ShownOrigin = FVector2D::ZeroVector;
    float ShownSize = 0;
    double StepSeconds = 0, StepWorst = 0, WaitSeconds = 0, SourceSeconds = 0, UploadSeconds = 0;
    int32 StepFrames = 0, Applied = 0, SkippedFrames = 0;
    float NextWaterSearch = 0;
    float FloatWave = 0;              // the highest water under a float since the last log (cm)
    float Skipped = 0;                // time not yet stepped (frames the worker was still busy)
    float NextLog = 0;
};
