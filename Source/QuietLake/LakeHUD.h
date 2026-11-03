#pragma once
#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "LakeHUD.generated.h"

/**
 * The angler's screen: a small status card (message, catch, bait, depth, fishing spot), the line
 * tension and casting bars, the time and weather, fading key hints, and the F3 "Live Fish" panel.
 */
UCLASS()
class QUIETLAKE_API ALakeHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
    virtual void BeginPlay() override;
    /** F3: the "Live Fish" panel: what the fish are doing, recent signs on the water, markers over nearby fish. */
    bool bShowLife = false;
    /** M: the whole lake over the screen, about 70% of it, with the places and the angler on it. */
    bool bShowMap = false;
private:
    void DrawLifePanel(float S);
    /** The minimap in the top-right corner and the big map on M. */
    void DrawMinimap(float ScreenScale);
    void DrawMap(float ScreenScale);
    /** A square picture of the lake and its banks, built once from LakeMap (a texture, not a mesh). */
    void BuildMapTexture();
    FVector2D MapOrigin = FVector2D::ZeroVector;   // world XY of the texture's top-left corner
    float MapWorldSize = 1.0f;                     // cm covered by the texture's edge
    UPROPERTY() TObjectPtr<class UTexture2D> MapTexture;
    FString LastStatus;
    float StatusTime = 0;
    float PlayStart = -1;   // when the menu last closed, for the fading key hints
};
