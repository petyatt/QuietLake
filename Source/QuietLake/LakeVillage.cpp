#include "LakeVillage.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
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

    TArray<FVector2D> SampleLane(const TArray<FVector2D>& Line)
    {
        TArray<FVector2D> Samples;
        for (int32 I = 0; I + 1 < Line.Num(); ++I)
        {
            const FVector2D A = Line[FMath::Max(0, I - 1)], B = Line[I], C = Line[I + 1];
            const FVector2D D = Line[FMath::Min(Line.Num() - 1, I + 2)];
            const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector2D::Distance(B, C) / 95.0f));
            for (int32 K = 0; K < Steps; ++K)
            {
                const float T = float(K) / Steps;
                Samples.Add(FMath::CubicInterp(B, (C - A) * 0.5f, C, (D - B) * 0.5f, T));
            }
        }
        Samples.Add(Line.Last());
        return Samples;
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
    for (const LakeMap::FVillagePath& Path : V.Paths)
    {
        const TArray<FVector2D> Line = SampleLane(Path.Line);
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
                Verts.Add(FVector(P, Ground(P) + (Path.bRoad ? 5.0f : 7.0f)));
                UVs.Add(FVector2D(U, Distance / 100.0f));
                Normals.Add(FVector::UpVector);
                Tangents.Add(FProcMeshTangent(FVector(Across, 0), false));
                if (I == 0 || J == 0) continue;
                const int32 A = (I - 1) * Columns + J - 1;
                const int32 B = A + 1, C = A + Columns, D = C + 1;
                Tris.Append({ A, B, C, B, D, C });
            }
        }
        auto* Component = NewObject<UProceduralMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->SetCastShadow(false);
        Component->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UVs, {}, Tangents, false);
        Component->SetMaterial(0, Path.bRoad ? Road : Footpath);
        AddInstanceComponent(Component);
        Component->RegisterComponent();
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

    const TCHAR* Sheds[] = { TEXT("PlankShed"), TEXT("LeanTo"), TEXT("LowShed"), TEXT("WoodShed") };
    for (int32 I = 0; I < V.Plots.Num(); ++I)
    {
        const LakeMap::FVillagePlot& Plot = V.Plots[I];
        const FVector2D F = Plot.Facing;
        const FVector2D Side(-F.Y, F.X);
        const float Yaw = YawOf(F);
        auto At = [&](float Ahead, float Across) { return Plot.House + F * Ahead + Side * Across; };
        const FString Name = Plot.bShop ? TEXT("Shop") : FString::Printf(TEXT("House%02d"), I + 1);
        Building(*Name, Plot.House, Yaw - 90.0f, FVector(1.0f, 1.0f, Plot.bShop ? 0.80f : 0.79f));
        Prop(TEXT("Flowers"), At(410, 350), Yaw, 0.85f);
        Prop(TEXT("FlowerBush"), At(200, -490), Yaw + 12, 0.9f);
        if (Plot.bShop)
        {
            Building(TEXT("WoodShed"), At(-110, 560), Yaw - 70, FVector(0.36f));
            Fence(TEXT("RailFence"), At(180, 720), At(-520, 720));
            continue;
        }

        const float W = Plot.HalfWidth;
        const FVector2D FL = At(Plot.Front, -W), FR = At(Plot.Front, W);
        const FVector2D BL = At(-Plot.Back, -W), BR = At(-Plot.Back, W);
        Fence(TEXT("Fence"), FL, At(Plot.Front, -115));
        Fence(TEXT("Fence"), At(Plot.Front, 115), FR);
        Fence(TEXT("RailFence"), BL, BR);
        Fence(TEXT("Fence"), FL, BL);
        Fence(TEXT("RailFence"), FR, BR);
        if (I % 2 == 0)
            Prop(TEXT("Gate"), At(Plot.Front - 65, -100), Yaw + 10.0f, 0.86f, true);
        for (int32 Row = 0; Row < 3; ++Row)
            Prop(TEXT("Vegetables"), At(-Plot.Back + 200 + Row * 155, W - 290), Yaw + 90, 0.96f);
        if (I != 5)
        {
            const FVector2D Shed = At(-Plot.Back + 225, -W + 215);
            const int32 Kind = I % UE_ARRAY_COUNT(Sheds);
            Building(Sheds[Kind], Shed, Yaw - 90, FVector(Kind == 3 ? 0.36f : 0.94f));
        }
        if (I % 3 != 1)
            Prop(TEXT("Clothesline"), At(-Plot.Back + 230, 95), Yaw + 75, 1.05f);
        if (I % 3 != 0)
        {
            if (auto* Birch = Group(TEXT("/Game/Plants/PV/SM_PV_Birch_A.SM_PV_Birch_A"), false))
            {
                const FVector2D P = At(-Plot.Back + 125, W - 125);
                Birch->AddInstance(FTransform(FRotator(0, Random.FRandRange(0, 360), 0), FVector(P, Ground(P)),
                    FVector(Random.FRandRange(0.34f, 0.50f))), true);
            }
        }
    }

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
        const FVector2D Eye(5080, 5480), Target(5820, 6550);
        Shot(TEXT("VillageLawn_Ground.png"), FVector(Eye, Ground(Eye) + 165),
            FVector(Target, Ground(Target) + 70), 69, 4);
        const FVector2D RoadEye(7150, 11250), RoadTarget(9250, 14500);
        Shot(TEXT("VillageLawn_Road.png"), FVector(RoadEye, Ground(RoadEye) + 165),
            FVector(RoadTarget, Ground(RoadTarget) + 60), 71, 8);
        Shot(TEXT("VillageLawn_Overview.png"), FVector(4200, -9100, 13400), FVector(1500, 10300, 220), 74, 12);
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
        const FVector2D Shed = V.Plots[0].House - V.Plots[0].Facing * (V.Plots[0].Back - 225)
            + FVector2D(-V.Plots[0].Facing.Y, V.Plots[0].Facing.X) * (215 - V.Plots[0].HalfWidth);
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
