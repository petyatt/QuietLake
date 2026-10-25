#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeFish.generated.h"

class ALakeCharacter;

UENUM()
enum class EFishSpecies : uint8 { Perch, Roach, Crucian, Pike, Count UMETA(Hidden) };

UENUM()
enum class EBait : uint8 { Worm, Maggot, Bread, Count UMETA(Hidden) };

/**
 * Where in the lake a species lives: how strongly it is drawn to each kind of water (0..1, see
 * LakeMap::FZones) and the water depth it keeps to. Home spots and wanderings are chosen by these
 * weights, so a new species needs only its own row in the species table.
 */
struct FFishHabitat
{
    float Shallows;     // warm weedy backwater, lily beds
    float ReedEdge;     // just outside the reed belt
    float DropOff;      // where the shelf falls into deep water
    float Cover;        // snags and pier piles
    float Deep;         // the deep middle and the hole
    float Open;         // open water away from the bank
    float MinDepthM, MaxDepthM;
    bool bAmbush;       // waits motionless at one spot and strikes from it
};

/** What the fish feel of the time and the weather right now (ALakeWeather builds it on the server). */
struct FFishConditions
{
    float SunElevation = 30;         // degrees
    bool bMorning = true;            // before solar noon
    float WaterTempC = 19;
    float Cloud = 0.3f, Rain = 0, Wind = 0.1f, Fog = 0;
    float PressureTrend = 0;         // mm Hg per game hour; negative = falling, a front coming
    float Shock = 0;                 // 0..1 just after a sharp change of the weather
    float StableHours = 0;           // game hours of steady weather
    FVector2D WindDir = FVector2D(1, 0);
};

/**
 * How a species' appetite and boldness follow the time and the weather. Activity multiplies how
 * often the fish notices bait, how readily it takes it and how long it holds it, and how lively it
 * swims; 1 is an ordinary good day.
 */
struct FFishMood
{
    float Dawn, Day, Dusk, Night;            // by time of day
    float TempLow, TempBest, TempHigh;       // water temperature (C): sluggish at the ends of the range
    float Overcast;                          // -1..+1 by day: prefers sun .. prefers a grey sky
    float LightRain, HeavyRain;              // multipliers
    float Ripple, StrongWind;                // light breeze, strong wind
    float Fog;                               // thick mist on the water
    float Front;                             // 0..1: feeds before a front, sulks after a sharp change
};

/** Tunable biology of one species. Speeds in cm/s, distances in cm, weights in kg. */
struct FFishSpeciesInfo
{
    const TCHAR* Name;              // shown to the player (Russian)
    const TCHAR* Mesh;              // asset path
    float MeshLength;               // cm, length of the imported mesh
    float Condition;                // Fulton K: weight[g] = K * length[cm]^3 / 100
    float MinWeight, MaxWeight;
    FFishHabitat Habitat;
    float DepthMin, DepthMax;       // preferred band in the water column, 0 = surface, 1 = bottom
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
    FFishMood Mood;
};

namespace LakeFish
{
    QUIETLAKE_API const FFishSpeciesInfo& Info(EFishSpecies Species);
    QUIETLAKE_API const TCHAR* BaitName(EBait Bait);
    /** Length in cm of a fish of the given species and weight (Fulton's condition factor). */
    QUIETLAKE_API float LengthCm(EFishSpecies Species, float WeightKg);
    /** 0..1 how attractive this bait at this depth is to the species, before mood and fear. */
    QUIETLAKE_API float BaitInterest(EFishSpecies Species, EBait Bait, float DepthFraction);
    /** 0..1 how good this point of the lake is for the species to live at (now, if conditions are given). */
    QUIETLAKE_API float HabitatScore(EFishSpecies Species, float X, float Y, const FFishConditions* Conditions = nullptr);
    /** A good place for the species, near Near within Range cm (Range <= 0: anywhere in the lake). */
    QUIETLAKE_API FVector PickSpot(EFishSpecies Species, FRandomStream& Random, FVector2D Near, float Range, const FFishConditions* Conditions = nullptr);
    /** Appetite and boldness now, about 0.05..1.6 (1 = an ordinary good day). */
    QUIETLAKE_API float Activity(EFishSpecies Species, const FFishConditions& Conditions);
    /** Metres the species moves its depth band: deeper in warm water and bright sun, up under a grey sky. */
    QUIETLAKE_API float DepthShiftM(EFishSpecies Species, const FFishConditions& Conditions);
    /** Bright sun on a glassy surface: fish see the angler and the line and are wary. */
    QUIETLAKE_API bool IsBrightCalm(const FFishConditions& Conditions);
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
    /** Appetite from the time and weather, refreshed as the fish swims (1 without a weather actor). */
    float Appetite() const { return Activity; }

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
    float NextRun = 0;                // hooked: seconds until the next run
    float Activity = 1;               // see LakeFish::Activity
    float NextMoodCheck = 0;
    /** Current conditions from the weather; false (and neutral values) without a weather actor. */
    bool GetConditions(FFishConditions& Out) const;
    FRandomStream Random;
};
