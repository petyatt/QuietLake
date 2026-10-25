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
    UPROPERTY(Replicated) int32 FishingState = 0;
    UPROPERTY(Replicated) int32 CatchCount = 0;
    UPROPERTY(Replicated) FString Status = TEXT("Подойдите к воде. ЛКМ — забросить.");
    UPROPERTY(Replicated) FVector FloatPosition = FVector::ZeroVector;
    UPROPERTY(Replicated) float Tension = 0;
    UPROPERTY(Replicated) float FishDistance = 850;
    UPROPERTY(Replicated) EBait Bait = EBait::Worm;
    /** Incremented on every nibble so clients can twitch the bobber. */
    UPROPERTY(Replicated) uint8 NibbleCounter = 0;

    // --- used by the fish (server) ---
    bool HasBaitInWater() const { return FishingState == 1; }
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
    UFUNCTION(Server, Reliable) void ServerSelectBait(EBait NewBait);
    FVector RodTip() const;
    void Forward(float Value);
    void Right(float Value);
    void Fish();
    void Cancel();
    void Release();
    void UpdateFight(float DeltaSeconds);
    void FinishFishing(const FString& Message);
    UFUNCTION(Server, Reliable) void ServerRelease();
    UFUNCTION(Server, Reliable) void ServerFish();
    UFUNCTION(Server, Reliable) void ServerCancel();
    void Escape();
    FTimerHandle FishingTimer;
    double NextActionTime = 0;
    bool bReeling = false;
    float FightTime = 0;
    float LineLength = 0;
    FVector BaitPoint = FVector::ZeroVector;
    TWeakObjectPtr<ALakeFish> BaitFish;     // fish testing or holding the bait
    TWeakObjectPtr<ALakeFish> HookedFish;
    uint8 SeenNibbles = 0;
    float NibbleVisualTime = -10;
    UPROPERTY() TObjectPtr<class UCameraComponent> FishingCamera;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Rod;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Bobber;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Line;
};

UCLASS()
class QUIETLAKE_API ALakeHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
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
