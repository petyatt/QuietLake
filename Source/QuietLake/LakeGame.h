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

/** Where one of the angler's rods is. */
UENUM()
enum class ERodPlace : uint8 { Case, Hands, Stand, Ground };
/** The rod case: carried on the back, or laid on the ground (and then it can be opened). */
UENUM()
enum class ECasePlace : uint8 { Back, Ground };

/**
 * One of the angler's three rods (index = ERodKind). The rod in the hands fishes with the angler's
 * own fields (FishingState, FloatPosition...); a rod put on its stand keeps its tackle here and goes
 * on fishing: the float on the water, fish trying and taking the bait.
 */
USTRUCT()
struct FLakeRodSlot
{
    GENERATED_BODY()
    UPROPERTY() ERodPlace Place = ERodPlace::Case;
    UPROPERTY() int8 CaseSlot = -1;                 // compartment 0..2 while in the case
    UPROPERTY() FVector_NetQuantize10 Base;         // the stand's foot, or the butt on the ground
    UPROPERTY() float Yaw = 0;                      // the way the stand or the rod on the ground faces
    UPROPERTY() float Pitch = 0;                    // lying on a slope: butt to tip
    // The tackle of a rod on its stand.
    UPROPERTY() EFishingState State = EFishingState::Idle;   // Idle, Waiting or Bite
    UPROPERTY() FVector_NetQuantize10 FloatPosition;
    UPROPERTY() float HookDepth = 100;
    UPROPERTY() float FloatDepth = 100;
    UPROPERTY() EBait Bait = EBait::Worm;
    UPROPERTY() uint8 Nibbles = 0;
    UPROPERTY() EBiteMotion BiteMotion = EBiteMotion::None;
    UPROPERTY() float BiteStart = 0;
    UPROPERTY() float BiteDuration = 1;
    UPROPERTY() float BiteAmount = 0;
    UPROPERTY() TObjectPtr<ALakeFish> LineFish;
    // Server only.
    FVector BaitPoint = FVector::ZeroVector;
    float BaitActiveTime = 0;
    float EscapeAt = -1;                            // a take not struck: when the fish lets go
    TWeakObjectPtr<ALakeFish> BaitFish;
};

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
    virtual void PossessedBy(AController* NewController) override;
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
     * the rod stops where it is and holds, the next press goes on from there. Nothing else moves the
     * rig: it hangs on the line and is drawn in and lifted by the rod tip alone.
     */
    UPROPERTY(Replicated) float RetrieveLift = 0;
    UPROPERTY(Replicated) bool bRetrieveHeld = false;
    /** The rod goes all the way up by itself: the rig landed on the bank. */
    bool bRetrieveAuto = false;
    /** Seconds of LMB held to raise the rod from the water to upright: slow, the pole is heavy. */
    static constexpr float RetrieveTime = 3.6f;
    /** A rig lying far out is lifted more slowly (times RetrieveTime): it skates in over the water
     *  and leaves it nearer the angler, instead of swinging in from afar and past him. */
    float RetrieveSlow = 1;
    /** At the top of the lift: the rod almost upright. */
    static constexpr float RetrieveTopElevation = 84;
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
    /**
     * The cast is aimed: the rig is laid on the water where the angler looks, as far as the line
     * reaches from where he stands (CastReach). The owner shows the spot (a ring on the water) and
     * sends it with the release; the server keeps it within reach and on the water.
     */
    UPROPERTY(Replicated) FVector_NetQuantize CastTarget;
    FVector CastAimPoint() const;
    float CastReach() const;
    FVector ValidCastTarget(FVector Wanted) const;
    static constexpr float CastNearest = 220.0f;
    FVector PendingCastAim = FVector::ZeroVector;   // the owner's aim sent with the release (zero: none)
    FVector HandFishFrom = FVector::ZeroVector;     // where the fish hung when the free hand took it
    bool bHandFishSeen = false;
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
        const bool bInWater = !bRetrieving && (FishingState == EFishingState::Waiting || FishingState == EFishingState::Bite || FishingState == EFishingState::Fight);
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
    /**
     * A bite or a fight: how far the hands have raised the rod above where the mouse aims it, in parts
     * of RodLiftDegrees. 1 = raised high; below zero a fish has dragged it down past the aim, at the
     * lowest the rod lies along the line and gives no more.
     */
    UPROPERTY(Replicated) float RodLift = 0;
    float RodLiftShown = 0;   // the lift the pose shows: follows RodLift, then eases out when the fish leaves
    float ArmGiveShown = 0;   // degrees the loaded rod is drawn down in the hands at once (from Tension), smoothed
    // The tackle in the hands (what its line, hook and rod stand: FLakeTackle) and the hands on a
    // loaded rod. Forces in gf at the rod tip.
    static FLakeTackle TackleOf(ERodKind Kind);
    FLakeTackle Tackle() const { return TackleOf(RodKind); }
    static constexpr float HandPullForce = 380.0f;     // the most the hands lift with (LMB held): a 6 m lever
    static constexpr float HandHoldForce = 220.0f;     // what they hold against once LMB is let go
    static constexpr float HandLandWeight = 1.2f;      // kg: a heavier fish is not lifted on the line, it is led to the bank
    static constexpr float RodLiftDegrees = 55.0f;
    static constexpr float RodLiftSpeed = 1.0f;        // parts per second on a slack line
    /**
     * The arms give under load like a second spring behind the rod's bend: ArmGiveForce gf at the tip
     * draws the rod down a degree at once (a degree moves the tip 10.5 cm). In series with the blank
     * (ALakeRod::TetherForce) this leaves HandSeries of the blank's own stiffness: with the medium rod
     * about 4.4 m of give before the line breaks instead of 1.4.
     */
    static constexpr float ArmGiveForce = 90.0f;
    static float HandSeries(const FLakeTackle& T) { return 1.0f / (1.0f + T.RodSpring * 10.47f / ArmGiveForce); }
    /** The hands on a loaded rod: LMB lifts against the load, a harder pull drags the rod down (LakeGame.cpp). */
    void UpdateRodHands(float DeltaSeconds, float Force);
    // --- the three rods and the case ---
    static constexpr int32 NumRods = (int32)ERodKind::Count;
    /** Every rod: in the case, in the hands, on its stand (fishing) or lying on the ground. */
    UPROPERTY(Replicated) TArray<FLakeRodSlot> Rods;
    UPROPERTY(Replicated) ECasePlace CasePlace = ECasePlace::Back;
    UPROPERTY(Replicated) FVector_NetQuantize10 CaseBase;
    UPROPERTY(Replicated) float CaseYaw = 0;
    /** The case on the ground with its lid open (its window shows for the angler who opened it). */
    UPROPERTY(Replicated) bool bCaseOpen = false;
    /** How near the angler must be to a rod or the case to use it, and how far he lays things down (cm). */
    static constexpr float Reach = 260.0f;
    static constexpr float PlaceReach = 250.0f;
    /** Seconds R is held to lay the rod on the ground, and F to do what the prompt shows. */
    static constexpr float LayHold = 2.0f;
    static constexpr float InteractHold = 0.5f;
    bool HasRodInHands() const { return Rods.IsValidIndex((int32)RodKind) && Rods[(int32)RodKind].Place == ERodPlace::Hands; }
    /** A new game starts with empty hands: every rod in its compartment of the case on the back. */
    void StowRodsInCase();
    /** StowRodsInCase for a real game start (not the previews), once the angler has a controller and has begun play. */
    void StowRodsAtStart();
    /** The rod in the hands can go into the case or onto the ground: its rig held in the free hand, nothing going on. */
    bool CanPutRodAway() const;
    /** Where a rod on its stand rests (butt on the ground behind the fork). */
    static FTransform StandHoldAt(FVector Base, float Yaw);
    /** A rod lying on the ground, butt at Base. */
    static FTransform GroundHoldAt(FVector Base, float Yaw, float Pitch);
    /** The rod on a stand or the ground nearest the angler within reach (-1: none); with bBiteOnly, only a take. */
    int32 NearestParkedRod(bool bBiteOnly) const;
    /** How far the angler is from where a rod on its stand or the ground can be taken (cm). */
    float DistanceToRod(int32 Rod) const;

    // --- local interface (the owner only, not replicated) ---
    enum class EPlacing : uint8 { None, Case, Rod };
    /** B, or R held: a blue ghost follows the aim; LMB lays it there, RMB cancels. */
    EPlacing Placing = EPlacing::None;
    bool bPlaceValid = false;
    FVector PlaceAt = FVector::ZeroVector;
    float PlaceYaw = 0;
    FString PlaceProblem;
    /** What the crosshair is on: the case lying near (Open / Put on the back), or a rod on the ground. */
    bool bAimCase = false;
    int32 AimRod = -1;
    TArray<FString> AimOptions;
    int32 AimChoice = 0;
    /** Real time F / R went down (-1: up). */
    float InteractDown = -1, LayDown = -1;
    bool bLayDone = false;
    /** The case window: three compartments and the hand, the rods dragged between them with the mouse. */
    bool bCaseWindow = false;
    float CaseWindowAsked = 0;
    bool bWindowInput = false;
    bool IsCaseWindowOpen() const { return bCaseWindow && bCaseOpen && CasePlace == ECasePlace::Ground; }
    void CloseCaseWindow();
    UPROPERTY(Replicated) TObjectPtr<ALakeFish> LineFish;  // fish holding the bait or hooked
    bool bThirdPerson = false;
    /** Dev camera for looking the level over: 8 toggles it, WASD flies, Q/E up and down, Shift ×3. */
    void ToggleFlyCamera();
    void FlyUpInput(float Value);
    void FlyBoostPressed();
    void FlyBoostReleased();
    void UpdateFlyCamera(float DeltaSeconds);
    bool bFlyCamera = false;
    bool bFlyBoost = false;
    UPROPERTY() TObjectPtr<class ACameraActor> FlyCamera;
    float FlyForwardInput = 0.0f, FlyRightInput = 0.0f, FlyUpValue = 0.0f;
    float FlyViewYaw = 0.0f, FlyViewPitch = 0.0f, FlyNextLog = 0.0f;
    /** Server world time, the same clock on every machine. */
    float ServerNow() const;

    // --- used by the fish (server) ---
    // Rig: which rod's tackle (index = ERodKind); -1 is the rod in the hands. A fish keeps the rig it
    // goes for in ALakeFish::TargetRig, so the calls taking a fish find it there.
    bool HasBaitInWater(int32 Rig = -1) const;
    FVector BaitLocation(int32 Rig = -1) const;
    EBait BaitOf(int32 Rig = -1) const;
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
    float RigDisturbance(int32 Rig = -1) const;
    /** The float starts a new motion of the bite. */
    void OnBiteMotion(ALakeFish* Fish, const FBiteStep& Step);
    /** The fish holding the bait lets go of it without being struck (as when the time to strike runs out). */
    void OnFishLetGo(ALakeFish* Fish);
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
    friend class FLakeRodCaseTest;
    friend class FLakeThreeRodsTest;
    friend class ALakeHUD;
    /** The rig a fish is after: its own TargetRig, or the rod in the hands. */
    int32 RigOf(const ALakeFish* Fish) const;
    /** The rig is the rod in the hands (its tackle lives in the angler's own fields). */
    bool IsHandRig(int32 Rig) const { return Rig < 0 || (HasRodInHands() && Rig == (int32)RodKind); }
    bool IsParkedRig(int32 Rig) const { return Rig >= 0 && Rig < Rods.Num() && Rods[Rig].Place == ERodPlace::Stand && Rods[Rig].State != EFishingState::Idle; }
    /** The rod in the hands goes onto its stand (its tackle goes on fishing) or onto the ground. */
    void ParkHandRod(ERodPlace Place, FVector Base, float Yaw, float Pitch = 0);
    /** Take a rod in the free hands: from the case, its stand or the ground (its tackle comes with it). */
    void TakeRodInHands(int32 Rod);
    /** Server, each frame: the tackle of the rods on their stands. */
    void UpdateParkedRods(float DeltaSeconds);
    void EscapeParked(int32 Rod);
    void PlaceBaitFor(FLakeRodSlot& Slot) const;
    /** Each rod's look: in the hands, on its stand, on the ground, or hidden in the case. */
    void UpdateRodVisuals(float DeltaSeconds);
    UPROPERTY() TArray<TObjectPtr<class ALakeRod>> RodVisuals;
    TArray<FTransform> RodShownHold;     // last frame's hold of each rod, for the blend to a new place
    TArray<float> RodBlend;              // 1 -> 0: easing from the old place to the new one
    TArray<ERodPlace> RodShownPlace;
    float HandsAlpha = 1;
    UPROPERTY() TObjectPtr<class ALakeRodCase> CaseVisual;
    bool bCaseSeenLogged = false;
    UPROPERTY() TObjectPtr<class ALakeGhost> Ghost;
    void UpdateCaseVisual(float DeltaSeconds);
    /** Local: the ghost under the aim, the thing under the crosshair, the held keys. */
    void UpdateLocalInterface(float DeltaSeconds);
    bool AimGround(FVector& OutPoint, FVector& OutNormal) const;
    /** The ground (or the pier's deck) under a point; false over water. */
    static bool GroundAt(const UWorld* World, FVector2D At, float NearZ, FVector& OutPoint, FVector* OutNormal = nullptr);
    void SetCaseWindowInput(bool bOpen);
    // Keys.
    void CaseKey();
    void InteractPressed();
    void InteractReleased();
    void LayPressed();
    void LayReleased();
    void WheelUp();
    void WheelDown();
    UFUNCTION(Server, Reliable) void ServerPlaceCase(FVector_NetQuantize10 At, float Yaw);
    /** 0 open, 1 close, 2 put it on the back. */
    UFUNCTION(Server, Reliable) void ServerCaseAction(uint8 Action);
    /** Move a rod in the open case: Target 0..2 a compartment, 3 the hands. */
    UFUNCTION(Server, Reliable) void ServerMoveRod(uint8 Rod, uint8 Target);
    UFUNCTION(Server, Reliable) void ServerLayRod(FVector_NetQuantize10 At, float Yaw);
    UFUNCTION(Server, Reliable) void ServerTakeRod(uint8 Rod);
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
    /** Where the free hand holds a fish it has taken: up in front of the chest. */
    FVector HeldFishPoint() const;
    /** The line's own load on a fish (gf): the rod's and the arms' spring, without the damping that steadies the fish. */
    float LineLoad(ALakeFish* Fish) const;
    bool bRodLaidFlat = false;   // a fish has dragged the rod down along the line: nothing gives any more
    /** The pull in the line (gf), and the part of it square to the rod that bends (and may break) the blank. */
    UPROPERTY(Replicated) float LineLoadGf = 0;
    float RodLoadGf = 0;
    /** The rod, not the line, is nearer what it stands (held across the line). */
    UPROPERTY(Replicated) bool bRodStrained = false;
    void ToggleRod();
    void ToggleView();
    void ToggleLifePanel();
    /** M: the whole lake over the screen (the minimap is always in the top-right corner). */
    void ToggleMap();
    /** F12: save a screenshot into Saved/Screenshots (for showing what the world looks like now). */
    void TakeScreenshot();
    /** The angler's own clothes over the hidden mannequin: boots, trousers, sweater, jacket, hands,
     *  head and cap. The mannequin keeps animating (it drives the bones the parts hang on). */
    void CreateFisherOutfit();
    /** 6 (development only): one game hour forward, to look at the light without waiting for the day. */
    void CycleDevTime() { ServerDevTime(); }
    UFUNCTION(Server, Reliable) void ServerDevTime();
    /** 7 (development only): the next weather in a short ring, to look at rain, fog and wind. */
    void CycleDevWeather() { ServerDevWeather(); }
    UFUNCTION(Server, Reliable) void ServerDevWeather();
    /** -LakePreview, -BitePreview, -LandPreview: scripted scenes, filmed and logged (LakePreviews.cpp). */
    void RunPreviews();
    /** -FishingBench: casts, bites and fights played by a scripted hand and measured (LakeBench.cpp). */
    void RunFishingBench();
    /** Previews: this rod in the hands at once, the one held put into its compartment. */
    void UseRod(ERodKind Kind);
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
    static constexpr float CatchSpeed = 480.0f;
    /** 0..1: how far the free hand reaches out for the rig coming in on the line. */
    float RigReach() const;
    /** Rod elevation while waiting (deg): with a deep float the main line is short, so the tip goes down to the water. */
    float WaitElevation(float Pitch, float Depth) const;
    void RodPoseTarget(float Pitch, float Now, float& OutElevation, FVector& OutGrip, float& OutStiffness, float& OutYaw) const;
    void UpdateRodPose(float DeltaSeconds);
    FTransform RodHold() const;
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
    /** The simulation of the rod in the hands (one of RodVisuals; null with empty hands or in tests). */
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
    UFUNCTION(Server, Reliable) void ServerReleaseWithAim(float Pitch, float Yaw, FVector_NetQuantize Aim);
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
    /** No rod in the hands: take the nearest one from its stand (a take first) or the ground; false if none in reach. */
    bool TakeNearestRod(bool bBiteOnly);
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
    /** Seconds of slack line while the fish is awake: too long and it throws the hook. */
    float SlackTime = 0;
    FVector BaitPoint = FVector::ZeroVector;
    TWeakObjectPtr<ALakeFish> BaitFish;     // fish testing or holding the bait
    TWeakObjectPtr<ALakeFish> HookedFish;
    UPROPERTY() TObjectPtr<class UCameraComponent> FishingCamera;
    /** MetaHuman face with its hair and brows, and the cap: they follow the body's bones and are hidden in the first person. */
    UPROPERTY() TArray<TObjectPtr<class USceneComponent>> OutfitHead;
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
    /** The ground's layer masks at a point, 0..1 each: R meadow, G forest floor, B bare clay on steep
     *  faces; none of them is the muddy shore and the lake bed. The terrain mesh carries them as vertex
     *  colour, the landscape as its Meadow / Forest / Clay layers (ULakeLevelLibrary). */
    static FLinearColor TerrainLayers(const FVector2D& P);
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
