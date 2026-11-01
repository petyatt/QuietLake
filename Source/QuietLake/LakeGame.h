#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "LakeFish.h"
#include "LakeGame.generated.h"

/** The pole in the angler's hands. All are 6 m long; the weight changes how they move: a light
 *  carbon pole swings quick and easy, a heavy fibreglass one slowly, lagging and springing more. */
UENUM()
enum class ERodKind : uint8 { Light, Medium, Heavy, Count UMETA(Hidden) };

UCLASS()
class QUIETLAKE_API ALakeCharacter : public ACharacter
{
    GENERATED_BODY()
public:
    ALakeCharacter();
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void BeginPlay() override;
    UPROPERTY(Replicated) EFishingState FishingState = EFishingState::Idle;
    UPROPERTY(Replicated) int32 CatchCount = 0;
    /** Balls of groundbait left in the bag (G throws one). */
    UPROPERTY(Replicated) int32 GroundbaitLeft = 30;
    UPROPERTY(Replicated) FString Status = TEXT("Подойдите к воде. ЛКМ — забросить.");
    UPROPERTY(Replicated) FVector FloatPosition = FVector::ZeroVector;
    UPROPERTY(Replicated) float Tension = 0;
    UPROPERTY(Replicated) float FishDistance = 850;
    UPROPERTY(Replicated) EBait Bait = EBait::Worm;
    /** Incremented on every nibble so clients can twitch the bobber. */
    UPROPERTY(Replicated) uint8 NibbleCounter = 0;
    /** What the float is doing now (a fish at the bait), replicated so every machine animates it. */
    UPROPERTY(Replicated) EBiteMotion BiteMotion = EBiteMotion::None;
    UPROPERTY(Replicated) float BiteStart = 0;        // server time
    UPROPERTY(Replicated) float BiteDuration = 1;
    UPROPERTY(Replicated) float BiteAmount = 0;
    // Cast and rod state, replicated so every machine can animate the rod and line itself.
    UPROPERTY(Replicated) bool bCharging = false;          // LMB held for a cast: the rig held in the hand, the rod tip loading
    UPROPERTY(Replicated) float ChargeServerStart = 0;     // when LMB was pressed
    UPROPERTY(Replicated) float CastDropElevation = 45;
    UPROPERTY(Replicated) FVector CastDropGrip = FVector(28, 18, 14);
    UPROPERTY(Replicated) FVector RigGripLocal = FVector(46, -20, 22);
    UPROPERTY(Replicated) float RigGripTime = -100;
    UPROPERTY(Replicated) bool bRetrieving = false;        // the rod is being raised, drawing the rig out of the water
    UPROPERTY(Replicated) float RetrieveServerTime = -100;
    /**
     * How far the rod has been raised, 0..1: it goes up only while LMB is held (bRetrieveHeld); let go,
     * the rod stops where it is and holds, the next press goes on from there.
     */
    UPROPERTY(Replicated) float RetrieveProgress = 0;
    UPROPERTY(Replicated) bool bRetrieveHeld = false;
    /** The rod goes all the way up by itself: the rig landed on the bank. */
    bool bRetrieveAuto = false;
    UPROPERTY(Replicated) float RetrieveFromElevation = 20;
    /** Shortest time of raising the rod from the water to upright (with LMB held all the way). */
    static constexpr float RetrieveTime = 2.5f;
    /** Fastest the rod tip moves lifting the rig out (cm/s): the line slack, taut with the rig in the water, the rig in the air. */
    static constexpr float RetrieveTipFast = 420;
    static constexpr float RetrieveTipSlow = 170;
    static constexpr float RetrieveTipAir = 320;
    /** The planned lift at progress R: the tip's place over the ground (actor space) and the right hand (OutGrip). */
    FVector2D RetrieveTipPlan(float R, FVector& OutGrip) const;
    /** cm the planned tip moves per unit of progress around R. */
    float RetrieveTipSpeed(float R) const;
    /** At the top of the lift: the hands up at the chest, the tip over the free hand (actor space, cm). */
    static constexpr float RetrieveGripX = 14;
    static constexpr float RetrieveGripY = 12;
    static constexpr float RetrieveGripZ = 36;
    static constexpr float RetrieveTipX = 38;
    static constexpr float RetrieveTipY = -22;
    UPROPERTY(Replicated) FVector RetrieveFromGrip = FVector::ZeroVector;
    UPROPERTY(Replicated) float RetrieveFromYaw = 7;
    /** Where the rig lay when the lift began (actor space, cm): the tip first goes up over it. */
    UPROPERTY(Replicated) FVector2D RetrieveRigLocal = FVector2D(300, 0);
    /**
     * The cast from the hand: holding LMB the angler takes the rig in the left hand and draws it
     * back to the hip, bending the rod tip; let go, the loaded tip swings the rig out in front. Let
     * go before the tip is loaded (CastLoad seconds) the rig falls short in a heap.
     */
    static constexpr float CastLoad = 0.5f;          // for the medium rod; see CastLoadTime
    UPROPERTY(Replicated) ERodKind RodKind = ERodKind::Medium;
    static float RodGrams(ERodKind Kind);
    static const TCHAR* RodName(ERodKind Kind);
    /** How sluggish the rod is: 1 for the medium rod, more for a heavier one (square root of the weight ratio). */
    float RodHeft() const { return FMath::Sqrt(RodGrams(RodKind) / RodGrams(ERodKind::Medium)); }
    /** Seconds until the rod tip is loaded for the cast: a heavier rod bends slower. */
    float CastLoadTime() const { return CastLoad * RodHeft(); }
    UPROPERTY(Replicated) ECastResult CastResult = ECastResult::Good;
    UPROPERTY(Replicated) float CastSeverity = 0;            // 0..1: how much too soon the rig was let go
    UPROPERTY(Replicated) FVector_NetQuantize10 CastHookLanding;   // where the hook came down (beyond the float when laid out straight)
    /** Seconds since LMB was pressed for the cast (0 when not casting). */
    float SwingTime(float Now) const { return bCharging ? FMath::Max(0.0f, Now - ChargeServerStart) : 0.0f; }
    UPROPERTY(Replicated) float CastServerTime = -100;      // when the cast was released
    /**
     * Persistent rod aim relative to the view. The mouse moves the rod in every fishing phase.
     * Up and down the camera moves with the rod; casting, it stays within lake.CastCameraMin/Max
     * and only the rod goes on (RodLowestAngle..RodHighestAngle). Aside the camera turns with the rod. LMB holds the shot,
     * release frees the rig without resetting the aim or choosing a landing point.
     * Degrees added to the rod's angle and turn (the owner's own values; others get them replicated).
     */
    UPROPERTY(Replicated) float SwingPitch = 0;
    UPROPERTY(Replicated) float SwingYaw = 0;
    /**
     * While the rig or a fish comes out of the water and swings in to the free hand the camera moves
     * with the mouse as usual (aside the angler turns, and the rod with him); up and down the rod is
     * also lifted a little more or less (degrees). Eases back to zero once the rig is in the hand.
     */
    UPROPERTY(Replicated) float RigSteerPitch = 0;
    UPROPERTY(EditDefaultsOnly, Category="Rod|Input") float RigSteerPitchLimit = 15;
    /** The rig or a fish is coming in to the hand: the rod is raised by itself, the mouse only nudges it. */
    bool IsSteeringRig() const;
    static constexpr float SwingBase = 45;          // the rod's angle ready to cast, the rig in the other hand
    /** Mouse degrees per input unit. */
    UPROPERTY(EditDefaultsOnly, Category="Rod|Input") float RodMouseSensitivity = 1.25f;
    UPROPERTY(EditDefaultsOnly, Category="Rod|Input") float RodYawLimit = 20;
    UPROPERTY(EditDefaultsOnly, Category="Rod|Input") float RodPitchDownLimit = 140;
    UPROPERTY(EditDefaultsOnly, Category="Rod|Input") float RodPitchUpLimit = 140;
    /** How far the rod can be lowered (its tip about at the ground) and raised, degrees above level. */
    UPROPERTY(EditDefaultsOnly, Category="Rod|Input") float RodLowestAngle = -15;
    UPROPERTY(EditDefaultsOnly, Category="Rod|Input") float RodHighestAngle = 95;
    /** The rod's angle with a level view while the rig is in the water: lower, nearer the water. */
    UPROPERTY(EditDefaultsOnly, Category="Rod|Input") float RodWaitAngle = 25;
    /** The rod's angle with a level view, before the mouse moves it: ready to cast, or waiting for a bite. */
    float RodBase() const
    {
        // (Also while the cast flies: the rod goes down with it, so the line reaches the place picked.)
        const bool bInWater = !bRetrieving && (FishingState == EFishingState::Waiting || FishingState == EFishingState::Bite);
        return bInWater ? RodWaitAngle : SwingBase;
    }
    /** The free hand holds the rig by the shot (it catches it as the rig swings by, and lets go to cast). */
    UPROPERTY(Replicated) bool bRigInHand = true;
    /** Let go and not yet down: the server takes where it lands from its own run of the rod simulation. */
    UPROPERTY(Replicated) bool bRigFlying = false;
    UPROPERTY(Replicated) float CastFlight = 1;
    UPROPERTY(Replicated) float StrikeServerTime = -100;
    /** Landing: when the tired fish was lifted out of the water on the line, to be swung to the hand. */
    UPROPERTY(Replicated) float LandServerTime = -100;
    UPROPERTY(Replicated) float LandProgress = 0;
    /**
     * The fish is taken in the free hand: it stays visible there for HandFishTime, then goes into the
     * keepnet. The catch counts when it is taken, not when it is put away.
     */
    UPROPERTY(Replicated) bool bFishInHand = false;
    UPROPERTY(Replicated) float HandFishStart = -100;
    static constexpr float HandFishTime = 2.5f;
    static constexpr float LandTime = 4.0f;
    /** Seconds of raising the rod with a fish on the line, LMB held. */
    static constexpr float LandLiftTime = 2.8f;
    UPROPERTY(Replicated) float HookDepth = 100;            // rig in the water: bobber to hook
    /** Bobber-to-hook distance the angler has set (cm); the bait sinks this deep, or lies on the bottom. */
    UPROPERTY(Replicated) float FloatDepth = 100;
    static constexpr float MinDepth = 10.0f;
    static constexpr float MaxDepth = 500.0f;   // about 17% short of the 6 m rod
    UPROPERTY(Replicated) bool bReeling = false;            // fighting: LMB held = lifting the rod, pulling
    UPROPERTY(Replicated) float RodLift = 0;                // fighting: 0 = rod lowered towards the fish, 1 = raised high
    UPROPERTY(Replicated) bool bRodOnStand = false;
    UPROPERTY(Replicated) FVector_NetQuantize10 StandBase;
    UPROPERTY(Replicated) float StandYaw = 0;
    UPROPERTY(Replicated) TObjectPtr<ALakeFish> LineFish;  // fish holding the bait or hooked
    bool bThirdPerson = false;
    /** Server world time, the same clock on every machine. */
    float ServerNow() const;

