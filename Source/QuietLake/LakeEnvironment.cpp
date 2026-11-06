#include "LakeGame.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "LandscapeProxy.h"
#include "WaterBodyActor.h"
#include "LakeBirds.h"
#include "LakeForest.h"
#include "LakeRod.h"
#include "Async/ParallelFor.h"
#include "ProceduralMeshComponent.h"
#include "KismetProceduralMeshLibrary.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "DynamicWindData.h"
#include "Components/AudioComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/HUD.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundAttenuation.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Misc/App.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"

ALakeEnvironment::ALakeEnvironment()
{
    bReplicates = true;
    bAlwaysRelevant = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("WorldRoot"));
}

float ALakeEnvironment::LakeRadius(float X, float Y) { return FMath::Clamp(1 - LakeMap::ShoreDistance(X, Y) / 10000, 0.0f, 2.0f); }
bool ALakeEnvironment::IsWater(FVector P) { return LakeMap::IsWater(P.X, P.Y); }
float ALakeEnvironment::GroundHeight(float X, float Y) { return LakeMap::Ground(X, Y); }

namespace
{
    float SegmentDistance2D(const FVector2D& P, const FVector2D& A, const FVector2D& B)
    {
        const FVector2D AB = B - A;
        const float T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(1.0f, AB.SizeSquared()), 0.0f, 1.0f);
        return FVector2D::Distance(P, A + AB * T);
    }
    /** A narrow, gently bent footpath from the village meadow to the pier. */
    float PierApproach(const FVector2D& P)
    {
        const auto& Pier = LakeMap::Pier();
        const FVector2D Side(-Pier.Dir.Y, Pier.Dir.X);
        const FVector2D A = Pier.Root - Pier.Dir * 1450 + Side * 120;
        const FVector2D B = Pier.Root - Pier.Dir * 500 + Side * 45;
        const FVector2D C = Pier.Root + Pier.Dir * 200;
        const float Distance = FMath::Min(SegmentDistance2D(P, A, B), SegmentDistance2D(P, B, C));
        const float Edge = 18 * FMath::PerlinNoise2D(P / 180);
        return 1 - FMath::SmoothStep(55.0f + Edge, 140.0f + Edge, Distance);
    }
    /** Where the forest begins, in metres from the water; close to the water in the woods, varying along the bank. */
    float ForestStart(const FVector2D& P) { return 7.0f + 7.0f * (0.5f + 0.5f * FMath::PerlinNoise2D(P / 4000)); }
    /** 0..1 how wooded a point is: the forest zone, from where the trees begin. */
    float Woodland(const FVector2D& P, float L)
    {
        const float FS = ForestStart(P);
        return LakeMap::ForestAmount(P) * FMath::SmoothStep(FS - 4, FS + 6, L);
    }
    float Slope(float X, float Y)
    {
        return FVector2D(LakeMap::Ground(X + 100, Y) - LakeMap::Ground(X - 100, Y), LakeMap::Ground(X, Y + 100) - LakeMap::Ground(X, Y - 100)).Size() / 200;
    }
    float YawOf(const FVector2D& Dir) { return FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X)); }
}

FLinearColor ALakeEnvironment::TerrainLayers(const FVector2D& P)
{
    static const FVector2D Start = LakeMap::StartSpot();
    constexpr float N = 200.0f;
    const float DX = (LakeMap::Ground(P.X + N, P.Y) - LakeMap::Ground(P.X - N, P.Y)) / (2 * N);
    const float DY = (LakeMap::Ground(P.X, P.Y + N) - LakeMap::Ground(P.X, P.Y - N)) / (2 * N);
    const float L = -LakeMap::ShoreDistance(P.X, P.Y) / 100;
    const float Bare = FMath::SmoothStep(0.55f, 0.95f, (float)FVector2D(DX, DY).Size()) * FMath::SmoothStep(0.2f, 1.0f, L);
    const float Open = FMath::SmoothStep(900.0f, 1800.0f, (float)FVector2D::Distance(P, Start));
    const float Forest = Woodland(P, L) * Open;
    const float ShorePatch = 0.55f * FMath::PerlinNoise2D(P / 170 + FVector2D(13, 5)) + 0.35f * FMath::PerlinNoise2D(P / 650);
    const float Meadow = FMath::SmoothStep(0.35f + ShorePatch, 3.2f + ShorePatch, L) * (1 - Forest);
    // Clay faces get some dark forest soil mixed in: dirty, not like a sand dune.
    const float Worn = 1 - 0.85f * FMath::Max(PierApproach(P), 0.8f * LakeMap::PathAmount(P));
    // The beach and the stream channels show the sandy bed.
    const float Sand = FMath::Max(LakeMap::BeachAmount(P), 1 - FMath::SmoothStep(140.0f, 260.0f, LakeMap::StreamDistance(P)));
    // Bare faces of bluffs and steep slopes: mostly dark forest earth with some clay showing,
    // so they read as earth banks, not sand dunes.
    return FLinearColor(Meadow * (1 - Bare) * Worn * (1 - Sand), (Forest * (1 - Bare) * Worn + 0.65f * Bare) * (1 - Sand), 0.35f * Bare * (1 - Sand));
}

