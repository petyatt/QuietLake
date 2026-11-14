#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "LakeSwimmer.h"
#include "LakeSwimLab.generated.h"

// Opt-in laboratory map mode. Never spawned by the lake's normal game mode.
UCLASS()
class QUIETLAKE_API ALakeSwimLab : public AActor
{
    GENERATED_BODY()
public:
    ALakeSwimLab();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
private:
    FLakeSwimmer Swimmer;
    UPROPERTY() TObjectPtr<class ALakeFish> Fish;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Body;
    UPROPERTY() TObjectPtr<class ACameraActor> Camera;
    UPROPERTY() TArray<TObjectPtr<class APointLight>> Lamps;
    TArray<FVector> LampOffsets;
    UPROPERTY() TObjectPtr<class AStaticMeshActor> Floor;
    // Effort of an unhurried swim, about one body length a second.
    static constexpr float Easy=0.15f;
    float Elapsed=0, BodyLength=30, BodyGrams=350;
    int32 NextShot=0;
    bool bAuto=false;
};

UCLASS()
class QUIETLAKE_API ALakeSwimLabMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    ALakeSwimLabMode();
    virtual void BeginPlay() override;
};