    // --- used by the fish (server) ---
    bool HasBaitInWater() const;
    FVector BaitLocation() const { return BaitPoint; }
    /** Only one fish at a time may test the bait; returns false if another fish is on it. */
    bool ClaimBait(ALakeFish* Fish);
    void ReleaseBaitClaim(ALakeFish* Fish);
    void OnFishNibble(ALakeFish* Fish);
    /**
     * The fish takes: the float may be struck until the fish lets go of the bait (it decides that
     * itself, see LakeFish::NextBiteStep), at most Window seconds (< 0: a typical hold for the
     * species, for a fish set on the bait directly).
     */
    void OnFishTake(ALakeFish* Fish, float Window = -1);
    /**
     * How much the rig moves under a fish's nose right now, 0..1: the rod swung or lifted quickly
     * (anything above ~15 deg/s), or the rig being drawn out. A fish testing the bait backs off; one
     * holding it feels it and spits it sooner.
     */
    float RigDisturbance() const;
    /** The float starts a new motion of the bite. */
    void OnBiteMotion(ALakeFish* Fish, const FBiteStep& Step);
    /** The fish holding the bait lets go of it without being struck (as when the time to strike runs out). */
    void OnFishLetGo(ALakeFish* Fish) { if (BaitFish.Get() == Fish) Escape(); }
private:
    friend class FLakeFishingTest;
    friend struct FLakeFishingFixture;
    friend class FLakeBitePatternsTest;
    friend class FLakeFightProfilesTest;
    friend class FLakeContactPhysicsTest;
    friend class FLakeFishBehaviourTest;
    friend class FLakeCastingTest;
    friend class FLakeWindDriftTest;
    friend class FLakeRodControlTest;
    friend class FLakeGroundbaitTest;
    void SelectBait(EBait NewBait);
    void BaitWorm() { SelectBait(EBait::Worm); }
    void BaitMaggot() { SelectBait(EBait::Maggot); }
    void BaitBread() { SelectBait(EBait::Bread); }
    void Shallower() { ServerChangeDepth(-1); }
    void Deeper() { ServerChangeDepth(1); }
    UFUNCTION(Server, Reliable) void ServerChangeDepth(float Delta);
    /** Hang the bait FloatDepth under the float, or on the bottom if the water is shallower. */
    void PlaceBait();
    UFUNCTION(Server, Reliable) void ServerSelectBait(EBait NewBait);
    FVector RodTip() const;
    void ToggleRod();
    void ToggleView();
    void ToggleLifePanel();
    void NextRod() { ServerNextRod(); }
    /** Q: take the next rod (only with no line out). */
    UFUNCTION(Server, Reliable) void ServerNextRod();
    /** -LakePreview, -BitePreview, -LandPreview: scripted scenes, filmed and logged (LakePreviews.cpp). */
    void RunPreviews();
    void RunRodDiagnostics(float DeltaSeconds);
    float RodDiagnosticTime = 0, NextRodDiagnosticLog = 0;
    int32 RodDiagnosticStage = 0;
    UFUNCTION(Server, Reliable) void ServerToggleRod();
    /** G: a ball of groundbait to the float (or where the angler looks), with the hook bait mixed in. */
    void ThrowGroundbait();
    /** Where a ball goes: the float when the rig is in the water, otherwise where the angler looks (up to 12 m). */
    FVector GroundbaitTarget() const;
    UFUNCTION(Server, Reliable) void ServerThrowGroundbait(FVector_NetQuantize Target);
    float NextGroundbait = 0;   // server: a moment between throws
    /** LMB pressed with nothing out: the rig taken in the hand. */
    void StartCast();
    /** LMB release: make a low cast, with distance determined by Held seconds. */
    void DoCast(float Held);
    /** The cast rig came down at Where: on the water the bait starts fishing, on the bank it is lifted out. */
    void OnRigLanded(FVector Where);
    /** Where the free hand holds the rig (Draw: 0..1 drawn back for the cast). */
    FVector HandPoint(float Draw) const;
    /** How far from HandPoint the free hand takes a fish lifted out (cm). */
    static constexpr float CatchReach = 32.0f;
    /** The left shoulder (actor space) and how far the hand reaches from it to take the rig by the shot. */
    FVector LeftShoulder() const;
    static constexpr float ArmReach = 62.0f;
    /** Fastest the shot may pass (cm/s, relative to the angler) and still be taken in the fingers. */
    static constexpr float CatchSpeed = 320.0f;
    /** 0..1: how far the free hand reaches out for the rig coming in on the line. */
    float RigReach() const;
    /** Rod elevation while waiting (deg): with a deep float the main line is short, so the tip goes down to the water. */
    float WaitElevation(float Pitch, float Depth) const;
    void RodPoseTarget(float Pitch, float Now, float& OutElevation, FVector& OutGrip, float& OutStiffness, float& OutYaw) const;
    void UpdateRodPose(float DeltaSeconds);
    FTransform RodHold() const;
    FTransform StandHold() const;
    void ConstrainFloat();
    /** Server: the wind drifts a free-hanging rig downwind until the line holds it. */
    void DriftFloat(float DeltaSeconds);
    static constexpr float DriftShare = 0.015f;   // of the wind's speed
    // Weighted rod pose (springs), the same on server and clients.
    float RodElevation = 45, RodElevationSpeed = 0, RodYaw = 0, RodYawSpeed = 0;
    FVector RodGrip = FVector(28, 20, 10), RodGripSpeed = FVector::ZeroVector;
    bool bRodPoseReady = false;
    void UpdateVisuals(float DeltaSeconds);
    float BaitActiveTime = 0;
    float StandAlpha = 0;
    UPROPERTY() TObjectPtr<class ALakeRod> RodVisual;
    UPROPERTY() TObjectPtr<class USpringArmComponent> ThirdPersonArm;
    UPROPERTY() TObjectPtr<class UCameraComponent> ThirdPersonCamera;
    void Forward(float Value);
    /** Mouse steers the rod; the outward part near the hand limits turns the view. */
    void Turn(float Value);
    void Look(float Value);
    void MoveRodAxis(float Delta, bool bPitch);
    UFUNCTION(Server, Unreliable) void ServerSwing(float Pitch, float Yaw);
    UFUNCTION(Server, Unreliable) void ServerSteer(float Pitch);
    UFUNCTION(Server, Reliable) void ServerReleaseWithAim(float Pitch, float Yaw);
    float NextAimSend = 0;
    void Right(float Value);
    void Fish();
    void Cancel();
    void Release();
    void UpdateFight(float DeltaSeconds);
    void ResolveHook(float DeltaSeconds);
    FVector LoadOnFish(ALakeFish* Fish) const;
    float StrikeUntil = -1, HookImpulse = 0;
    float YieldUntil = -1;
    void FinishFishing(const FString& Message);
    /** Raise the rod and draw the rig out while the button is held (or by itself, bAuto); the cast ends when the rod is up. */
    void StartRetrieve(const FString& Message, const FString& EndMessage, bool bAuto = false);
    void UpdateRetrieve(float DeltaSeconds);
    /** Lift the tired fish out of the water: it hangs on the line and swings to the angler's hand. */
    void StartLanding(class ALakeFish* Fish);
    void UpdateLanding();
    bool TakeRodFromStand();
    FString RetrieveEndMessage;
    /** Server: what the catch line says once the fish is in the hand. */
    FString CatchMessage;
    UFUNCTION(Server, Reliable) void ServerRelease();
    UFUNCTION(Server, Reliable) void ServerFish();
    UFUNCTION(Server, Reliable) void ServerCancel();
    /** RMB let go: the rig stops coming out of the water. */
    UFUNCTION(Server, Reliable) void ServerStopRetrieve();
    void CancelReleased();
    void Escape();
    /** RMB: the strike. On a bite it hooks the fish (or misses it); otherwise it frightens a fish trying the bait. */
    void Strike();
    FTimerHandle FishingTimer;
    double NextActionTime = 0;
    float FightTime = 0;
    int32 FightHint = -1;
    FVector BaitPoint = FVector::ZeroVector;
    TWeakObjectPtr<ALakeFish> BaitFish;     // fish testing or holding the bait
    TWeakObjectPtr<ALakeFish> HookedFish;
    UPROPERTY() TObjectPtr<class UCameraComponent> FishingCamera;
};

UCLASS()
class QUIETLAKE_API ALakeEnvironment : public AActor
{
    GENERATED_BODY()
public:
    ALakeEnvironment();
    virtual void BeginPlay() override;
    static float GroundHeight(float X, float Y);
    static bool IsWater(FVector Position);
    /** 1 at the shoreline of the elliptic lake, 0 at its centre. */
    static float LakeRadius(float X, float Y);
};

UCLASS()
class QUIETLAKE_API ALakeGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    ALakeGameMode();
    virtual void BeginPlay() override;
    virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
private:
    int32 NextSpawnSlot = 0;
};
