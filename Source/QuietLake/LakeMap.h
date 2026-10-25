#pragma once
#include "CoreMinimal.h"

/**
 * The pond and the land around it: shoreline, lake bed, terrain relief, the places on the bank
 * (pier, gaps in the reeds, village, snags, lily beds) and the kinds of water fish live in.
 * Pure deterministic functions, identical on every machine, used by the environment builder,
 * the fish, the angler and the tests. Distances in cm unless a name says metres.
 *
 * Layout: an overgrown mid-Russian pond about 120 x 150 m with a long weedy backwater to the
 * north-east, a village bank with a pier and a boat landing on the west shore, clay bluffs on the
 * south-east and north shores, meadow, hills and terraced ground behind, forest all around.
 */
namespace LakeMap
{
    constexpr float SurfaceZ = -25.0f;
    inline const FVector2D Centre(6100.0f, 0.0f);

    /** Distance from Centre to the waterline in the direction Angle (radians, atan2 of Y, X). */
    QUIETLAKE_API float ShoreRadius(float Angle);
    QUIETLAKE_API float AngleOf(float X, float Y);
    /** 0 at the centre, 1 on the waterline, above 1 on land. */
    QUIETLAKE_API float Radius(float X, float Y);
    /** Signed distance to the waterline: positive in the water, negative on land. */
    QUIETLAKE_API float ShoreDistance(float X, float Y);
    /** Point on the ray from Centre at the given fraction of the shore radius. */
    QUIETLAKE_API FVector2D PointAt(float Angle, float R);
    /** Terrain or lake bed height. */
    QUIETLAKE_API float Ground(float X, float Y);
    QUIETLAKE_API bool IsWater(float X, float Y);
    /** Water depth in cm (0 on land). */
    QUIETLAKE_API float Depth(float X, float Y);
    /** Unit 2D direction towards deeper water. */
    QUIETLAKE_API FVector2D DeeperDirection(float X, float Y);

    /** The waterline as a closed polyline with outward normals, about every metre. */
    struct FShorePoint { FVector2D Location, Outward; float Angle; };
    QUIETLAKE_API const TArray<FShorePoint>& Shoreline();
    /** The waterline point in the direction Angle from the centre. */
    QUIETLAKE_API FShorePoint ShoreAt(float Angle);

    /** Gaps in the reed belt where the angler can stand and cast. */
    struct FOpening { float Angle, HalfWidth; const TCHAR* Name; };
    QUIETLAKE_API TConstArrayView<FOpening> Openings();
    /** 1 inside an opening of the reed belt, 0 away from it. */
    QUIETLAKE_API float OpeningAmount(float Angle);
    /** Height (m) of the clay bluff on this stretch of shore, 0 where the bank is low. */
    QUIETLAKE_API float BluffHeight(float Angle);
    /** 1 in the north-east backwater. */
    QUIETLAKE_API float BackwaterAmount(float X, float Y);

    /** Where the angler starts: on the bank in the main opening, facing the water. */
    QUIETLAKE_API FVector2D StartSpot(FVector2D* OutFacing = nullptr);

    struct FPier { FVector2D Root, Dir; float Length, Width; };
    QUIETLAKE_API const FPier& Pier();
    QUIETLAKE_API bool OnPier(const FVector& P);
    /** Middle of the village clearing behind the boat landing. */
    QUIETLAKE_API FVector2D VillageCentre();

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

    /** Dirt roads through the forest far from the lake, and footpaths to the lake and to each fishing spot. */
    struct FTrack { TArray<FVector2D> Points; float Width; bool bRoad; };
    /** Roads first, then paths; points about every metre. */
    QUIETLAKE_API const TArray<FTrack>& Tracks();
    /** Distance (cm) from the edge of the nearest road or path, negative inside it; bRoad tells which. */
    QUIETLAKE_API float TrackClearance(float X, float Y, bool* bRoad = nullptr);

    /** Playable ground; walking past it brings the angler back. */
    QUIETLAKE_API bool InPlayArea(float X, float Y);
    /** Extent of the generated terrain. */
    inline const FBox2D TerrainBounds(FVector2D(-16000, -24000), FVector2D(30000, 26000));
}
