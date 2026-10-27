#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeFish.h"
#include "LakeRod.generated.h"

/** Everything the rod needs from its angler each frame (filled by ALakeCharacter). */
struct FRodInput
{
    FTransform Hold;              // rod butt; X = along the rod towards the tip
    int32 FishingState = 0;
    EBait Bait = EBait::Worm;
    FVector FloatTarget = FVector::ZeroVector;   // where the bobber sits / lands (server)
    float HookDepth = 100;        // leader length under the bobber
    float SinceCast = -1;         // seconds since the line was released, < 0 if not casting
    float CastFlight = 1;         // seconds the bobber flies down onto the water
    bool bSweeping = false;       // LMB held: the line is laid out ahead of the lowering rod
    float SweepAngle = -90;       // line angle below the horizon during the sweep (deg)
    FVector Facing = FVector::ForwardVector;
    float Tension = 0;
    uint8 Nibbles = 0;
    EBiteMotion BiteMotion = EBiteMotion::None;   // what a fish at the bait makes the float do
    float BiteElapsed = 0, BiteDuration = 1, BiteAmount = 0;
    bool bReeling = false;
    bool bOnStand = false;
    FVector StandBase = FVector::ZeroVector;
    float StandYaw = 0;
    class ALakeFish* LineFish = nullptr;          // fish holding the bait or hooked
    // Lifting a fish out (state 4): it hangs on the hook with its weight; at the end the angler's
    // free hand takes it.
    float FishGrams = 0, FishLength = 0;
    FVector HandPoint = FVector::ZeroVector;
    float HandAlpha = 0;
};

/**
 * Visual rod, line, float rig and bait of one angler. Purely cosmetic and simulated locally on
 * every machine from the angler's replicated state, so the cast, the bend and the line look
 * the same everywhere without streaming any of it over the network.
 *  - The blank bends as one elastic cantilever driven by the hand's motion and the line's pull.
 *  - Main line and leader are light, inextensible, slightly stiff verlet chains with strong air
 *    drag, drawn as thin cylinders; slack main line lies on the water.
 *  - The bobber dangles from the tip, flies on a ballistic arc when cast, floats with a buoyancy
 *    spring, twitches on nibbles and goes under on a take. The hook follows a fish once taken.
 */
UCLASS()
class QUIETLAKE_API ALakeRod : public AActor
{
    GENERATED_BODY()
public:
    ALakeRod();
    virtual void Tick(float DeltaSeconds) override;
    void UpdateRod(const FRodInput& In, float DeltaSeconds);

    // A 6 m pole rod (Russian "makhovaya") with a fixed 6 m line and no reel.
    static constexpr float HandleLength = 44.0f;
    static constexpr float RodLength = 600.0f;
    static constexpr float LineLength = 600.0f;  // tip to hook: the same as the rod, so the rig never drags
    static constexpr float GripPoint = 30.0f;    // right hand, from the butt

    FVector Tip() const { return Points.Num() ? Points.Last() : FVector::ZeroVector; }
    FVector HookLocation() const { return HookPos; }
    FVector BobberLocation() const { return BobberPos; }
    float GetBiteTilt() const { return BiteTilt; }
    /** The tip of the float's antenna (how much of it shows above the water tells the angler everything). */
    FVector FloatTip() const { return FloatT; }
    FVector ShotLocation() const { return ShotPos; }
    FVector FloatKeel() const { return FloatK; }
    /** Up along the line from the hook towards the rod tip: a fish hanging on it points this way. */
    FVector HookLineUp() const { return (Tip() - HookPos).GetSafeNormal(); }

private:
    /** A thin, nearly weightless and inextensible line between two pinned ends. */
    struct FLineSim
    {
        TArray<FVector> P, Prev;
        FVector LastA = FVector::ZeroVector, LastB = FVector::ZeroVector;
    };
    void SimulateLine(FLineSim& Line, FVector A, FVector B, float Length, float DeltaSeconds, bool bFloatsOnWater, bool bReset);
    void LayoutLine(const FLineSim& Line, const TArray<TObjectPtr<class UStaticMeshComponent>>& Segments, float Radius);
    FLineSim MainSim, LeaderSim, TailSim;
    void SimulateBlank(const FTransform& Hold, FVector TipLoad, float DeltaSeconds);
    void UpdateBobber(const FRodInput& In, float DeltaSeconds);
    void Layout(const FRodInput& In);

    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Handle;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Reel;
    UPROPERTY() TArray<TObjectPtr<class UStaticMeshComponent>> Blank;
    UPROPERTY() TArray<TObjectPtr<class UStaticMeshComponent>> MainLine;
    UPROPERTY() TArray<TObjectPtr<class UStaticMeshComponent>> Leader;
    UPROPERTY() TArray<TObjectPtr<class UStaticMeshComponent>> Tail;    // leader below the shot
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Bobber;
    UPROPERTY() TObjectPtr<class USceneComponent> BobberEye;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Sinker;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Hook;
    UPROPERTY() TArray<TObjectPtr<class UStaticMeshComponent>> Baits;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Stand;

    bool bCastLaunched = false, bBobberInWater = false;
    TArray<FVector> Points;                       // blank: from the handle end to the tip
    FVector Deflection = FVector::ZeroVector, DeflectionVelocity = FVector::ZeroVector;
    FVector RestTipPrev = FVector::ZeroVector, RestTipPrev2 = FVector::ZeroVector, LastTip = FVector::ZeroVector;
    FVector BobberPos, BobberPrev, HookPos, HookPrev, FlightStart, FlightVelocity;
    float Time = 0, NibbleTime = -10;
    float LeaderNow = 100;
    float SinkNow = 0;                            // how far a taking fish has drawn the float under (cm)                        // float-to-hook distance as shown, easing to the set depth
    uint8 SeenNibbles = 0;
    float BiteTilt = 0;                           // the float leaning (deg): lying down after a lift, pulled at a slant
    float LastBiteElapsed = 0;
    FVector BiteAnchor = FVector::ZeroVector;     // where the hook lay when the fish began this motion
    FVector BiteFloat = FVector::ZeroVector;      // where the float was then
    // The float as a rigid stick (keel eye, antenna tip) and the shot, simulated with their weights.
    FVector FloatK = FVector::ZeroVector, FloatKPrev = FVector::ZeroVector, FloatT = FVector::ZeroVector, FloatTPrev = FVector::ZeroVector;
    FVector ShotPos = FVector::ZeroVector, ShotPrev = FVector::ZeroVector;
    EBiteMotion LastBiteMotion = EBiteMotion::None;
    float LandLine = -1;                          // lifting a fish out: tip-to-hook reach, easing to the line's length
    bool bInitialised = false;
};
