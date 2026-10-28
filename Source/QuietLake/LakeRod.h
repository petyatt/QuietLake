#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeFish.h"
#include "LakeRod.generated.h"

/** Everything the rod needs from its angler each frame (filled by ALakeCharacter). */
struct FRodInput
{
    FTransform Hold;              // rod butt; X = along the rod towards the tip
    EFishingState FishingState = EFishingState::Idle;
    EBait Bait = EBait::Worm;
    FVector FloatTarget = FVector::ZeroVector;   // where the bobber sits / lands (server)
    float HookDepth = 100;        // leader length under the bobber
    float SinceCast = -1;         // seconds since the line was released, < 0 if not casting
    float CastFlight = 1;         // seconds the bobber flies down onto the water
    bool bSwinging = false;       // LMB held for a cast: the hook held in the hand
    float HookHold = 0;           // 0..1: how firmly the angler's left hand has the rig (1: the shot held in the fingers)
    float RetrieveT = -1;         // drawing the rig out of the water: 0..1 of the way, < 0 when not
    float RodHeft = 1;            // how sluggish the rod is: 1 medium, less for a light rod, more for a heavy one
    float CastDraw = 0;           // 0..1: how far the hand has drawn the rig back for the cast (it loads the tip)
    FVector Wind = FVector::ZeroVector;   // at the water, cm/s (the weather's, with gusts)
    FVector HookLanding = FVector::ZeroVector;    // where the hook comes down (beyond the float when laid out straight)
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
    // The angler's free hand: it holds the hook for the cast, and takes a fish lifted out
    // (Landing), where the fish hangs on the hook with its weight.
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
    /** How far the tip is bent off the line of the butt (cm). */
    float TipBend() const
    {
        if (Points.Num() < 3) return 0;
        const FVector Axis = (Points[1] - Points[0]).GetSafeNormal();
        const FVector D = Points.Last() - Points[0];
        return (D - Axis * FVector::DotProduct(D, Axis)).Size();
    }
    /** The main line's belly: the farthest its points stray sideways from the straight tip-to-float
     *  line (cm, horizontally), and how many of its points lie on the water. */
    float LineBelly(int32* OutOnWater = nullptr) const;
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
    /** Once a frame: the blank's weights and stiffness for this rod, and where the hands are. */
    void PrepareBlank(const FTransform& Hold);
    /** One substep of the blank (S: 0..1 through the frame, moving the hands smoothly). */
    void StepBlank(float H, float S);
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

    bool bCastLaunched = false, bCastLanded = false, bBobberInWater = false;
    TArray<FVector> Points;                       // blank: from the handle end to the tip
    // The blank as a chain of points (Points), each with its share of the rod's weight, kept straight
    // by its bending stiffness (stiff at the butt, soft at the tip).
    TArray<FVector> BlankPrev;
    TArray<float> BlankW;                         // inverse masses: the first two points are in the hand
    TArray<float> BlankAlpha;                     // bending compliance at each joint
    TArray<FVector> BlankLambda;                  // XPBD multipliers, per substep
    FTransform HoldFrom, HoldTo;                  // the hands at the start and at the end of this frame
    FVector TipForce = FVector::ZeroVector;       // this frame's load on the tip (a fish, the cast draw), dyn
    float BlankDamp = 11;                         // 1/s
    FVector LastTip = FVector::ZeroVector;
    FVector BobberPos, BobberPrev, HookPos, HookPrev, FlightStart, FlightVelocity;
    FVector HookFlightStart = FVector::ZeroVector, FlightApex = FVector::ZeroVector;   // the cast's arcs: the hook's start, the high point both pass near
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
    float LandLine = -1;
    float CastLine = -1;                          // a cast: tip-to-float line let out while the rig flies, taken back in after
    bool bCatchLaunched = false;
    FVector CatchStart = FVector::ZeroVector;     // where the shot was when the free hand began to bring it in                          // lifting a fish out: tip-to-hook reach, easing to the line's length
    bool bInitialised = false;
    float Heft = 1;                               // the rod's weight (see FRodInput::RodHeft)
};
