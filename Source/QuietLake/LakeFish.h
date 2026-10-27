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
 * What a free fish is doing with its day (see ALakeLife, "Живой пруд"): feeding (predators hunt),
 * resting in cover, or cruising its patch of water. Chosen from hunger, tiredness, the species'
 * daily rhythm and the weather; school members follow their leader.
 */
UENUM()
enum class EFishGoal : uint8 { Cruise, Feed, Rest, Count UMETA(Hidden) };

/** What the float does while a fish works at the bait. */
/** Where an angler is in the fishing cycle (replicated: every machine animates the rod from it). */
UENUM()
enum class EFishingState : uint8
{
    Idle,       // no line in the water
    Waiting,    // the bait is in the water (also while the rig is being lifted out)
    Bite,       // a fish holds the bait: strike now
    Fight,      // hooked: playing the fish
    Landing,    // a tired fish lifted out of the water on the line and swung to the hand
};

UENUM()
enum class EBiteMotion : uint8
{
    None,       // still (a pause between tries)
    Tremble,    // fine quick quivering on the spot: lips at the bait
    Peck,       // a short dip and back: a tug at the bait
    Lift,       // rises slowly (the bait and the shot lifted off the bottom); lifted high it lies down
    Drift,      // moves off to the side, slowly settling at a slant
    Sink,       // pulled under and away
};

/**
 * One step of a bite as the float shows it. Amount: centimetres of quiver, dip, rise or settling
 * (Tremble, Peck, Lift, Drift); cm/s the float is pulled under (Sink). bTake: the fish holds the
 * bait in its mouth now, a strike can hook it.
 */
struct FBiteStep
{
    EBiteMotion Motion = EBiteMotion::None;
    float Duration = 1;
    float Amount = 0;
    bool bTake = false;
};

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
    FFishHabitat FeedHabitat;       // where it feeds (predators: where they hunt or lie in ambush)
    FFishHabitat RestHabitat;       // where it rests
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
    /** A good place for the species, near Near within Range cm (Range <= 0: anywhere in the lake), for what it is doing. */
    QUIETLAKE_API FVector PickSpot(EFishSpecies Species, FRandomStream& Random, FVector2D Near, float Range, const FFishConditions* Conditions = nullptr,
                                   EFishGoal Goal = EFishGoal::Cruise);
    QUIETLAKE_API const FFishHabitat& HabitatFor(EFishSpecies Species, EFishGoal Goal);
    QUIETLAKE_API const TCHAR* GoalName(EFishSpecies Species, EFishGoal Goal);
    /**
     * How a fish takes the bait, step by step, as anglers know it:
     *  crucian - tests the bait at length (the float trembles, pauses, trembles; small ones keep at
     *            it), then lifts it off the bottom - the float rises and, lifted high, lies down -
     *            and leads it off to the side, the float slowly settling; bait off the bottom: a dip
     *            and a slow lead away;
     *  roach   - quick pecks, then the float goes under at a slant (from the bottom it sometimes lifts);
     *  perch   - a tap or two, then the float is pulled under sharply and led away;
     *  pike    - a jerk, then the float goes under fast.
     * Size01 0..1 within the species, Activity as LakeFish::Activity, bWary: bright calm water.
     */
    QUIETLAKE_API TArray<FBiteStep> MakeBite(EFishSpecies Species, float Size01, float Activity, bool bBaitOnBottom, bool bWary, FRandomStream& Random);
    /** What the angler sees, for the status line. */
    QUIETLAKE_API const TCHAR* BiteText(EBiteMotion Motion, float Amount);
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
    /** It let go of the bait it held (the angler did not strike): swims off, wary of that bait for a while. */
    void DropBait();
    /** Drawn out of the water on the line (server): from now on the angler's rod carries it. */
    void LiftOut();
    bool IsLifted() const { return bLifted; }
    /**
     * Out of the water on the line: placed on every machine from the rod's simulated hook. It hangs
     * from its mouth along the line, swings with it and thrashes; still in the water it is drawn
     * mouth first.
     */
    void HangOnLine(FVector Mouth, FVector LineUp, float DeltaSeconds);
    bool IsHooked() const { return State == EState::Hooked; }
    float StaminaFraction() const { return Stamina / FMath::Max(0.01f, MaxStamina); }
    FVector MouthLocation() const;
    FVector Velocity() const { return Vel; }
    /** Appetite from the time and weather, refreshed as the fish swims (1 without a weather actor). */
    float Appetite() const { return Activity; }

    /** Bounds of the swimmable water at a point (cm). */
    static float BottomZ(float X, float Y);

    // --- daily life (see ALakeLife) ---
    EFishGoal GetGoal() const { return Goal; }
    int32 GetSchool() const { return School; }
    float GetHunger() const { return Hunger; }
    float GetEnergy() const { return Energy; }
    /** Free to live its life: not testing, holding or hooked on anyone's bait, not fleeing. */
    bool IsFree() const { return State == EState::Roam; }
    /** On someone's bait or line. */
    bool IsBusy() const { return State == EState::Investigate || State == EState::Nibble || State == EState::Holding || State == EState::Hooked; }
    /** Full simulation near players; far away the fish is stepped coarsely and replicated rarely. */
    void SetNearPlayers(bool bNear);
    /** A predator struck at the fry next to it, or a hunting school mate dashes: a short burst towards Target. */
    void Dash(FVector Target, float Seconds);
    /** Eat: a mouthful of fry or a whole fish (0..1 of a full meal). */
    void Eat(float Meal);

    // Exposed for tests: advance the simulation by a fixed step.
    void Simulate(float DeltaSeconds);