void ALakeEnvironment::BeginPlay()
{
    Super::BeginPlay();
    const double BuildStart = FPlatformTime::Seconds();
    auto Material = [](const TCHAR* Name)
    {
        return LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Materials/%s.%s"), Name, Name));
    };
    // One deterministic layout per peer; foliage transforms do not consume network bandwidth.
    FRandomStream Random(20261027);
    auto Group = [this, &Material](const TCHAR* Shape, const TCHAR* Mat, bool Collision)
    {
        auto* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Shape, Shape)));
        Component->SetMaterial(0, Material(Mat));
        Component->SetCollisionEnabled(Collision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
        Component->SetCollisionResponseToAllChannels(ECR_Block);
        Component->SetCullDistances(16000, 22000);
        Component->RegisterComponent();
        AddInstanceComponent(Component);
        return Component;
    };
    auto Add = [](UHierarchicalInstancedStaticMeshComponent* Group, FVector Location, FVector Scale, FRotator Rotation = FRotator::ZeroRotator)
    {
        Group->AddInstance(FTransform(Rotation, Location, Scale));
    };
    auto Ground = [](const FVector2D& P) { return LakeMap::Ground(P.X, P.Y); };
    // Build the distance field, the ground and the fixed places before anything else.
    FVector2D StartFacing(1, 0);
    const FVector2D Start = LakeMap::StartSpot(&StartFacing);
    const FVector2D Village = LakeMap::VillageCentre();
    const LakeMap::FPier& Pier = LakeMap::Pier();
    const FVector2D PierEnd = Pier.Root + Pier.Dir * Pier.Length;
    const LakeMap::FShorePoint Landing = LakeMap::Landing();
    LakeMap::LilyBeds();
    LakeMap::Snags();
    const FBox2D LakeBox = LakeMap::LakeBounds();
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BUILD map %.1f s"), FPlatformTime::Seconds() - BuildStart);

    // The lake level (/Game/Maps/Lake, Tools/BuildLakeLevel.py) brings its own ground and water: a
    // Landscape and the engine Water lake and streams. Then the terrain mesh and the water slab are
    // not built; the distant ring of hills still is.
    const bool bLevelGround = !!TActorIterator<ALandscapeProxy>(GetWorld());
    const bool bLevelWater = !!TActorIterator<AWaterBody>(GetWorld());
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BUILD level ground %d, level water %d"), bLevelGround, bLevelWater);
    if (bLevelGround)
    {
        // How deep the landscape's bowl is against LakeMap's: from the middle of the lake towards the shore.
        FString Line;
        const FVector2D Shore = LakeMap::NearestShore(LakeMap::Centre).Location;
        for (float T : { 0.0f, 0.25f, 0.5f, 0.75f, 0.9f })
        {
            const FVector2D P = FMath::Lerp(LakeMap::Centre, Shore, T);
            // The landscape only: the Water bodies have their own collision at the surface.
            TArray<FHitResult> Hits;
            GetWorld()->LineTraceMultiByObjectType(Hits, FVector(P, 5000), FVector(P, -5000), FCollisionObjectQueryParams(ECC_WorldStatic));
            const FHitResult* Bed = Hits.FindByPredicate([](const FHitResult& H) { return H.GetActor() && H.GetActor()->IsA<ALandscapeProxy>(); });
            Line += FString::Printf(TEXT(" | %.0f%%: landscape %.0f cm, LakeMap %.0f cm"), T * 100, Bed ? LakeMap::SurfaceZ - Bed->ImpactPoint.Z : -1.0f, LakeMap::Depth(P.X, P.Y));
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL_DEPTHS%s"), *Line);
        // What the float fisher finds in front of each spot, 3 and 6 m out.
        FString Spots;
        for (const LakeMap::FSpot& Spot : LakeMap::Spots())
        {
            const FVector2D A = Spot.Location - Spot.Outward * 300, B = Spot.Location - Spot.Outward * 600;
            Spots += FString::Printf(TEXT(" | %s %.0f/%.0f"), Spot.Name, LakeMap::Depth(A.X, A.Y), LakeMap::Depth(B.X, B.Y));
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL_SPOTS cm at 3/6 m%s"), *Spots);
    }

    // --- terrain: 64 m chunks, each its own component; 1 m cells near the water, 2 m and 4 m further off --
    {
        const FBox2D Bounds = LakeMap::TerrainBounds;
        constexpr float ChunkSize = 6400.0f;
        UMaterialInterface* TerrainMaterial = Material(TEXT("M_TerrainScanned"));
        int32 Chunks = 0, Vertices1 = 0;
        for (float CY = Bounds.Min.Y; CY < Bounds.Max.Y - 1 && !bLevelGround; CY += ChunkSize)
            for (float CX = Bounds.Min.X; CX < Bounds.Max.X - 1; CX += ChunkSize)
            {
                const float W = FMath::Min(ChunkSize, (float)Bounds.Max.X - CX), Hh = FMath::Min(ChunkSize, (float)Bounds.Max.Y - CY);
                // Detail by the distance to the water: the banks, the shallows and the faces of the hills near it are fine.
                float Near = TNumericLimits<float>::Max();
                for (float Y = CY; Y <= CY + Hh; Y += 400)
                    for (float X = CX; X <= CX + W; X += 400) Near = FMath::Min(Near, FMath::Abs(LakeMap::ShoreDistance(X, Y)));
                const float Step = Near < 6000 ? 100.0f : Near < 18000 ? 200.0f : 400.0f;
                const int32 NX = FMath::RoundToInt(W / Step), NY = FMath::RoundToInt(Hh / Step);
                TArray<FVector> Verts, Normals;
                TArray<FVector2D> UVs;
                TArray<int32> Triangles;
                TArray<FLinearColor> Colors;
                TArray<FProcMeshTangent> Tangents;
                auto Vertex = [&](float X, float Y, float Drop)
                {
                    const FVector2D P(X, Y);
                    const float H = Ground(P);
                    constexpr float N = 200.0f;     // the same for every chunk, so chunks of different detail shade alike
                    const float DX = (Ground(P + FVector2D(N, 0)) - Ground(P - FVector2D(N, 0))) / (2 * N);
                    const float DY = (Ground(P + FVector2D(0, N)) - Ground(P - FVector2D(0, N))) / (2 * N);
                    Verts.Add(FVector(P, H - Drop));
                    Normals.Add(FVector(-DX, -DY, 1).GetSafeNormal());
                    Tangents.Add(FProcMeshTangent(FVector(1, 0, DX).GetSafeNormal(), false));
                    // On steep faces the texture is laid along the slope, not stretched down it.
                    const FVector2D Grad(DX, DY);
                    const float Steep = FMath::SmoothStep(0.4f, 1.2f, (float)Grad.Size());
                    UVs.Add((P + Grad.GetSafeNormal() * (H - LakeMap::SurfaceZ) * Steep) / 250);
                    Colors.Add(ALakeEnvironment::TerrainLayers(P));
                };
                for (int32 Y = 0; Y <= NY; ++Y)
                    for (int32 X = 0; X <= NX; ++X)
                    {
                        Vertex(CX + X * Step, CY + Y * Step, 0);
                        if (X < NX && Y < NY)
                        {
                            const int32 A = Y * (NX + 1) + X;
                            Triangles.Append({ A, A + NX + 1, A + 1, A + 1, A + NX + 1, A + NX + 2 });
                        }
                    }
                // Skirts: a short curtain hanging from each edge hides the cracks where chunks of
                // different detail meet.
                auto Skirt = [&](int32 X0, int32 Y0, int32 DX, int32 DY, int32 Count)
                {
                    for (int32 K = 0; K < Count; ++K)
                    {
                        const int32 Base = Verts.Num();
                        const float AX = CX + (X0 + DX * K) * Step, AY = CY + (Y0 + DY * K) * Step;
                        const float BX = AX + DX * Step, BY = AY + DY * Step;
                        Vertex(AX, AY, 0); Vertex(BX, BY, 0); Vertex(AX, AY, 120); Vertex(BX, BY, 120);
                        Triangles.Append({ Base, Base + 2, Base + 1, Base + 1, Base + 2, Base + 3, Base, Base + 1, Base + 2, Base + 1, Base + 3, Base + 2 });
                    }
                };
                Skirt(0, 0, 1, 0, NX); Skirt(0, NY, 1, 0, NX); Skirt(0, 0, 0, 1, NY); Skirt(NX, 0, 0, 1, NY);
                auto* Terrain = NewObject<UProceduralMeshComponent>(this);
                Terrain->SetupAttachment(RootComponent);
                Terrain->bUseAsyncCooking = true;
                Terrain->RegisterComponent();
                AddInstanceComponent(Terrain);
                Terrain->CreateMeshSection_LinearColor(0, Verts, Triangles, Normals, UVs, Colors, Tangents, true);
                Terrain->SetMaterial(0, TerrainMaterial);
                ++Chunks;
                Vertices1 += Verts.Num();
            }
        // The distant ring: wooded hills out to 3 km on a 40 m grid, no collision, tucked under the
        // terrain where they overlap. They close the horizon seen across the lake.
        constexpr float Far = 300000.0f, FarCell = 4000.0f, FarChunk = 64000.0f;
        const FBox2D Outer = Bounds.ExpandBy(Far);
        for (float CY = Outer.Min.Y; CY < Outer.Max.Y - 1; CY += FarChunk)
            for (float CX = Outer.Min.X; CX < Outer.Max.X - 1; CX += FarChunk)
            {
                const FBox2D ChunkBox(FVector2D(CX, CY), FVector2D(FMath::Min(CX + FarChunk, (float)Outer.Max.X), FMath::Min(CY + FarChunk, (float)Outer.Max.Y)));
                if (Bounds.ExpandBy(-FarCell).IsInside(ChunkBox)) continue;
                const int32 NX = FMath::RoundToInt(ChunkBox.GetSize().X / FarCell), NY = FMath::RoundToInt(ChunkBox.GetSize().Y / FarCell);
                TArray<FVector> Verts, Normals;
                TArray<FVector2D> UVs;
                TArray<int32> Triangles;
                TArray<FLinearColor> Colors;
                TArray<FProcMeshTangent> Tangents;
                for (int32 Y = 0; Y <= NY; ++Y)
                    for (int32 X = 0; X <= NX; ++X)
                    {
                        const FVector2D P = ChunkBox.Min + FVector2D(X, Y) * FarCell;
                        auto H = [&](const FVector2D& Q) { return LakeMap::Ground(Q.X, Q.Y) - (Bounds.ExpandBy(-FarCell * 0.5f).IsInside(Q) ? 300.0f : 0.0f); };
                        const float DX = (H(P + FVector2D(FarCell, 0)) - H(P - FVector2D(FarCell, 0))) / (2 * FarCell);
                        const float DY = (H(P + FVector2D(0, FarCell)) - H(P - FVector2D(0, FarCell))) / (2 * FarCell);
                        Verts.Add(FVector(P, H(P)));
                        Normals.Add(FVector(-DX, -DY, 1).GetSafeNormal());
                        Tangents.Add(FProcMeshTangent(FVector(1, 0, DX).GetSafeNormal(), false));
                        UVs.Add(P / 250);
                        // Seen only from afar: green hills read better than bare forest floor.
                        const float Forest = LakeMap::ForestAmount(P);
                        Colors.Add(FLinearColor(1 - 0.4f * Forest, 0.4f * Forest, 0));
                        if (X < NX && Y < NY)
                        {
                            const int32 A = Y * (NX + 1) + X;
                            Triangles.Append({ A, A + NX + 1, A + 1, A + 1, A + NX + 1, A + NX + 2 });
                        }
                    }
                auto* Ring = NewObject<UProceduralMeshComponent>(this);
                Ring->SetupAttachment(RootComponent);
                Ring->RegisterComponent();
                AddInstanceComponent(Ring);
                Ring->CreateMeshSection_LinearColor(0, Verts, Triangles, Normals, UVs, Colors, Tangents, false);
                Ring->SetMaterial(0, TerrainMaterial);
            }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BUILD terrain %d chunks, %d vertices, %.1f s"), Chunks, Vertices1, FPlatformTime::Seconds() - BuildStart);
    }

    // --- water: one surface over the whole lake -----------------------------------------------------
    auto* Water = Group(TEXT("Cube"), TEXT("M_LakeSurface"), true);
    const FVector2D WaterExtent = LakeBox.GetExtent() + FVector2D(3000, 3000);
    if (!bLevelWater) Add(Water, FVector(LakeBox.GetCenter(), LakeMap::SurfaceZ - 6), FVector(WaterExtent.X / 50, WaterExtent.Y / 50, 0.12));
    Water->SetCullDistances(0, 0);
    // The water slab must not shade what is under it: the sun and the sky light the shallows, the
    // bottom and the fish through the water (the water material dims them by depth itself).
    Water->SetCastShadow(false);
    Water->bAffectDistanceFieldLighting = false;
    Water->bAffectDynamicIndirectLighting = false;
    auto* Bark = Group(TEXT("Cylinder"), TEXT("M_OldBark"), true);    // piles, snags, stakes, logs
    auto* Wood = Group(TEXT("Cube"), TEXT("M_WeatheredWood"), true);
    auto* Metal = Group(TEXT("Cylinder"), TEXT("M_Metal"), true);
    auto* Burrows = Group(TEXT("Sphere"), TEXT("M_OldBark"), false);     // sand martins' holes in the clay
    Burrows->SetCastShadow(false);
    UHierarchicalInstancedStaticMeshComponent* Roots = nullptr;            // roots in the cliffs' faces
    if (UStaticMesh* RootMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Bottom/SM_BottomBranch2.SM_BottomBranch2")))
    {
        Roots = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        Roots->SetupAttachment(RootComponent);
        Roots->SetStaticMesh(RootMesh);
        Roots->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Roots->SetCullDistances(12000, 16000);
        Roots->RegisterComponent();
        AddInstanceComponent(Roots);
    }
    // Invisible trunk colliders: the Megaplants trees are skinned meshes without per-instance collision.
    auto* Trunks = Group(TEXT("Cylinder"), TEXT("M_Bark"), true);
    Trunks->SetHiddenInGame(true);
    Trunks->SetCastShadow(false);

    // --- Quixel Megaplants: Nanite skinned meshes animated by the Dynamic Wind plugin ----------------
    // One instanced component per mesh variant draws every copy of that variant in one batch.
    auto* Wind = NewObject<UDynamicWindData>(this);
    // bSway = false: the deep forest, seen from the lake as the far wall of trees: no wind and no
    // shadow casting. Instanced skinned meshes redraw their virtual shadow pages every frame even
    // when still (they always count as moving), which is the main cost of a forest.
    auto PlantComponent = [this, Wind](USkinnedAsset* Mesh, float CullDistance, bool bSway)
    {
        auto* Component = NewObject<UInstancedSkinnedMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetSkinnedAssetAndUpdate(Mesh);
        if (bSway) Component->SetTransformProvider(Wind);
        else Component->SetCastShadow(false);       // far forest: Lumen shades it, no shadow pages to redraw
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->SetCullDistances(CullDistance * 0.8f, CullDistance);
        Component->RegisterComponent();
        AddInstanceComponent(Component);
        return Component;
    };
    auto Plants = [&PlantComponent](const TCHAR* Folder, const TCHAR* Prefix, const TCHAR* Letters, float CullDistance, bool bSway = true)
    {
        TArray<UInstancedSkinnedMeshComponent*> Variants;
        for (const TCHAR* Letter = Letters; *Letter; ++Letter)
        {
            const FString Name = FString::Printf(TEXT("%s_%c"), Prefix, *Letter);
            auto* Mesh = LoadObject<USkinnedAsset>(nullptr, *FString::Printf(TEXT("/Game/Megaplant_Library/%s/%s.%s"), Folder, *Name, *Name));
            if (!Mesh)
            {
                UE_LOG(LogTemp, Warning, TEXT("Megaplant mesh missing: %s/%s"), Folder, *Name);
                continue;
            }
            Variants.Add(PlantComponent(Mesh, CullDistance, bSway));
        }
        return Variants;
    };
    // Plants grown in the Procedural Vegetation Editor (graphs and exports in /Game/Plants/PV).
    auto GrownPlant = [&PlantComponent](const TCHAR* Name, float CullDistance, bool bSway = true)
    {
        TArray<UInstancedSkinnedMeshComponent*> Variants;
        if (auto* Mesh = LoadObject<USkinnedAsset>(nullptr, *FString::Printf(TEXT("/Game/Plants/PV/%s.%s"), Name, Name)))
            Variants.Add(PlantComponent(Mesh, CullDistance, bSway));
        else
            UE_LOG(LogTemp, Warning, TEXT("Grown plant missing: %s"), Name);
        return Variants;
    };
    // Trees by the water sway and cast shadows; the forest behind stands still and is seen from
    // across the lake, so it is drawn far.
    const auto Pines = Plants(TEXT("Tree_Baltic_Pine/Tree_Baltic_Pine_01"), TEXT("Tree_Baltic_Pine_01"), TEXT("ABCD"), 40000);
    auto Spruces = Plants(TEXT("Tree_Norway_Spruce/Tree_Norway_Spruce_01"), TEXT("Tree_Norway_Spruce_01"), TEXT("ABCD"), 40000);
    Spruces.Append(GrownPlant(TEXT("SKM_PV_Spruce"), 40000));            // a fifth spruce, grown in the editor
    const auto Birches = Plants(TEXT("Tree_Silver_Birch/Tree_Silver_Birch_01"), TEXT("Tree_Silver_Birch_01"), TEXT("ABCD"), 40000);
    const auto Alders = Plants(TEXT("Tree_Black_Alder/Tree_Black_Alder_01"), TEXT("Tree_Black_Alder_01"), TEXT("ABCD"), 40000);
    const auto StillPines = Plants(TEXT("Tree_Baltic_Pine/Tree_Baltic_Pine_01"), TEXT("Tree_Baltic_Pine_01"), TEXT("ABCD"), 110000, false);
    auto StillSpruces = Plants(TEXT("Tree_Norway_Spruce/Tree_Norway_Spruce_01"), TEXT("Tree_Norway_Spruce_01"), TEXT("ABCD"), 110000, false);
    StillSpruces.Append(GrownPlant(TEXT("SKM_PV_Spruce"), 110000, false));
    // Undergrowth: a broad deciduous shrub (3.4 m tall, 5 m across at full size), drawn near only.
    const auto Shrubs = GrownPlant(TEXT("SKM_PV_Shrub"), 15000);
    const auto StillBirches = Plants(TEXT("Tree_Silver_Birch/Tree_Silver_Birch_01"), TEXT("Tree_Silver_Birch_01"), TEXT("ABCD"), 110000, false);
    const auto StillAlders = Plants(TEXT("Tree_Black_Alder/Tree_Black_Alder_01"), TEXT("Tree_Black_Alder_01"), TEXT("ABCD"), 110000, false);
    const auto Bulrush = Plants(TEXT("Plant_Bulrush/Plant_Bulrush_01"), TEXT("Plant_Common_Bulrush_01"), TEXT("ABCDE"), 18000);
    const auto DryReed = Plants(TEXT("Plant_Dry_Common_Reeds/Plant_Dry_Common_Reeds_01"), TEXT("Plant_Dry_Common_Reed"), TEXT("ABCD"), 18000);
    // Instances are gathered and added to each component in one go (one by one is very slow).
    TMap<UInstancedSkinnedMeshComponent*, TArray<FTransform>> Pending;
    auto Place = [&Random, &Pending](const TArray<UInstancedSkinnedMeshComponent*>& Variants, const FTransform& Transform)
    {
        if (Variants.Num() == 0) return;
        Pending.FindOrAdd(Variants[Random.RandRange(0, Variants.Num() - 1)]).Add(Transform);
    };
    auto Upright = [&Random](FVector Location, float Scale)
    {
        return FTransform(FRotator(0, Random.FRandRange(0, 360), 0), Location, FVector(Scale));
    };

    // Forest: dense along the wooded banks (birch and alder by the water, pine and spruce behind,
    // pines on the bluff), thinner further off; groves and lone trees in the meadow. The camp, the
    // village and the spots stay open.
    int32 TreeCount = 0;
    TArray<FTransform> TrunkTransforms;
    const bool PCGForest = ALakeForest::HasGraph();
    if (!PCGForest)
    {
        const FBox2D Land = LakeMap::TerrainBounds.ExpandBy(-500);
        constexpr float TreeCell = 500.0f;
        for (float CY = Land.Min.Y; CY < Land.Max.Y; CY += TreeCell)
            for (float CX = Land.Min.X; CX < Land.Max.X; CX += TreeCell)
            {
                const FVector2D P(CX + Random.FRandRange(0, TreeCell), CY + Random.FRandRange(0, TreeCell));
                const float L = -LakeMap::ShoreDistance(P.X, P.Y) / 100;
                if (L < 3) continue;
                const float Forest = LakeMap::ForestAmount(P);
                const float FS = ForestStart(P);
                const bool bForest = Forest > 0.5f && L > FS + Random.FRandRange(-4, 4);
                // Trees per cell: one in two by the water, fewer deep in the woods.
                const float Chance = bForest ? (L < 70 ? 0.5f : L < 180 ? 0.24f : 0.13f) : 0.012f;
                if (Random.FRand() > Chance) continue;
                if (FVector2D::Distance(P, Village) < 9000 || FVector2D::Distance(P, Start) < 1600) continue;
                if (LakeMap::SpotAmount(P) > 0.05f || SegmentDistance2D(P, Pier.Root - Pier.Dir * 800, Pier.Root) < 600) continue;
                if (Slope(P.X, P.Y) > 0.8f) continue;               // not on the faces of bluffs, scarps and ravines
                // Out in the meadows trees stand in small groves of birch and alder.
                const int32 Grove = bForest ? 1 : Random.RandRange(1, 4);
                for (int32 G = 0; G < Grove; ++G)
                {
                    const FVector2D Q = P + (G ? FVector2D(Random.FRandRange(-600, 600), Random.FRandRange(-600, 600)) : FVector2D::ZeroVector);
                    if (-LakeMap::ShoreDistance(Q.X, Q.Y) < 300) continue;
                    // Sink the base slightly so roots never float on slopes.
                    const FVector Base(Q, Ground(Q) - 15);
                    // By the water and in the meadows trees sway; deeper in they stand still.
                    const bool bSway = !bForest || L < FS + 14;
                    const float Pick = Random.FRand();
                    const bool bBluffTop = LakeMap::BluffHeight(Q) > 2.5f;
                    const TArray<UInstancedSkinnedMeshComponent*>* Species;
                    if (!bForest) Species = Pick < 0.6f ? &Birches : &Alders;
                    else if (bBluffTop) Species = bSway ? &Pines : &StillPines;
                    else if (L < FS + 20) Species = Pick < 0.5f ? (bSway ? &Birches : &StillBirches) : Pick < 0.85f ? (bSway ? &Alders : &StillAlders) : (bSway ? &Pines : &StillPines);
                    else Species = Pick < 0.5f ? (bSway ? &Pines : &StillPines) : (bSway ? &Spruces : &StillSpruces);
                    Place(*Species, Upright(Base, Random.FRandRange(0.7f, 1.1f)));
                    TrunkTransforms.Add(FTransform(FRotator::ZeroRotator, Base + FVector(0, 0, 250), FVector(0.45f, 0.45f, 5.0f)));
                    ++TreeCount;
                }
            }
    }
    // Alders and birches on the wooded banks, leaning out over the water.
    const TArray<LakeMap::FShorePoint>& Shore = LakeMap::Shoreline();
    for (int32 I = 0; I < Shore.Num(); I += 9)
    {
        const LakeMap::FShorePoint& S = Shore[I];
        if (LakeMap::SpotAmount(S.Location) > 0.1f || LakeMap::BluffHeight(S.Location) > 0.5f || Random.FRand() > 0.45f * LakeMap::ForestAmount(S.Location)) continue;
        if (SegmentDistance2D(S.Location, Pier.Root, PierEnd) < 1500) continue;
        const FVector2D P = S.Location + S.Outward * Random.FRandRange(150, 450);
        const FVector Toward(-S.Outward, 0);
        const FVector Axis = FVector::CrossProduct(FVector::UpVector, Toward).GetSafeNormal();
        const FQuat Lean = FQuat(Axis, FMath::DegreesToRadians(Random.FRandRange(6, 16))) * FQuat(FVector::UpVector, Random.FRandRange(0, 2 * PI));
        Place(Random.FRand() < 0.6f ? Alders : Birches, FTransform(Lean, FVector(P, Ground(P) - 15), FVector(Random.FRandRange(0.65f, 0.95f))));
        TrunkTransforms.Add(FTransform(FRotator::ZeroRotator, FVector(P, Ground(P) + 235), FVector(0.4f, 0.4f, 4.5f)));
        ++TreeCount;
    }
    // The little bay at the west end: a few alders and birches leaning out over it.
    {
        const FVector2D Bay = LakeMap::WestBay();
        int32 Leaning = 0;
        for (int32 I = 0; I < Shore.Num() && Leaning < 5; I += 3)
        {
            const LakeMap::FShorePoint& S = Shore[I];
            if (FVector2D::Distance(S.Location, Bay) > 2200 || LakeMap::SpotAmount(S.Location) > 0.05f || Random.FRand() > 0.35f) continue;
            const FVector2D P = S.Location + S.Outward * Random.FRandRange(180, 320);
            const FVector Toward(-S.Outward, 0);
            const FVector Axis = FVector::CrossProduct(FVector::UpVector, Toward).GetSafeNormal();
            const FQuat Lean = FQuat(Axis, FMath::DegreesToRadians(Random.FRandRange(10, 20))) * FQuat(FVector::UpVector, Random.FRandRange(0, 2 * PI));
            Place(Random.FRand() < 0.6f ? Alders : Birches, FTransform(Lean, FVector(P, Ground(P) - 15), FVector(Random.FRandRange(0.75f, 1.0f))));
            TrunkTransforms.Add(FTransform(FRotator::ZeroRotator, FVector(P, Ground(P) + 235), FVector(0.4f, 0.4f, 4.5f)));
            ++TreeCount;
            ++Leaning;
        }
    }
    // Details of the banks: the old pine on the high bank at "Омут", a birch grove on the hill above
    // "Сосновый яр", alders crowding the bank at "Ольхи".
    {
        FRandomStream DetailRandom(20261103);
        const LakeMap::FBankThing Pine = LakeMap::OldPine();
        Place(Pines, FTransform(FRotator(0, DetailRandom.FRandRange(0, 360), 0), FVector(Pine.Location, Ground(Pine.Location) - 20), FVector(1.25f)));
        TrunkTransforms.Add(FTransform(FRotator::ZeroRotator, FVector(Pine.Location, Ground(Pine.Location) + 300), FVector(0.6f, 0.6f, 6.0f)));
        float GroveRadius = 0;
        const FVector2D Grove = LakeMap::BirchGrove(&GroveRadius);
        for (int32 I = 0, Placed = 0; I < 40 && Placed < 9; ++I)
        {
            const FVector2D P = Grove + FVector2D(1, 0).GetRotated(DetailRandom.FRandRange(0, 360)) * GroveRadius * FMath::Sqrt(DetailRandom.FRand());
            if (LakeMap::ShoreDistance(P.X, P.Y) > -400 || Slope(P.X, P.Y) > 0.6f || LakeMap::PathAmount(P) > 0.2f) continue;
            Place(Birches, Upright(FVector(P, Ground(P) - 12), DetailRandom.FRandRange(0.75f, 1.05f)));
            TrunkTransforms.Add(FTransform(FRotator::ZeroRotator, FVector(P, Ground(P) + 230), FVector(0.35f, 0.35f, 4.5f)));
            ++Placed;
            ++TreeCount;
        }
        for (const LakeMap::FShorePoint& S : Shore)
        {
            if (FVector2D::Distance(S.Location, FVector2D(-27400, 6000)) > 1600 || LakeMap::SpotAmount(S.Location) > 0.05f || DetailRandom.FRand() > 0.12f) continue;
            const FVector2D P = S.Location + S.Outward * DetailRandom.FRandRange(300, 900);
            const FVector Toward(-S.Outward, 0);
            const FQuat Lean = FQuat(FVector::CrossProduct(FVector::UpVector, Toward).GetSafeNormal(), FMath::DegreesToRadians(DetailRandom.FRandRange(4, 14)))
                * FQuat(FVector::UpVector, DetailRandom.FRandRange(0, 2 * PI));
            Place(Alders, FTransform(Lean, FVector(P, Ground(P) - 15), FVector(DetailRandom.FRandRange(0.6f, 0.9f))));
            TrunkTransforms.Add(FTransform(FRotator::ZeroRotator, FVector(P, Ground(P) + 235), FVector(0.4f, 0.4f, 4.5f)));
            ++TreeCount;
        }
    }
    // Weeping willows over the water and the old oak by the pier (bought models, static meshes).
    {
        auto Tree = [this](const TCHAR* Name, const LakeMap::FBankThing& Spot, float Scale, float Lean)
        {
            UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Plants/Trees/SM_%s.SM_%s"), Name, Name));
            if (!Mesh) return false;
            auto* C = NewObject<UStaticMeshComponent>(this);
            C->SetupAttachment(RootComponent);
            C->SetStaticMesh(Mesh);
            C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            C->RegisterComponent();
            AddInstanceComponent(C);
            const FVector Toward(Spot.Facing, 0);
            const FQuat Tilt = FQuat(FVector::CrossProduct(FVector::UpVector, Toward).GetSafeNormal(), FMath::DegreesToRadians(Lean));
            C->SetWorldTransform(FTransform(Tilt * FQuat(FVector::UpVector, FMath::Atan2(Spot.Facing.Y, Spot.Facing.X)),
                FVector(Spot.Location, LakeMap::Ground(Spot.Location.X, Spot.Location.Y) - 25), FVector(Scale)));
            return true;
        };
        int32 Willows = 0;
        for (const LakeMap::FBankThing& Willow : LakeMap::Willows())
        {
            Willows += Tree(TEXT("Willow"), Willow, Random.FRandRange(0.9f, 1.1f), Random.FRandRange(4, 9));
            TrunkTransforms.Add(FTransform(FRotator::ZeroRotator, FVector(Willow.Location, Ground(Willow.Location) + 200), FVector(0.5f, 0.5f, 4.0f)));
        }
        const bool bOak = Tree(TEXT("Oak"), LakeMap::OldOak(), 1.0f, 0);
        if (bOak) TrunkTransforms.Add(FTransform(FRotator::ZeroRotator, FVector(LakeMap::OldOak().Location, Ground(LakeMap::OldOak().Location) + 250), FVector(0.8f, 0.8f, 5.0f)));
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_TREES willows %d, old oak %d"), Willows, bOak);
    }
    // Small groves frame the camp; keep the central view and the walking approach open.
    {
        FRandomStream GroveRandom(311026);
        const FVector2D Side(-StartFacing.Y, StartFacing.X);
        const FVector2D Centres[] = { Start - StartFacing * 1050 + Side * 1850,
            Start - StartFacing * 1550 - Side * 1650, Start + StartFacing * 150 + Side * 2700 };
        for (const FVector2D& Centre : Centres)
            for (int32 I = 0; I < 3; ++I)
            {
                const FVector2D P = Centre + FVector2D(GroveRandom.FRandRange(-260, 260), GroveRandom.FRandRange(-260, 260));
                if (LakeMap::ShoreDistance(P.X, P.Y) > -200 || Slope(P.X, P.Y) > 0.6f || LakeMap::SpotAmount(P) > 0.1f) continue;
                const auto& Variants = I == 1 ? Alders : Birches;
                if (Variants.IsEmpty()) continue;
                const FVector Base(P, Ground(P) - 12);
                const float Size = GroveRandom.FRandRange(0.55f, 0.85f);
                const FQuat Lean = FQuat(FVector(StartFacing.Y, -StartFacing.X, 0), FMath::DegreesToRadians(I == 0 ? 8.0f : 3.0f));
                Pending.FindOrAdd(Variants[GroveRandom.RandRange(0, Variants.Num() - 1)]).Add(
                    FTransform(Lean * FQuat(FVector::UpVector, GroveRandom.FRandRange(0, 2 * PI)), Base, FVector(Size)));
                TrunkTransforms.Add(FTransform(FRotator::ZeroRotator, Base + FVector(0, 0, 180), FVector(0.28f, 0.28f, 3.6f)));
                ++TreeCount;
            }
    }
    Trunks->AddInstances(TrunkTransforms, false, true);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BUILD trees %d, %.1f s"), TreeCount, FPlatformTime::Seconds() - BuildStart);

    // Shrubs: thick along the forest edge, a few under the trees, clumps on the wooded banks a few
    // metres from the water. The camp, the spots, the pier and the village stay open.
    int32 ShrubCount = 0;
    auto PlaceShrub = [&](const FVector2D& P, float MinScale, float MaxScale)
    {
        if (LakeMap::PathAmount(P) > 0.2f || LakeMap::StreamDistance(P) < 350) return;
        if (FVector2D::Distance(P, Village) < 9000 || FVector2D::Distance(P, Start) < 1400) return;
        if (LakeMap::SpotAmount(P) > 0.05f || SegmentDistance2D(P, Pier.Root - Pier.Dir * 800, PierEnd) < 600) return;
        if (Slope(P.X, P.Y) > 0.7f || -LakeMap::ShoreDistance(P.X, P.Y) < 180) return;
        const FQuat Tilt = FQuat(FVector(Random.FRandRange(-1, 1), Random.FRandRange(-1, 1), 0).GetSafeNormal(), FMath::DegreesToRadians(Random.FRandRange(0, 5)));
        const float S = Random.FRandRange(MinScale, MaxScale);
        Place(Shrubs, FTransform(Tilt * FQuat(FVector::UpVector, Random.FRandRange(0, 2 * PI)), FVector(P, Ground(P) - 10), FVector(S, S, S * Random.FRandRange(0.85f, 1.1f))));
        ++ShrubCount;
    };
    {
        const FBox2D Land = LakeMap::TerrainBounds.ExpandBy(-500);
        constexpr float ShrubCell = 450.0f;
        for (float CY = Land.Min.Y; CY < Land.Max.Y; CY += ShrubCell)
            for (float CX = Land.Min.X; CX < Land.Max.X; CX += ShrubCell)
            {
                const FVector2D P(CX + Random.FRandRange(0, ShrubCell), CY + Random.FRandRange(0, ShrubCell));
                const float L = -LakeMap::ShoreDistance(P.X, P.Y) / 100;
                if (L < 3 || L > 160) continue;
                const float Forest = LakeMap::ForestAmount(P);
                if (Forest < 0.3f) continue;
                const float FS = ForestStart(P);
                // Edge: from a few metres before the first trees to ten metres in; then sparse.
                const float Edge = 1 - FMath::SmoothStep(4.0f, 14.0f, FMath::Abs(L - FS - 4));
                if (Random.FRand() > 0.06f * Forest + 0.4f * Edge * Forest) continue;
                PlaceShrub(P, 0.35f, 0.7f);
            }
        for (int32 I = 0; I < Shore.Num(); I += 6)
        {
            const LakeMap::FShorePoint& S = Shore[I];
            if (LakeMap::BluffHeight(S.Location) > 0.5f || Random.FRand() > 0.3f * LakeMap::ForestAmount(S.Location)) continue;
            PlaceShrub(S.Location + S.Outward * Random.FRandRange(220, 500), 0.3f, 0.55f);
        }
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BUILD shrubs %d"), ShrubCount);

    // Mossy boulders (Quixel Megascans, Nanite): under the bluffs, along the banks, a few in the
    // fields and the woods.
    auto* Rocks = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
    Rocks->SetupAttachment(RootComponent);
    Rocks->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Fab/Megascans/3D/Mossy_Rocks_tjmudcfda/High/tjmudcfda_tier_1/StaticMeshes/tjmudcfda_tier_1.tjmudcfda_tier_1")));
    Rocks->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Rocks->SetCollisionResponseToAllChannels(ECR_Block);
    Rocks->SetCullDistances(30000, 40000);
    Rocks->RegisterComponent();
    AddInstanceComponent(Rocks);
    for (int32 I = 0; I < 900; ++I)
    {
        FVector2D P;
        float S = Random.FRandRange(0.35f, 0.9f);
        if (I < 500)
        {
            const LakeMap::FShorePoint& Sh = Shore[Random.RandRange(0, Shore.Num() - 1)];
            const float Bluff = LakeMap::BluffHeight(Sh.Location);
            if (LakeMap::SpotAmount(Sh.Location) > 0.3f || Random.FRand() > 0.25f + 0.75f * FMath::SmoothStep(0.5f, 1.5f, Bluff)) continue;
            // Under the bluffs the stones lie at the foot of the clay face, partly in the water.
            P = Sh.Location + Sh.Outward * (Bluff > 0.5f ? Random.FRandRange(-160, 40) : Random.FRandRange(-180, 260));
        }
        else
        {
            P = FVector2D(Random.FRandRange(LakeBox.Min.X - 15000, LakeBox.Max.X + 15000), Random.FRandRange(LakeBox.Min.Y - 15000, LakeBox.Max.Y + 15000));
            if (LakeMap::ShoreDistance(P.X, P.Y) > -500 || FVector2D::Distance(P, Village) < 9000) continue;
        }
        if (SegmentDistance2D(P, Pier.Root, PierEnd) < 500) continue;
        Add(Rocks, FVector(P, Ground(P) - 4), FVector(S, S, S * Random.FRandRange(0.8f, 1.6f)),
            FRotator(Random.FRandRange(-4, 4), Random.FRandRange(0, 360), Random.FRandRange(-4, 4)));
    }

    // Pebbles, washed-up twigs and exposed roots break the waterline in small, irregular pockets.
    {
        FRandomStream DetailRandom(61031);
        int32 Pebbles = 0, Twigs = 0;
        for (int32 I = 0; I < Shore.Num(); I += 3)
        {
            const auto& Sh = Shore[I];
            if (LakeMap::BluffHeight(Sh.Location) > 1.0f || SegmentDistance2D(Sh.Location, Pier.Root, PierEnd) < 250) continue;
            const float Patch = FMath::PerlinNoise2D(Sh.Location / 720 + FVector2D(8, 19));
            if (Patch < -0.15f) continue;
            const FVector2D Along(-Sh.Outward.Y, Sh.Outward.X);
            for (int32 K = 0; K < 4; ++K)
            {
                const FVector2D P = Sh.Location + Sh.Outward * DetailRandom.FRandRange(-45, 160)
                    + Along * DetailRandom.FRandRange(-120, 120);
                if (Slope(P.X, P.Y) > 0.6f) continue;
                const float Size = DetailRandom.FRandRange(0.025f, 0.075f);
                Add(Rocks, FVector(P, Ground(P) - 2), FVector(Size, Size * 0.8f, Size * 0.55f), FRotator(0, DetailRandom.FRandRange(0, 360), 0));
                ++Pebbles;
            }
            // Keep debris outside the angler's feet and casting lane.
            if (LakeMap::SpotAmount(Sh.Location) > 0.15f || DetailRandom.FRand() > 0.35f) continue;
            const FVector2D P = Sh.Location + Sh.Outward * DetailRandom.FRandRange(15, 100);
            const FVector2D Q = P + Along.GetRotated(DetailRandom.FRandRange(-25, 25)) * DetailRandom.FRandRange(45, 130);
            const FVector A(P, Ground(P) + 1.5f), B(Q, Ground(Q) + 1.5f);
            Add(Bark, (A + B) * 0.5f, FVector(0.025f, 0.025f, FVector::Dist(A, B) / 100), FRotationMatrix::MakeFromZ(B - A).Rotator());
            ++Twigs;
            if (LakeMap::ForestAmount(P) > 0.5f)
                for (int32 Root = 0; Root < 2; ++Root)
                {
                    const FVector2D R = P + Sh.Outward * DetailRandom.FRandRange(100, 200) + Along * (Root ? 35 : -35);
                    const FVector End(R, Ground(R) + 2);
                    Add(Bark, (A + End) * 0.5f, FVector(0.045f, 0.045f, FVector::Dist(A, End) / 100), FRotationMatrix::MakeFromZ(End - A).Rotator());
                }
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_SHORE pebbles=%d driftwood=%d"), Pebbles, Twigs);
    }

    // Reed belt along the low banks: bulrush standing in the water, dry reed at the edge. It is broken
    // by the fishing spots, the pier, the dam and the bluffs; widest in the backwater, the bay and the
    // upper end, patchy under the trees.
    int32 ReedCount = 0;
    for (const LakeMap::FShorePoint& S : Shore)
    {
        if (LakeMap::SpotAmount(S.Location) > 0.35f || LakeMap::BluffHeight(S.Location) > 0.7f) continue;
        if (SegmentDistance2D(S.Location, Pier.Root, PierEnd) < 450) continue;
        const float Back = LakeMap::BackwaterAmount(S.Location.X, S.Location.Y);
        const float Shade = 0.35f * LakeMap::ForestAmount(S.Location);
        if (Random.FRand() > 0.55f + 0.35f * FMath::PerlinNoise2D(S.Location / 2500) + 0.4f * Back - Shade) continue;
        const FVector2D Along(-S.Outward.Y, S.Outward.X);
        const int32 Clumps = 1 + (Random.FRand() < 0.5f) + (Back > 0.5f ? 2 : 0);
        for (int32 C = 0; C < Clumps; ++C)
        {
            const float Into = Random.FRandRange(-0.6f, Back > 0.5f ? 6.0f : 2.6f);   // metres into the water
            const FVector2D P = S.Location - S.Outward * Into * 100 + Along * Random.FRandRange(-60, 60);
            if (LakeMap::Depth(P.X, P.Y) > 110) continue;
            const bool bBulrush = Into > 1.2f || Random.FRand() < 0.4f;
            Place(bBulrush ? Bulrush : DryReed, Upright(FVector(P, Ground(P) - 8), Random.FRandRange(0.8f, 1.3f)));
            ++ReedCount;
        }
    }

    // A dense stand of cattails at the upper end, broken by the window at "Верховье".
    {
        FRandomStream CattailRandom(20261104);
        int32 Cattails = 0;
        for (int32 I = 0; I < 4000 && Cattails < 260; ++I)
        {
            const FVector2D P(CattailRandom.FRandRange(26000, 39000), CattailRandom.FRandRange(0, 8500));
            if (!LakeMap::InCattails(P) || !LakeMap::IsWater(P.X, P.Y) || LakeMap::Depth(P.X, P.Y) > 110) continue;
            if (!LakeMap::LilyAllowed(P) || LakeMap::SpotAmount(P) > 0.2f || LakeMap::LilyAmount(P.X, P.Y) > 0.3f) continue;
            Place(Bulrush, Upright(FVector(P, Ground(P) - 8), CattailRandom.FRandRange(0.9f, 1.35f)));
            ++Cattails;
        }
        ReedCount += Cattails;
    }

    {
        FRandomStream LandingRandom(311031);
        const FVector2D Side(-Pier.Dir.Y, Pier.Dir.X);
        auto Plant = [&](const TArray<UInstancedSkinnedMeshComponent*>& Variants, const FVector2D& P, float Scale)
        {
            if (Variants.IsEmpty()) return;
            Pending.FindOrAdd(Variants[LandingRandom.RandRange(0, Variants.Num() - 1)]).Add(
                FTransform(FRotator(0, LandingRandom.FRandRange(0, 360), 0), FVector(P, Ground(P) - 6), FVector(Scale)));
        };
        int32 Bushes = 0, Clumps = 0;
        for (float Sign : { -1.0f, 1.0f })
        {
            for (int32 I = 0; I < 3; ++I)
            {
                const FVector2D P = Pier.Root + Side * Sign * (900 + I * 230) - Pier.Dir * LandingRandom.FRandRange(100, 400);
                if (LakeMap::ShoreDistance(P.X, P.Y) > -150 || PierApproach(P) > 0.1f) continue;
                Plant(Shrubs, P, LandingRandom.FRandRange(0.16f, 0.25f));
                ++Bushes;
            }
            for (int32 I = 0; I < 16; ++I)
            {
                const FVector2D P = Pier.Root + Side * Sign * LandingRandom.FRandRange(650, 1550)
                    + Pier.Dir * LandingRandom.FRandRange(300, 750);
                const float Depth = LakeMap::Depth(P.X, P.Y);
                if (!LakeMap::IsWater(P.X, P.Y) || Depth < 5 || Depth > 95) continue;
                Plant(I % 3 ? Bulrush : DryReed, P, LandingRandom.FRandRange(0.5f, 0.8f));
                ++Clumps;
            }
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_PIER_DRESSING low shrubs=%d reed clumps=%d; village meadow left open"), Bushes, Clumps);
    }

    for (TPair<UInstancedSkinnedMeshComponent*, TArray<FTransform>>& Batch : Pending)
    {
        TArray<int32> Animations;
        Animations.Init(0, Batch.Value.Num());
        Batch.Key->AddInstances(Batch.Value, Animations, false);
    }
    Pending.Reset();
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BUILD reeds placed, %.1f s"), FPlatformTime::Seconds() - BuildStart);

    // Water lily beds: floating leaves, white flowers and the yellow cups of the brandy-bottle lily.
    auto LilyGroup = [this](const TCHAR* Name)
    {
        auto* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Plants/SM_%s.SM_%s"), Name, Name)));
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->SetCullDistances(20000, 26000);
        // Hundreds of thousands of small plants: keep them out of the distance fields and the Lumen
        // scene; they are lit by it but do not need to light anything themselves.
        Component->bAffectDistanceFieldLighting = false;
        Component->bAffectDynamicIndirectLighting = false;
        Component->bAffectIndirectLightingWhileHidden = false;
        Component->RegisterComponent();
        AddInstanceComponent(Component);
        return Component;
    };
    UHierarchicalInstancedStaticMeshComponent* Pads[] = { LilyGroup(TEXT("LilyPad_A")), LilyGroup(TEXT("LilyPad_B")), LilyGroup(TEXT("LilyPad_C")) };
    auto* Flowers = LilyGroup(TEXT("LilyFlower"));
    auto* Yellow = LilyGroup(TEXT("LilyYellow"));
    int32 LilyCount = 0;
    for (const LakeMap::FLilyBed& Bed : LakeMap::LilyBeds())
    {
        const int32 Count = FMath::RoundToInt(PI * FMath::Square(Bed.Radius / 100) * 5.0f);
        for (int32 K = 0; K < Count; ++K)
        {
            // Denser in the middle of a bed, thinning out at its edge.
            const float A = Random.FRandRange(0, 2 * PI), R = Bed.Radius * FMath::Pow(Random.FRand(), 0.7f);
            const FVector2D P = Bed.Centre + FVector2D(FMath::Cos(A), FMath::Sin(A)) * R;
            const float D = LakeMap::Depth(P.X, P.Y);
            if (!LakeMap::IsWater(P.X, P.Y) || D < 20 || D > Bed.MaxDepth + 10 || !LakeMap::LilyAllowed(P)) continue;
            const float Pick = Random.FRand();
            const FRotator Rot(Random.FRandRange(-2, 2), Random.FRandRange(0, 360), Random.FRandRange(-2, 2));
            Add(Pads[Pick < 0.5f ? 0 : Pick < 0.82f ? 1 : 2], FVector(P, LakeMap::SurfaceZ + 0.3f), FVector(Random.FRandRange(0.75f, 1.35f)), Rot);
            ++LilyCount;
            // White water lilies in most beds; the yellow brandy-bottle in the backwater and the upper end.
            const float Bloom = Random.FRand();
            if (Bed.bYellow ? Bloom < 0.01f : Bloom < 0.035f) Add(Flowers, FVector(P + FVector2D(12, 5), LakeMap::SurfaceZ + 0.6f), FVector(Random.FRandRange(0.8f, 1.2f)), FRotator(0, Random.FRandRange(0, 360), 0));
            else if (Bed.bYellow ? Bloom < 0.05f : Bloom < 0.042f) Add(Yellow, FVector(P + FVector2D(-8, 10), LakeMap::SurfaceZ), FVector(Random.FRandRange(0.9f, 1.2f)), FRotator(0, Random.FRandRange(0, 360), 0));
        }
    }

    // --- bottom decoration: pebbles, sunken wood, shells and crayfish ---
    // The bed up close would read as bare ground: stones and sunken wood scattered over the shallows
    // and the deeper bed, with shells and a crayfish or two where the water is clear to the eye.
    int32 PebbleCount = 0, WoodPieces = 0, BedFinds = 0;
    {
        auto BottomGroup = [this](const TCHAR* MeshName, float CullNear, float CullFar, bool bShadows)
        {
            auto* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
            Component->SetupAttachment(RootComponent);
            Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Bottom/SM_%s.SM_%s"), MeshName, MeshName)));
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetCullDistances(CullNear, CullFar);
            Component->SetCastShadow(bShadows);
            Component->bAffectDistanceFieldLighting = false;
            Component->bAffectDynamicIndirectLighting = false;
            Component->bAffectIndirectLightingWhileHidden = false;
            Component->RegisterComponent();
            AddInstanceComponent(Component);
            return Component;
        };
        UHierarchicalInstancedStaticMeshComponent* Pebbles[] = {
            BottomGroup(TEXT("Pebble_Round"), 3500, 4500, false),
            BottomGroup(TEXT("Pebble_Asymmetric"), 3500, 4500, false),
            BottomGroup(TEXT("Pebble_Teardrop"), 3500, 4500, false),
            BottomGroup(TEXT("Pebble_FlatOval"), 3500, 4500, false) };
        auto* Logs = BottomGroup(TEXT("BottomLog"), 6000, 8000, true);
        UHierarchicalInstancedStaticMeshComponent* Branches[] = {
            BottomGroup(TEXT("BottomBranch1"), 4500, 6000, false),
            BottomGroup(TEXT("BottomBranch2"), 4500, 6000, false),
            BottomGroup(TEXT("BottomBranch3"), 4500, 6000, false) };
        auto* Shells = BottomGroup(TEXT("BottomShell"), 2500, 3500, false);
        auto* Crayfish = BottomGroup(TEXT("BottomCrayfish"), 3000, 4000, false);
        auto DropPebble = [&](const FVector2D& P, float SizeScale)
        {
            if (!LakeMap::IsWater(P.X, P.Y) || LakeMap::Depth(P.X, P.Y) < 18) return;
            // Sunk a little into the silt: no floating stones.
            const FVector At(P, Ground(P) - Random.FRandRange(1.0f, 4.0f));
            const float Scale = SizeScale * Random.FRandRange(0.75f, 1.35f);
            Add(Pebbles[PebbleCount % 4], At, FVector(Scale),
                FRotator(Random.FRandRange(-8.0f, 8.0f), Random.FRandRange(0, 360), Random.FRandRange(-8.0f, 8.0f)));
            ++PebbleCount;
        };
        // Pebble fields over the shallows all round the lake, kept clear of the fishing corridors.
        const FBox2D Bounds = LakeMap::LakeBounds();
        for (int32 Try = 0; Try < 1600; ++Try)
        {
            const FVector2D P(Random.FRandRange(Bounds.Min.X, Bounds.Max.X), Random.FRandRange(Bounds.Min.Y, Bounds.Max.Y));
            const float D = LakeMap::Depth(P.X, P.Y);
            if (!LakeMap::IsWater(P.X, P.Y) || D < 20 || D > 160) continue;
            if (LakeMap::SpotAmount(P) > 0.05f) continue;
            const int32 Count = Random.RandRange(3, 8);
            for (int32 K = 0; K < Count; ++K)
                DropPebble(P + FVector2D(Random.FRandRange(-60.0f, 60.0f), Random.FRandRange(-60.0f, 60.0f)), Random.FRandRange(0.8f, 1.5f));
        }
        // And the shallows by the camp and the pier, where the water is clear to the eye.
        for (const LakeMap::FShorePoint& Bank : LakeMap::Shoreline())
        {
            const bool bCamp = FVector2D::Distance(Bank.Location, Start) < 3000;
            const bool bPier = FVector2D::Distance(Bank.Location, PierEnd) < 2500;
            if (!bCamp && !bPier) continue;
            const int32 Clusters = Random.RandRange(1, bCamp ? 3 : 2);
            for (int32 C = 0; C < Clusters; ++C)
            {
                const FVector2D Into = Bank.Location - Bank.Outward * Random.FRandRange(50.0f, 500.0f);
                const int32 Stones = Random.RandRange(2, 6);
                for (int32 K = 0; K < Stones; ++K)
                    DropPebble(Into + FVector2D(Random.FRandRange(-50.0f, 50.0f), Random.FRandRange(-50.0f, 50.0f)), Random.FRandRange(0.8f, 1.6f));
                if (Random.FRand() < 0.5f)
                {
                    const int32 ShellCount = Random.RandRange(1, 3);
                    for (int32 S = 0; S < ShellCount; ++S)
                    {
                        const FVector2D Q = Into + FVector2D(Random.FRandRange(-60.0f, 60.0f), Random.FRandRange(-60.0f, 60.0f));
                        if (!LakeMap::IsWater(Q.X, Q.Y) || LakeMap::Depth(Q.X, Q.Y) < 8) continue;
                        Add(Shells, FVector(Q, Ground(Q) - 0.5f), FVector(Random.FRandRange(0.8f, 1.3f)),
                            FRotator(Random.FRandRange(-10.0f, 10.0f), Random.FRandRange(0, 360), Random.FRandRange(-10.0f, 10.0f)));
                        ++BedFinds;
                    }
                }
                if (Random.FRand() < 0.10f)
                {
                    const FVector2D Q = Into + FVector2D(Random.FRandRange(-70.0f, 70.0f), Random.FRandRange(-70.0f, 70.0f));
                    if (LakeMap::IsWater(Q.X, Q.Y) && LakeMap::Depth(Q.X, Q.Y) >= 15)
                    {
                        Add(Crayfish, FVector(Q, Ground(Q) + 1.0f), FVector(Random.FRandRange(0.9f, 1.4f)),
                            FRotator(Random.FRandRange(-6.0f, 6.0f), Random.FRandRange(0, 360), 0.0f));
                        ++BedFinds;
                    }
                }
            }
        }
        // Sunken wood: a log or a branch lying along the slant of the deeper bed.
        auto DropWood = [&](UHierarchicalInstancedStaticMeshComponent* Group, const FVector2D& P, float HalfLen, float Sink)
        {
            const float Yaw = Random.FRandRange(0, 360);
            const FVector Dir = FRotator(0, Yaw, 0).Vector();
            const float Ahead = LakeMap::Ground(P.X + Dir.X * HalfLen, P.Y + Dir.Y * HalfLen);
            const float Behind = LakeMap::Ground(P.X - Dir.X * HalfLen, P.Y - Dir.Y * HalfLen);
            const float Tilt = FMath::RadiansToDegrees(FMath::Atan2(Ahead - Behind, 2 * HalfLen));
            Add(Group, FVector(P, Ground(P) - Sink), FVector(1.0f), FRotator(Tilt, Yaw, Random.FRandRange(-6.0f, 6.0f)));
            ++WoodPieces;
        };
        for (int32 Try = 0; Try < 1200 && WoodPieces < 90; ++Try)
        {
            const FVector2D P(Random.FRandRange(Bounds.Min.X, Bounds.Max.X), Random.FRandRange(Bounds.Min.Y, Bounds.Max.Y));
            const float D = LakeMap::Depth(P.X, P.Y);
            if (!LakeMap::IsWater(P.X, P.Y) || D < 70 || D > 300) continue;
            if (LakeMap::SpotAmount(P) > 0.05f) continue;
            if (Random.FRand() < 0.30f) DropWood(Logs, P, 110.0f, 18.0f);
            else DropWood(Branches[Random.RandRange(0, 2)], P, 45.0f, 3.0f);
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BOTTOM pebbles=%d wood=%d finds=%d"), PebbleCount, WoodPieces, BedFinds);
    }

    // Grass (Nanite, swaying in the wind): sedge along the damp bank; dense waist-high grass and
    // meadowsweet in stretches along the open banks and the forest edge; short and tall grass with
    // chamomile and buttercups in the meadows; sparse under the trees; trampled at the camp.
    int32 GrassCount = 0;
    {
        UHierarchicalInstancedStaticMeshComponent* Short = LilyGroup(TEXT("Grass_Short"));
        UHierarchicalInstancedStaticMeshComponent* Tall = LilyGroup(TEXT("Grass_Tall"));
        UHierarchicalInstancedStaticMeshComponent* Sedge = LilyGroup(TEXT("Sedge"));
        UHierarchicalInstancedStaticMeshComponent* Blooms = LilyGroup(TEXT("Meadow_Flowers"));
        UHierarchicalInstancedStaticMeshComponent* Waist = LilyGroup(TEXT("Grass_Waist"));
        UHierarchicalInstancedStaticMeshComponent* Meadowsweet = LilyGroup(TEXT("Meadowsweet"));
        // Grass sways only near the camera (WPO is the expensive part); far tufts stay still and then fade out.
        for (auto* G : { Short, Tall, Sedge, Blooms, Waist, Meadowsweet })
        {
            G->SetCullDistances(4500, 6000);
            G->SetCastShadow(false);
            G->SetWorldPositionOffsetDisableDistance(2500);
        }
        const FBox2D Area = FBox2D(LakeBox.Min - FVector2D(4500, 4500), LakeBox.Max + FVector2D(4500, 4500));
        constexpr float Spacing = 60.0f;
        for (float Y = Area.Min.Y; Y < Area.Max.Y; Y += Spacing)
            for (float X = Area.Min.X; X < Area.Max.X; X += Spacing)
            {
                const FVector2D P(X + Random.FRandRange(-30, 30), Y + Random.FRandRange(-30, 30));
                const float SD = LakeMap::ShoreDistance(P.X, P.Y);
                if (SD > 65 || SD < -4500) continue;
                const float L = -SD / 100;
                const float Trees = Woodland(P, L);
                const FVector2D CampLocal = P - Start;
                const float CampSide = FMath::Abs(FVector2D::DotProduct(CampLocal, FVector2D(-StartFacing.Y, StartFacing.X)));
                const bool bTrampled = FVector2D::Distance(P, Start) < 800 && CampSide < 180;
                const float Patch = FMath::PerlinNoise2D(P / 340 + FVector2D(21, 8));
                // The wet edge alternates low sedge and small bare, muddy pockets.
                if (L < 1.8f && Random.FRand() > FMath::Clamp(0.62f + Patch * 0.6f, 0.15f, 0.9f)) continue;
                if (Trees > 0.5f && Random.FRand() > 0.12f) continue;     // thin under the trees
                const float Path = FMath::Max(PierApproach(P), LakeMap::PathAmount(P));
                if (Path > 0.65f || LakeMap::BeachAmount(P) > 0.5f || LakeMap::StreamDistance(P) < 150) continue;
                if (SegmentDistance2D(P, Pier.Root - Pier.Dir * 50, Pier.Root + Pier.Dir * 400) < 140) continue;
                if (Slope(P.X, P.Y) > 0.6f) continue;                      // bare clay faces
                const float Spot = LakeMap::SpotAmount(P);
                UHierarchicalInstancedStaticMeshComponent* Kind;
                // Tall bank grass in long stretches (not at the spots or under the bluffs).
                const bool bBank = L > 1.2f && L < 14.0f + 3.0f * FMath::PerlinNoise2D(P / 1500) && !bTrampled && Path < 0.05f && Spot < 0.05f
                    && FMath::PerlinNoise2D(P / 4500 + FVector2D(7, 3)) > -0.25f && LakeMap::BluffHeight(P) < 0.8f;
                // Bank grass: fewer, larger tufts (cheaper to draw, just as dense to the eye).
                if (bBank && Random.FRand() < 0.4f) continue;
                if (bBank) Kind = FMath::PerlinNoise2D(P / 700 + FVector2D(31, 7)) > 0.3f && Random.FRand() < 0.6f ? Meadowsweet : Waist;
                else if (bTrampled || Path > 0.1f || (Spot > 0.3f && (FVector2D::Distance(P, Start) > 1200 || CampSide < 210))) { if (Random.FRand() < 0.35f) continue; Kind = Short; }
                else if (L < 2.5f) Kind = Patch > -0.05f ? Sedge : Short;
                else if (FMath::PerlinNoise2D(P / 900) > 0.3f && Random.FRand() < 0.5f) Kind = Blooms;
                else Kind = Random.FRand() < 0.5f ? Short : Tall;
                const float Scale = Random.FRandRange(0.65f, 1.25f) * (bTrampled || Path > 0.1f ? 0.55f : L < 1.5f ? 0.6f : bBank ? 1.1f : 1.0f);
                Add(Kind, FVector(P, Ground(P) - 2), FVector(Scale, Scale, Scale * Random.FRandRange(0.85f, 1.2f)), FRotator(0, Random.FRandRange(0, 360), 0));
                ++GrassCount;
            }
        // Sedge tussocks standing in the shallows at "Ольхи".
        FRandomStream TussockRandom(20261105);
        for (int32 I = 0, Tussocks = 0; I < 600 && Tussocks < 18; ++I)
        {
            const FVector2D P(TussockRandom.FRandRange(-29200, -24200), TussockRandom.FRandRange(4200, 8200));
            if (!LakeMap::InSedgeTussocks(P) || !LakeMap::LilyAllowed(P) || LakeMap::SpotAmount(P) > 0.1f) continue;
            const float Scale = TussockRandom.FRandRange(1.3f, 1.9f);
            Add(Sedge, FVector(P, Ground(P) + 20), FVector(Scale, Scale, Scale * 1.2f), FRotator(0, TussockRandom.FRandRange(0, 360), 0));
            ++Tussocks;
            ++GrassCount;
        }
        // Weed beds under water by the hump and off "Отмель": tall blades swaying on the bed.
        for (int32 I = 0, Weeds = 0; I < 6000 && Weeds < 500; ++I)
        {
            const FVector2D P(TussockRandom.FRandRange(4500, 14500), TussockRandom.FRandRange(-2500, 1500));
            const float Amount = LakeMap::WeedAmount(P);
            if (Amount < 0.4f || TussockRandom.FRand() > Amount) continue;
            const float Bed = Ground(P);
            const float Scale = FMath::Min(TussockRandom.FRandRange(0.9f, 1.6f), (LakeMap::SurfaceZ - 40 - Bed) / 90);
            if (Scale < 0.5f) continue;
            Add(Tall, FVector(P, Bed - 2), FVector(Scale, Scale, Scale * 1.3f), FRotator(TussockRandom.FRandRange(-8, 8), TussockRandom.FRandRange(0, 360), 0));
            ++Weeds;
            ++GrassCount;
        }
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BUILD reeds %d, lilies %d, grass %d, %.1f s"), ReedCount, LilyCount, GrassCount, FPlatformTime::Seconds() - BuildStart);

    // --- the pier: planks on stringers over pairs of piles, a wider landing stage at the end --------
    {
        const FVector2D Side(-Pier.Dir.Y, Pier.Dir.X);
        const float Yaw = YawOf(Pier.Dir);
        const float DeckZ = LakeMap::SurfaceZ + 50;
        for (float A = 0; A < Pier.Length; A += 17)
        {
            const bool bHead = A > Pier.Length - 300;
            if (!bHead && A > 300 && Random.FRand() < 0.02f) continue;     // a missing plank here and there
            const float Width = (bHead ? 460 : Pier.Width) + Random.FRandRange(-6, 6);
            const FVector2D C = Pier.Root + Pier.Dir * A + Side * Random.FRandRange(-4, 4);
            Add(Wood, FVector(C, DeckZ + Random.FRandRange(-1.5f, 1.0f)), FVector(0.15f, Width / 100, 0.045f),
                FRotator(Random.FRandRange(-0.8f, 0.8f), Yaw + Random.FRandRange(-1.5f, 1.5f), Random.FRandRange(-1, 1)));
        }
        for (float S : { -1.0f, 1.0f })
            Add(Wood, FVector(Pier.Root + Pier.Dir * Pier.Length * 0.5f + Side * S * (Pier.Width * 0.5f - 15), DeckZ - 9), FVector(Pier.Length / 100, 0.12f, 0.14f), FRotator(0, Yaw, 0));
        for (float S : { -1.0f, 1.0f })
            Add(Wood, FVector(Pier.Root + Pier.Dir * (Pier.Length - 150) + Side * S * 215, DeckZ - 9), FVector(3.0f, 0.12f, 0.14f), FRotator(0, Yaw, 0));
        for (float A = 120; A < Pier.Length; A += 220)
        {
            const bool bHead = A > Pier.Length - 320;
            for (float S : { -1.0f, 1.0f })
            {
                const FVector2D P = Pier.Root + Pier.Dir * A + Side * S * (bHead ? 215 : Pier.Width * 0.5f - 10);
                const float Bottom = Ground(P) - 40;
                const float Top = DeckZ + (Random.FRand() < 0.2f ? 60 : 4);  // some piles stand up as mooring posts
                Add(Bark, FVector(P, (Bottom + Top) * 0.5f), FVector(0.2f, 0.2f, (Top - Bottom) / 100),
                    FRotator(Random.FRandRange(-2, 2), 0, Random.FRandRange(-2, 2)));
            }
        }
    }

    {
        const FVector2D Side(-Pier.Dir.Y, Pier.Dir.X);
        for (float A : { 380.0f, 1040.0f })
            for (float Sign : { -1.0f, 1.0f })
            {
                const FVector2D Q = Pier.Root + Pier.Dir * A + Side * Sign * (Pier.Width * 0.5f - 20);
                const FVector Low(Q, LakeMap::SurfaceZ - 12);
                const FVector High(Q + Pier.Dir * 170, LakeMap::SurfaceZ + 37);
                Add(Wood, (Low + High) * 0.5f, FVector(0.07f, 0.07f, FVector::Dist(Low, High) / 100), FRotationMatrix::MakeFromZ(High - Low).Rotator());
            }
    }

    // --- snags: fallen trees lying from the bank into the water, branches sticking up ---------------
    auto Log = [&](const FVector& Root, const FVector& Tip, float Radius, int32 Branches)
    {
        const FVector Dir = Tip - Root;
        Add(Bark, (Root + Tip) * 0.5f, FVector(Radius / 50, Radius / 50, Dir.Size() / 100), FRotationMatrix::MakeFromZ(Dir).Rotator());
        for (int32 B = 0; B < Branches; ++B)
        {
            const float T = Random.FRandRange(0.2f, 0.95f);
            const FVector Base = Root + Dir * T;
            const FVector Out = (FVector::UpVector * Random.FRandRange(0.4f, 1.0f) + FVector(Random.FRandRange(-1, 1), Random.FRandRange(-1, 1), 0) * 0.7f).GetSafeNormal();
            const float Len = Random.FRandRange(80, 230) * (1 - T * 0.5f), Rad = Radius * Random.FRandRange(0.22f, 0.38f);
            Add(Bark, Base + Out * Len * 0.5f, FVector(Rad / 50, Rad / 50, Len / 100), FRotationMatrix::MakeFromZ(Out).Rotator());
        }
    };
    for (const LakeMap::FSnag& Snag : LakeMap::Snags()) Log(Snag.Root, Snag.Tip, Snag.Radius, 6);
    // Windfalls in the woods: trunks lying on the forest floor.
    for (int32 Try = 0, Logs = 0; Try < 4000 && Logs < 90; ++Try)
    {
        const FVector2D P(Random.FRandRange(LakeBox.Min.X - 12000, LakeBox.Max.X + 12000), Random.FRandRange(LakeBox.Min.Y - 12000, LakeBox.Max.Y + 12000));
        const float L = -LakeMap::ShoreDistance(P.X, P.Y) / 100;
        if (L < 8 || L > 120 || Woodland(P, L) < 0.8f || Slope(P.X, P.Y) > 0.5f || LakeMap::SpotAmount(P) > 0) continue;
        const FVector2D Dir = FVector2D(1, 0).GetRotated(Random.FRandRange(0, 360));
        const float Length = Random.FRandRange(500, 1100);
        const FVector2D Q = P + Dir * Length;
        Log(FVector(P, Ground(P) + 15), FVector(Q, Ground(Q) + 12), Random.FRandRange(12, 22), Random.RandRange(1, 4));
        ++Logs;
    }

    // --- the landing: boats and nets drying; the village itself comes later on the plateau above ---
    {
        auto Prop = [this](const TCHAR* Name, FVector Location, FRotator Rotation, bool bCollision)
        {
            auto* C = NewObject<UStaticMeshComponent>(this);
            C->SetupAttachment(RootComponent);
            C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Village/SM_%s.SM_%s"), Name, Name)));
            C->SetCollisionEnabled(bCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
            C->SetCollisionResponseToAllChannels(ECR_Block);
            C->RegisterComponent();
            AddInstanceComponent(C);
            C->SetWorldLocationAndRotation(Location, Rotation);
            return C;
        };
        const FVector2D Along(-Landing.Outward.Y, Landing.Outward.X);
        // A boat pulled up on the landing, bow up the bank, lying along the slope.
        {
            const FVector2D Dir = Landing.Outward;
            const FVector2D P = Landing.Location + Dir * 170;
            const float Pitch = FMath::RadiansToDegrees(FMath::Atan2(Ground(P + Dir * 190) - Ground(P - Dir * 190), 380));
            Prop(TEXT("Boat_B"), FVector(P, Ground(P) - 4), FRotator(Pitch, YawOf(Dir) + 6, 4), true);
        }
        // An old boat half sunk beside the pier; the fish use it as cover (LakeMap::ZonesAt).
        {
            float Yaw = 0;
            const FVector2D P = LakeMap::OldBoat(&Yaw);
            if (LakeMap::IsWater(P.X, P.Y)) Prop(TEXT("Boat_A"), FVector(P, LakeMap::SurfaceZ - 34), FRotator(-3, Yaw, 8), false);
        }
        // One moored at the pier's landing stage.
        {
            const FVector2D Side(-Pier.Dir.Y, Pier.Dir.X);
            const FVector2D P = Pier.Root + Pier.Dir * (Pier.Length - 170) + Side * 330;
            Prop(TEXT("Boat_A"), FVector(P, LakeMap::SurfaceZ - 16), FRotator(0, YawOf(Pier.Dir) + 172, 1.5f), true);
            auto* Mooring = Group(TEXT("Cylinder"), TEXT("M_OldBark"), false);
            auto Rope = [&](FVector A, FVector B)
            {
                FVector Previous = A;
                for (int32 K = 1; K <= 10; ++K)
                {
                    const float T = K / 10.0f;
                    const FVector Next = FMath::Lerp(A, B, T) - FVector(0, 0, 10 * 4 * T * (1 - T));
                    Add(Mooring, (Previous + Next) * 0.5f, FVector(0.012f, 0.012f, FVector::Dist(Previous, Next) / 100),
                        FRotationMatrix::MakeFromZ(Next - Previous).Rotator());
                    Previous = Next;
                }
            };
            for (float End : { -1.0f, 1.0f })
            {
                const FVector2D Post = Pier.Root + Pier.Dir * (Pier.Length - 170 + End * 120) + Side * 215;
                Add(Bark, FVector(Post, LakeMap::SurfaceZ + 45), FVector(0.14f, 0.14f, 1.4f));
                const FVector2D Tie = P + Pier.Dir * End * 160 - Side * 25;
                Rope(FVector(Post, LakeMap::SurfaceZ + 105), FVector(Tie, LakeMap::SurfaceZ + 30));
            }
        }
        // Nets hung to dry above the landing.
        for (float S : { -1.0f, 1.0f })
        {
            const FVector2D P = Landing.Location + Landing.Outward * 900 + Along * S * 450;
            Prop(TEXT("NetRack"), FVector(P, Ground(P)), FRotator(0, YawOf(Along) + Random.FRandRange(-8, 8), 0), false);
        }
    }

    // --- the west end: where an old weir once held the water. A ragged row of its rotten piles still
    // stands in the shallows; boulders along the waterline, a tree fallen into the little bay and an
    // old boat left upside down on the meadow bank. ---------------------------------------------------
    {
        const LakeMap::FShorePoint Tip = LakeMap::NearestShore(LakeMap::WestEnd());
        const FVector2D Along(-Tip.Outward.Y, Tip.Outward.X);
        int32 Piles = 0, Boulders = 0;
        bool bFallen = false, bBoat = false;
        for (int32 K = -6; K <= 6; ++K)
        {
            if (Random.FRand() < 0.3f) continue;
            const FVector2D P = Tip.Location - Tip.Outward * (800 + Random.FRandRange(-80, 80)) + Along * (K * 220 + Random.FRandRange(-40, 40));
            if (!LakeMap::IsWater(P.X, P.Y) || LakeMap::SpotAmount(P) > 0) continue;
            // Most stick up a little; some are broken off at the waterline.
            const float Bed = Ground(P) - 40, Top = LakeMap::SurfaceZ + (Random.FRand() < 0.3f ? Random.FRandRange(-6, 4) : Random.FRandRange(15, 75));
            const float Radius = Random.FRandRange(9, 14);
            Add(Bark, FVector(P, (Bed + Top) * 0.5f), FVector(Radius / 50, Radius / 50, (Top - Bed) / 100), FRotator(Random.FRandRange(-9, 9), 0, Random.FRandRange(-9, 9)));
            ++Piles;
        }
        // Mossy boulders along the rounded end, some half in the water.
        for (const LakeMap::FShorePoint& S : Shore)
        {
            if (FVector2D::Distance(S.Location, Tip.Location) > 3500 || LakeMap::SpotAmount(S.Location) > 0.2f || Random.FRand() > 0.06f) continue;
            const FVector2D P = S.Location + S.Outward * Random.FRandRange(-150, 120);
            const float Size = Random.FRandRange(0.4f, 1.1f);
            Add(Rocks, FVector(P, Ground(P) - 6), FVector(Size, Size, Size * Random.FRandRange(0.7f, 1.3f)), FRotator(Random.FRandRange(-10, 10), Random.FRandRange(0, 360), Random.FRandRange(-10, 10)));
            ++Boulders;
        }
        // A tree fallen from the back of the bay into the water.
        {
            const LakeMap::FShorePoint Back = LakeMap::NearestShore(LakeMap::WestBay());
            const FVector2D Root2 = Back.Location + Back.Outward * 150;
            const FVector2D Tip2 = Root2 + (-Back.Outward).GetRotated(18) * 1300;
            bFallen = LakeMap::IsWater(Tip2.X, Tip2.Y) && LakeMap::SpotAmount(Tip2) == 0;
            if (bFallen) Log(FVector(Root2, Ground(Root2) + 22), FVector(Tip2, FMath::Min(Ground(Tip2) + 25, LakeMap::SurfaceZ - 25)), 24, 7);
        }
        // An old boat upside down on the meadow bank south of the end.
        {
            const LakeMap::FShorePoint Bank = LakeMap::NearestShore(Tip.Location - Along * 3200);
            const FVector2D P = Bank.Location + Bank.Outward * 500;
            if (LakeMap::SpotAmount(P) == 0)
            {
                auto* Boat = NewObject<UStaticMeshComponent>(this);
                Boat->SetupAttachment(RootComponent);
                Boat->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Village/SM_Boat_A.SM_Boat_A")));
                Boat->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
                Boat->SetCollisionResponseToAllChannels(ECR_Block);
                Boat->RegisterComponent();
                AddInstanceComponent(Boat);
                // Turned over, it rests on its gunwales: lift it by the hull's height.
                const float Hull = Boat->GetStaticMesh() ? Boat->GetStaticMesh()->GetBoundingBox().Max.Z : 40.0f;
                const FVector2D Dir(-Bank.Outward.Y, Bank.Outward.X);
                Boat->SetWorldLocationAndRotation(FVector(P, Ground(P) + Hull - 6), FRotator(2, YawOf(Dir) + 12, 178));
                bBoat = true;
            }
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_WEST piles %d, boulders %d, fallen tree %d, boat %d"), Piles, Boulders, bFallen, bBoat);
    }

    // --- streams: a ribbon of water down each channel; stones on the outflow's riffle ----------------
    int32 StreamPoints = 0, RiffleStones = 0;
    for (const LakeMap::FStream& Stream : LakeMap::Streams())
    {
        TArray<FVector> Verts, Normals;
        TArray<FVector2D> UVs;
        TArray<int32> Triangles;
        TArray<FLinearColor> Colors;
        TArray<FProcMeshTangent> Tangents;
        float Along = 0;
        for (int32 I = 0; I < Stream.Surface.Num(); ++I)
        {
            const FVector Here = Stream.Surface[I];
            const FVector2D Dir = (FVector2D(Stream.Surface[FMath::Min(I + 1, Stream.Surface.Num() - 1)]) - FVector2D(Stream.Surface[FMath::Max(I - 1, 0)])).GetSafeNormal();
            const FVector2D Across(-Dir.Y, Dir.X);
            const float Half = Stream.Width * 0.5f * (0.85f + 0.3f * FMath::PerlinNoise1D(I * 0.37f));
            if (I > 0) Along += FVector::Dist(Here, Stream.Surface[I - 1]);
            for (float Side : { -1.0f, 1.0f })
            {
                Verts.Add(Here + FVector(Across * Side * Half, 0));
                Normals.Add(FVector::UpVector);
                UVs.Add(FVector2D(Along, Side * Half) / 250);
                Colors.Add(FLinearColor::White);
                Tangents.Add(FProcMeshTangent(FVector(Dir, 0), false));
            }
            if (I > 0) { const int32 A = (I - 1) * 2; Triangles.Append({ A, A + 2, A + 1, A + 1, A + 2, A + 3 }); }
            // The outflow leaves over a stony riffle: stones across its first dozen metres.
            if (Stream.bOutflow && Along < 1400 && I % 2 == 0)
                for (int32 K = 0; K < 3; ++K)
                {
                    const FVector2D P = FVector2D(Here) + Across * Random.FRandRange(-Half * 1.3f, Half * 1.3f) + Dir * Random.FRandRange(-50, 50);
                    const float Size = Random.FRandRange(0.12f, 0.32f);
                    Add(Rocks, FVector(P, FMath::Max(Ground(P), Here.Z - 25) - 4), FVector(Size, Size * 0.85f, Size * 0.6f), FRotator(Random.FRandRange(-8, 8), Random.FRandRange(0, 360), 0));
                    ++RiffleStones;
                }
        }
        auto* StreamWater = NewObject<UProceduralMeshComponent>(this);
        StreamWater->SetupAttachment(RootComponent);
        StreamWater->RegisterComponent();
        AddInstanceComponent(StreamWater);
        StreamWater->CreateMeshSection_LinearColor(0, Verts, Triangles, Normals, UVs, Colors, Tangents, false);
        StreamWater->SetMaterial(0, Material(TEXT("M_LakeSurface")));
        StreamWater->SetCastShadow(false);
        StreamWater->bAffectDistanceFieldLighting = false;
        StreamWater->bAffectDynamicIndirectLighting = false;
        StreamPoints += Stream.Surface.Num();
    }
    // Footbridges where the path crosses the streams: two log stringers and a deck of planks.
    for (const LakeMap::FFootbridge& Bridge : LakeMap::Footbridges())
    {
        const FVector2D Dir = (Bridge.B - Bridge.A).GetSafeNormal(), Side(-Dir.Y, Dir.X);
        const float Length = FVector2D::Distance(Bridge.A, Bridge.B);
        const float Z = FMath::Max(Ground(Bridge.A), Ground(Bridge.B)) + 12;
        for (float S : { -35.0f, 35.0f })
        {
            const FVector A(Bridge.A + Side * S - Dir * 60, Z), B(Bridge.B + Side * S + Dir * 60, Z);
            Add(Bark, (A + B) * 0.5f, FVector(0.16f, 0.16f, FVector::Dist(A, B) / 100), FRotationMatrix::MakeFromZ(B - A).Rotator());
        }
        for (float T = 0; T <= Length; T += 26)
        {
            const FVector2D P = Bridge.A + Dir * T;
            Add(Wood, FVector(P, Z + 10), FVector(0.22f, 1.05f, 0.04f), FRotator(Random.FRandRange(-1.5f, 1.5f), YawOf(Dir) + Random.FRandRange(-3, 3), 0));
        }
    }
    // The log frame over the spring: three courses of logs round a square of clear water.
    {
        const LakeMap::FBankThing Spring = LakeMap::Spring();
        const FVector2D F = Spring.Facing, R(-F.Y, F.X);
        const float Base = Ground(Spring.Location);
        for (int32 Course = 0; Course < 3; ++Course)
            for (int32 Wall = 0; Wall < 4; ++Wall)
            {
                const FVector2D Normal = Wall == 0 ? F : Wall == 1 ? -F : Wall == 2 ? R : -R;
                const FVector2D Run(-Normal.Y, Normal.X);
                const FVector2D C = Spring.Location + Normal * 60;
                const FVector A(C - Run * 80, Base + 8 + Course * 17 + (Wall >= 2 ? 8 : 0)), B(C + Run * 80, A.Z);
                Add(Bark, (A + B) * 0.5f, FVector(0.16f, 0.16f, 1.6f), FRotationMatrix::MakeFromZ(B - A).Rotator());
            }
    }
    // A row of old stakes in the south bay, sticking out of the water at different heights.
    for (const FVector2D& Stake : LakeMap::Stakes())
    {
        const float Bed = Ground(Stake) - 30, Top = LakeMap::SurfaceZ + Random.FRandRange(10, 70);
        Add(Bark, FVector(Stake, (Bed + Top) * 0.5f), FVector(0.1f, 0.1f, (Top - Bed) / 100), FRotator(Random.FRandRange(-10, 10), 0, Random.FRandRange(-10, 10)));
    }
    // The lookout by the old pine: a plank bench on two stumps, facing the sunset over the outflow.
    {
        const LakeMap::FBankThing Seat = LakeMap::Lookout();
        const FVector2D Side(-Seat.Facing.Y, Seat.Facing.X);
        const float Yaw = YawOf(Seat.Facing);
        for (float S : { -70.0f, 70.0f })
        {
            const FVector2D P = Seat.Location + Side * S;
            Add(Bark, FVector(P, Ground(P) + 20), FVector(0.3f, 0.3f, 0.4f), FRotator::ZeroRotator);
        }
        Add(Wood, FVector(Seat.Location, Ground(Seat.Location) + 44), FVector(0.4f, 1.9f, 0.06f), FRotator(0, Yaw, 0));
    }
    // Sand martins' burrows: rows of small dark holes high on the clay faces.
    for (const LakeMap::FBankThing& Colony : LakeMap::SwallowBanks())
    {
        const FVector2D Along(-Colony.Facing.Y, Colony.Facing.X);
        for (int32 K = 0; K < 28; ++K)
        {
            const FVector2D P = Colony.Location + Along * Random.FRandRange(-600, 600) - Colony.Facing * Random.FRandRange(-40, 60);
            const float Z = Ground(P) + Random.FRandRange(-30, 30);
            if (Z < LakeMap::SurfaceZ + 120) continue;
            Add(Burrows, FVector(P + Colony.Facing * 6, Z), FVector(0.09f, 0.09f, 0.07f), FRotator(0, YawOf(Colony.Facing), 0));
        }
    }
    // Cliffs: boulders at the foot and roots sticking out of the clay near the top.
    int32 CliffRoots = 0;
    for (int32 I = 0; I < Shore.Num(); I += 2)
    {
        const LakeMap::FShorePoint& S = Shore[I];
        if (!LakeMap::CliffAt(S.Location) || LakeMap::SpotAmount(S.Location) > 0.05f) continue;
        if (Random.FRand() < 0.35f)
        {
            const FVector2D P = S.Location + S.Outward * Random.FRandRange(-120, 60);
            const float Size = Random.FRandRange(0.3f, 0.8f);
            Add(Rocks, FVector(P, Ground(P) - 8), FVector(Size, Size, Size * Random.FRandRange(0.7f, 1.2f)), FRotator(Random.FRandRange(-10, 10), Random.FRandRange(0, 360), Random.FRandRange(-10, 10)));
        }
        if (Roots && Random.FRand() < 0.3f)
        {
            // Find the top of the face: where the ground stops rising steeply.
            FVector2D Edge = S.Location;
            for (float Up = 100; Up < 450; Up += 50)
            {
                const FVector2D Q = S.Location + S.Outward * Up;
                if (Ground(Q) - Ground(Edge) < 20) break;
                Edge = Q;
            }
            const FVector Out(-S.Outward, 0);
            const FRotator Rot = (Out * 0.8f + FVector(0, 0, -0.6f)).Rotation() + FRotator(0, Random.FRandRange(-30, 30), Random.FRandRange(0, 360));
            const float Size = Random.FRandRange(0.9f, 1.6f);
            Add(Roots, FVector(Edge, Ground(Edge) - 25), FVector(Size), Rot);
            ++CliffRoots;
        }
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_DETAILS streams %d (%d points), riffle stones %d, footbridges %d, stakes %d, cliff roots %d"),
        LakeMap::Streams().Num(), StreamPoints, RiffleStones, LakeMap::Footbridges().Num(), LakeMap::Stakes().Num(), CliffRoots);

    // --- fishing spots: boards at the water's edge and two rod stakes ------------------------------
    for (const LakeMap::FSpot& Spot : LakeMap::Spots())
    {
        if (!Spot.bBoards) continue;
        // Timber belongs at the used, soft-bank landings; wild banks stay natural.
        if (FCString::Strcmp(Spot.Name, TEXT("Лагерь")) != 0 && FCString::Strcmp(Spot.Name, TEXT("Заводь")) != 0
            && FCString::Strcmp(Spot.Name, TEXT("Коряжник")) != 0 && FCString::Strcmp(Spot.Name, TEXT("Верховье")) != 0
            && FCString::Strcmp(Spot.Name, TEXT("Отмель")) != 0 && FCString::Strcmp(Spot.Name, TEXT("Луговая бровка")) != 0) continue;
        const FVector2D Along(-Spot.Outward.Y, Spot.Outward.X);
        const float Yaw = YawOf(Spot.Outward);
        for (int32 K = 0; K < 3; ++K)
        {
            const FVector2D P = Spot.Location + Spot.Outward * (60 + K * 22);
            Add(Wood, FVector(P, Ground(P) + 3), FVector(0.2f, 1.5f, 0.04f), FRotator(Random.FRandRange(-2, 2), Yaw + Random.FRandRange(-4, 4), 0));
        }
        for (float Sgn : { -1.0f, 1.0f })
        {
            const FVector2D P = Spot.Location - Spot.Outward * 30 + Along * Sgn * 70;
            Add(Bark, FVector(P, Ground(P) + 10), FVector(0.07f, 0.07f, 0.9f), FRotator(Random.FRandRange(-5, 5), 0, Random.FRandRange(-5, 5)));
        }
        // A log to sit on a little way up the bank.
        const FVector2D Seat = Spot.Location + Spot.Outward * 380 + Along * Random.FRandRange(-150, 150);
        Add(Bark, FVector(Seat, Ground(Seat) + 18), FVector(0.36f, 0.36f, 1.7f), FRotator(90, Yaw + 90 + Random.FRandRange(-12, 12), 0));
    }

    // --- the angler's camp at the start spot: bench, tackle table, stool, bucket, logs and a sign ----
    {
        const FVector2D Right(-StartFacing.Y, StartFacing.X);
        const float Yaw = YawOf(StartFacing);
        auto At = [&](float Forward, float Side, float Up) { const FVector2D P = Start + StartFacing * Forward + Right * Side; return FVector(P, Ground(P) + Up); };
        const FRotator Face(0, Yaw, 0);
        for (float S : { 480.0f, 620.0f }) Add(Wood, At(-300, S, 30), FVector(0.12f, 0.12f, 0.6f), Face);
        Add(Wood, At(-300, 550, 62), FVector(0.55f, 2, 0.12f), Face);
        Add(Wood, At(-335, 550, 98), FVector(0.10f, 2, 0.6f), Face);
        Add(Wood, At(-520, -580, 78), FVector(1.3f, 1, 0.12f), Face);
        for (float F : { -570.0f, -470.0f }) for (float S : { -620.0f, -540.0f }) Add(Wood, At(F, S, 38), FVector(0.08f, 0.08f, 0.75f), Face);
        Add(Metal, At(-440, -340, 18), FVector(0.28f, 0.28f, 0.38f), Face);
        Add(Wood, At(-60, -430, 25), FVector(0.45f, 0.45f, 0.5f), Face);
        for (int32 I = 0; I < 5; ++I) Add(Bark, At(-810 + (I % 3) * 26, 750, 15 + (I / 3) * 24), FVector(0.25f, 0.25f, 1.6f), FRotator(90, Yaw, 0));
        Add(Wood, At(-800, -900, 80), FVector(0.12f, 0.12f, 1.6f), Face);
        Add(Wood, At(-800, -900, 175), FVector(0.10f, 1.9f, 0.65f), Face);
        auto* Sign = NewObject<UTextRenderComponent>(this);
        Sign->SetupAttachment(RootComponent);
        Sign->SetText(FText::FromString(TEXT("ТИХАЯ ЗАВОДЬ")));
        Sign->SetHorizontalAlignment(EHTA_Center);
        Sign->SetWorldSize(18);
        Sign->SetTextRenderColor(FColor(235, 224, 180));
        Sign->RegisterComponent();
        AddInstanceComponent(Sign);
        Sign->SetWorldLocationAndRotation(At(-793, -900, 168), Face);
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_MAP lake=%.0fx%.0f m lilies=%d lily beds=%d snags=%d spots=%d trees=%d, built in %.1f s"),
        LakeBox.GetSize().X / 100, LakeBox.GetSize().Y / 100, LilyCount, LakeMap::LilyBeds().Num(), LakeMap::Snags().Num(), LakeMap::Spots().Num(), TreeCount,
        FPlatformTime::Seconds() - BuildStart);

    // Depth survey: what a 6 m pole reaches. At every spot straight out from the waterline, then
    // along all banks: the share of the bank with 0.8-2 m of water at 2, 4 and 6 m out.
    {
        auto Out = [](const FVector2D& Location, const FVector2D& Outward, float Metres)
        {
            const FVector2D P = Location - Outward * Metres * 100;
            return LakeMap::Depth(P.X, P.Y);
        };
        for (const LakeMap::FSpot& Spot : LakeMap::Spots())
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_DEPTH spot %s: 1 m %.0f, 2 m %.0f, 3 m %.0f, 4 m %.0f, 6 m %.0f cm"), Spot.Name,
                Out(Spot.Location, Spot.Outward, 1), Out(Spot.Location, Spot.Outward, 2), Out(Spot.Location, Spot.Outward, 3),
                Out(Spot.Location, Spot.Outward, 4), Out(Spot.Location, Spot.Outward, 6));
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_DEPTH pier end: under it %.0f, 2 m beyond %.0f, 4 m %.0f, 6 m %.0f cm"),
            Out(PierEnd, -Pier.Dir, 0), Out(PierEnd, -Pier.Dir, 2), Out(PierEnd, -Pier.Dir, 4), Out(PierEnd, -Pier.Dir, 6));
        const float Marks[] = { 1, 2, 4, 6 };
        for (int32 Kind = 0; Kind < 2; ++Kind)
        {
            // 0: the main banks, 1: the weedy shallows (backwater, bay, upper end, the crucian shallow).
            int32 Count = 0, Good[4] = {}, AllGood = 0;
            TArray<float> At[4];
            for (const LakeMap::FShorePoint& S : LakeMap::Shoreline())
            {
                const float Back = LakeMap::BackwaterAmount(S.Location.X, S.Location.Y);
                const int32 Own = Back > 0.5f ? 1 : 0;
                if (Own != Kind || (Kind == 0 && Back > 0.2f)) continue;
                ++Count;
                bool bAll = true;
                for (int32 M = 0; M < 4; ++M)
                {
                    const float D = Out(S.Location, S.Outward, Marks[M]);
                    At[M].Add(D);
                    const bool bGood = D >= 80 && D <= 200;
                    Good[M] += bGood;
                    if (M > 0) bAll &= bGood;
                }
                AllGood += bAll;
            }
            if (!Count) continue;
            FString Line;
            for (int32 M = 0; M < 4; ++M)
            {
                At[M].Sort();
                Line += FString::Printf(TEXT(", %.0f m: %.0f/%.0f/%.0f cm %d%%"), Marks[M], At[M][Count / 10], At[M][Count / 2], At[M][Count * 9 / 10], Good[M] * 100 / Count);
            }
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_DEPTH banks %s: %d points (10th/50th/90th percentile, share 0.8-2 m)%s; 0.8-2 m at all of 2-6 m: %d%%"),
                Kind == 0 ? TEXT("main") : TEXT("shallows"), Count, *Line, AllGood * 100 / Count);
        }
    }

    if (GetNetMode() != NM_DedicatedServer)
    {
        // Early morning. The sun is low and ahead of the starting view, just above the
        // far tree line, so the mist and foliage are backlit and light shafts form.
        auto* Sun = GetWorld()->SpawnActor<ADirectionalLight>(FVector(0,0,3000), FRotator(-6,205,0));
        Sun->SetMobility(EComponentMobility::Movable);
        auto* SunComponent = Cast<UDirectionalLightComponent>(Sun->GetLightComponent());
        SunComponent->SetAtmosphereSunLight(true);
        // The sun is the forward shading light by day: the moon takes over at night (LakeWeather).
        SunComponent->SetForwardShadingPriority(1);
        SunComponent->SetIntensity(6.5f);
        SunComponent->SetUseTemperature(true);
        SunComponent->SetTemperature(5500.0f);
        SunComponent->SetLightSourceAngle(0.8f);
        SunComponent->SetEnableLightShaftBloom(true);
        SunComponent->SetBloomScale(0.04f);
        auto* Atmosphere = GetWorld()->SpawnActor<ASkyAtmosphere>();
        Atmosphere->GetComponent()->SetMieScatteringScale(0.008f); // humid morning haze
        // The sun is dimmed (6.5 instead of 10) so sunlit ground is not glaring; the sky, which the
        // atmosphere lights by the sun, is raised back so the shade and the sky stay as they were.
        Atmosphere->GetComponent()->SetSkyLuminanceFactor(FLinearColor(10.0f / 6.5f, 10.0f / 6.5f, 10.0f / 6.5f));
        GetWorld()->SpawnActor<AVolumetricCloud>();
        auto* Sky = GetWorld()->SpawnActor<ASkyLight>();
        Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        Sky->GetLightComponent()->SetIntensity(1.0f);
        Sky->GetLightComponent()->SetRealTimeCapture(true);

        // Two fog layers: haze over the whole valley plus a dense layer hugging the water.
        // Density is extinction per km, falloff halves density every 1000/falloff cm, so for a
        // ~50 m scene the values are far above the engine defaults.
        auto* Fog = GetWorld()->SpawnActor<AExponentialHeightFog>(FVector(0,0,-25), FRotator::ZeroRotator)->GetComponent();
        Fog->SetFogDensity(0.045f);
        Fog->SetFogHeightFalloff(0.5f);
        FExponentialHeightFogData WaterMist;
        WaterMist.FogDensity = 0.3f;
        WaterMist.FogHeightFalloff = 2.0f;   // the engine maximum: halves every 5 m
        WaterMist.FogHeightOffset = 0.0f;
        Fog->SetSecondFogData(WaterMist);
        Fog->SetVolumetricFog(true);
        Fog->SetVolumetricFogScatteringDistribution(0.25f); // a soft glow toward the sun, the mist itself stays grey-white
        // The haze beyond the volumetric fog: neutral grey with a little coolness, a pale glow toward the sun.
        Fog->SetFogInscatteringColor(FLinearColor(0.42f, 0.47f, 0.55f));
        Fog->SetDirectionalInscatteringColor(FLinearColor(0.2f, 0.19f, 0.16f));
        Fog->SetVolumetricFogAlbedo(FColor(250,252,255)); // milky, slightly cool
        Fog->SetVolumetricFogExtinctionScale(1.0f);
        Fog->SetVolumetricFogDistance(12000.0f);
        auto* Post = GetWorld()->SpawnActor<APostProcessVolume>();
        Post->bUnbound = true;
        auto& PP = Post->Settings;
        PP.bOverride_AutoExposureMethod = true;
        PP.AutoExposureMethod = AEM_Histogram;
        PP.bOverride_AutoExposureBias = true;
        PP.AutoExposureBias = 0.3f;
        PP.bOverride_AutoExposureSpeedUp = true;
        PP.AutoExposureSpeedUp = 2.0f;
        PP.bOverride_AutoExposureSpeedDown = true;
        PP.AutoExposureSpeedDown = 1.5f;
        PP.bOverride_BloomIntensity = true;
        PP.BloomIntensity = 0.35f;
        PP.bOverride_VignetteIntensity = true;
        PP.VignetteIntensity = 0.3f;
        PP.bOverride_MotionBlurAmount = true;
        PP.MotionBlurAmount = 0.0f;
        PP.bOverride_ColorSaturation = true;
        PP.ColorSaturation = FVector4(1.0f,1.0f,1.0f,0.92f);
        PP.bOverride_ColorContrast = true;
        PP.ColorContrast = FVector4(1.0f,1.0f,1.0f,1.05f);
        PP.bOverride_ColorGainHighlights = true;
        PP.ColorGainHighlights = FVector4(1.02f,1.0f,0.97f,1.0f); // gently warm sunlit highlights
        PP.bOverride_ColorGainShadows = true;
        PP.ColorGainShadows = FVector4(0.95f,1.0f,1.06f,1.0f);    // cool shade
        PP.bOverride_FilmGrainIntensity = true;
        PP.FilmGrainIntensity = 0.08f;
        PP.bOverride_LumenFinalGatherQuality = true;
        PP.LumenFinalGatherQuality = 1.0f;   // 2 doubled the indirect lighting cost among the bank grass
        PP.bOverride_LumenReflectionQuality = true;
        PP.LumenReflectionQuality = 2.0f;

        auto Audio = [this](const TCHAR* Name, FVector Location, float Volume, bool Spatial)
        {
            auto* Sound = LoadObject<USoundWave>(nullptr, *FString::Printf(TEXT("/Game/Audio/%s.%s"),Name,Name));
            if (!Sound) return;
            auto* Component = NewObject<UAudioComponent>(this);
            Component->SetupAttachment(RootComponent);
            Component->SetSound(Sound);
            Component->SetVolumeMultiplier(Volume);
            Component->bAutoActivate = false;
            if (Spatial)
            {
                FSoundAttenuationSettings Settings;
                Settings.bAttenuate = true;
                Settings.bSpatialize = true;
                Settings.AttenuationShapeExtents = FVector(800,0,0);
                Settings.FalloffDistance = 4500;
                Component->AdjustAttenuation(Settings);
            }
            Component->RegisterComponent();
            AddInstanceComponent(Component);
            Component->SetWorldLocation(Location);
            Component->Play();
        };
        Audio(TEXT("A_Wind"),FVector::ZeroVector,0.32f,false);
        // Quiet local sounds: the streams babbling, frogs in the warm shallows, a cuckoo far off.
        for (const LakeMap::FSoundPlace& Sound : LakeMap::SoundPlaces())
            Audio(Sound.Sound, FVector(Sound.Location, LakeMap::SurfaceZ + 60), Sound.Volume, true);
        // No bird ambience: it was the wrong sound for this lake and got on the player's nerves.
        // The flocks themselves (-LakeBirds) still fly over the water.
    }
#if !UE_BUILD_SHIPPING
    // Direct art review: one selected camera, one screenshot, no lake tour or fishing sequence.
    FString SceneView;
    if (FParse::Value(FCommandLine::Get(), TEXT("ScenePreview="), SceneView))
    {
        const FVector2D Side(-StartFacing.Y, StartFacing.X);
        FVector Eye(Start - StartFacing * 250, Ground(Start - StartFacing * 250) + 170);
        FRotator View(-6, YawOf(StartFacing) + 25, 0);
        if (SceneView.Equals(TEXT("Shore"), ESearchCase::IgnoreCase))
        {
            Eye = FVector(Start + StartFacing * 500 + Side * 500, LakeMap::SurfaceZ + 160);
            const FVector2D Bank = Start + Side * 450;
            View = (FVector(Bank, Ground(Bank) + 25) - Eye).Rotation();
        }
        else if (SceneView.Equals(TEXT("Pier"), ESearchCase::IgnoreCase))
        {
            Eye = FVector(Pier.Root + Pier.Dir * 600 + FVector2D(-Pier.Dir.Y, Pier.Dir.X) * 900, LakeMap::SurfaceZ + 240);
            View = (FVector(Pier.Root + Pier.Dir * 700, LakeMap::SurfaceZ + 70) - Eye).Rotation();
        }
        else if (SceneView.Equals(TEXT("PierWide"), ESearchCase::IgnoreCase))
        {
            const FVector2D PierSide(-Pier.Dir.Y, Pier.Dir.X);
            Eye = FVector(Pier.Root + Pier.Dir * 750 + PierSide * 1850, LakeMap::SurfaceZ + 360);
            View = (FVector(Pier.Root + Pier.Dir * 750, LakeMap::SurfaceZ + 50) - Eye).Rotation();
        }
        else if (SceneView.Equals(TEXT("PierBank"), ESearchCase::IgnoreCase))
        {
            const FVector2D PierSide(-Pier.Dir.Y, Pier.Dir.X);
            Eye = FVector(Pier.Root + Pier.Dir * 500 + PierSide * 950, LakeMap::SurfaceZ + 230);
            const FVector2D Target = Pier.Root - Pier.Dir * 420;
            View = (FVector(Target, Ground(Target) + 35) - Eye).Rotation();
        }
        else if (SceneView.Equals(TEXT("Forest"), ESearchCase::IgnoreCase))
        {
            const LakeMap::FShorePoint* Best = nullptr;
            for (const auto& Sh : LakeMap::Shoreline())
                if (LakeMap::ForestAmount(Sh.Location) > 0.9f && LakeMap::BluffHeight(Sh.Location) < 0.3f
                    && LakeMap::SpotAmount(Sh.Location) < 0.02f && FVector2D::Distance(Sh.Location, Start) > 3000
                    && (!Best || FVector2D::Distance(Sh.Location, Start) < FVector2D::Distance(Best->Location, Start))) Best = &Sh;
            if (Best)
            {
                Eye = FVector(Best->Location - Best->Outward * 1600, LakeMap::SurfaceZ + 260);
                View = (FVector(Best->Location + Best->Outward * 1000, LakeMap::SurfaceZ + 380) - Eye).Rotation();
            }
        }
        else if (SceneView.Equals(TEXT("Air"), ESearchCase::IgnoreCase))
        {
            // The whole lake from 350 m up, south of it: the bowl, the shores and the streams.
            const FBox2D Box = LakeMap::LakeBounds();
            Eye = FVector(Box.GetCenter() + FVector2D(0, Box.GetSize().Y * 0.9f), LakeMap::SurfaceZ + 35000);
            View = (FVector(Box.GetCenter(), LakeMap::SurfaceZ) - Eye).Rotation();
        }
        else if (SceneView.Equals(TEXT("Bowl"), ESearchCase::IgnoreCase))
        {
            // The lake level's landscape with its water hidden: the bowl the Water lake carved.
            for (TActorIterator<AWaterBody> It(GetWorld()); It; ++It) It->SetActorHiddenInGame(true);
            GEngine->Exec(GetWorld(), TEXT("r.Water.Enabled 0"));
            Eye = FVector(Start - StartFacing * 1500, Ground(Start - StartFacing * 1500) + 2500);
            View = (FVector(Start + StartFacing * 6000, LakeMap::SurfaceZ - 300) - Eye).Rotation();
        }
        else if (SceneView.Equals(TEXT("Birds"), ESearchCase::IgnoreCase))
        {
            Eye = FVector(Start + StartFacing * 5500 + Side * 3500, LakeMap::SurfaceZ + 2600);
            View = FRotator(0, YawOf(StartFacing), 0);
        }
        else if (SceneView.Equals(TEXT("Outflow"), ESearchCase::IgnoreCase) && LakeMap::Streams().Num() > 0)
        {
            // The outflow's riffle and its footbridge, from the bank beside the stream.
            for (const LakeMap::FStream& Stream : LakeMap::Streams())
                if (Stream.bOutflow && Stream.Surface.Num() > 25)
                {
                    const FVector2D A(Stream.Surface[3]), B(Stream.Surface[25]);
                    const FVector2D Dir = (B - A).GetSafeNormal();
                    const FVector2D P = A + Dir * 900 + FVector2D(-Dir.Y, Dir.X) * 900;
                    Eye = FVector(P, Ground(P) + 170);
                    View = (FVector(A + Dir * 1200, Stream.Surface[12].Z + 20) - Eye).Rotation();
                }
        }
        else if (SceneView.Equals(TEXT("Spring"), ESearchCase::IgnoreCase))
        {
            const LakeMap::FBankThing Spring = LakeMap::Spring();
            const FVector2D P = Spring.Location + Spring.Facing * 900 + FVector2D(-Spring.Facing.Y, Spring.Facing.X) * 500;
            Eye = FVector(P, FMath::Max(Ground(P), LakeMap::SurfaceZ) + 170);
            View = (FVector(Spring.Location, Ground(Spring.Location) + 40) - Eye).Rotation();
        }
        else if (SceneView.Equals(TEXT("Oak"), ESearchCase::IgnoreCase) || SceneView.Equals(TEXT("Willow"), ESearchCase::IgnoreCase))
        {
            // From the water, looking at the old oak above the pier or at the first willow.
            const LakeMap::FBankThing Tree = SceneView.Equals(TEXT("Oak"), ESearchCase::IgnoreCase) ? LakeMap::OldOak() : LakeMap::Willows()[0];
            const LakeMap::FShorePoint Bank = LakeMap::NearestShore(Tree.Location);
            Eye = FVector(Bank.Location - Bank.Outward * 2600, LakeMap::SurfaceZ + 180);
            View = (FVector(Tree.Location, Ground(Tree.Location) + 450) - Eye).Rotation();
        }
        else if (!SceneView.Equals(TEXT("Camp"), ESearchCase::IgnoreCase))
        {
            for (const auto& Spot : LakeMap::Spots())
                if (SceneView.Equals(Spot.Name, ESearchCase::IgnoreCase))
                {
                    const FVector2D P = Spot.Location + Spot.Outward * 220;
                    Eye = FVector(P, Ground(P) + 170);
                    View = FRotator(-8, YawOf(-Spot.Outward), 0);
                }
        }
        FTimerHandle CameraTimer, CaptureTimer, ExitTimer;
        GetWorldTimerManager().SetTimer(CameraTimer, [this, Eye, View, SceneView]() mutable
        {
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Clear;
                Clear.Cloud = 0.3f; Clear.Fog = 0; Clear.Rain = 0; Clear.Wind = 0.1f; Clear.WindYaw = 40;
                Weather->SetWeatherNow(Clear); Weather->SetHours(9.5f);
                if (SceneView.StartsWith(TEXT("Night")))
                {
                    Clear.Cloud = SceneView == TEXT("NightCloudy") ? 1.0f : 0.05f;
                    Weather->SetWeatherNow(Clear); Weather->SetHours(0.0f);
                    const FVector SunDir = Weather->SunDirection();
                    const FVector MoonDir(-SunDir.X, -SunDir.Y, FMath::Max(0.3f, -SunDir.Z * 0.8f + 0.2f));
                    View = MoonDir.Rotation();
                    View.Pitch -= 12;
                }
            }
            if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
            {
                PC->SetIgnoreLookInput(true); PC->SetIgnoreMoveInput(true);
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
                if (PC->GetPawn()) PC->GetPawn()->SetActorHiddenInGame(true);
                for (TActorIterator<ALakeRod> It(GetWorld()); It; ++It) It->SetActorHiddenInGame(true);
                auto* Cam = GetWorld()->SpawnActor<ACameraActor>(Eye, View);
                Cam->GetCameraComponent()->SetFieldOfView(60); PC->SetViewTarget(Cam);
            }
        }, 0.2f, false);
        const FString File = TEXT("Scene_") + FPaths::MakeValidFileName(SceneView) + TEXT(".png");
        GetWorldTimerManager().SetTimer(CaptureTimer, [File]
        {
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / File, false, false);
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_SCENE_PREVIEW %s frame=%.1f ms"), *File, FApp::GetDeltaTime() * 1000);
        }, 8.0f, false);
        if (!FParse::Param(FCommandLine::Get(), TEXT("PreviewStay")))
            GetWorldTimerManager().SetTimer(ExitTimer, [] { FPlatformMisc::RequestExit(false); }, 9.0f, false);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_SCENE_CAMERA %s eye=%s view=%s"), *SceneView, *Eye.ToCompactString(), *View.ToCompactString());
    }
    // Reproducible visual check; only enabled by an explicit command-line switch.
    if (FParse::Param(FCommandLine::Get(),TEXT("LakePreview")))
    {
        // Fixed viewpoints with input and HUD off, so screenshots are comparable between runs.
        // The angler stands at a spot (bCamera = false) or a free camera looks from the air.
        struct FShot { FVector Location; FRotator Rotation; const TCHAR* File; bool bCamera; };
        TArray<FShot> Shots;
        auto Stand = [](FVector2D P) { return FVector(P, LakeMap::Ground(P.X, P.Y) + 100); };
        auto SpotView = [&](const TCHAR* Name, float Turn, const TCHAR* File)
        {
            for (const LakeMap::FSpot& Spot : LakeMap::Spots())
                if (FCString::Strcmp(Spot.Name, Name) == 0)
                {
                    const FVector2D Eye = Spot.Location + Spot.Outward * 200;
                    Shots.Add({ Stand(Eye), FRotator(-6, YawOf(-Spot.Outward) + Turn, 0), File, false });
                }
        };
        {
            FVector2D Facing;
            const FVector2D StartP = LakeMap::StartSpot(&Facing);
            Shots.Add({ Stand(StartP - Facing * 250), FRotator(-6, YawOf(Facing) + 25, 0), TEXT("WorldPreview.png"), false });
            const LakeMap::FPier& Pr = LakeMap::Pier();
            Shots.Add({ FVector(Pr.Root + Pr.Dir * (Pr.Length - 60), LakeMap::SurfaceZ + 150), FRotator(-5, YawOf(Pr.Dir) - 40, 0), TEXT("WorldPreview_2.png"), false });
            // The high bluff seen from the water.
            const LakeMap::FShorePoint Bluff = LakeMap::NearestShore({ 2850, -9375 });
            const FVector BluffEye(Bluff.Location - Bluff.Outward * 4500, LakeMap::SurfaceZ + 300);
            Shots.Add({ BluffEye, (FVector(Bluff.Location + Bluff.Outward * 400, LakeMap::SurfaceZ + 400) - BluffEye).Rotation(), TEXT("WorldPreview_3.png"), true });
            SpotView(TEXT("Заводь"), -55, TEXT("WorldPreview_5.png"));
            // The rounded west end with its little bay, and from the water: the old weir's piles.
            SpotView(TEXT("Омут"), 30, TEXT("WorldPreview_West.png"));
            const LakeMap::FShorePoint WestTip = LakeMap::NearestShore(LakeMap::WestEnd());
            const FVector WestEye(WestTip.Location - WestTip.Outward * 3200 + FVector2D(0, 600), LakeMap::SurfaceZ + 260);
            Shots.Add({ WestEye, (FVector(WestTip.Location - WestTip.Outward * 600, LakeMap::SurfaceZ + 80) - WestEye).Rotation(), TEXT("WorldPreview_WestEnd.png"), true });
            const LakeMap::FShorePoint BayBack = LakeMap::NearestShore(LakeMap::WestBay());
            const FVector BayEye(BayBack.Location - BayBack.Outward * 2600, LakeMap::SurfaceZ + 220);
            Shots.Add({ BayEye, (FVector(BayBack.Location - BayBack.Outward * 500, LakeMap::SurfaceZ + 60) - BayEye).Rotation(), TEXT("WorldPreview_WestBay.png"), true });
            SpotView(TEXT("Отмель"), 0, TEXT("WorldPreview_Hump.png"));     // the crucian shallow, the hump beyond
            SpotView(TEXT("Коряжник"), -30, TEXT("WorldPreview_Snags.png"));   // its own fallen tree on the left
            SpotView(TEXT("Верховье"), 40, TEXT("WorldPreview_Upper.png"));
            // A low wooded bank near the camp: the shrubs and the grown spruce, from the water and
            // from the bank itself looking along the forest edge.
            {
                const FVector2D From = LakeMap::StartSpot();
                const LakeMap::FShorePoint* Best = nullptr;
                for (const LakeMap::FShorePoint& S : LakeMap::Shoreline())
                    if (LakeMap::ForestAmount(S.Location) > 0.9f && LakeMap::BluffHeight(S.Location) < 0.3f && LakeMap::SpotAmount(S.Location) < 0.02f
                        && FVector2D::Distance(S.Location, From) > 3000
                        && (!Best || FVector2D::Distance(S.Location, From) < FVector2D::Distance(Best->Location, From)))
                        Best = &S;
                if (Best)
                {
                    const FVector Eye(Best->Location - Best->Outward * 2400, LakeMap::SurfaceZ + 220);
                    Shots.Add({ Eye, (FVector(Best->Location + Best->Outward * 900, LakeMap::SurfaceZ + 350) - Eye).Rotation(), TEXT("WorldPreview_Shrubs.png"), true });
                    const FVector2D Along(-Best->Outward.Y, Best->Outward.X);
                    const FVector2D OnBank = Best->Location + Best->Outward * 400;
                    Shots.Add({ Stand(OnBank) + FVector(0, 0, 70), FRotator(-4, YawOf(Along + Best->Outward * 0.6f), 0), TEXT("WorldPreview_Edge.png"), true });
                }
            }
            // From the meadow above the bay, where the village will stand.
            const FVector2D Plateau = LakeMap::VillageCentre() + FVector2D(0, -4000);
            Shots.Add({ Stand(Plateau), FRotator(-4, YawOf(LakeMap::Landing().Location - Plateau), 0), TEXT("WorldPreview_Village.png"), false });
            // From the air: the whole lake from the south-west, and straight down.
            const FBox2D Box = LakeMap::LakeBounds();
            const FVector Air(Box.Min.X - 15000, Box.Max.Y + 32000, 16000);
            Shots.Add({ Air, (FVector(Box.GetCenter() + FVector2D(4000, -2000), LakeMap::SurfaceZ) - Air).Rotation(), TEXT("WorldPreview_6.png"), true });
            Shots.Add({ FVector(Box.GetCenter(), 82000), FRotator(-90, 0, 0), TEXT("WorldPreview_Map.png"), true });
        }
        // A clear, calm morning for the views (the game itself starts at a misty dawn).
        FTimerHandle WeatherTimer;
        GetWorldTimerManager().SetTimer(WeatherTimer, [this]
        {
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Clear;
                Clear.Cloud = 0.3f; Clear.Fog = 0.0f; Clear.Rain = 0; Clear.Wind = 0.1f; Clear.WindYaw = 40;
                Weather->SetWeather(Clear, 1);
                Weather->SetHours(9.5f);
            }
        }, 15.0f, false);
        for (int32 I = 0; I < Shots.Num(); ++I)
        {
            const float At = 26.0f + I * 7.0f;
            const FShot Shot = Shots[I];
            FTimerHandle PoseTimer, ShotTimer;
            GetWorldTimerManager().SetTimer(PoseTimer, [this, Shot]
            {
                APlayerController* PC = GetWorld()->GetFirstPlayerController();
                if (!PC || !PC->GetPawn()) return;
                PC->SetIgnoreLookInput(true);
                PC->SetIgnoreMoveInput(true);
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
                if (Shot.bCamera)
                {
                    auto* Cam = GetWorld()->SpawnActor<ACameraActor>(Shot.Location, Shot.Rotation);
                    Cam->GetCameraComponent()->SetFieldOfView(60);
                    PC->SetViewTarget(Cam);
                    return;
                }
                PC->SetViewTarget(PC->GetPawn());
                PC->GetPawn()->SetActorLocation(Shot.Location, false, nullptr, ETeleportType::TeleportPhysics);
                PC->SetControlRotation(Shot.Rotation);
            }, At - 5.0f, false);
            GetWorldTimerManager().SetTimer(ShotTimer, [Shot]
            {
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Shot.File, false, false);
                if (FParse::Param(FCommandLine::Get(), TEXT("ProfileShots")) && GEngine) GEngine->Exec(nullptr, TEXT("ProfileGPU"));
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_PREVIEW %s frame=%.1f ms"), Shot.File, FApp::GetDeltaTime() * 1000.0);
            }, At, false);
        }
        // Then a free camera just above the water looking down at the shallowest fish near the start.
        const float FishShotAt = 26.0f + Shots.Num() * 7.0f;
        FTimerHandle FishPose, FishShot, ExitTimer;
        GetWorldTimerManager().SetTimer(FishPose, [this]
        {
            ALakeFish* Best = nullptr;
            const FVector2D StartP = LakeMap::StartSpot();
            for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
                if (FVector2D::Distance(FVector2D(It->GetActorLocation()), StartP) < 8000 && (!Best || It->GetActorLocation().Z > Best->GetActorLocation().Z)) Best = *It;
            APlayerController* PC = GetWorld()->GetFirstPlayerController();
            if (!Best || !PC) return;
            const FVector Target = Best->GetActorLocation();
            FVector CamPos = Target - Best->GetActorRightVector() * 160 - Best->GetActorForwardVector() * 60;
            CamPos.Z = LakeFish::SurfaceZ + 110;
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(CamPos, (Target - CamPos).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(55);
            PC->SetViewTarget(Cam);
        }, FishShotAt - 0.6f, false);
        GetWorldTimerManager().SetTimer(FishShot, [] { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("WorldPreview_4.png"), false, false); }, FishShotAt, false);
        // The angler's own preview sequence (ALakeCharacter) runs after these shots.
        GetWorldTimerManager().SetTimer(ExitTimer, [] { FPlatformMisc::RequestExit(false); }, FishShotAt + 32, false);
    }
#endif
    if (PCGForest) GetWorld()->SpawnActor<ALakeForest>(FVector(LakeMap::TerrainBounds.GetCenter(), 0), FRotator::ZeroRotator);
    if (GetNetMode() != NM_DedicatedServer) GetWorld()->SpawnActor<ALakeBirds>();
    // The fish are stocked by Live Fish (ALakeLife::StockPond).
    // Diagnostic for the network test: how many fish this machine sees after replication settles.
    FTimerHandle FishCountTimer;
    GetWorldTimerManager().SetTimer(FishCountTimer, [this]
    {
        int32 Count = 0;
        TArray<int32> BySpecies;
        BySpecies.Init(0, LakeFish::NumSpecies());
        for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It) { ++Count; if (BySpecies.IsValidIndex((int32)It->Species)) ++BySpecies[(int32)It->Species]; }
        FString Line;
        for (int32 Sp = 0; Sp < BySpecies.Num(); ++Sp) Line += FString::Printf(TEXT(" %s %d"), *LakeFish::Info((EFishSpecies)Sp).Id, BySpecies[Sp]);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH_VISIBLE %d (netmode %d):%s"), Count, (int32)GetNetMode(), *Line);
    }, 8.0f, false);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_WORLD_READY: terrain, lake, dam, island, forest and props in %.1f s"), FPlatformTime::Seconds() - BuildStart);
}
