#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeBirds.generated.h"

/** Small, local ambient flocks; cosmetic only, with no effect on fish or networking. */
UCLASS()
class QUIETLAKE_API ALakeBirds : public AActor
{
    GENERATED_BODY()
public:
    ALakeBirds();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
private:
    UPROPERTY() TObjectPtr<class UProceduralMeshComponent> Mesh;
    TArray<FVector> Centres;
    bool bReady = false;
};
