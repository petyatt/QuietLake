#pragma once
#include "CoreMinimal.h"

/**
 * The lake and the land around it: shoreline, lake bed, terrain relief, the places on the bank
 * (fishing spots, pier, island, snags, lily beds) and the kinds of water fish live in.
 * Pure deterministic functions, identical on every machine, used by the environment builder,
 * the fish, the angler and the tests. Distances in cm unless a name says metres.
 *
 * Layout ("Деревня над плёсом"): a long village lake, about 660 m from west to east and 22-165 m
 * across, winding along a valley in hilly forest. The outflow leaves the rounded west end over a
 * stony riffle; a brook comes into the shallow reedy upper end in the east; a short weedy backwater
 * runs north into the forest with a spring at its head. Where an island once stood there is an
 * underwater hump (1.8 m) with a hole and sunken trees beside it, 40-50 m off the south bank. Deep
 * water lies in the west hole and in troughs under the northern bluffs (7-8.5 m). The south bank is
 * hilly meadow from the camp past the warm south bay to the village on a hill above the middle of the
 * lake, with the pier below it; everything else is forest. Banks vary: bluffs and cliffs, high banks
 * with a path down, low undercut steps and flat shelving banks.
 *
 * The layout is written in metres (game axes: X east, Y south) and converted to cm.
 */
namespace LakeMap
{
    constexpr float SurfaceZ = -25.0f;
    /** A point of open water in the middle of the lake, west of the hump and its hole. */
    inline const FVector2D Centre(2000.0f, -3000.0f);

    /** Signed distance to the nearest waterline: positive in the water. */
    QUIETLAKE_API float ShoreDistance(float X, float Y);
    /** Terrain or lake bed height: the lake level's landscape as baked by Tools/BuildLakeLevel.py
     *  (Content/Maps/LakeGround.bin, the bowl the Water lake carved), else the designed ground. */
    QUIETLAKE_API float Ground(float X, float Y);
    /** The designed ground (land and the old hand-made bed), from which the landscape is built. */
    QUIETLAKE_API float DesignGround(float X, float Y);
    /** Whether Ground comes from the baked landscape. */
    QUIETLAKE_API bool HasBakedGround();
    /** Where the baked ground lives (relative to the project's Content folder). */
    inline const TCHAR* BakedGroundFile = TEXT("Maps/LakeGround.bin");
    QUIETLAKE_API bool IsWater(float X, float Y);
    /** Water depth in cm (0 on land). */
    QUIETLAKE_API float Depth(float X, float Y);
    /** Unit 2D direction towards deeper water. */
    QUIETLAKE_API FVector2D DeeperDirection(float X, float Y);

    /** Points on every waterline about a metre apart (unordered), with the normal pointing onto the land. */
    struct FShorePoint { FVector2D Location, Outward; };
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

    /** Height (m) of the clay bluff or cliff at the bank nearest to P, 0 where the bank is low or a path goes down. */
    QUIETLAKE_API float BluffHeight(const FVector2D& P);
    /** 1 in the warm weedy shallows: the backwater, the south bay, the upper end and the crucian shallow. */
    QUIETLAKE_API float BackwaterAmount(float X, float Y);
    /** 1 where the forest grows, 0 in the hilly meadow along the south bank and round the village. */
    QUIETLAKE_API float ForestAmount(const FVector2D& P);

    /** Where the angler starts: on the bank at the camp, facing the water. */
    QUIETLAKE_API FVector2D StartSpot(FVector2D* OutFacing = nullptr);

    struct FPier { FVector2D Root, Dir; float Length, Width; };
    QUIETLAKE_API const FPier& Pier();
    QUIETLAKE_API bool OnPier(const FVector& P);
    /** The boat landing next to the pier. */
    QUIETLAKE_API FShorePoint Landing();
    /** An old boat half sunk beside the pier (cover for fish): where it lies and the yaw of its keel. */
    QUIETLAKE_API FVector2D OldBoat(float* OutYaw = nullptr);
    /** The hilly meadow above the middle of the lake kept for the village. */
    QUIETLAKE_API FVector2D VillageCentre();

