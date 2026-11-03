#pragma once
#include "CoreMinimal.h"

/**
 * The lake and the land around it: shoreline, lake bed, terrain relief, the places on the bank
 * (fishing spots, pier, island, snags, lily beds) and the kinds of water fish live in.
 * Pure deterministic functions, identical on every machine, used by the environment builder,
 * the fish, the angler and the tests. Distances in cm unless a name says metres.
 *
 * Layout: a long village lake, about 660 m from west to east and 22-165 m across, winding along a
 * valley. The west end is a rounded natural bank with a small bay and the deepest water (7-8 m);
 * only a row of rotten piles is left of the old dam. The wide middle reach holds a wooded island,
 * a hole and an underwater hump; the lake narrows to a shallow reedy upper end where a brook comes
 * in. A long weedy backwater runs north into the forest; a shallow bay with the pier and boats
 * opens to the south; a snaggy forest cove and a ravine cove break the banks. The north and east
 * banks are forested hills with ravines and a clay bluff; the south-west and the west end are open
 * hilly meadow, with a plateau left for a village.
 *
 * The lake's layout is written at its old size and laid out at LakeScale (LakeMap::Lay); depths,
 * heights, the width of the shallows and of the reeds are in real metres.
 */
namespace LakeMap
{
    constexpr float SurfaceZ = -25.0f;
    /** A point of open water in the middle reach, between the island and the south bank. */
    inline const FVector2D Centre(6000.0f, 750.0f);

    /** Signed distance to the nearest waterline (the lake's or the island's): positive in the water. */
    QUIETLAKE_API float ShoreDistance(float X, float Y);
    /** Terrain or lake bed height. */
    QUIETLAKE_API float Ground(float X, float Y);
    QUIETLAKE_API bool IsWater(float X, float Y);
    /** Water depth in cm (0 on land). */
    QUIETLAKE_API float Depth(float X, float Y);
    /** Unit 2D direction towards deeper water. */
    QUIETLAKE_API FVector2D DeeperDirection(float X, float Y);

    /** Points on every waterline about a metre apart (unordered), with the normal pointing onto the land. */
    struct FShorePoint { FVector2D Location, Outward; bool bIsland = false; };
    QUIETLAKE_API const TArray<FShorePoint>& Shoreline();
    /** The waterline point nearest to P. */
    QUIETLAKE_API FShorePoint NearestShore(const FVector2D& P);

    /** Places on the bank cleared for fishing: no reeds or tall grass, boards at the water. */
    struct FSpot
    {
        const TCHAR* Name;         // shown to the player
        FVector2D Location;        // on the waterline
        FVector2D Outward;         // onto the bank
        float HalfWidth;           // of the clearing along the bank (cm)
        bool bBoards;              // boards and rod stakes at the water's edge
    };
    QUIETLAKE_API TConstArrayView<FSpot> Spots();
    /** 1 in the clearing of a fishing spot (and the lane cast from it), 0 away from them. */
    QUIETLAKE_API float SpotAmount(const FVector2D& P);
    /** The fishing spot whose clearing P is in, or null. */
    QUIETLAKE_API const FSpot* SpotAt(const FVector2D& P);

    /** Height (m) of the clay bluff at the bank nearest to P, 0 where the bank is low. */
    QUIETLAKE_API float BluffHeight(const FVector2D& P);
    /** 1 in the warm weedy shallows: the backwater, the upper end and the south bay. */
    QUIETLAKE_API float BackwaterAmount(float X, float Y);
    /** 1 where the forest grows (the north and east), 0 in the open meadow (south-west, the west end, the village plateau). */
    QUIETLAKE_API float ForestAmount(const FVector2D& P);

    /** Where the angler starts: on the bank at the camp, facing the water. */
    QUIETLAKE_API FVector2D StartSpot(FVector2D* OutFacing = nullptr);

    struct FPier { FVector2D Root, Dir; float Length, Width; };
    QUIETLAKE_API const FPier& Pier();
    QUIETLAKE_API bool OnPier(const FVector& P);
    /** The boat landing next to the pier. */
    QUIETLAKE_API FShorePoint Landing();
    /** The level plateau above the south bay kept for the village. */
    QUIETLAKE_API FVector2D VillageCentre();
    QUIETLAKE_API FVector2D IslandCentre();
    /** The far end of the little bay at the west end, and a point beyond the rounded west end (its bank is the nearest shore). */
    QUIETLAKE_API FVector2D WestBay();
    QUIETLAKE_API FVector2D WestEnd();

    /** Fallen trees lying from the bank into the water: cover for perch and pike. */
    struct FSnag { FVector Root, Tip; float Radius; };
    QUIETLAKE_API const TArray<FSnag>& Snags();
    /** Beds of water lilies in the shallows; centres and radii. */
    struct FLilyBed { FVector2D Centre; float Radius; };
    QUIETLAKE_API const TArray<FLilyBed>& LilyBeds();
    QUIETLAKE_API float LilyAmount(float X, float Y);

    /** How much a point is each kind of fish habitat, 0..1. */
    struct FZones { float Shallows = 0, ReedEdge = 0, DropOff = 0, Cover = 0, Deep = 0, Open = 0; };
    QUIETLAKE_API FZones ZonesAt(float X, float Y);

    /** Extent of the generated terrain. */
    inline const FBox2D TerrainBounds(FVector2D(-62000, -58000), FVector2D(72000, 46000));
    /** Playable ground; walking past it brings the angler back. */
    QUIETLAKE_API bool InPlayArea(float X, float Y);
    /** Bounding box of the water. */
    QUIETLAKE_API FBox2D LakeBounds();
}
