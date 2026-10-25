#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeFish.generated.h"

class ALakeCharacter;

UENUM()
enum class EFishSpecies : uint8 { Perch, Roach, Crucian, Pike, Count UMETA(Hidden) };

UENUM()
enum class EBait : uint8 { Worm, Maggot, Bread, Count UMETA(Hidden) };

/** Where in the water column and in the lake a species prefers to be. */
enum class EFishHabitat : uint8 { OpenWater, Reeds, Bottom, Ambush };

/** Tunable biology of one species. Speeds in cm/s, distances in cm, weights in kg. */
struct FFishSpeciesInfo
{
    const TCHAR* Name;              // shown to the player (Russian)
    const TCHAR* Mesh;              // asset path
    float MeshLength;               // cm, length of the imported mesh
    float Condition;                // Fulton K: weight[g] = K * length[cm]^3 / 100
    float MinWeight, MaxWeight;
    EFishHabitat Habitat;
    float DepthMin, DepthMax;       // preferred band, 0 = surface, 1 = bottom
    float CruiseSpeed, BurstSpeed;
    float TurnRate;                 // deg/s at cruise
    float Acceleration;             // cm/s^2
    int32 SchoolSize;               // 1 = solitary
    float SenseRadius;              // how far it notices bait
    float BaitLike[(int32)EBait::Count];
    int32 MinNibbles, MaxNibbles;   // cautious species test the bait before taking it
    float Strength;                 // thrust per kg relative to a perch, used when hooked
    float Stamina;                  // seconds of hard pulling at full strength
    float TailBeat;                 // base tail beat rate (rad/s)
};

namespace LakeFish
{
    QUIETLAKE_API const FFishSpeciesInfo& Info(EFishSpecies Species);
    QUIETLAKE_API const TCHAR* BaitName(EBait Bait);
    /** Length in cm of a fish of the given species and weight (Fulton's condition factor). */
    QUIETLAKE_API float LengthCm(EFishSpecies Species, float WeightKg);
    /** 0..1 how attractive this bait at this depth is to the species, before mood and fear. */
    QUIETLAKE_API float BaitInterest(EFishSpecies Species, EBait Bait, float DepthFraction);
    /** Water surface height (cm). */
    constexpr float SurfaceZ = -25.0f;
}

/**
 * A single fish. The server simulates it: steering behaviours pick a desired velocity
 * (wander in its habitat, school with its group, investigate bait, flee, fight on the line),
 * and a simple swimming model turns that into motion: limited thrust and turn rate,
 * quadratic water drag, mass from the fish's weight, and the water volume as hard bounds.
 * Clients only interpolate the replicated position.
 */
UCLASS()
class QUIETLAKE_API ALakeFish : public AActor
{
    GENERATED_BODY()
public:
    ALakeFish();
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;

    void Setup(EFishSpecies InSpecies, float InWeight, FVector Home, int32 InSchool);

    UPROPERTY(Replicated) EFishSpecies Species = EFishSpecies::Perch;
    UPROPERTY(Replicated) float WeightKg = 0.2f;

    // --- interaction with anglers (server) ---
    /** Scare the fish away from a point, e.g. the splash of a cast or a failed strike. */
    void Spook(FVector From, float Seconds);
    /** The angler struck while this fish held the bait. */
    void Hook(ALakeCharacter* Angler);
    /** Line tension pulls the hooked fish towards the rod tip; force in kg*cm/s^2. */
    void ApplyLineForce(FVector Force);
    /** The fish got off: lost bait, broken line or released. */
    void Release(float SpookSeconds);
    bool IsHooked() const { return State == EState::Hooked; }
    float StaminaFraction() const { return Stamina / FMath::Max(0.01f, MaxStamina); }
    FVector MouthLocation() const;
    FVector Velocity() const { return Vel; }

    /** Bounds of the swimmable water at a point (cm). */
    static float BottomZ(float X, float Y);

    // Exposed for tests: advance the simulation by a fixed step.
    void Simulate(float DeltaSeconds);

private:
    enum class EState : uint8 { Roam, Investigate, Nibble, Holding, Hooked, Flee };
    void ChooseRoamTarget();
    FVector SteerRoam() const;
    FVector SteerSchool() const;
    FVector SteerBounds() const;
    void UpdateBait(float DeltaSeconds);
    void Move(FVector DesiredVelocity, float MaxSpeed, float DeltaSeconds);
    void UpdateVisual(float DeltaSeconds);

    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Body;

    UPROPERTY(Replicated) FVector_NetQuantize10 NetLocation;
    UPROPERTY(Replicated) FRotator NetRotation;
    UPROPERTY(Replicated) float NetEffort = 0;   // 0..1, drives the tail beat on clients

    EState State = EState::Roam;
    int32 School = -1;
    FVector HomePoint, RoamTarget, Vel = FVector::ZeroVector;
    FVector FleeFrom;
    float StateTime = 0, FleeTime = 0, NextThink = 0, Hunger = 1, Effort = 0;
    float Stamina = 1, MaxStamina = 1, BurstTime = 0, Phase = 0;
    int32 NibblesLeft = 0;
    TWeakObjectPtr<ALakeCharacter> TargetAngler;
    TMap<TWeakObjectPtr<ALakeCharacter>, float> IgnoreBaitUntil;
    FVector PendingLineForce = FVector::ZeroVector;
    float SimTime = 0;
    FRandomStream Random;
};
