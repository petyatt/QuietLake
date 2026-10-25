#include "LakeGame.h"
#include "LakeMap.h"
#include "LakeWeather.h"
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

float ALakeEnvironment::LakeRadius(float X, float Y) { return LakeMap::Radius(X, Y); }
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
    // Where the forest begins, in metres from the water; varies around the pond.
    float ForestStart(const FVector2D& P) { return 15.0f + 9.0f * FMath::PerlinNoise2D(P / 4000); }
    float Slope(float X, float Y)
    {
        return FVector2D(LakeMap::Ground(X + 100, Y) - LakeMap::Ground(X - 100, Y), LakeMap::Ground(X, Y + 100) - LakeMap::Ground(X, Y - 100)).Size() / 200;
    }
    float YawOf(const FVector2D& Dir) { return FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X)); }
}

void ALakeEnvironment::BeginPlay()
{
    Super::BeginPlay();
    auto Material = [](const TCHAR* Name)
    {
        return LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Materials/%s.%s"), Name, Name));
    };
    // One deterministic layout per peer; foliage transforms do not consume network bandwidth.
    FRandomStream Random(20261025);
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
    // Build the shore distance field and the fixed places before anything runs in parallel.
    const FVector2D StartFacingDefault(1, 0);
    FVector2D StartFacing = StartFacingDefault;
    const FVector2D Start = LakeMap::StartSpot(&StartFacing);
    const FVector2D Village = LakeMap::VillageCentre();
    const LakeMap::FPier& Pier = LakeMap::Pier();
    LakeMap::LilyBeds();
    LakeMap::Snags();
    const LakeMap::FShorePoint Landing = LakeMap::ShoreAt(-2.6f);
    // Roads and footpaths (LakeMap::Tracks): bare ground under and a little beside each strip.
    LakeMap::Tracks();
    auto PathAmount = [](const FVector2D& P)
    {
        bool bRoad = false;
        const float Clearance = LakeMap::TrackClearance(P.X, P.Y, &bRoad) + 25 * FMath::PerlinNoise2D(P / 300);
        return 1 - FMath::SmoothStep(-20.0f, bRoad ? 120.0f : 60.0f, Clearance);
    };

    // --- terrain: a 1 m grid in square chunks, each its own component for culling ------------------
    {
        const FBox2D Bounds = LakeMap::TerrainBounds;
        constexpr float CellSize = 100.0f;
        constexpr int32 Chunk = 96;
        const int32 NX = FMath::CeilToInt((Bounds.Max.X - Bounds.Min.X) / CellSize);
        const int32 NY = FMath::CeilToInt((Bounds.Max.Y - Bounds.Min.Y) / CellSize);
        const int32 Stride = NX + 3;   // one extra sample on every side for normals
        TArray<float> Heights;
        Heights.SetNumUninitialized(Stride * (NY + 3));
        ParallelFor(NY + 3, [&](int32 Y)
        {
            for (int32 X = 0; X < Stride; ++X)
                Heights[Y * Stride + X] = LakeMap::Ground(Bounds.Min.X + (X - 1) * CellSize, Bounds.Min.Y + (Y - 1) * CellSize);
        });
        auto H = [&](int32 X, int32 Y) { return Heights[(Y + 1) * Stride + X + 1]; };
        UMaterialInterface* TerrainMaterial = Material(TEXT("M_TerrainScanned"));
        for (int32 CY = 0; CY < NY; CY += Chunk)
            for (int32 CX = 0; CX < NX; CX += Chunk)
            {
                const int32 W = FMath::Min(Chunk, NX - CX), Hh = FMath::Min(Chunk, NY - CY);
                TArray<FVector> Vertices, Normals;
                TArray<FVector2D> UVs;
                TArray<int32> Triangles;
                TArray<FLinearColor> Colors;
                TArray<FProcMeshTangent> Tangents;
                for (int32 Y = CY; Y <= CY + Hh; ++Y)
                    for (int32 X = CX; X <= CX + W; ++X)
                    {
                        const FVector2D P(Bounds.Min.X + X * CellSize, Bounds.Min.Y + Y * CellSize);
                        const float DX = (H(X + 1, Y) - H(X - 1, Y)) / (2 * CellSize), DY = (H(X, Y + 1) - H(X, Y - 1)) / (2 * CellSize);
                        Vertices.Add(FVector(P, H(X, Y)));
                        Normals.Add(FVector(-DX, -DY, 1).GetSafeNormal());
                        Tangents.Add(FProcMeshTangent(FVector(1, 0, DX).GetSafeNormal(), false));
                        UVs.Add(P / 250);
                        // Layer masks for M_TerrainScanned: R = meadow, G = forest floor, B = bare clay on steep
                        // faces; none of them = the muddy shore and the lake bed.
                        const float L = -LakeMap::ShoreDistance(P.X, P.Y) / 100;
                        const float Steep = FVector2D(DX, DY).Size();
                        const float Bare = FMath::SmoothStep(0.55f, 0.95f, Steep) * FMath::SmoothStep(0.2f, 1.0f, L);
                        const float Open = FMath::SmoothStep(2800.0f, 4500.0f, (float)FVector2D::Distance(P, Village)) * FMath::SmoothStep(900.0f, 1800.0f, (float)FVector2D::Distance(P, Start));
                        const float FS = ForestStart(P);
                        const float Forest = FMath::SmoothStep(FS - 4, FS + 6, L) * Open;
                        const float Meadow = FMath::SmoothStep(1.5f, 5.0f, L) * (1 - Forest);
                        const float Trodden = 1 - PathAmount(P);
                        Colors.Add(FLinearColor(Meadow * (1 - Bare) * Trodden, Forest * (1 - Bare) * Trodden, Bare));
                        if (X < CX + W && Y < CY + Hh)
                        {
                            const int32 A = (Y - CY) * (W + 1) + (X - CX);
                            Triangles.Append({ A, A + W + 1, A + 1, A + 1, A + W + 1, A + W + 2 });
                        }
                    }
                auto* Terrain = NewObject<UProceduralMeshComponent>(this);
                Terrain->SetupAttachment(RootComponent);
                Terrain->bUseAsyncCooking = true;
                Terrain->RegisterComponent();
                AddInstanceComponent(Terrain);
                Terrain->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, true);
                Terrain->SetMaterial(0, TerrainMaterial);
            }
    }

    // --- water: one surface over the whole pond -----------------------------------------------------
    FBox2D LakeBox(ForceInit);
    for (const LakeMap::FShorePoint& P : LakeMap::Shoreline()) LakeBox += P.Location;
    auto* Water = Group(TEXT("Cube"), TEXT("M_LakeSurface"), true);
    const FVector2D WaterExtent = LakeBox.GetExtent() + FVector2D(3000, 3000);
    Add(Water, FVector(LakeBox.GetCenter(), LakeMap::SurfaceZ - 6), FVector(WaterExtent.X / 50, WaterExtent.Y / 50, 0.12));
    auto* Bark = Group(TEXT("Cylinder"), TEXT("M_OldBark"), true);    // piles, snags, stakes, firewood
    auto* Wood = Group(TEXT("Cube"), TEXT("M_WeatheredWood"), true);
    auto* Metal = Group(TEXT("Cylinder"), TEXT("M_Metal"), true);
    // Invisible trunk colliders: the Megaplants trees are skinned meshes without per-instance collision.
    auto* Trunks = Group(TEXT("Cylinder"), TEXT("M_Bark"), true);
    Trunks->SetHiddenInGame(true);
    Trunks->SetCastShadow(false);

    // --- Quixel Megaplants: Nanite skinned meshes animated by the Dynamic Wind plugin ----------------
    // One instanced component per mesh variant draws every copy of that variant in one batch.
    auto* Wind = NewObject<UDynamicWindData>(this);
    // bSway = false: the deep forest, seen only from afar through the haze: no wind and no shadow
    // casting. Instanced skinned meshes redraw their virtual shadow pages every frame even when
    // still (they always count as moving), which cost most of the frame with 2600 trees.
    auto Plants = [this, Wind](const TCHAR* Folder, const TCHAR* Prefix, const TCHAR* Letters, float CullDistance, bool bSway = true)
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
            auto* Component = NewObject<UInstancedSkinnedMeshComponent>(this);
            Component->SetupAttachment(RootComponent);
            Component->SetSkinnedAssetAndUpdate(Mesh);
            if (bSway) Component->SetTransformProvider(Wind);
            else Component->SetCastShadow(false);       // far forest: Lumen shades it, no shadow pages to redraw
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetCullDistances(CullDistance * 0.8f, CullDistance);
            Component->RegisterComponent();
            AddInstanceComponent(Component);
            Variants.Add(Component);
        }
        return Variants;
    };
    const auto Pines = Plants(TEXT("Tree_Baltic_Pine/Tree_Baltic_Pine_01"), TEXT("Tree_Baltic_Pine_01"), TEXT("ABCD"), 40000);
    const auto Spruces = Plants(TEXT("Tree_Norway_Spruce/Tree_Norway_Spruce_01"), TEXT("Tree_Norway_Spruce_01"), TEXT("ABCD"), 40000);
    const auto Birches = Plants(TEXT("Tree_Silver_Birch/Tree_Silver_Birch_01"), TEXT("Tree_Silver_Birch_01"), TEXT("ABCD"), 40000);
    const auto Alders = Plants(TEXT("Tree_Black_Alder/Tree_Black_Alder_01"), TEXT("Tree_Black_Alder_01"), TEXT("ABCD"), 40000);
    const auto StillPines = Plants(TEXT("Tree_Baltic_Pine/Tree_Baltic_Pine_01"), TEXT("Tree_Baltic_Pine_01"), TEXT("ABCD"), 40000, false);
    const auto StillSpruces = Plants(TEXT("Tree_Norway_Spruce/Tree_Norway_Spruce_01"), TEXT("Tree_Norway_Spruce_01"), TEXT("ABCD"), 40000, false);
    const auto StillBirches = Plants(TEXT("Tree_Silver_Birch/Tree_Silver_Birch_01"), TEXT("Tree_Silver_Birch_01"), TEXT("ABCD"), 40000, false);
    const auto StillAlders = Plants(TEXT("Tree_Black_Alder/Tree_Black_Alder_01"), TEXT("Tree_Black_Alder_01"), TEXT("ABCD"), 40000, false);
    const auto Bulrush = Plants(TEXT("Plant_Bulrush/Plant_Bulrush_01"), TEXT("Plant_Common_Bulrush_01"), TEXT("ABCDE"), 25000);
    const auto DryReed = Plants(TEXT("Plant_Dry_Common_Reeds/Plant_Dry_Common_Reeds_01"), TEXT("Plant_Dry_Common_Reed"), TEXT("ABCD"), 25000);
    auto Place = [&Random](const TArray<UInstancedSkinnedMeshComponent*>& Variants, const FTransform& Transform)
    {
        if (Variants.Num() == 0) return;
        Variants[Random.RandRange(0, Variants.Num() - 1)]->AddInstance(Transform, 0);
    };
    auto Upright = [&Random](FVector Location, float Scale)
    {
        return FTransform(FRotator(0, Random.FRandRange(0, 360), 0), Location, FVector(Scale));
    };

    // Forest: birch and alder nearer the water, pine and spruce behind; a few lone birches in the meadows.
    const FBox2D Land = LakeMap::TerrainBounds.ExpandBy(-500);
    for (int32 Try = 0, Placed = 0; Try < 9000 && Placed < 2600; ++Try)
    {
        const FVector2D P(Random.FRandRange(Land.Min.X, Land.Max.X), Random.FRandRange(Land.Min.Y, Land.Max.Y));
        const float L = -LakeMap::ShoreDistance(P.X, P.Y) / 100;
        if (L < 3 || FVector2D::Distance(P, Village) < 3400 || FVector2D::Distance(P, Start) < 1600) continue;
        bool bRoad = false;
        if (LakeMap::TrackClearance(P.X, P.Y, &bRoad) < (bRoad ? 300 : 120)) continue;      // keep roads and paths open
        const float FS = ForestStart(P);
        const bool bForest = L > FS + Random.FRandRange(-6, 6);
        if (!bForest && Random.FRand() > 0.06f) continue;
        if (Slope(P.X, P.Y) > 0.8f) continue;               // not on the faces of bluffs and scarps
        // Out in the meadows trees stand in small groves of birch and alder.
        const int32 Grove = bForest ? 1 : Random.RandRange(1, 4);
        for (int32 G = 0; G < Grove; ++G)
        {
            const FVector2D Q = P + (G ? FVector2D(Random.FRandRange(-600, 600), Random.FRandRange(-600, 600)) : FVector2D::ZeroVector);
            if (-LakeMap::ShoreDistance(Q.X, Q.Y) < 300) continue;
            // Sink the base slightly so roots never float on slopes.
            const FVector Base(Q, Ground(Q) - 15);
            // Trees near the water, in the meadows and at the forest edge sway; deeper in they stand still.
            const bool bSway = !bForest || L < FS + 12;
            const float Pick = Random.FRand();
            const TArray<UInstancedSkinnedMeshComponent*>& Species = !bForest ? (Pick < 0.6f ? Birches : Alders)
                : L < FS + 18 ? (Pick < 0.55f ? (bSway ? Birches : StillBirches) : (bSway ? Alders : StillAlders))
                : (Pick < 0.55f ? (bSway ? Pines : StillPines) : (bSway ? Spruces : StillSpruces));
            Place(Species, Upright(Base, Random.FRandRange(0.7f, 1.1f)));
            Add(Trunks, Base + FVector(0, 0, 250), FVector(0.45f, 0.45f, 5.0f));
            ++Placed;
        }
    }
    // Alders and birches on the low banks, leaning out over the water.
    const TArray<LakeMap::FShorePoint>& Shore = LakeMap::Shoreline();
    const FVector2D PierEnd = Pier.Root + Pier.Dir * Pier.Length;
    for (int32 I = 0; I < Shore.Num(); I += 7)
    {
        const LakeMap::FShorePoint& S = Shore[I];
        if (LakeMap::OpeningAmount(S.Angle) > 0.1f || LakeMap::BluffHeight(S.Angle) > 0.5f || Random.FRand() > 0.4f) continue;
        if (SegmentDistance2D(S.Location, Pier.Root, PierEnd) < 1500) continue;
        const FVector2D P = S.Location + S.Outward * Random.FRandRange(150, 450);
        if (LakeMap::TrackClearance(P.X, P.Y) < 150) continue;
        const FVector Toward(-S.Outward, 0);
        const FVector Axis = FVector::CrossProduct(FVector::UpVector, Toward).GetSafeNormal();
        const FQuat Lean = FQuat(Axis, FMath::DegreesToRadians(Random.FRandRange(6, 14))) * FQuat(FVector::UpVector, Random.FRandRange(0, 2 * PI));
        Place(Random.FRand() < 0.6f ? Alders : Birches, FTransform(Lean, FVector(P, Ground(P) - 15), FVector(Random.FRandRange(0.65f, 0.95f))));
        Add(Trunks, FVector(P, Ground(P) + 235), FVector(0.4f, 0.4f, 4.5f));
    }

    // Mossy boulders (Quixel Megascans, Nanite): under the bluffs, along the banks, a few in the fields.
    auto* Rocks = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
    Rocks->SetupAttachment(RootComponent);
    Rocks->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Fab/Megascans/3D/Mossy_Rocks_tjmudcfda/High/tjmudcfda_tier_1/StaticMeshes/tjmudcfda_tier_1.tjmudcfda_tier_1")));
    Rocks->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Rocks->SetCollisionResponseToAllChannels(ECR_Block);
    Rocks->RegisterComponent();
    AddInstanceComponent(Rocks);
    for (int32 I = 0; I < 320; ++I)
    {
        FVector2D P;
        float S = Random.FRandRange(0.35f, 0.9f);
        if (I < 200)
        {
            const LakeMap::FShorePoint& Sh = Shore[Random.RandRange(0, Shore.Num() - 1)];
            const float Bluff = LakeMap::BluffHeight(Sh.Angle);
            if (LakeMap::OpeningAmount(Sh.Angle) > 0.3f || Random.FRand() > 0.3f + 0.7f * FMath::SmoothStep(0.5f, 1.5f, Bluff)) continue;
            // Under the bluffs the stones lie at the foot of the clay face, partly in the water.
            P = Sh.Location + Sh.Outward * (Bluff > 0.5f ? Random.FRandRange(-160, 40) : Random.FRandRange(-180, 260));
        }
        else
        {
            P = FVector2D(Random.FRandRange(Land.Min.X, Land.Max.X), Random.FRandRange(Land.Min.Y, Land.Max.Y));
            if (LakeMap::ShoreDistance(P.X, P.Y) > -500 || FVector2D::Distance(P, Village) < 3000) continue;
        }
        if (SegmentDistance2D(P, Pier.Root, PierEnd) < 500 || LakeMap::TrackClearance(P.X, P.Y) < 150) continue;
        Add(Rocks, FVector(P, Ground(P) - 4), FVector(S, S, S * Random.FRandRange(0.8f, 1.6f)),
            FRotator(Random.FRandRange(-4, 4), Random.FRandRange(0, 360), Random.FRandRange(-4, 4)));
    }

    // Reed belt along the low banks: bulrush standing in the water, dry reed at the edge. It is broken
    // by the openings (where the angler casts), the pier and the bluffs, and widest in the backwater.
    for (const LakeMap::FShorePoint& S : Shore)
    {
        if (LakeMap::OpeningAmount(S.Angle) > 0.35f || LakeMap::BluffHeight(S.Angle) > 0.7f) continue;
        if (SegmentDistance2D(S.Location, Pier.Root, PierEnd) < 450) continue;
        const float Back = LakeMap::BackwaterAmount(S.Location.X, S.Location.Y);
        if (Random.FRand() > 0.68f + 0.35f * FMath::PerlinNoise2D(S.Location / 2500) + 0.35f * Back) continue;
        const FVector2D Along(-S.Outward.Y, S.Outward.X);
        const int32 Clumps = 1 + (Random.FRand() < 0.6f) + (Random.FRand() < 0.3f) + (Back > 0.5f ? 2 : 0);
        for (int32 C = 0; C < Clumps; ++C)
        {
            const float Into = Random.FRandRange(-0.6f, Back > 0.5f ? 5.0f : 2.6f);   // metres into the water
            const FVector2D P = S.Location - S.Outward * Into * 100 + Along * Random.FRandRange(-60, 60);
            if (LakeMap::Depth(P.X, P.Y) > 110) continue;
            const bool bBulrush = Into > 1.2f || Random.FRand() < 0.4f;
            Place(bBulrush ? Bulrush : DryReed, Upright(FVector(P, Ground(P) - 8), Random.FRandRange(0.8f, 1.3f)));
        }
    }

    // Water lily beds: floating leaves, white flowers and the yellow cups of the brandy-bottle lily.
    auto LilyGroup = [this](const TCHAR* Name)
    {
        auto* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Plants/SM_%s.SM_%s"), Name, Name)));
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->SetCullDistances(20000, 26000);
        // Tens of thousands of small plants: keep them out of the distance fields and the Lumen
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
            if (!LakeMap::IsWater(P.X, P.Y) || D < 20 || D > 190) continue;
            const float Pick = Random.FRand();
            const FRotator Rot(Random.FRandRange(-2, 2), Random.FRandRange(0, 360), Random.FRandRange(-2, 2));
            Add(Pads[Pick < 0.5f ? 0 : Pick < 0.82f ? 1 : 2], FVector(P, LakeMap::SurfaceZ + 0.3f), FVector(Random.FRandRange(0.75f, 1.35f)), Rot);
            ++LilyCount;
            if (Random.FRand() < 0.035f) Add(Flowers, FVector(P + FVector2D(12, 5), LakeMap::SurfaceZ + 0.6f), FVector(Random.FRandRange(0.8f, 1.2f)), FRotator(0, Random.FRandRange(0, 360), 0));
            else if (Random.FRand() < 0.02f) Add(Yellow, FVector(P + FVector2D(-8, 10), LakeMap::SurfaceZ), FVector(Random.FRandRange(0.9f, 1.2f)), FRotator(0, Random.FRandRange(0, 360), 0));
        }
    }

    // Meadow grass (Nanite, swaying in the wind): sedge along the damp bank, short and tall grass
    // with patches of chamomile and buttercups out to the forest edge, trampled short around the
    // angler's camp and in the village.
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
        // The fishing spots stay clear of the tall bank grass so the angler can see and cast.
        TArray<FVector2D> Spots;
        for (const LakeMap::FOpening& O : LakeMap::Openings()) Spots.Add(LakeMap::ShoreAt(O.Angle).Location);
        const FBox2D Area = FBox2D(LakeBox.Min - FVector2D(4500, 4500), LakeBox.Max + FVector2D(4500, 4500));
        constexpr float Spacing = 60.0f;
        int32 GrassCount = 0;
        for (float Y = Area.Min.Y; Y < Area.Max.Y; Y += Spacing)
            for (float X = Area.Min.X; X < Area.Max.X; X += Spacing)
            {
                const FVector2D P(X + Random.FRandRange(-30, 30), Y + Random.FRandRange(-30, 30));
                const float L = -LakeMap::ShoreDistance(P.X, P.Y) / 100;
                if (L < -0.3f || L > 45) continue;
                const float FS = ForestStart(P);
                const bool bTrampled = FVector2D::Distance(P, Start) < 1100 || FVector2D::Distance(P, Village) < 2600;
                if (L > FS + 5 && !bTrampled && Random.FRand() > 0.12f) continue;      // thin under the trees
                if (SegmentDistance2D(P, Pier.Root - Pier.Dir * 50, Pier.Root + Pier.Dir * 400) < 140) continue;
                if (Slope(P.X, P.Y) > 0.6f || PathAmount(P) > 0.4f) continue;        // bare clay faces, paths
                UHierarchicalInstancedStaticMeshComponent* Kind;
                bool bNearSpot = false;
                for (const FVector2D& Spot : Spots) bNearSpot |= FVector2D::DistSquared(P, Spot) < FMath::Square(900.0f);
                const bool bBank = L > 1.2f && L < 14.0f + 3.0f * FMath::PerlinNoise2D(P / 1500) && !bTrampled && !bNearSpot
                    && PathAmount(P) < 0.05f && LakeMap::BluffHeight(LakeMap::AngleOf(P.X, P.Y)) < 0.8f;
                // Bank grass: fewer, larger tufts (cheaper to draw, just as dense to the eye).
                if (bBank && Random.FRand() < 0.4f) continue;
                if (bBank) Kind = FMath::PerlinNoise2D(P / 700 + FVector2D(31, 7)) > 0.3f && Random.FRand() < 0.6f ? Meadowsweet : Waist;
                else if (bTrampled) { if (Random.FRand() < 0.35f) continue; Kind = Short; }
                else if (L < 2.5f) Kind = Random.FRand() < 0.75f ? Sedge : Tall;
                else if (FMath::PerlinNoise2D(P / 900) > 0.3f && Random.FRand() < 0.5f) Kind = Blooms;
                else Kind = Random.FRand() < 0.5f ? Short : Tall;
                const float Scale = Random.FRandRange(0.8f, 1.3f) * (bTrampled ? 0.8f : bBank ? 1.2f : 1.0f);
                Add(Kind, FVector(P, Ground(P) - 2), FVector(Scale, Scale, Scale * Random.FRandRange(0.85f, 1.2f)), FRotator(0, Random.FRandRange(0, 360), 0));
                ++GrassCount;
            }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_GRASS %d clumps"), GrassCount);
    }

    // --- roads and footpaths: strips laid on the terrain, textured across their width -------------
    {
        UMaterialInterface* RoadMaterial = Material(TEXT("M_Road"));
        UMaterialInterface* PathMaterial = Material(TEXT("M_Path"));
        for (const LakeMap::FTrack& T : LakeMap::Tracks())
        {
            constexpr int32 Across = 6;
            TArray<FVector> Vertices, Normals;
            TArray<FVector2D> UVs;
            TArray<int32> Triangles;
            TArray<FLinearColor> Colors;
            TArray<FProcMeshTangent> Tangents;
            float Along = 0;
            const TArray<FVector2D>& Pts = T.Points;
            for (int32 I = 0; I < Pts.Num(); ++I)
            {
                const FVector2D Dir = (Pts[FMath::Min(I + 1, Pts.Num() - 1)] - Pts[FMath::Max(I - 1, 0)]).GetSafeNormal();
                const FVector2D Side(-Dir.Y, Dir.X);
                if (I) Along += FVector2D::Distance(Pts[I], Pts[I - 1]);
                for (int32 K = 0; K <= Across; ++K)
                {
                    const float U = float(K) / Across;
                    const FVector2D P = Pts[I] + Side * (U - 0.5f) * T.Width;
                    const float DX = (LakeMap::Ground(P.X + 50, P.Y) - LakeMap::Ground(P.X - 50, P.Y)) / 100;
                    const float DY = (LakeMap::Ground(P.X, P.Y + 50) - LakeMap::Ground(P.X, P.Y - 50)) / 100;
                    Vertices.Add(FVector(P, LakeMap::Ground(P.X, P.Y) + 6));
                    Normals.Add(FVector(-DX, -DY, 1).GetSafeNormal());
                    UVs.Add(FVector2D(U, Along / T.Width));
                    Tangents.Add(FProcMeshTangent(FVector(Dir, 0), false));
                    Colors.Add(FLinearColor::White);
                    if (I > 0 && K < Across)
                    {
                        const int32 A = (I - 1) * (Across + 1) + K, B = I * (Across + 1) + K;
                        Triangles.Append({ A, A + 1, B, A + 1, B + 1, B });
                    }
                }
            }
            // Face up whichever way the strip runs (front faces wind like the terrain's: cross product down).
            if (Triangles.Num() >= 3 && FVector::CrossProduct(Vertices[Triangles[1]] - Vertices[Triangles[0]], Vertices[Triangles[2]] - Vertices[Triangles[0]]).Z > 0)
                for (int32 I = 0; I + 2 < Triangles.Num(); I += 3) Swap(Triangles[I + 1], Triangles[I + 2]);
            auto* Strip = NewObject<UProceduralMeshComponent>(this);
            Strip->SetupAttachment(RootComponent);
            Strip->SetCastShadow(false);
            Strip->RegisterComponent();
            AddInstanceComponent(Strip);
            Strip->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, false);
            Strip->SetMaterial(0, T.bRoad ? RoadMaterial : PathMaterial);
        }
    }

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

    // --- snags: fallen trees lying from the bank into the water, branches sticking up ---------------
    for (const LakeMap::FSnag& Snag : LakeMap::Snags())
    {
        const FVector Dir = Snag.Tip - Snag.Root;
        Add(Bark, (Snag.Root + Snag.Tip) * 0.5f, FVector(Snag.Radius / 50, Snag.Radius / 50, Dir.Size() / 100), FRotationMatrix::MakeFromZ(Dir).Rotator());
        for (int32 B = 0; B < 6; ++B)
        {
            const float T = Random.FRandRange(0.2f, 0.95f);
            const FVector Base = Snag.Root + Dir * T;
            const FVector Out = (FVector::UpVector * Random.FRandRange(0.4f, 1.0f) + FVector(Random.FRandRange(-1, 1), Random.FRandRange(-1, 1), 0) * 0.7f).GetSafeNormal();
            const float Len = Random.FRandRange(80, 230) * (1 - T * 0.5f), Rad = Snag.Radius * Random.FRandRange(0.22f, 0.38f);
            Add(Bark, Base + Out * Len * 0.5f, FVector(Rad / 50, Rad / 50, Len / 100), FRotationMatrix::MakeFromZ(Out).Rotator());
        }
    }

    // --- the village: a log house facing the lake, boats at the landing, nets drying, a garden ------
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
        const FVector2D ToLake = (Landing.Location - Village).GetSafeNormal();
        const FVector2D Across(-ToLake.Y, ToLake.X);
        Prop(TEXT("Izba"), FVector(Village, Ground(Village) - 6), FRotator(0, YawOf(ToLake), 0), true);
        // A boat pulled up on the landing, bow up the bank, lying along the slope.
        {
            const FVector2D Dir = Landing.Outward;
            const FVector2D P = Landing.Location + Dir * 170;
            const float Pitch = FMath::RadiansToDegrees(FMath::Atan2(Ground(P + Dir * 190) - Ground(P - Dir * 190), 380));
            Prop(TEXT("Boat_B"), FVector(P, Ground(P) - 4), FRotator(Pitch, YawOf(Dir) + 6, 4), true);
        }
        // An old boat half sunk among the reeds next to the landing.
        {
            const LakeMap::FShorePoint S = LakeMap::ShoreAt(-2.5f);
            const FVector2D P = S.Location - S.Outward * 260;
            Prop(TEXT("Boat_A"), FVector(P, LakeMap::SurfaceZ - 34), FRotator(-3, YawOf(FVector2D(-S.Outward.Y, S.Outward.X)) + 20, 8), false);
        }
        // One moored at the pier's landing stage.
        {
            const FVector2D Side(-Pier.Dir.Y, Pier.Dir.X);
            const FVector2D P = Pier.Root + Pier.Dir * (Pier.Length - 170) + Side * 330;
            Prop(TEXT("Boat_A"), FVector(P, LakeMap::SurfaceZ - 16), FRotator(0, YawOf(Pier.Dir) + 172, 1.5f), true);
        }
        // Nets hung to dry between the house and the water.
        for (float S : { -1.0f, 1.0f })
        {
            const FVector2D P = Village + ToLake * 1050 + Across * S * 520;
            Prop(TEXT("NetRack"), FVector(P, Ground(P)), FRotator(0, YawOf(Across) + Random.FRandRange(-8, 8), 0), false);
        }
        // A rail fence round the garden behind the house, with a gap for a gate.
        const FVector2D GardenCentre = Village - ToLake * 1250;
        const FVector2D Half(600, 450);
        const FVector2D Corners[] = {
            GardenCentre - ToLake * Half.Y - Across * Half.X, GardenCentre - ToLake * Half.Y + Across * Half.X,
            GardenCentre + ToLake * Half.Y + Across * Half.X, GardenCentre + ToLake * Half.Y - Across * Half.X };
        for (int32 Side = 0; Side < 4; ++Side)
        {
            const FVector2D A = Corners[Side], B = Corners[(Side + 1) % 4];
            const int32 Sections = FMath::RoundToInt(FVector2D::Distance(A, B) / 300);
            for (int32 K = 0; K < Sections; ++K)
            {
                if (Side == 2 && K == Sections / 2) continue;                     // the gate, towards the house
                const FVector2D P = FMath::Lerp(A, B, float(K) / Sections);
                Prop(TEXT("Fence"), FVector(P, Ground(P)), FRotator(0, YawOf(B - A) + Random.FRandRange(-3, 3), Random.FRandRange(-2, 2)), false);
            }
        }
        // Firewood stacked against the side of the house.
        for (int32 I = 0; I < 9; ++I)
        {
            const FVector2D P = Village + Across * 520 + ToLake * (-150 + (I % 5) * 28);
            Add(Bark, FVector(P, Ground(P) + 14 + (I / 5) * 26), FVector(0.26f, 0.26f, 1.2f), FRotator(90, YawOf(Across), 0));
        }
    }

    // --- angler spots in the openings: a few boards laid at the water's edge and two stakes ---------
    for (const LakeMap::FOpening& O : LakeMap::Openings())
    {
        if (FCString::Strcmp(O.Name, TEXT("Pier")) == 0 || FCString::Strcmp(O.Name, TEXT("Landing")) == 0) continue;
        const LakeMap::FShorePoint S = LakeMap::ShoreAt(O.Angle);
        const FVector2D Along(-S.Outward.Y, S.Outward.X);
        const float Yaw = YawOf(S.Outward);
        for (int32 K = 0; K < 3; ++K)
        {
            const FVector2D P = S.Location + S.Outward * (60 + K * 22);
            Add(Wood, FVector(P, Ground(P) + 3), FVector(0.2f, 1.5f, 0.04f), FRotator(Random.FRandRange(-2, 2), Yaw + Random.FRandRange(-4, 4), 0));
        }
        for (float Sgn : { -1.0f, 1.0f })
        {
            const FVector2D P = S.Location - S.Outward * 30 + Along * Sgn * 70;
            Add(Bark, FVector(P, Ground(P) + 10), FVector(0.07f, 0.07f, 0.9f), FRotator(Random.FRandRange(-5, 5), 0, Random.FRandRange(-5, 5)));
        }
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
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_MAP lake=%.0fx%.0f m lilies=%d lily beds=%d snags=%d pier=%.0f m"),
        LakeBox.GetSize().X / 100, LakeBox.GetSize().Y / 100, LilyCount, LakeMap::LilyBeds().Num(), LakeMap::Snags().Num(), Pier.Length / 100);

    if (GetNetMode() != NM_DedicatedServer)
    {
        // Early morning. The sun is low and ahead of the starting view, just above the
        // far tree line, so the mist and foliage are backlit and light shafts form.
        auto* Sun = GetWorld()->SpawnActor<ADirectionalLight>(FVector(0,0,3000), FRotator(-6,205,0));
        Sun->SetMobility(EComponentMobility::Movable);
        auto* SunComponent = Cast<UDirectionalLightComponent>(Sun->GetLightComponent());
        SunComponent->SetAtmosphereSunLight(true);
        SunComponent->SetIntensity(10.0f);
        SunComponent->SetUseTemperature(true);
        SunComponent->SetTemperature(5500.0f);
        SunComponent->SetLightSourceAngle(0.8f);
        SunComponent->SetEnableLightShaftBloom(true);
        SunComponent->SetBloomScale(0.08f);
        auto* Atmosphere = GetWorld()->SpawnActor<ASkyAtmosphere>();
        Atmosphere->GetComponent()->SetMieScatteringScale(0.008f); // humid morning haze
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
        Fog->SetVolumetricFogScatteringDistribution(0.4f); // glows toward the sun, still visible away from it
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
        PP.BloomIntensity = 0.5f;
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
        Audio(TEXT("A_Birds"),FVector(LakeMap::StartSpot() + FVector2D(-1800, 1200), 550),0.3f,true);
    }
