#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeFish.h"
#include "LakeLife.generated.h"

/** Visible signs of fish life on the water. */
UENUM()
enum class ELakeSign : uint8
{
    Rise,       // rings of a fish sipping an insect off the surface (roach at dusk); a float landing
    Bubbles,    // a trail of bubbles over a crucian rooting in the silt
    Swirl,      // a bulge and wide rings: a strike or a sharp turn just under the surface
    Splash,     // a breaking splash: a predator striking at the top, a hooked fish thrashing
    Scatter,    // fry spraying out of the water as perch drive them ("котёл")
    Jump,       // a fish leaping clear of the water
    Count UMETA(Hidden)
};

USTRUCT()
struct FLakeSign
{
    GENERATED_BODY()
    UPROPERTY() ELakeSign Kind = ELakeSign::Rise;
    UPROPERTY() FVector_NetQuantize10 Location;          // on the water surface
    UPROPERTY() float Size = 1;                          // about 0.2 (tiny) .. 2 (a big pike)
    UPROPERTY() float Yaw = 0;                           // Jump: direction of the leap
    UPROPERTY() EFishSpecies Species = EFishSpecies::Roach;
    UPROPERTY() float WeightKg = 0.2f;
    UPROPERTY() bool bQuiet = false;                     // the angler's own float or hooked fish: no note in the panel
};

/** A cloud of fry in the shallows, simulated as one point and drawn as a swarm near the players. */
USTRUCT()
struct FFryShoal
{
    GENERATED_BODY()
    UPROPERTY() FVector_NetQuantize10 Location;
    UPROPERTY() uint8 Count = 30;
    UPROPERTY() float ScatteredAt = -100;   // server time of the last attack
    // Server only.
    FVector Target = FVector::ZeroVector;
    float NextWander = 0, Panic = 0, Speed = 20, AttackedAt = -100;
};

/**
 * "Живой пруд" — the life of the pond. One actor watches over every fish:
 *  - a spatial grid for neighbours (schools, predators and prey);
 *  - school leaders, whose decisions the whole school follows;
 *  - fry shoals in the shallows that perch drive and pike strike at;
 *  - predators eating fish (the pond is restocked later);
 *  - signs on the water that show what goes on below: rises, bubbles, swirls, splashes,
 *    spraying fry and leaping fish, with their sounds, sent to every player;
 *  - detail by distance: fish far from every player are stepped coarsely and replicated rarely.
 * Each fish decides for itself what to do (ALakeFish::Decide) from hunger, tiredness, its species'
 * daily rhythm and the weather. The server simulates; clients draw the signs and the fry.
 */
UCLASS()
class QUIETLAKE_API ALakeLife : public AActor
{
    GENERATED_BODY()
public:
    ALakeLife();
    static ALakeLife* Get(const UWorld* World);
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // --- server ---
    /** Advance the pond: rebuild the grid, move the fry, update detail levels (Tick calls it; tests call it directly). */
    void Step(float DeltaSeconds);
    void ForEachFishNear(const FVector& P, float Radius, TFunctionRef<void(ALakeFish*)> Visit) const;
    /** The member whose decisions the school follows, or null. */
    ALakeFish* SchoolLeader(int32 School) const;
    /** Nearest fry shoal within Radius (2D), or -1. */
    int32 NearestFry(const FVector& P, float Radius) const;
    FVector FryLocation(int32 Index) const { return Fry.IsValidIndex(Index) ? FVector(Fry[Index].Location) : FVector::ZeroVector; }
    /** A predator drives into a fry shoal: the fry spray out, the hunters nearby dash and eat. */
    void AttackFry(int32 Index, ALakeFish* Hunter);
    /** A predator caught Prey: it is gone, and the pond is restocked later. */
    void Devour(ALakeFish* Prey, ALakeFish* Predator);
    /** A fish was hooked or eaten: school mates close by bolt, those further off shy away. */
    void Alarm(ALakeFish* Victim, float Radius);
    /** The water was disturbed at At (a rig landing, heavy steps): fish within Radius shy away, the closest bolt. */
    void Disturb(const FVector& At, float Radius, float Strength);
    /** Every machine: how smooth one fish's shown motion was this frame (acceleration cm/s^2, change of turn deg/s^2). */
    void RecordShownMotion(const FVector& Where, float Accel, float TurnAccel, EFishSpecies Species);
    /** Server: log how the fish swim (speeds, turns, schools) as QUIET_LAKE_FISH_MOTION. */
    void LogMotion();
    /** Show a sign on the water to the players near it. */
    void EmitSign(ELakeSign Kind, const FVector& Location, float Size, EFishSpecies Species, float WeightKg = 0.2f, float Yaw = 0, bool bQuiet = false);
    void CountPikeStrike() { ++Counters.PikeStrikes; }
    /** A player's pawn within Radius of P (2D). */
    bool IsNearPlayer(const FVector& P, float Radius) const;

