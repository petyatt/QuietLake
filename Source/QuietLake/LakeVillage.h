#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeVillage.generated.h"

/**
 * The village on the hill above the middle of the lake (LakeMap::Village()): log houses with gardens,
 * fences and gates, the main street towards the pier, the cross street, the market square with its
 * stalls and fish, the fishing shop and the village store. Built from engine primitives and the
 * village props of Content/Village (Tools/GenerateVillageModels.py); visual only, the server keeps
 * no state here.
 */
UCLASS()
class ALakeVillage : public AActor
{
    GENERATED_BODY()

public:
    ALakeVillage();
    virtual void BeginPlay() override;

#if !UE_BUILD_SHIPPING
    /** -VillagePreview: a handful of frames of the village for a review, then the game closes. */
    void RunPreview();
#endif

private:
    void Build();
};
