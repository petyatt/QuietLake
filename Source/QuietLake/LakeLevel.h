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

    /** A Water lake along the waterline that carves its bowl into the landscape: ChannelDepthCm deep,
     *  sloping down over RampWidthCm from the edge. Replaces an existing one. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static AActor* SpawnLake(float ChannelDepthCm = 600, float RampWidthCm = 4000, float SpacingCm = 600);

    /** Merges the landscape's edit layers with the water brushes now and returns how deep (cm) the
     *  landscape lies under the water in the middle of the lake (0 if the bowl is not carved). */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static float UpdateLandscape();

    /** A Water river along every stream of LakeMap (its width, DepthCm deep). Returns how many. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Level")
    static int32 SpawnStreams(float DepthCm = 60);
};