    /** The village on the hill: one main street from the south edge towards the pier, a cross street
        with the market square at its east end, and the plots along the main street. Positions in cm. */
    struct FVillagePlot
    {
        FVector2D House;        // where SM_Izba stands: its origin, on the ground in the middle
        FVector2D Facing;       // the porch looks this way, at the street (unit)
        FVector2D Street;       // on the main street's centre line, in front of the plot
        FVector2D Gate;         // the gate in the front fence
        FVector2D Garden;       // the middle of the garden behind the house
        float HalfWidth;        // half of the plot along the street
        float Front;            // the front fence: this far from House along Facing
        float Back;             // the back fence: this far from House against Facing
        int32 Kind;             // 0 an izba, 1 the fishing shop, 2 the village store
    };
    struct FVillage
    {
        FVector2D Centre;
        FVector2D Axis;         // along the main street, towards the pier (unit)
        FVector2D Side;         // across it, to the right of Axis (unit)
        float Radius;
        TArray<FVillagePlot> Plots;
        TArray<FVector2D> MainStreet;   // from the south edge towards the pier
        TArray<FVector2D> CrossStreet;
        TArray<FVector2D> MarketLane;   // from the main street to the market square
        TArray<FVector2D> PierPath;     // from the north end of the street down to the pier's footpath
        FVector2D Market;               // the middle of the market square
        float MarketHalfAcross = 1050.0f, MarketHalfAlong = 750.0f;
        FVector2D Well;
        float StreetHalfWidth = 325.0f;
    };
    QUIETLAKE_API const FVillage& Village();
    /** True on the village's streets and the market square: ground kept clear of grass and bushes. */
    QUIETLAKE_API bool VillagePaved(const FVector2D& P);
    /** The far end of the little bay at the west end, and a point beyond the rounded west end where the outflow leaves (its bank is the nearest shore). */
    QUIETLAKE_API FVector2D WestBay();
    QUIETLAKE_API FVector2D WestEnd();

    /** Fallen trees lying from the bank into the water, and trees sunk in open water: cover for perch and pike. */
    struct FSnag { FVector Root, Tip; float Radius; bool bSunken = false; };
    QUIETLAKE_API const TArray<FSnag>& Snags();
    /** Beds of water lilies; centres, radii, the deepest water they grow in, and whether they are yellow brandy-bottle lilies. */
    struct FLilyBed { FVector2D Centre; float Radius; float MaxDepth = 180.0f; bool bYellow = false; };
    QUIETLAKE_API const TArray<FLilyBed>& LilyBeds();
    QUIETLAKE_API float LilyAmount(float X, float Y);
    /** False in the casting lanes and windows in front of the spots and along the pier: no lily leaves there. */
    QUIETLAKE_API bool LilyAllowed(const FVector2D& P);

    // --- details of the banks (decoration; the environment builds them) ---------------------------
    /** A thing on the bank: where it stands and the direction it faces (towards the water). */
    struct FBankThing { FVector2D Location, Facing; };
    /** Weeping willows leaning over the water on the meadow bank and over the outflow. */
    QUIETLAKE_API TConstArrayView<FBankThing> Willows();
    /** The old oak by the pier, the old pine on the high bank at "Омут" and the lookout bench beside it. */
    QUIETLAKE_API FBankThing OldOak();
    QUIETLAKE_API FBankThing OldPine();
    QUIETLAKE_API FBankThing Lookout();
    /** The log frame over the spring at the head of the backwater. */
    QUIETLAKE_API FBankThing Spring();
    /** Streams with water: the outflow, the brook, the spring's run. Points along the water surface (cm, Z included), from the lake outwards. */
    struct FStream { TArray<FVector> Surface; float Width; bool bOutflow; };
    QUIETLAKE_API const TArray<FStream>& Streams();
    /** Distance (cm) to the middle of the nearest stream's channel; large far from any. */
    QUIETLAKE_API float StreamDistance(const FVector2D& P);
    /** Footbridges where the path round the lake crosses the streams. */
    struct FFootbridge { FVector2D A, B; };
    QUIETLAKE_API const TArray<FFootbridge>& Footbridges();
    /** 1 on the footpath round the lake (about 7 m up the bank), 0 off it. */
    QUIETLAKE_API float PathAmount(const FVector2D& P);
    /** 1 on the little sandy beach in the south bay. */
    QUIETLAKE_API float BeachAmount(const FVector2D& P);
    /** 1 in the submerged weed beds (by the hump and off "Отмель"). */
    QUIETLAKE_API float WeedAmount(const FVector2D& P);
    /** True in the cattail stand at the upper end, and in the sedge tussocks by "Ольхи". */
    QUIETLAKE_API bool InCattails(const FVector2D& P);
    QUIETLAKE_API bool InSedgeTussocks(const FVector2D& P);
    /** The row of old stakes in the south bay. */
    QUIETLAKE_API TConstArrayView<FVector2D> Stakes();
    /** Clay faces where sand martins dig their burrows (on the bank, facing the water). */
    QUIETLAKE_API TConstArrayView<FBankThing> SwallowBanks();
    /** The birch grove on the hill above "Сосновый яр": centre and radius. */
    QUIETLAKE_API FVector2D BirchGrove(float* OutRadius = nullptr);
    /** True where the bank at P is a cliff (a steep clay wall), not a sloping bluff. */
    QUIETLAKE_API bool CliffAt(const FVector2D& P);
    /** Places for the quiet sounds of the lake: the streams, frogs, a cuckoo in the forest. */
    struct FSoundPlace { const TCHAR* Sound; FVector2D Location; float Volume; };
    QUIETLAKE_API TConstArrayView<FSoundPlace> SoundPlaces();

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
