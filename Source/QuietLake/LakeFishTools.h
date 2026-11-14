#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LakeFishTools.generated.h"

class UStaticMesh;

/**
 * Editor tools for the fish models (Tools/MarkFishFins.py). Outside the editor they do nothing.
 */
UCLASS()
class QUIETLAKE_API ULakeFishTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /**
     * Finds the fins of a fish model and marks them in its vertex colours, every LOD: green = 1 - how
     * much of a fin the vertex is (1 on the body, falling to 0 out at a fin's edge), blue = which fin
     * (1 body, 0.75 dorsal, 0.5 pectoral, 0.25 pelvic or anal, 0 the tail fin). Red (the wave
     * coordinate along the body) is kept. The model lies snout at X = 0, tail along +X. A fin is what
     * stands out of the body's cross-section as a thin sheet: per slice along the body the body's
     * width is measured near its middle height and its depth on its flanks, and whatever lies well
     * outside that ellipse is fin. Returns a line of what it found.
     */
    UFUNCTION(BlueprintCallable, Category = "Lake|Fish")
    static FString MarkFishFins(UStaticMesh* Mesh);
};
