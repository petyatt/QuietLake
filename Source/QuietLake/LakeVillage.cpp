#include "LakeVillage.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "TimerManager.h"
#include "UnrealClient.h"

namespace
{
    float Ground(const FVector2D& P) { return LakeMap::Ground(P.X, P.Y); }
    float YawOf(const FVector2D& Dir) { return FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X)); }

    FString VillageMesh(const TCHAR* Name)
    {
        return FString::Printf(TEXT("/Game/Village/Lakeside/SM_%s.SM_%s"), Name, Name);
    }

    FString PlantMesh(const TCHAR* Name)
    {
        return FString::Printf(TEXT("/Game/Plants/PV/SM_PV_%s.SM_PV_%s"), Name, Name);
    }
}

ALakeVillage::ALakeVillage()
{
    PrimaryActorTick.bCanEverTick = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ALakeVillage::BeginPlay()
{
    Super::BeginPlay();
    Build();
#if !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("VillagePreview"))) RunPreview();
#endif
}

void ALakeVillage::Build()
{
    const LakeMap::FVillage& V = LakeMap::Village();
    FRandomStream Random(20261109);
    UMaterialInterface* Road = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Road.M_Road"));
    UMaterialInterface* Footpath = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Path.M_Path"));
    UMaterialInterface* Soil = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/Village/Lakeside/M_GardenSoil.M_GardenSoil"));
    auto GroundMesh = [this](UMaterialInterface* Material, const TArray<FVector>& Verts, const TArray<int32>& Tris,
        const TArray<FVector>& Normals, const TArray<FVector2D>& UVs, const TArray<FProcMeshTangent>& Tangents)
    {
        auto* Component = NewObject<UProceduralMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->SetCastShadow(false);
        Component->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UVs, {}, Tangents, false);
        Component->SetMaterial(0, Material);
        AddInstanceComponent(Component);
        Component->RegisterComponent();
    };
    auto Quad = [](TArray<int32>& Tris, int32 Index, int32 Stride)
    {
        const int32 A = Index - Stride - 1;
        const int32 B = A + 1, C = A + Stride, D = C + 1;
        Tris.Append({ A, B, C, B, D, C });
    };
    for (const LakeMap::FVillagePath& Path : V.Paths)
    {
        const TArray<FVector2D>& Line = Path.Line;
        constexpr int32 Columns = 7;
        TArray<FVector> Verts, Normals;
        TArray<FVector2D> UVs;
        TArray<int32> Tris;
        TArray<FProcMeshTangent> Tangents;
        float Distance = 0;
        for (int32 I = 0; I < Line.Num(); ++I)
        {
            const FVector2D Dir = (Line[FMath::Min(I + 1, Line.Num() - 1)] - Line[FMath::Max(0, I - 1)]).GetSafeNormal();
            const FVector2D Across(-Dir.Y, Dir.X);
            if (I > 0) Distance += FVector2D::Distance(Line[I], Line[I - 1]);
            for (int32 J = 0; J < Columns; ++J)
            {
                const float U = float(J) / (Columns - 1);
                const FVector2D P = Line[I] + Across * ((U - 0.5f) * Path.Width);
                Verts.Add(FVector(P, Ground(P) + Path.Height));
                UVs.Add(FVector2D(U, Distance / 100.0f));
                Normals.Add(FVector::UpVector);
                Tangents.Add(FProcMeshTangent(FVector(Across, 0), false));
                if (I > 0 && J > 0) Quad(Tris, I * Columns + J, Columns);
            }
        }
        GroundMesh(Path.bRoad ? Road : Footpath, Verts, Tris, Normals, UVs, Tangents);
    }

    // Repeated fences, plants and small props use one component per mesh.
    TMap<FString, UHierarchicalInstancedStaticMeshComponent*> Groups;
    auto Group = [this, &Groups](const FString& Path, bool bCollision)
    {
        if (auto* const* Existing = Groups.Find(Path)) return *Existing;
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path);
        if (!Mesh)
        {
            UE_LOG(LogTemp, Error, TEXT("QUIET_LAKE_LAKESIDE missing mesh %s"), *Path);
            Groups.Add(Path, nullptr);
            return static_cast<UHierarchicalInstancedStaticMeshComponent*>(nullptr);
        }
        auto* C = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        C->SetupAttachment(RootComponent);
        C->SetStaticMesh(Mesh);
        C->SetCollisionProfileName(bCollision ? TEXT("BlockAll") : TEXT("NoCollision"));
        AddInstanceComponent(C);
        C->RegisterComponent();
        Groups.Add(Path, C);
        return C;
    };
    auto Prop = [&Group](const TCHAR* Name, const FVector2D& P, float Yaw, float Scale = 1.0f, bool bCollision = false)
    {
        if (auto* C = Group(VillageMesh(Name), bCollision))
            C->AddInstance(FTransform(FRotator(0, Yaw, 0), FVector(P, Ground(P) - 3.0f), FVector(Scale)), true);
    };
    auto Plant = [&Group, &Random](const TCHAR* Name, const FVector2D& P, float Scale, bool bTree = false)
    {
        if (auto* C = Group(PlantMesh(Name), bTree))
            C->AddInstance(FTransform(FRotator(0, Random.FRandRange(0, 360), 0),
                FVector(P, Ground(P) - 5.0f), FVector(Scale)), true);
    };
    auto Fence = [&Group](const TCHAR* Name, const FVector2D& A, const FVector2D& B)
    {
        auto* C = Group(VillageMesh(Name), true);
        if (!C) return;
        const float Section = C->GetStaticMesh()->GetBounds().BoxExtent.X * 2.0f;
        const int32 Count = FMath::Max(1, FMath::RoundToInt(FVector2D::Distance(A, B) / Section));
        for (int32 I = 0; I < Count; ++I)
        {
            const FVector2D P = FMath::Lerp(A, B, float(I) / Count);
            const FVector2D Q = FMath::Lerp(A, B, float(I + 1) / Count);
            const FVector Start(P, Ground(P) - 4.0f), End(Q, Ground(Q) - 4.0f);
            C->AddInstance(FTransform((End - Start).Rotation(), (Start + End) * 0.5f,
                FVector(FVector::Dist(Start, End) / Section, 1.0f, 0.78f)), true);
        }
    };

    UHierarchicalInstancedStaticMeshComponent* Footings = nullptr;
    auto Support = [&Group](const FVector2D& A, const FVector2D& B, float Top)
    {
        const float Length = FVector2D::Distance(A, B);
        const int32 Count = FMath::Max(1, FMath::CeilToInt(Length / 72.0f));
        const float Yaw = YawOf(B - A);
        for (int32 I = 0; I < Count; ++I)
        {
            const FVector2D Point = FMath::Lerp(A, B, (I + 0.5f) / Count);
            const float Bottom = Ground(Point) - 13.0f;
            if (Top - Bottom < 16.0f) continue;
            if (auto* Stone = Group(VillageMesh(TEXT("FlatRock")), true))
            {
                const FVector Size = Stone->GetStaticMesh()->GetBounds().BoxExtent * 2;
                Stone->AddInstance(FTransform(FRotator(0, Yaw + (I % 3 - 1) * 8, 0), FVector(Point, Bottom),
                    FVector((Length / Count + 14.0f) / Size.X, 49.0f / Size.Y, (Top - Bottom + 8.0f) / Size.Z)), true);
            }
        }
    };
    auto Building = [&](const TCHAR* Name, const FVector2D& P, float Yaw, const FVector& Scale)
    {
        auto* C = Group(VillageMesh(Name), true);
        if (!C) return;
        const FBoxSphereBounds Bounds = C->GetStaticMesh()->GetBounds();
        const FVector Half = Bounds.BoxExtent * Scale;
        const FString AssetName(Name);
        const bool bPorch = AssetName.StartsWith(TEXT("House")) || AssetName == TEXT("Shop");
        const float Back = -0.88f * Half.Y, Front = (bPorch ? 0.47f : 0.86f) * Half.Y;
        const float Across = 0.86f * Half.X;
        auto World = [&](float X, float Y) { return P + FVector2D(X, Y).GetRotated(Yaw); };
        float Low = Ground(P), High = Low;
        for (float X : { -Across, 0.0f, Across })
            for (float Y : { Back, (Back + Front) * 0.5f, Front })
            {
                const float Z = Ground(World(X, Y));
                Low = FMath::Min(Low, Z);
                High = FMath::Max(High, Z);
            }
        // Put the lowest porch step at path grade; the downhill side gets a stone plinth.
        const float Base = bPorch ? Ground(World(0, Half.Y * 0.93f)) - 7.0f : Low - 4.0f;
        C->AddInstance(FTransform(FRotator(0, Yaw, 0), FVector(P, Base), Scale), true);
        const float Top = Base + 14.0f;
        if (Top - Low > 9.0f)
        {
            if (!Footings)
            {
                Footings = Group(TEXT("/Engine/BasicShapes/Cube.Cube"), true);
                if (Footings)
                    Footings->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Stone.M_Stone")));
            }
            const float Bottom = Low - 24.0f;
            const FVector2D Centre = World(0, (Back + Front) * 0.5f);
            if (Footings)
                Footings->AddInstance(FTransform(FRotator(0, Yaw, 0), FVector(Centre, (Top + Bottom) * 0.5f),
                    FVector((Across * 2 - 18.0f) / 100.0f, (Front - Back - 18.0f) / 100.0f, (Top - Bottom) / 100.0f)), true);
            Support(World(-Across, Back), World(Across, Back), Top);
            Support(World(-Across, Back), World(-Across, Front), Top);
            Support(World(Across, Back), World(Across, Front), Top);
            Support(World(-Across, Front), World(Across, Front), Top);
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LAKESIDE building %s at %.0f %.0f, grade %.0f cm"),
            Name, P.X, P.Y, High - Low);
    };

    auto Garden = [&](const LakeMap::FVillagePlot& Plot, int32 Index)
    {
        const FBox2D Bed = Plot.Garden();
        const FVector2D Size = Bed.GetSize();
        const int32 Rows = FMath::FloorToInt(Size.Y / 145.0f);
        const float Spacing = Size.Y / Rows;
        const float RowYaw = YawOf(Plot.Facing);
        auto Height = [&](float Along, float Side)
        {
            const float Phase = (Side - Bed.Min.Y) / Spacing;
            const float Crown = FMath::Square(FMath::Sin(PI * Phase));
            const float Ends = FMath::SmoothStep(0.0, 55.0, FMath::Min(Along - Bed.Min.X, Bed.Max.X - Along));
            return Ground(Plot.At(Along, Side)) + 3.0f + 12.0f * Crown * Ends;
        };

        const int32 Columns = Rows * 8 + 1;
        const int32 LengthSteps = FMath::CeilToInt(Size.X / 32.0f);
        TArray<FVector> Verts, Normals;
        TArray<FVector2D> UVs;
        TArray<int32> Tris;
        TArray<FProcMeshTangent> Tangents;
        for (int32 I = 0; I <= LengthSteps; ++I)
        {
            const float Along = FMath::Lerp(Bed.Min.X, Bed.Max.X, float(I) / LengthSteps);
            for (int32 J = 0; J < Columns; ++J)
            {
                const float Side = FMath::Lerp(Bed.Min.Y, Bed.Max.Y, float(J) / (Columns - 1));
                const float Slope = (Height(Along, Side + 3) - Height(Along, Side - 3)) / 6;
                const float Grade = (Height(Along + 3, Side) - Height(Along - 3, Side)) / 6;
                const FVector Tangent(Plot.Across(), Slope);
                const FVector Forward(Plot.Facing, Grade);
                Verts.Add(FVector(Plot.At(Along, Side), Height(Along, Side)));
                Normals.Add(FVector::CrossProduct(Forward, Tangent).GetSafeNormal());
                UVs.Add(FVector2D(float(J) / 8, (Along - Bed.Min.X) / 100));
                Tangents.Add(FProcMeshTangent(Tangent.GetSafeNormal(), false));
                if (I > 0 && J > 0) Quad(Tris, I * Columns + J, Columns);
            }
        }
        GroundMesh(Soil, Verts, Tris, Normals, UVs, Tangents);

        for (int32 Row = 0; Row < Rows; ++Row)
        {
            // Some rows have just been sown. The remaining crops sit along the raised soil.
            if ((Row + Index) % 5 == 3) continue;
            const bool bBox = Index % 3 == 0 && Row == Rows - 1;
            auto* Crops = Group(VillageMesh(bBox ? TEXT("Vegetables") : TEXT("GardenCrops")), false);
            if (!Crops) continue;
            const FVector SizeCm = Crops->GetStaticMesh()->GetBounds().BoxExtent * 2.0f;
            const float Side = Bed.Min.Y + (Row + 0.5f) * Spacing;
            const int32 Count = FMath::Max(1, FMath::FloorToInt(Size.X / 230.0f));
            const float Pitch = Size.X / Count;
            for (int32 I = 0; I < Count; ++I)
            {
                const float Along = Bed.Min.X + (I + 0.5f) * Pitch + Random.FRandRange(-16, 16);
                const float HeightScale = Random.FRandRange(0.75f, 1.05f) * (bBox ? 0.48f : 1.0f);
                const FVector Scale(0.84f * Pitch / SizeCm.X, 0.71f * Spacing / SizeCm.Y, HeightScale);
                Crops->AddInstance(FTransform(FRotator(0, RowYaw + Random.FRandRange(-8, 8) + (I % 2) * 180, 0),
                    FVector(Plot.At(Along, Side), Height(Along, Side) - 4), Scale), true);
            }
        }
    };

    const TCHAR* Sheds[] = { TEXT("PlankShed"), TEXT("LeanTo"), TEXT("LowShed"), TEXT("WoodShed") };
    for (int32 I = 0; I < V.Plots.Num(); ++I)
    {
        const LakeMap::FVillagePlot& Plot = V.Plots[I];
        const FVector2D F = Plot.Facing;
        const float Yaw = YawOf(F);
        auto At = [&](float Ahead, float Across) { return Plot.At(Ahead, Across); };
        const FString Name = Plot.IsShop() ? TEXT("Shop") : FString::Printf(TEXT("House%02d"), Plot.HouseVariant);
        const float Scale = I == 8 ? 0.92f : I == 9 ? 0.88f : 1.0f;
        Building(*Name, Plot.House, Yaw - 90.0f, FVector(Scale, Scale, 0.79f * Scale));
        Prop(TEXT("Flowers"), At(Plot.Front - 210, 330), Yaw, 0.85f);
        Prop(TEXT("FlowerBush"), At(Plot.Front - 270, -455), Yaw + 12, 0.9f);
        if (Plot.IsShop())
        {
            Building(TEXT("WoodShed"), At(-620, 560), Yaw - 70, FVector(0.46f));
            Fence(TEXT("RailFence"), At(260, 810), At(-850, 810));
            Plant(TEXT("Alder_A"), At(-720, -640), 0.36f, true);
            Plant(TEXT("Shrub_B"), At(380, 640), 0.29f);
            continue;
        }

        const float W = Plot.HalfWidth;
        const FVector2D FL = At(Plot.Front, -W), FR = At(Plot.Front, W);
        const FVector2D BL = At(-Plot.Back, -W), BR = At(-Plot.Back, W);
        Fence(TEXT("Fence"), FL, At(Plot.Front, -145));
        Fence(TEXT("Fence"), At(Plot.Front, 145), FR);
        Fence(TEXT("RailFence"), BL, BR);
        Fence(TEXT("Fence"), FL, BL);
        Fence(TEXT("RailFence"), FR, BR);
        Garden(Plot, I);
        const int32 Kind = I % UE_ARRAY_COUNT(Sheds);
        Building(Sheds[Kind], Plot.Shed(), Yaw - 90, FVector(Kind == 3 ? 0.38f : 0.90f));
        if (I % 3 != 1 && Plot.Back > 1200)
            Prop(TEXT("Clothesline"), At(-Plot.Back + 170, -210), Yaw, 0.95f);

        Plant(I % 2 ? TEXT("Birch_A") : TEXT("Alder_A"), At(Plot.Front - 425, W - 280),
            Random.FRandRange(0.31f, 0.40f), true);
        Plant(TEXT("Shrub_A"), At(Plot.Front - 275, -W + 135), 0.31f);
        Plant(TEXT("Shrub_B"), At(-Plot.Back + 105, -W + 105), 0.23f);
        for (float Along = 20; Along < Plot.Front - 360; Along += 300)
        {
            const float Side = (I % 2 ? -1 : 1) * (W - 140);
            Prop(TEXT("Flowers"), At(Along, Side), Yaw + Random.FRandRange(-30, 30), 0.54f);
        }
    }

    // Irregular verge planting stays away from junctions, gates, and the fenced lots.
    TArray<FVector2D> VergePlants;
    for (const LakeMap::FVillagePath& Path : V.Paths)
    {
        if (!Path.bRoad) continue;
        for (int32 I = 5; I + 5 < Path.Line.Num(); I += 6)
        {
            const FVector2D Direction = (Path.Line[I + 1] - Path.Line[I - 1]).GetSafeNormal();
            const FVector2D Across(-Direction.Y, Direction.X);
            for (float Sign : { -1.0f, 1.0f })
            {
                if (Random.FRand() < 0.26f) continue;
                const FVector2D P = Path.Line[I] + Across * (Sign * (Path.Width * 0.5f + Random.FRandRange(145, 310)));
                if (P.X < -3800 || P.Y > 18800 || LakeMap::VillageYard(P, 155)
                    || LakeMap::VillagePaved(P, 90) || LakeMap::ShoreDistance(P.X, P.Y) > -550) continue;
                if (VergePlants.ContainsByPredicate([&P](const FVector2D& Other) { return FVector2D::DistSquared(P, Other) < FMath::Square(405.0f); }))
                    continue;
                VergePlants.Add(P);
                if (Random.FRand() < 0.22f && !LakeMap::VillageYard(P, 380) && !LakeMap::VillagePaved(P, 215))
                    Plant(TEXT("Birch_B"), P, Random.FRandRange(0.27f, 0.40f), true);
                Prop(TEXT("Flowers"), P + Across * (Sign * 55), Random.FRandRange(0, 360), 0.42f);
            }
        }
    }
    Plant(TEXT("Birch_A"), { 9510, 6670 }, 0.49f, true);
    Plant(TEXT("Birch_B"), { 9900, 6900 }, 0.39f, true);
    Plant(TEXT("Shrub_A"), { 9100, 6510 }, 0.41f);
    Plant(TEXT("Shrub_B"), { 9290, 6350 }, 0.34f);

    // The narrow bank beyond the inlet holds gardens and sheds, with open access to the water.
    const FVector2D WoodLot(-8500, 15650), Cabin(-13450, 16000);
    Building(TEXT("WoodShed"), WoodLot, -25, FVector(1));
    Building(TEXT("Cabin"), Cabin, -30, FVector(1));
    for (int32 I = 0; I < 4; ++I)
        Prop(TEXT("Vegetables"), Cabin + FVector2D(510 + I * 180, -170), -10);
    Fence(TEXT("RailFence"), Cabin + FVector2D(270, -380), Cabin + FVector2D(1220, -380));

    // A few rocks and reeds away from the pier approach and casting lanes.
    const FVector2D ShoreSamples[] = { { 7300, 2700 }, { 6200, 3100 }, { 1400, 2800 },
        { -1500, 3950 }, { -4000, 6250 }, { -6000, 9800 }, { -9200, 13600 }, { -13800, 13400 } };
    for (int32 I = 0; I < UE_ARRAY_COUNT(ShoreSamples); ++I)
    {
        const LakeMap::FShorePoint Shore = LakeMap::NearestShore(ShoreSamples[I]);
        const FVector2D Bank = Shore.Location + Shore.Outward * (130.0f + 35.0f * (I % 3));
        if (LakeMap::IsWater(Bank.X, Bank.Y) || LakeMap::SpotAmount(Bank) > 0.20f) continue;
        const float Yaw = Random.FRandRange(0, 360);
        Prop(I % 2 ? TEXT("FlatRock") : TEXT("Boulders"), Bank, Yaw, 0.66f + 0.15f * (I % 3), true);
        Prop(TEXT("Reeds"), Shore.Location + Shore.Outward * 45, Yaw + 90, 0.94f);
        if (I % 3 == 0) Prop(TEXT("Branch"), Bank + Shore.Outward * 140, Yaw, 0.85f);
        const FVector2D Water = Shore.Location - Shore.Outward * 130.0f;
        if (LakeMap::IsWater(Water.X, Water.Y) && LakeMap::Depth(Water.X, Water.Y) < 85.0f)
            if (auto* Lilies = Group(VillageMesh(TEXT("Lilies")), false))
                Lilies->AddInstance(FTransform(FRotator(0, Yaw, 0), FVector(Water, LakeMap::SurfaceZ + 1.0f), FVector(0.68f)), true);
    }

    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LAKESIDE %d village plots, %d paths"), V.Plots.Num(), V.Paths.Num());
}