    /** Counters for the HUD panel, the log and the tests. */
    struct FStats
    {
        int32 Signs[(int32)ELakeSign::Count] = {};
        int32 FryAttacks = 0, PikeStrikes = 0, Eaten = 0;
    };
    const FStats& Stats() const { return Counters; }
    const TArray<FFryShoal>& FryShoals() const { return Fry; }

    // --- every machine ---
    /** Recent signs near this player, newest first, for the HUD panel (client time, text). */
    struct FNote { float Time; FString Text; };
    const TArray<FNote>& Notes() const { return RecentNotes; }

    UFUNCTION(NetMulticast, Unreliable) void MulticastSign(const FLakeSign& Sign);

private:
    void SpawnFry();
    void UpdateFry(float DeltaSeconds);
    void UpdateDetail();
    void UpdateRandomEvents();
    bool FryCanBe(const FVector2D& P) const;
    FVector FrySpotNear(const FVector2D& P, float Range);
    void LogStats();
    void RunPreview();
    void RunFishPreview();
    void RunWaterPreview();
    void SampleMotion(float DeltaSeconds);
    void UpdateFootsteps();
    void LogShownMotion();

    // Client visuals.
    void SetupVisuals();
    float ServerTime() const;
    void ShowSign(const FLakeSign& Sign);
    void AddRing(ELakeSign Kind, const FVector& Location, float Size);
    void AddSpray(const FVector& Location, float Size);
    void UpdateJumps(float Now);
    void UpdateFryVisuals(float DeltaSeconds);
    void PlaySound(class USoundBase* Sound, const FVector& Location, float Volume, float Pitch);
    FVector ViewLocation() const;

    UPROPERTY(Replicated) TArray<FFryShoal> Fry;

    // Server.
    TMap<FIntPoint, TArray<ALakeFish*>> Grid;
    TMap<int32, TArray<TWeakObjectPtr<ALakeFish>>> Schools;
    TArray<TWeakObjectPtr<ALakeFish>> AllFish;
    TArray<FVector> Players;
    float Time = 0, NextDetail = 0, NextEvent = 0, NextLog = 0, NextSample = 0;
    FStats Counters;
public:
    /** How the fish swim, sampled on the server a few times a second (see LogMotion). */
    struct FMotionStats
    {
        double Speed = 0, Turn = 0;           // sums: cm/s, |deg/s|
        float MaxTurn = 0, MaxTurnAccel = 0;  // deg/s, deg/s^2
        double TurnAccel = 0;
        int32 Samples = 0, Still = 0, Spins = 0;  // Spins: turning hard while nearly still
        double Spacing = 0, Order = 0;        // schools: nearest mate in body lengths, how aligned (0..1)
        int32 SchoolSamples = 0;
    };
    FMotionStats Motion[(int32)EFishSpecies::Count];
    /** Shown motion on this machine: acceleration histogram in 50 cm/s^2 steps, turn changes in 50 deg/s^2 steps. */
    struct FShownStats
    {
        int32 Accel[64] = {}, Turn[64] = {};
        int32 Samples = 0;
        float MaxAccel = 0, MaxTurn = 0;
    };
    FShownStats Shown;
    /** p-th percentile (0..1) of a histogram in Step-wide buckets. */
    static float Percentile(const int32* Buckets, int32 Count, int32 Total, float P, float Step);
private:
    TMap<TWeakObjectPtr<ALakeFish>, float> LastTurn;
    float NextShownLog = 0;
    float NextSteps = 0;
    FRandomStream Random;

    // Client.
    UPROPERTY() TObjectPtr<class UInstancedStaticMeshComponent> Rings;
    UPROPERTY() TObjectPtr<class ALakeRipples> Ripples;   // the live waves near the viewer
    UPROPERTY() TObjectPtr<class UInstancedStaticMeshComponent> Sprays;
    UPROPERTY() TArray<TObjectPtr<class UInstancedStaticMeshComponent>> FrySwarms;
    UPROPERTY() TArray<TObjectPtr<class UStaticMeshComponent>> Jumpers;
    UPROPERTY() TObjectPtr<class USoundAttenuation> Attenuation;
    UPROPERTY() TObjectPtr<class USoundBase> SplashSound;
    UPROPERTY() TObjectPtr<class USoundBase> PlopSound;
    UPROPERTY() TObjectPtr<class USoundBase> BubbleSound;
    UPROPERTY() TObjectPtr<class USoundBase> ScatterSound;
    UPROPERTY() TObjectPtr<class APostProcessVolume> Underwater;   // murk while the camera is under the water
    struct FJump { float Start = -100, Duration = 0.7f, Height = 40, Length = 100; FVector From; FVector Dir; float Size = 1, FishLength = 20; bool bLanded = true; };
    TArray<FJump> Jumps;
    struct FSwarmView { FVector Location = FVector::ZeroVector, Heading = FVector(1, 0, 0); float Seen = -1, NextDimple = 0; TArray<FVector> Offsets; };
    TArray<FSwarmView> SwarmViews;
    int32 NextRing = 0, NextSpray = 0;
    TArray<FNote> RecentNotes;
    bool bVisualsReady = false;
    friend class FLakePondLifeTest;
};
