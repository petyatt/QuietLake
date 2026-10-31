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
    /** F3: the "Live Fish" panel: what the fish are doing, recent signs on the water, markers over nearby fish. */
    bool bShowLife = false;
private:
    void DrawLifePanel(float S);
    FString LastStatus;
    float StatusTime = 0;
    float PlayStart = -1;   // when the menu last closed, for the fading key hints
};
