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
    /** The case window (LMB pressed / released over it): picks up the rod under the mouse, drops it on a slot. */
    void CaseWindowPress();
    /** True with a move to ask the server for: Rod (ERodKind) to Target (0..2 a compartment, 3 the hands). */
    bool CaseWindowRelease(int32& OutRod, int32& OutTarget);
private:
    /** The case window, the prompt over the thing under the crosshair, the ghost's hint, the held R. */
    void DrawCaseWindow(class ALakeCharacter* Fisher, float S);
    void DrawInteraction(class ALakeCharacter* Fisher, float S);
    /** Which window slot is under the mouse (0..2 compartments, 3 the hands, -1 none). */
    int32 SlotUnderMouse() const;
    FBox2D SlotRects[4];
    int32 DragRod = -1;
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
