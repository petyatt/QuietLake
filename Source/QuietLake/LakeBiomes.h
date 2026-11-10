#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LakeBiomes.generated.h"

/**
 * Where what grows round the lake, for PCG Biome Core (Tools/BuildLakeBiomes.py). LakeMap decides the
 * biome of every point (BiomeAt): forest kinds by height, shade and water, the forest edge, the
 * meadows, the reed belt and the shallows; nothing in the clearings, on the paths and on steep faces.
 * BuildBiomeMap paints it into a texture whose colours are the biomes' colours, which Biome Core's
 * texture actors read. Also small reflection helpers to fill Biome Core's Blueprint data assets and
 * actors from Python by property text (their struct members carry generated names).
 */
UCLASS()
class QUIETLAKE_API ULakeBiomeLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** The biomes in their order: names and the colours they have in the biome map. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static TArray<FString> BiomeNames();
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static FLinearColor BiomeColor(const FString& Name);

    /** The biome at a point (cm), "" where nothing grows. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static FString BiomeAt(float X, float Y);

    /** Why nothing grows at a point (cm): "spot", "camp", "pier", "path", "steep", "deep water"...;
     *  "" where something does. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static FString ClearingAt(float X, float Y);

    /** The ground a biome map covers, (min X, min Y, max X, max Y) in cm: all the terrain, or (bShore)
     *  the lake with 40 m round it, for a finer map of the narrow biomes by the water. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static FVector4 BiomeMapRect(bool bShore = false);
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static FVector4 VillageLawnMapRect();

    /** Paints the biomes over Rect (Width x Height, row 0 at min Y, column 0 at min X) into the texture
     *  asset at PackagePath (created or replaced; uncompressed, no mips, nearest filtering) and into
     *  Saved/<asset name>.png. Returns the share of the map each biome covers, as log lines. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static FString BuildBiomeMap(const FString& PackagePath, FVector4 Rect, int32 Width = 2048, int32 Height = 2048, bool bLawnCoverage = false);

    /** Copies every instanced mesh of Source (the Biome Core actor after generating) into hierarchical
     *  instanced meshes of a new plain actor labelled Label (one per mesh, with the same collision, cull
     *  distances and shadows), so the level keeps the plants without the biome actors and their PCG
     *  caches. Returns the new actor. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static AActor* BakeInstances(AActor* Source, const FString& Label);

    /** A PCG graph instance asset at PackagePath (created or reused) running Graph, whose parameters the
     *  PCG parameter helpers can then override (Biome Core point generators with their own density). */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static UObject* MakeGraphInstance(const FString& PackagePath, UObject* Graph);

    /** Every property of Object as text "Name [type] = value", one per line (only the Blueprint's own
     *  ones if bBlueprintOnly). */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static FString DescribeProperties(UObject* Object, bool bBlueprintOnly = true);

    /** A property's value as exported text; Name is the property's name or its display name. */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static FString GetPropertyText(UObject* Object, const FString& Name);

    /** Sets a property from text in the export format (as GetPropertyText gives it). */
    UFUNCTION(BlueprintCallable, Category = "Lake|Biomes")
    static bool SetPropertyText(UObject* Object, const FString& Name, const FString& Text);
};
