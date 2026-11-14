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
    bool bRigFlying = false;      // let go and not yet down on the water (the server says when it lands)

    bool bFreeRig = false;        // no network drift correction during casts, pulls or settling
    float HookHold = 0;           // 0..1: how firmly the angler's left hand has the rig (1: the shot held in the fingers)
    float RodHeft = 1;            // how sluggish the rod is: 1 medium, less for a light rod, more for a heavy one
    FVector Wind = FVector::ZeroVector;   // at the water, cm/s (the weather's, with gusts)
    FVector Facing = FVector::ForwardVector;
    float Tension = 0;
    uint8 Nibbles = 0;
    EBiteMotion BiteMotion = EBiteMotion::None;   // what a fish at the bait makes the float do
    float BiteElapsed = 0, BiteDuration = 1, BiteAmount = 0;
    bool bReeling = false;
    bool bOnStand = false;
    /** Lying on the ground: the blank rests on it instead of sagging through. */
    bool bOnGround = false;
    FVector StandBase = FVector::ZeroVector;
    float StandYaw = 0;
    class ALakeFish* LineFish = nullptr;          // fish holding the bait or hooked
    // The angler's free hand: it holds the hook for the cast, and takes a fish lifted out
    // (Landing), where the fish hangs on the hook with its weight.
    float FishGrams = 0, FishLength = 0;
    /** Landing with the rod let down: a fish lying on the bank flops back towards the water. */
    bool bFishLowered = false;
    /** The fish is in the angler's free hand, the hook still in its mouth. */
    bool bFishHeld = false;
    /**
     * gf per cm: the spring the server holds a fish with (LoadOnFish): the rod's bend and the arms
     * to a pull across the rod, and the far stiffer rod and arms to one along it.
     */
    float LineSpring = 0, LineSpringAlong = 0;
    /**
     * The tip of the rod as the hands hold it, unbent and before the arms give (the server's RodTip),
     * and its axis: the spring is measured from there, exactly as the server measures it (the give of
     * the arms is already in LineSpring).
     */
    FVector SpringTip = FVector::ZeroVector, SpringAxis = FVector::ZeroVector;
    FVector HandPoint = FVector::ZeroVector;
};

/**
 * Rod, line, float rig and bait of one angler, simulated locally from replicated input.
 * The server also runs this simulation to decide physical splashdown and catching the rig;
 * clients render their own simulation and receive the authoritative fishing state.
 *  - The blank bends as one elastic cantilever driven by the hand's motion and the line's pull.
 *  - Main line and leader are light, inextensible, slightly stiff verlet chains with strong air
 *    drag, drawn as thin cylinders; slack main line lies on the water.
 *  - The bobber dangles from the tip, swings freely on the fixed line, floats by submerged volume, twitches on nibbles and goes under on a take. The hook follows a fish once taken.
 */
UCLASS()
class QUIETLAKE_API ALakeRod : public AActor
{
    GENERATED_BODY()
public:
    ALakeRod();
    virtual void Tick(float DeltaSeconds) override;
    void UpdateRod(const FRodInput& In, float DeltaSeconds);
    /** Start again from the hold next frame (the rod taken out of the case). */
    void ResetSim() { bInitialised = false; }

    // A 6 m pole rod (Russian "makhovaya") with a fixed 6 m line and no reel.
    static constexpr float HandleLength = 44.0f;
    static constexpr float RodLength = 600.0f;
    static constexpr float LineLength = 600.0f;  // tip to hook: the same as the rod, so the rig never drags
    static constexpr float GripPoint = 12.0f;    // right hand, from the butt (at the end of the handle)
    /**
     * The pendulum cast's numbers: the rod's angle as the fingers let go (Start), how high it is raised
     * while the rig swings down under it (Raise, by RaiseTime seconds for the medium rod), and how far it
     * is brought forward and down while the rig swings up beyond the tip (Follow, by LayAfter); held on,
     * the rod lays the rig down at the top of its swing (over LayTime). Development builds take
     * -CastTune=Start,Raise,RaiseTime,Follow,LayAfter,LayTime to try others without a rebuild.
     */
    struct FCastTuning { float Start = 45, Raise = 68, RaiseTime = 1.0f, Follow = 34, LayAfter = 1.95f, LayTime = 0.35f; };
    static const FCastTuning& CastTuning();

