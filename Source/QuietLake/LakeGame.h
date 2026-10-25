#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "LakeFish.h"
#include "LakeGame.generated.h"

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
    UPROPERTY(Replicated) int32 FishingState = 0;
    UPROPERTY(Replicated) int32 CatchCount = 0;
    UPROPERTY(Replicated) FString Status = TEXT("Подойдите к воде. ЛКМ — забросить.");
    UPROPERTY(Replicated) FVector FloatPosition = FVector::ZeroVector;
    UPROPERTY(Replicated) float Tension = 0;
    UPROPERTY(Replicated) float FishDistance = 850;
    UPROPERTY(Replicated) EBait Bait = EBait::Worm;
    /** Incremented on every nibble so clients can twitch the bobber. */
    UPROPERTY(Replicated) uint8 NibbleCounter = 0;
    // Cast and rod state, replicated so every machine can animate the rod and line itself.
    UPROPERTY(Replicated) bool bCharging = false;          // LMB held: the rod sweeps forward, laying the line out
    UPROPERTY(Replicated) float ChargeServerStart = 0;
    UPROPERTY(Replicated) bool bRetrieving = false;        // the rod is being raised, drawing the rig out of the water
    UPROPERTY(Replicated) float RetrieveServerTime = -100;
    UPROPERTY(Replicated) float RetrieveFromElevation = 20;
    /** Seconds of holding LMB for the longest cast, and of lifting the rig out of the water. */
    static constexpr float ChargeTime = 2.0f;
    static constexpr float RetrieveTime = 2.5f;
    /** 0..1: how far the casting sweep has gone. */
    float CastCharge(float Now) const { return bCharging ? FMath::Clamp((Now - ChargeServerStart) / ChargeTime, 0.0f, 1.0f) : 0.0f; }
    /** Angle of the line below the horizon (deg) during the sweep: hanging straight down at the start. */
    static float SweepLineAngle(float Charge) { return FMath::Lerp(-88.0f, -14.0f, FMath::InterpEaseInOut(0.0f, 1.0f, Charge, 1.8f)); }
    static float SweepElevation(float Idle, float Charge) { return FMath::Lerp(Idle, 18.0f, FMath::InterpEaseInOut(0.0f, 1.0f, Charge, 1.6f)); }
    UPROPERTY(Replicated) float CastServerTime = -100;
    UPROPERTY(Replicated) float CastFlight = 1;
    UPROPERTY(Replicated) float CastPower = 0;
    UPROPERTY(Replicated) float StrikeServerTime = -100;
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
    void OnFishTake(ALakeFish* Fish);
private:
    friend class FLakeFishingTest;
    friend class FLakeFishBehaviourTest;
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
    bool CanWalk() const;
    void ToggleRod();
    void ToggleView();
    UFUNCTION(Server, Reliable) void ServerToggleRod();
    void DoCast(float Power);
    /** Rod elevation while waiting (deg): with a deep float the main line is short, so the tip goes down to the water. */
    float WaitElevation(float Pitch, float Depth) const;
    void RodPoseTarget(float Pitch, float Now, float& OutElevation, FVector& OutGrip, float& OutStiffness) const;
    void UpdateRodPose(float DeltaSeconds);
    FTransform RodHold() const;
    FTransform StandHold() const;
    void ConstrainFloat();
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
    void Right(float Value);
    void Fish();
    void Cancel();
    void Release();
    void UpdateFight(float DeltaSeconds);
    void FinishFishing(const FString& Message);
    /** Slowly raise the rod and draw the rig out; the cast ends when the rod is up. */
    void StartRetrieve(const FString& Message, const FString& EndMessage);
    void UpdateRetrieve();
    bool TakeRodFromStand();
    FString RetrieveEndMessage;
    UFUNCTION(Server, Reliable) void ServerRelease();
    UFUNCTION(Server, Reliable) void ServerFish();
    UFUNCTION(Server, Reliable) void ServerCancel();
    void Escape();
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
class QUIETLAKE_API ALakeHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
private:
    FString LastStatus;
    float StatusTime = 0;
    float PlayStart = -1;   // when the menu last closed, for the fading key hints
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
    /** Replace a caught fish with a new one of the same species after a while (server). */
    void ScheduleRespawn(EFishSpecies Species);
private:
    void SpawnFish(EFishSpecies Species, int32 Count, int32 School);
    FRandomStream FishRandom;
    int32 NextSchool = 0;
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