#if !UE_BUILD_SHIPPING
void ALakeVillage::RunPreview()
{
    const LakeMap::FVillage& V = LakeMap::Village();
    // The game mode creates the weather actor after the environment and village.
    FTimerHandle Light;
    GetWorldTimerManager().SetTimer(Light, [this]
    {
        if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
        {
            FLakeWeatherState State;
            State.Cloud = 0.30f; State.Fog = 0; State.Rain = 0; State.Wind = 0.12f; State.WindYaw = 40;
            Weather->SetWeatherNow(State);
            Weather->SetHours(11.0f);
        }
    }, 1.0f, false);
    const auto Shot = [this](const TCHAR* File, const FVector& Eye, const FVector& Target, float Fov, float At)
    {
        FTimerHandle Position;
        GetWorldTimerManager().SetTimer(Position, [this, Eye, Target, Fov]
        {
            if (auto* Cam = GetWorld()->SpawnActor<ACameraActor>(Eye, (Target - Eye).Rotation()))
            {
                Cam->GetCameraComponent()->SetFieldOfView(Fov);
                if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
                {
                    PC->SetViewTarget(Cam);
                    if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
                }
            }
        }, At, false);
        FTimerHandle Capture;
        const FString Output = FPaths::ProjectSavedDir() / File;
        GetWorldTimerManager().SetTimer(Capture, [Output]
        {
            FScreenshotRequest::RequestScreenshot(Output, false, false);
        }, At + 2.0f, false);
    };
    const LakeMap::FPier& Pier = LakeMap::Pier();
    const auto& Shop = V.Plots.Last();
    if (FParse::Param(FCommandLine::Get(), TEXT("VillageLawnPreview")))
    {
        const auto& Yard = V.Plots[1];
        const FVector2D Eye = Yard.At(-Yard.Back + 95, -25), Target = Yard.At(-680, 335);
        Shot(TEXT("VillageLawn_Ground.png"), FVector(Eye, Ground(Eye) + 165),
            FVector(Target, Ground(Target) + 70), 69, 4);
        const FVector2D RoadEye(11290, 7480), RoadTarget(11260, 10800);
        Shot(TEXT("VillageLawn_Road.png"), FVector(RoadEye, Ground(RoadEye) + 165),
            FVector(RoadTarget, Ground(RoadTarget) + 60), 71, 8);
        Shot(TEXT("VillageLawn_Overview.png"), FVector(6400, -5850, 14500), FVector(6400, 11500, 220), 67, 12);
        FTimerHandle Check;
        GetWorldTimerManager().SetTimer(Check, [this]
        {
            const auto& Village = LakeMap::Village();
            int32 Checked = 0, Blocked = 0, Wet = 0;
            for (const auto& Path : Village.Paths)
            {
                // Skip the last few steps onto each porch.
                for (int32 I = 1; I + (Path.bRoad ? 0 : 4) < Path.Line.Num(); ++I)
                {
                    const FVector2D& P = Path.Line[I];
                    ++Checked;
                    if (LakeMap::IsWater(P.X, P.Y)) ++Wet;
                    TArray<FOverlapResult> Hits;
                    if (GetWorld()->OverlapMultiByChannel(Hits, FVector(P, Ground(P) + 94), FQuat::Identity,
                        ECC_Pawn, FCollisionShape::MakeCapsule(34, 64)))
                    {
                        ++Blocked;
                        for (const auto& Hit : Hits)
                        {
                            if (!Hit.bBlockingHit) continue;
                            const auto* Mesh = Cast<UStaticMeshComponent>(Hit.GetComponent());
                            UE_LOG(LogTemp, Warning, TEXT("QUIET_LAKE_LAKESIDE blocked path at %.0f %.0f by %s / %s"),
                                P.X, P.Y, *GetNameSafe(Hit.GetActor()),
                                Mesh ? *GetNameSafe(Mesh->GetStaticMesh()) : *GetNameSafe(Hit.GetComponent()));
                        }
                    }
                }
            }
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LAKESIDE checked %d path points, %d blocked, %d underwater"),
                Checked, Blocked, Wet);
        }, 3.0f, false);
        FTimerHandle Exit;
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 16.0f, false);
        return;
    }
    FTimerHandle CollisionCheck;
    GetWorldTimerManager().SetTimer(CollisionCheck, [this]
    {
        const LakeMap::FPier& P = LakeMap::Pier();
        const float DeckZ = LakeMap::SurfaceZ + 85.0f;
        int32 DeckHits = 0, BuildingHits = 0;
        FHitResult Hit;
        for (float Along : { 50.0f, P.Length * 0.5f, P.Length - 50.0f })
        {
            const FVector2D XY = P.Root + P.Dir * Along;
            if (GetWorld()->LineTraceSingleByChannel(Hit, FVector(XY, DeckZ + 8), FVector(XY, DeckZ - 8), ECC_Visibility)
                && Hit.GetComponent() && Hit.GetComponent()->GetFName() == TEXT("VillagePierWalkSurface")) ++DeckHits;
        }
        for (const LakeMap::FVillagePlot& Plot : LakeMap::Village().Plots)
        {
            const float Z = Ground(Plot.House);
            if (GetWorld()->LineTraceSingleByChannel(Hit, FVector(Plot.House, Z + 800), FVector(Plot.House, Z + 60), ECC_Visibility)
                && Hit.GetActor() == this) ++BuildingHits;
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LAKESIDE collision pier %d/3, buildings %d/%d"),
            DeckHits, BuildingHits, LakeMap::Village().Plots.Num());
    }, 3.0f, false);
    if (FParse::Param(FCommandLine::Get(), TEXT("VillageTexturePreview")))
    {
        const auto& Plot = V.Plots[0];
        const FVector2D Fence = Plot.At(Plot.Front, Plot.HalfWidth * 0.55f);
        const FVector2D FenceEye = Fence + Plot.Facing * 190 + Plot.Across() * 35;
        Shot(TEXT("VillageTextures_Fence.png"), FVector(FenceEye, Ground(FenceEye) + 145),
            FVector(Fence, Ground(Fence) + 95), 65, 4);
        const FVector2D Shed = Plot.Shed();
        const FVector2D ShedEye = Shed + Plot.Facing * 470 - Plot.Across() * 185;
        Shot(TEXT("VillageTextures_Shed.png"), FVector(ShedEye, Ground(ShedEye) + 170),
            FVector(Shed, Ground(Shed) + 150), 65, 8);
        const auto& House = V.Plots[2];
        const FVector2D HouseEye = House.At(-120, 780);
        Shot(TEXT("VillageTextures_House.png"), FVector(HouseEye, Ground(HouseEye) + 190),
            FVector(House.House, Ground(House.House) + 215), 65, 12);
        const FVector2D RoofEye = House.At(-480, 360);
        Shot(TEXT("VillageTextures_Roof.png"), FVector(RoofEye, Ground(RoofEye) + 640),
            FVector(House.At(-60, 0), Ground(House.House) + 460), 67, 16);
        FTimerHandle Exit;
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 21.0f, false);
        return;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("VillageDetails")))
    {
        auto Porch = [&Shot, &V](int32 PlotIndex, const TCHAR* File, float At, float Lateral)
        {
            const LakeMap::FVillagePlot& Plot = V.Plots[PlotIndex];
            const FVector2D Side(-Plot.Facing.Y, Plot.Facing.X);
            const FVector2D Eye = Plot.House + Plot.Facing * 770 + Side * Lateral;
            Shot(File, FVector(Eye, Ground(Eye) + 160), FVector(Plot.House, Ground(Plot.House) + 165), 74, At);
        };
        Porch(6, TEXT("LakesideVillage_PorchRepair.png"), 4, -210);
        Porch(5, TEXT("LakesideVillage_FoundationRepair.png"), 8, 465);
        Porch(0, TEXT("LakesideVillage_HouseDetail.png"), 12, -440);
        const FVector2D Shed = V.Plots[0].Shed();
        const FVector2D Eye = Shed - V.Plots[0].Facing * 400 + FVector2D(90, 210);
        Shot(TEXT("LakesideVillage_ShedDetail.png"), FVector(Eye, Ground(Eye) + 160), FVector(Shed, Ground(Shed) + 130), 65, 16);
        FTimerHandle Exit;
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 21.0f, false);
        return;
    }
    Shot(TEXT("LakesideVillage_Air.png"), FVector(4200, -9100, 13400), FVector(1500, 10300, 220), 74, 4);
    Shot(TEXT("LakesideVillage_Lane.png"), FVector(4300, 8780, Ground({4300, 8780}) + 165),
        FVector(7900, 12100, Ground({7900, 12100}) + 240), 72, 8);
    const FVector2D From = Shop.House + Shop.Facing * 1080 + FVector2D(-Shop.Facing.Y, Shop.Facing.X) * 420;
    Shot(TEXT("LakesideVillage_Shop.png"), FVector(From, Ground(From) + 165),
        FVector(Shop.House, Ground(Shop.House) + 230), 69, 12);
    const FVector2D PierEye = Pier.Root + Pier.Dir * 1100 + FVector2D(-Pier.Dir.Y, Pier.Dir.X) * 480;
    Shot(TEXT("LakesideVillage_Pier.png"), FVector(PierEye, LakeMap::SurfaceZ + 255),
        FVector(Pier.Root - Pier.Dir * 490, LakeMap::SurfaceZ + 180), 71, 16);
    FTimerHandle Exit;
    GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 21.0f, false);
}
#endif