    FVector Tip() const { return Points.Num() ? Points.Last() : FVector::ZeroVector; }
    static float FishSupport(float MouthZ, float Length);
    static FVector TetherForce(FVector Anchor, FVector Mouth, FVector RelativeVelocity, float FreeLength, float Mass, float Spring = 2600.0f / 140.0f);
    FVector PullOnFish(FVector Mouth, FVector Velocity, float Mass) const;
    FVector HookLocation() const { return HookPos; }
    FVector BobberLocation() const { return BobberPos; }
    float GetBiteTilt() const { return BiteTilt; }
    /** The tip of the float's antenna (how much of it shows above the water tells the angler everything). */
    FVector FloatTip() const { return FloatT; }
    FVector ShotLocation() const { return ShotPos; }
    FVector ShotVelocity() const { return ShotVel; }
    FVector HookVelocity() const { return HookVel; }
    FVector FloatKeel() const { return FloatK; }
    /** The leader's shape (float to shot): how far it bows from straight and how far apart its ends are, cm. */
    void LeaderShape(float& OutBow, float& OutChord, float& OutLength) const
    {
        OutBow = OutChord = OutLength = 0;
        if (LeaderSim.P.Num() < 2) return;
        const FVector A = LeaderSim.P[0], B = LeaderSim.P.Last();
        OutChord = FVector::Dist(A, B);
        const FVector Dir = (B - A).GetSafeNormal();
        for (int32 I = 0; I < LeaderSim.P.Num(); ++I)
        {
            const FVector D = LeaderSim.P[I] - A;
            OutBow = FMath::Max(OutBow, (D - Dir * FVector::DotProduct(D, Dir)).Size());
            if (I > 0) OutLength += FVector::Dist(LeaderSim.P[I - 1], LeaderSim.P[I]);
        }
    }
    /** The float has come down: on the water's surface, or on the ground. */
    bool FloatDown() const;

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
    /** The main line from the rod tip to the float, as simulated (for the waves it makes on the water). */
    const TArray<FVector>& MainLinePoints() const { return MainSim.P; }
    /** Up along the line from the hook towards the rod tip: a fish hanging on it points this way. */
    FVector HookLineUp() const { return (Tip() - HookPos).GetSafeNormal(); }

private:
    /** A thin line as a chain of points, simulated with the rig (UpdateBobber). */
    struct FLineSim
    {
        TArray<FVector> P, Prev;
        FVector LastA = FVector::ZeroVector, LastB = FVector::ZeroVector;
    };
    void LayoutLine(const FLineSim& Line, const TArray<TObjectPtr<class UStaticMeshComponent>>& Segments, float Radius);
    FLineSim MainSim, LeaderSim, TailSim;
    /** The live waves near the viewer (rendering machines only): the float rides them. */
    TWeakObjectPtr<class ALakeRipples> Ripples;
    float NextRipplesLookup = 0;
    FVector ShotVel = FVector::ZeroVector, HookVel = FVector::ZeroVector;
    // The cast: the shot, the rig's weight, is thrown to the spot aimed at and flies there like any
    // thrown weight (a parabola); the float, the hook and the line fly after it on their own.

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

    bool bBobberInWater = false;
    TArray<FVector> Points;                       // blank: from the handle end to the tip
    // The blank as a chain of points (Points), each with its share of the rod's weight, kept straight
    // by its bending stiffness (stiff at the butt, soft at the tip).
    TArray<FVector> BlankPrev;
    TArray<float> BlankW;                         // inverse masses: the first two points are in the hand
    TArray<float> BlankAlpha;                     // bending compliance at each joint
    TArray<FVector> BlankLambda;                  // XPBD multipliers, per substep
    TArray<float> BlankFloor;                     // lying on the ground: the lowest each point may go (empty otherwise)
    bool bLyingDown = false;
    FTransform HoldFrom, HoldTo;                  // the hands at the start and at the end of this frame
    FVector TipForce = FVector::ZeroVector;       // this frame's load on the tip (a fish, the cast draw), dyn
    float BlankDamp = 11;                         // 1/s
    FVector StraightTip = FVector::ZeroVector, StraightTipVel = FVector::ZeroVector;   // the tip of the rod were it not bent, and its speed
    FVector TipAnchor = FVector::ZeroVector;      // the line holds the tip within TipReach of this point: the hand at the cast draw, a fish's mouth
    float TipReach = -1;
    FVector LastTip = FVector::ZeroVector;
    FVector BobberPos, BobberPrev, HookPos, HookPrev;
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
    float NextFlop = 0;
    bool bCatchLaunched = false;
    FVector CatchStart = FVector::ZeroVector;     // where the shot was when the free hand began to bring it in                          // lifting a fish out: tip-to-hook reach, easing to the line's length
    bool bInitialised = false;
    float Heft = 1;                               // the rod's weight (see FRodInput::RodHeft)
};
