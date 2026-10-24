#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
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
private:
    friend class FLakeFishingTest;
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
    void Bite();
    void Escape();
    FTimerHandle FishingTimer;
    double NextActionTime = 0;
    bool bReeling = false;
    float FightTime = 0;
    float FishWeight = 0;
    int32 SpeciesIndex = 0;
    FVector CastOrigin;
    FVector CastDirection;
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
