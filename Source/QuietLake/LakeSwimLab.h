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
    FVector ViewSide=FVector(0,-1,0);
    UPROPERTY() TObjectPtr<class AStaticMeshActor> Floor;
    // Effort of an unhurried swim, about one body length a second.
    static constexpr float Easy=0.15f;
    // The water's surface and the bottom: the fish starts 40 cm deep, 60 cm above the bottom.
    static constexpr float SurfaceZ=40;
    static constexpr float FloorZ=-60;
    float Elapsed=0, BodyLength=30, BodyGrams=350, LastPitch=0;
    bool bSideView=false, bHooked=false;
    void StartFish();
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
