#pragma once
#include "CoreMinimal.h"
#include "LakeSwimmer.h"

class ALakeFish;

// Volume-weighted body samples, expressed relative to the hooked mouth.
struct FFishImmersion
{
    float Fraction = 0;
    FVector BuoyantMoment = FVector::ZeroVector;
};

/**
 * A fish on the line, part of ALakeFish: how it fights (bouts of pulling, rushes and series of
 * them, rests, a leap, a last run at the bank, tiring, the hook working loose) and its physical
 * body on the hook (FLakeSwimmer: six points from the mouth to the tail fin, the water, its weight,
 * the line at the mouth). The server runs the fight; every machine that shows the fish bends its
 * body behind a mouth held from outside (the rod's hook out of the water, the server's fish on a
 * client).
 */
class QUIETLAKE_API FLakeFishFight
{
public:
    enum class EFightPhase : uint8 { Pull, Burst, Gap, Pause };

    /** Struck: a fresh fight, the body laid out straight behind the mouth as the fish is now. */
    void Start();
    /** Drawn out of the water on the line (server): from now on the angler's rod carries it. */
    void LiftOut();
    void LowerIntoWater(FVector LineVelocity);
    /** How much of the body the water holds with the mouth at Mouth and the body turned so. */
    FFishImmersion ImmersionAt(FVector Mouth, FQuat Rotation) const;
    /**
     * Out of the water on the line: placed on every machine from the rod's simulated hook. It hangs
     * from its mouth along the line, swings with it and thrashes; still in the water it is drawn
     * mouth first.
     */
    void HangOnLine(FVector Mouth, FVector LineUp, float DeltaSeconds);
    /** Consider a final run before landing; a leap or final run must finish first. */
    bool ReadyToLand();
    /** Which way it fights now (Desired, MaxSpeed): bouts, rushes, rests, against the line. */
    void UpdateFightMotion(float DeltaSeconds, FVector& Desired, float& MaxSpeed);
    /** Where the next bout heads; bFinalRun: down and away, the rush on feeling the hook or at the bank. */
    void ChooseFightTarget(bool bFinalRun = false);
    /** On a slack line: somewhere it would rather be (weeds, snags, the deep, or back at the angler). */
    void ChooseEscapeTarget();
    void StartFightBurst();
    void FightSplash();
    /** On the hook: the fish's swimmer (FLakeSwimmer) under its own muscles, the water and the line. */
    void MoveHooked(FVector DesiredVelocity, float MaxSpeed, float DeltaSeconds);
    /** The body's greatest height as a share of its length. */
    float BodyHeightShare() const;
    /** How fast this species beats its tail for its length: its strength (see FLakeSwimmer::BeatScale). */
    float SwimBeatScale() const;
    /** Lays the swimmer out straight behind the mouth along Direction, moving as the fish does. */
    void StartSwimmer(FVector Direction);
    /** Puts the fish on its swimmer: the head along the swimmer's head, the mouth where it has it. */
    void PoseFromSwimmer(float RollDegrees);
    /** One step of the swimmer with its mouth held at Mouth from outside. */
    void SwimHeldByMouth(FVector Mouth, float TailEffort, float Steering, float Climb, float DeltaSeconds);

    ALakeFish* Fish = nullptr;        // the fish this is part of (set by its constructor)

    // The fight (server).
    EFightPhase FightPhase = EFightPhase::Pause;
    float FightRemaining = 0, FightIndividual = 1, NextFightJump = 0;
    bool bFightAwake = false;         // the line has pulled: it now knows and fights
    int32 FightPulses = 0;
    bool bFightJump = false, bShoreTried = false, bShoreRun = false;
    bool bFirstRunDone = false;       // the rush a fish makes the moment it feels the hook
    float SurfaceRoll = 0;            // degrees: how far a hooked fish has turned on its side at the top
    bool bWasSlack = false;           // the line was slack last step
    float NextEscape = 0;             // when a fish on a slack line picks where to make off to again
    float NextFightDisturb = 0;
    float HookGrip = 1;               // how well the hook holds, 1 .. 0 (off)
    float HookedWater = 1;            // how much of the body the water holds (see ALakeFish::BodyInWater)
    FVector PendingLineForce = FVector::ZeroVector;   // the line's pull at the mouth this frame, gram-force
    float PendingLineSpring = 0;

    // The body on the hook: on the server all of its motion (MoveHooked), out of the water and on
    // clients the bending of the body behind a mouth held from outside (SwimHeldByMouth).
    FLakeSwimmer Swim;
    bool bSwimming = false;
    // Where the swimmer left the fish, to notice it being moved or thrown from outside.
    FVector SwimAt = FVector::ZeroVector, SwimVel = FVector::ZeroVector;
    FQuat SwimPose = FQuat::Identity;
    // How far the line has the better of it (0..1): drawn back through the water tail first, the
    // fish has lost the fight for its head (MoveHooked).
    float Drawn = 0;
    // Hanging on the line (every machine).
    float HangTime = 0, HangEffort = 0;
    uint64 LastHangFrame = 0;
};
