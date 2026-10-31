#pragma once
#include "CoreMinimal.h"
#include "LakeFish.h"

/**
 * The natural food of the pond ("Live Fish"): plankton in the open water, insects at the top, the
 * bottom fauna in the silt, soft weed. Each place holds some of each by its nature (Richness: the
 * lily beds and the warm backwater are rich in silt fauna and weed, the reed edge and wooded banks
 * in insects, the open water in plankton); the time and the weather set how much of it the fish
 * can get now (Season: insects hatch on calm warm dawns and evenings, the silt fauna is out at
 * night, plankton thrives in warm water and hangs deep by day, at the top at night); feeding fish
 * eat a patch down and it grows back (FLakeFoodField). Fish eat what their diet and the food at
 * hand make best (ALakeFish::ChooseRoamTarget) and feed where that food is.
 */
namespace LakeFood
{
    /** Shown to people: "планктон". */
    QUIETLAKE_API const TCHAR* Name(EFishFood Food);
    /** How much of a food a place holds by its nature, 0..1. */
    QUIETLAKE_API float Richness(EFishFood Food, float X, float Y);
    /** How much of it the fish can get now, about 0.05..1.25. */
    QUIETLAKE_API float Season(EFishFood Food, const FFishConditions& C);
    /** How deep under the surface the plankton hangs now (m): about 2 m in bright day, near the top at night. */
    QUIETLAKE_API float PlanktonDepthM(const FFishConditions& C);
    /** The height (cm) a fish feeding on Food at (X, Y) feeds at. */
    QUIETLAKE_API float FeedingZ(EFishFood Food, float X, float Y, const FFishConditions& C, FRandomStream& Random);
    /** How well a bait goes with the food a fish is on, 0.85..1.15 (a worm with the silt fauna, bread with weed). */
    QUIETLAKE_API float BaitMatch(EFishFood Food, EBait Bait);
    inline bool IsNatural(EFishFood Food) { return Food >= EFishFood::Plankton && Food <= EFishFood::Plants; }
}

/** What is left of the natural food over the lake, on a 5 m grid (server). */
class QUIETLAKE_API FLakeFoodField
{
public:
    static constexpr float Cell = 500;
    bool IsReady() const { return NX > 0; }
    /** Lay the grid over the lake and work out what each place holds (once, a fraction of a second). */
    void Init();
    /** What a fish can find of Food at P now: the place's richness * the season * what is left (0..~1.25). */
    float Available(EFishFood Food, const FVector2D& P, const FFishConditions& C) const;
    /** What is left of Food at P: 1 untouched, 0 eaten out. */
    float Left(EFishFood Food, const FVector2D& P) const;
    /** Fish weighing Kg ate Meal (as much as fills them, see ALakeFish::Live) of Food at P. */
    void Eat(EFishFood Food, const FVector2D& P, float Meal, float Kg);
    /** It grows back: plankton in a few minutes, insects keep hatching, silt fauna and weed in a quarter of an hour or more. */
    void Step(float DeltaSeconds);
    /** Mean of what is left over the water (for the log). */
    float MeanLeft(EFishFood Food) const;
    /** 5 m patches eaten below Below of what they hold (for the log). */
    int32 EatenPatches(EFishFood Food, float Below = 0.7f) const;
    /** Seconds for an eaten patch to grow halfway back. */
    static float RegrowSeconds(EFishFood Food);
private:
    int32 Index(const FVector2D& P) const;
    struct FPlace
    {
        float Rich[4] = {};
        float Shallow = 0;          // 1 in water under 1.2 m (the silt fauna there is easier to get at night)
        float NearBank = 0;         // 1 within a few metres of the bank (insects blown onto it)
        FVector2D Outward = FVector2D::ZeroVector;
        bool bWater = false;
    };
    TArray<FPlace> Places;
    TArray<float> Eaten[4];         // 0 untouched .. 1 eaten out
    FVector2D Origin = FVector2D::ZeroVector;
    int32 NX = 0, NY = 0;
    float Pending = 0;
};