private:
    enum class EState : uint8 { Roam, Investigate, Nibble, Holding, Hooked, Flee };
    void ChooseRoamTarget(bool bLocal = false);
    /** Pick what to do next from hunger, tiredness, the daily rhythm and the weather. */
    void Decide();
    void SetGoal(EFishGoal NewGoal, float Seconds);
    /** Goal behaviour while roaming: feeding signs, hunting, the speed it swims at. */
    void Live(float DeltaSeconds, FVector& Desired, float& MaxSpeed);
    void UpdatePikeAmbush(float DeltaSeconds, FVector& Desired, float& MaxSpeed);
    /** The fish this one follows: the school leader, or itself. */
    const ALakeFish* Leader() const;
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
    UPROPERTY(Replicated) EFishGoal Goal = EFishGoal::Cruise;
    UPROPERTY(Replicated) bool bLifted = false;  // out of the water on an angler's line
    // Hanging on the line (cosmetic, every machine).
    float HangTime = 0, HangEffort = 0;
    FVector HangForward = FVector::ForwardVector;
    uint64 LastHangFrame = 0;

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
    // Daily life.
    float Energy = 1;                 // 1 rested .. 0 tired
    float GoalUntil = 0;              // SimTime the current goal is reconsidered
    float NextSign = 0;               // next feeding sign (bubbles, a rise)
    float NextLook = 0;               // predators: next look for prey
    float NextFryAttack = 0;
    float StrikeEnd = -1;             // pike: SimTime the strike ends
    TWeakObjectPtr<ALakeFish> Prey;   // pike: the fish it strikes at
    int32 PreyFry = -1;               // pike/perch: the fry shoal it strikes at
    bool bAtSpot = false;             // reached its feeding or resting spot
    bool bNear = true;                // full detail
    float LastBeat = -1, LastAmp = -1;// what the material was last given
    // The bite in progress (see LakeFish::MakeBite).
    TArray<FBiteStep> Bite;
    int32 BiteIndex = -1;
    float BiteStepEnd = 0;
    float HoldSpeed = 0.6f;           // swimming with the bait, fraction of the cruise speed
    void NextBiteStep(class ALakeCharacter* Angler);
    friend class FLakePondLifeTest;
    /** Current conditions from the weather; false (and neutral values) without a weather actor. */
    bool GetConditions(FFishConditions& Out) const;
    FRandomStream Random;
};
