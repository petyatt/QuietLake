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

/** Groundbait on the bottom: balls thrown within a metre and a half of each other lie as one patch. */
struct FGroundbaitSpot
{
    FVector2D Location = FVector2D::ZeroVector;
    float Balls = 0;            // how much is left, in balls
    float Added[3] = {};        // balls mixed with worm, maggot, bread (the angler's hook bait)
    float Since = 0;            // field time the first ball reached the bottom
};

namespace LakeFood
{
    /**
     * How strongly a fish with this diet goes for this groundbait, 0..~1.3: its taste for groundbait
     * (crumbs and their smell), more when what is mixed in is its food (chopped worm - bottom fauna,
     * maggots - insects and bottom fauna, more bread - weed).
     */
    QUIETLAKE_API float GroundbaitAppeal(const FFishDiet& Diet, const FGroundbaitSpot& Spot);
    /** The bait most of it was mixed with. */
    QUIETLAKE_API EBait GroundbaitMix(const FGroundbaitSpot& Spot);
    /** How rich it is to feed on, 0.9..1.4: concentrated food, richer than any natural patch (more balls, more food). */
    QUIETLAKE_API float GroundbaitRichness(const FGroundbaitSpot& Spot);
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

    // Groundbait.
    /** A ball reached the bottom at P, mixed with With. */
    void AddGroundbait(const FVector2D& P, EBait With);
    /** How strongly the smell of Spot reaches P, 0..1: it builds over a minute and spreads from a few metres to about 15 in three. */
    float Scent(const FGroundbaitSpot& Spot, const FVector2D& P) const;
    /** The groundbait that smells strongest at P (index into Groundbait(), or -1), and how strongly. */
    int32 SmellGroundbait(const FVector2D& P, float& OutScent) const;
    /** A fish weighing Kg ate Meal (as much as fills it) of the groundbait at Spot. */
    void EatGroundbait(int32 Spot, float Meal, float Kg);
    const TArray<FGroundbaitSpot>& Groundbait() const { return GroundbaitSpots; }
    /** Seconds for groundbait to dissolve halfway (fish eating it make it go faster). */
    static constexpr float DissolveSeconds = 600;
    /** The field's own clock (s), stepped with the pond. */
    float Now() const { return Clock; }
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
    float Clock = 0;
    TArray<FGroundbaitSpot> GroundbaitSpots;
};
