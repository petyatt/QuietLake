#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LakeLevel.generated.h"

/**
 * Editor tools that build the lake level (/Game/Maps/Lake) from LakeMap: the Landscape with its
 * heights and layers, the engine Water lake whose spline follows the waterline and carves the bowl,
 * and a Water river along each stream. Called from Tools/BuildLakeLevel.py in the full editor (the
 * water brushes render on the GPU). Outside the editor every function does nothing.
 */
UCLASS()
class QUIETLAKE_API ULakeLevelLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** The landscape layer info asset LI_<LayerName> in Folder, created if missing (no weight blending:
     *  each layer keeps its own 0..1 mask, as the terrain material expects). */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static UObject* CreateLayerInfo(const FString& Folder, FName LayerName);

    /** Replaces any landscape in the editor world with one built from LakeMap: 1 m quads over the
     *  terrain bounds, heights from LakeMap::Ground, layers Meadow / Forest / Clay from the terrain
     *  masks (LayerInfos in that order). */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static AActor* BuildLandscape(UMaterialInterface* Material, const TArray<UObject*>& LayerInfos);

    /** The lake's waterline as one closed loop, ordered, a point every SpacingCm, at the water level. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static TArray<FVector> LakeOutline(float SpacingCm = 600);

    /** The lake bed's profile across the ramp for the Water lake's carving: the curve asset Name in
     *  Folder (created or reset): steep by the bank, where the float fisher reaches, flattening towards
     *  the middle. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static UObject* CreateBedProfile(const FString& Folder, const FString& Name);

    /** A Water lake along the waterline that carves its bowl into the landscape: ChannelDepthCm deep,
     *  sloping down over RampWidthCm from the edge along Profile (a curve; null: the engine's default).
     *  Replaces an existing one. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static AActor* SpawnLake(float ChannelDepthCm = 600, float RampWidthCm = 4000, float SpacingCm = 600, UObject* Profile = nullptr);

    /** Writes the landscape's final heights (with the carving) to Content/<LakeMap::BakedGroundFile>,
     *  which LakeMap::Ground then reads on every machine and in the tests. Returns the cells written. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static int32 BakeGround();

    /** Warm weedy flats on the carved bowl: a Water island (a brush that only raises the ground to its
     *  spline, sloping off outside at FalloffAngle) DepthCm under the water over each flat of the
     *  designed bed shallower than MaxDesignDepthCm, at least MinBankDistanceCm out and of at least
     *  MinAreaM2. Returns how many. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static int32 SpawnShallows(float MaxDesignDepthCm = 140, float MinBankDistanceCm = 500, float MinAreaM2 = 400, float DepthCm = 100, float FalloffAngle = 12);

    /** Merges the landscape's edit layers with the water brushes now and returns how deep (cm) the
     *  landscape lies under the water in the middle of the lake (0 if the bowl is not carved). */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static float UpdateLandscape();

    /** A Water river along every stream of LakeMap (its width, DepthCm deep). Returns how many. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static int32 SpawnStreams(float DepthCm = 60);
};
