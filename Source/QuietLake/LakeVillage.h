#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeVillage.generated.h"

/**
 * The lakeside village from LakeMap::Village(): separate homesteads, a shop, yards and footpaths.
 * Meshes come from Content/Village/Lakeside, prepared by Tools/PrepareVillageAssets.py.
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