#if !UE_BUILD_SHIPPING
    // Reproducible visual check; only enabled by an explicit command-line switch.
    // Three fixed viewpoints with input and HUD off, so screenshots are comparable between runs.
    if (FParse::Param(FCommandLine::Get(),TEXT("LakePreview")))
    {
        // Fixed viewpoints with input and HUD off, so screenshots are comparable between runs.
        // The angler stands at a spot (bCamera = false) or a free camera looks from the air.
        struct FShot { FVector Location; FRotator Rotation; const TCHAR* File; bool bCamera; };
        TArray<FShot> Shots;
        auto Stand = [](FVector2D P) { return FVector(P, LakeMap::Ground(P.X, P.Y) + 100); };
        {
            FVector2D Facing;
            const FVector2D StartP = LakeMap::StartSpot(&Facing);
            Shots.Add({ Stand(StartP - Facing * 250), FRotator(-6, YawOf(Facing), 0), TEXT("WorldPreview.png"), false });
            const LakeMap::FPier& Pr = LakeMap::Pier();
            Shots.Add({ FVector(Pr.Root + Pr.Dir * (Pr.Length - 60), LakeMap::SurfaceZ + 150), FRotator(-5, YawOf(Pr.Dir) + 155, 0), TEXT("WorldPreview_2.png"), false });
            const LakeMap::FShorePoint Bluff = LakeMap::ShoreAt(-0.55f);
            const FVector BluffEye(Bluff.Location - Bluff.Outward * 2600, LakeMap::SurfaceZ + 260);
            Shots.Add({ BluffEye, (FVector(Bluff.Location + Bluff.Outward * 300, LakeMap::SurfaceZ + 150) - BluffEye).Rotation(), TEXT("WorldPreview_3.png"), true });
            const LakeMap::FShorePoint Bay = LakeMap::ShoreAt(0.78f);
            const FVector2D BayEye = Bay.Location + Bay.Outward * 200;
            Shots.Add({ Stand(BayEye), FRotator(-7, YawOf(LakeMap::PointAt(1.05f, 0.92f) - BayEye), 0), TEXT("WorldPreview_5.png"), false });
            {
                const LakeMap::FShorePoint Ld = LakeMap::ShoreAt(-2.6f);
                const FVector2D Eye = Ld.Location + Ld.Outward * 300 + FVector2D(-Ld.Outward.Y, Ld.Outward.X) * 1200;
                Shots.Add({ Stand(Eye), FRotator(-4, YawOf(LakeMap::VillageCentre() - Eye) + 12, 0), TEXT("WorldPreview_Village.png"), false });
            }
            const FVector Air(LakeMap::Centre + FVector2D(-9500, -11000), 5200);
            Shots.Add({ Air, (FVector(LakeMap::Centre + FVector2D(0, 1500), LakeMap::SurfaceZ) - Air).Rotation(), TEXT("WorldPreview_6.png"), true });
            Shots.Add({ FVector(LakeMap::Centre + FVector2D(800, 1800), 19000), FRotator(-90, 0, 0), TEXT("WorldPreview_Map.png"), true });
        }
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
        // Then a free camera just above the water looking down at the shallowest fish.
        const float FishShotAt = 26.0f + Shots.Num() * 7.0f;
        FTimerHandle FishPose, FishShot, ExitTimer;
        GetWorldTimerManager().SetTimer(FishPose, [this]
        {
            ALakeFish* Best = nullptr;
            for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
                if (!Best || It->GetActorLocation().Z > Best->GetActorLocation().Z) Best = *It;
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
    // Fish live on the server and replicate to every client, each species in its own kind of water.
    if (HasAuthority())
    {
        FishRandom.Initialize(20261026);
        for (int32 I = 0; I < 15; ++I) SpawnFish(EFishSpecies::Perch, 6, NextSchool++);
        for (int32 I = 0; I < 15; ++I) SpawnFish(EFishSpecies::Roach, 8, NextSchool++);
        SpawnFish(EFishSpecies::Crucian, 48, -1);
        SpawnFish(EFishSpecies::Pike, 12, -1);
        for (int32 Species = 0; Species < (int32)EFishSpecies::Count; ++Species)
        {
            float Depth = 0, Count = 0;
            for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
                if ((int32)It->Species == Species) { Depth += LakeMap::Depth(It->GetActorLocation().X, It->GetActorLocation().Y); ++Count; }
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH %s count=%.0f mean water depth=%.1f m"), LakeFish::Info((EFishSpecies)Species).Name, Count, Count ? Depth / Count / 100 : 0.0f);
        }
    }
    // Diagnostic for the network test: how many fish this machine sees after replication settles.
    FTimerHandle FishCountTimer;
    GetWorldTimerManager().SetTimer(FishCountTimer, [this]
    {
        int32 Count = 0;
        for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It) ++Count;
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH_VISIBLE %d (netmode %d)"), Count, (int32)GetNetMode());
    }, 8.0f, false);
    UE_LOG(LogTemp,Display,TEXT("QUIET_LAKE_WORLD_READY: terrain, lake, pier, foliage and props"));
}

void ALakeEnvironment::SpawnFish(EFishSpecies Species, int32 Count, int32 School)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    // Half of the homes are picked near the places people fish from, the rest anywhere in the pond;
    // in both cases the spot must suit the species (see the habitat weights in LakeFish.cpp).
    // Respawned fish settle where the water suits them now; at the start there is no weather yet.
    FFishConditions Now;
    const ALakeWeather* Weather = ALakeWeather::Get(GetWorld());
    if (Weather) Now = Weather->FishConditions();
    const FFishConditions* C = Weather ? &Now : nullptr;
    auto RandomPoint = [this, Species, C]()
    {
        if (FishRandom.FRand() < 0.5f)
        {
            const TConstArrayView<LakeMap::FOpening> Openings = LakeMap::Openings();
            const LakeMap::FShorePoint Shore = LakeMap::ShoreAt(Openings[FishRandom.RandRange(0, Openings.Num() - 1)].Angle);
            return LakeFish::PickSpot(Species, FishRandom, Shore.Location - Shore.Outward * 1400, 1600, C);
        }
        return LakeFish::PickSpot(Species, FishRandom, FVector2D::ZeroVector, 0, C);
    };
    const FVector Home = RandomPoint();
    for (int32 I = 0; I < Count; ++I)
    {
        const FVector Start = School >= 0 ? Home + FVector(FishRandom.FRandRange(-80, 80), FishRandom.FRandRange(-80, 80), FishRandom.FRandRange(-10, 10)) : RandomPoint();
        // Most fish are small; big ones are rare.
        const float Weight = FMath::Lerp(S.MinWeight, S.MaxWeight, FMath::Pow(FishRandom.FRand(), 2.5f));
        auto* Fish = GetWorld()->SpawnActor<ALakeFish>(Start, FRotator(0, FishRandom.FRandRange(0, 360), 0));
        if (Fish) Fish->Setup(Species, Weight, School >= 0 ? Home : Start, School);
    }
}

void ALakeEnvironment::ScheduleRespawn(EFishSpecies Species)
{
    FTimerHandle Timer;
    GetWorldTimerManager().SetTimer(Timer, [this, Species] { SpawnFish(Species, 1, -1); }, 45.0f, false);
}

