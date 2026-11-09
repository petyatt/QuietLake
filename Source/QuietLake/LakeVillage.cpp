#include "LakeVillage.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"

namespace
{
    float Ground(const FVector2D& P) { return LakeMap::Ground(P.X, P.Y); }
    float YawOf(const FVector2D& Dir) { return FMath::RadiansToDegrees(FMath::Atan2(Dir.Y, Dir.X)); }
    FRotator Along(const FVector2D& Dir)                    // a cylinder whose axis is Dir
    {
        return FRotationMatrix::MakeFromZ(FVector(Dir, 0)).Rotator();
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

    auto Material = [](const TCHAR* Name)
    {
        return LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Materials/%s.%s"), Name, Name));
    };
    // A group of engine primitives under one material: one draw call for many posts, crates or cabbages.
    auto Group = [this](const TCHAR* Shape, const TCHAR* Mat, bool bCollision)
    {
        auto* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Shape, Shape)));
        Component->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Materials/%s.%s"), Mat, Mat)));
        Component->SetCollisionEnabled(bCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
        Component->SetCollisionResponseToAllChannels(ECR_Block);
        Component->RegisterComponent();
        AddInstanceComponent(Component);
        return Component;
    };
    auto Add = [](UHierarchicalInstancedStaticMeshComponent* G, const FVector& Location, const FVector& Scale, const FRotator& Rotation = FRotator::ZeroRotator)
    {
        if (G) G->AddInstance(FTransform(Rotation, Location, Scale));
    };
    auto* Bark = Group(TEXT("Cylinder"), TEXT("M_OldBark"), true);       // posts, poles, logs
    auto* Wood = Group(TEXT("Cube"), TEXT("M_WeatheredWood"), true);     // boards, counters, crates
    auto* Planks = Group(TEXT("Cube"), TEXT("M_Planks"), true);           // clean planks of the tables
    auto* Stone = Group(TEXT("Cube"), TEXT("M_Stone"), true);            // house footings
    auto* Soil = Group(TEXT("Cube"), TEXT("M_Soil"), false);             // garden beds
    auto* Canvas = Group(TEXT("Cube"), TEXT("M_Canvas"), false);         // awnings and sign boards
    auto* Green = Group(TEXT("Sphere"), TEXT("M_Leaves"), false);        // cabbages and apple crowns
    auto* Tops = Group(TEXT("Cone"), TEXT("M_Leaves"), false);           // carrot tops
    auto* Metal = Group(TEXT("Cylinder"), TEXT("M_Metal"), true);        // buckets and hoops
    auto* Barrels = Group(TEXT("Cylinder"), TEXT("M_WeatheredWood"), true);
    auto* Fence = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
    Fence->SetupAttachment(RootComponent);
    Fence->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Village/SM_Fence.SM_Fence")));
    Fence->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Fence->SetCastShadow(false);
    Fence->RegisterComponent();
    AddInstanceComponent(Fence);

    // A strip of road or path laid over the hill in short steps, so it follows the ground.
    auto Ribbon = [this, &Material](const TArray<FVector2D>& Line, float Width, const TCHAR* Mat, float Up)
    {
        TArray<FVector2D> Dense;
        for (int32 I = 0; I + 1 < Line.Num(); ++I)
        {
            const float Len = FVector2D::Distance(Line[I], Line[I + 1]);
            const int32 Steps = FMath::Max(1, FMath::CeilToInt(Len / 250.0f));
            for (int32 K = 0; K < Steps; ++K) Dense.Add(FMath::Lerp(Line[I], Line[I + 1], (float)K / Steps));
        }
        Dense.Add(Line.Last());
        TArray<FVector> Verts, Normals;
        TArray<FVector2D> UVs;
        TArray<int32> Tris;
        TArray<FLinearColor> Colors;
        TArray<FProcMeshTangent> Tangents;
        float Along2 = 0;
        for (int32 I = 0; I < Dense.Num(); ++I)
        {
            const FVector2D Prev = I > 0 ? Dense[I - 1] : Dense[I];
            const FVector2D Next = I + 1 < Dense.Num() ? Dense[I + 1] : Dense[I];
            const FVector2D Dir = (Next - Prev).GetSafeNormal();
            const FVector2D Side2(-Dir.Y, Dir.X);
            if (I > 0) Along2 += FVector2D::Distance(Dense[I], Dense[I - 1]);
            for (int32 S = 0; S < 2; ++S)
            {
                const FVector2D Q = Dense[I] + Side2 * (S == 0 ? -0.5f : 0.5f) * Width;
                Verts.Add(FVector(Q, Ground(Q) + Up));
                Normals.Add(FVector::UpVector);
                UVs.Add(FVector2D((float)S, Along2 / 100));
                Colors.Add(FLinearColor::White);
                Tangents.Add(FProcMeshTangent(FVector(Dir, 0), false));
            }
            if (I > 0) { const int32 A = (I - 1) * 2; Tris.Append({ A, A + 1, A + 2, A + 1, A + 3, A + 2 }); }
        }
        auto* Component = NewObject<UProceduralMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->RegisterComponent();
        AddInstanceComponent(Component);
        Component->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UVs, Colors, Tangents, false);
        Component->SetMaterial(0, Material(Mat));
        Component->SetCastShadow(false);
    };
    // A trodden patch (the market square): a grid over the ground, so it cannot clip into the hill.
    auto Patch = [this, &Material](const FVector2D& C, const FVector2D& AlongDir, const FVector2D& Across,
        float HalfAlong, float HalfAcross, const TCHAR* Mat)
    {
        const int32 N = 8, M = 6;
        TArray<FVector> Verts, Normals;
        TArray<FVector2D> UVs;
        TArray<int32> Tris;
        TArray<FLinearColor> Colors;
        TArray<FProcMeshTangent> Tangents;
        for (int32 J = 0; J <= M; ++J)
            for (int32 I = 0; I <= N; ++I)
            {
                const FVector2D P = C + AlongDir * (FMath::Lerp(-HalfAlong, HalfAlong, (float)J / M))
                    + Across * (FMath::Lerp(-HalfAcross, HalfAcross, (float)I / N));
                Verts.Add(FVector(P, Ground(P) + 6.0f));
                Normals.Add(FVector::UpVector);
                UVs.Add(FVector2D((float)I / N, (float)J / M));
                Colors.Add(FLinearColor::White);
                Tangents.Add(FProcMeshTangent(FVector(AlongDir, 0), false));
            }
        for (int32 J = 0; J < M; ++J)
            for (int32 I = 0; I < N; ++I)
            {
                const int32 A = J * (N + 1) + I, B = A + 1, Cc = A + N + 1, D = Cc + 1;
                Tris.Append({ A, B, Cc, B, D, Cc });
            }
        auto* Component = NewObject<UProceduralMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->RegisterComponent();
        AddInstanceComponent(Component);
        Component->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UVs, Colors, Tangents, false);
        Component->SetMaterial(0, Material(Mat));
        Component->SetCastShadow(false);
    };
    auto FenceLine = [&Add, &Fence](const FVector2D& A, const FVector2D& B)
    {
        const float Len = FVector2D::Distance(A, B);
        if (Len < 30) return;
        const FVector2D Dir = (B - A).GetSafeNormal();
        const int32 Count = FMath::Max(1, FMath::CeilToInt(Len / 300.0f));
        for (int32 K = 0; K < Count; ++K)
        {
            const FVector2D P0 = A + Dir * (Len * K / Count);
            const FVector2D P1 = A + Dir * (Len * (K + 1) / Count);
            const float Z0 = Ground(P0), Z1 = Ground(P1);
            const float Pitch = FMath::RadiansToDegrees(FMath::Atan2(Z1 - Z0, FMath::Max(1.0f, FVector2D::Distance(P0, P1))));
            Add(Fence, FVector(P0, Z0 + 2), FVector(FVector2D::Distance(P0, P1) / 300.0f, 1, 1), FRotator(Pitch, YawOf(Dir), 0));
        }
    };
    auto MeshProp = [this](const TCHAR* Path, const FVector& Location, const FRotator& Rotation, bool bCollision)
    {
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, Path);
        if (!Mesh) return (UStaticMeshComponent*)nullptr;
        auto* Component = NewObject<UStaticMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetStaticMesh(Mesh);
        Component->SetCollisionEnabled(bCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
        Component->SetCollisionResponseToAllChannels(ECR_Block);
        Component->RegisterComponent();
        AddInstanceComponent(Component);
        Component->SetWorldLocationAndRotation(Location, Rotation);
        return Component;
    };
    // A painted sign: our own quad (Tools/GeneratedSigns, drawn with a Cyrillic font), its UVs ours to set.
    auto SignPlate = [this](const TCHAR* Name, const FVector2D& P, float Up, const FVector2D& Facing, float Width, float Height)
    {
        UMaterialInterface* SignBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Village/M_SignText.M_SignText"));
        UTexture2D* SignTexture = LoadObject<UTexture2D>(nullptr, *FString::Printf(TEXT("/Game/Village/Signs/T_Sign_%s.T_Sign_%s"), Name, Name));
        if (!SignBase || !SignTexture) return;
        const FVector Centre(P + Facing * 8.0f, Ground(P) + Up);
        const FVector Fwd(Facing, 0);
        const FVector Right = FVector::UpVector.Cross(Fwd).GetSafeNormal();   // to the right for someone facing the sign
        const FVector L = Centre - Right * (Width * 0.5f), R = Centre + Right * (Width * 0.5f);
        TArray<FVector> Verts = {
            L - FVector(0, 0, Height * 0.5f), L + FVector(0, 0, Height * 0.5f),
            R - FVector(0, 0, Height * 0.5f), R + FVector(0, 0, Height * 0.5f) };
        TArray<FVector2D> UVs = { FVector2D(0.0f, 1.0f), FVector2D(0.0f, 0.0f), FVector2D(1.0f, 1.0f), FVector2D(1.0f, 0.0f) };
        TArray<FVector> Normals = { Fwd, Fwd, Fwd, Fwd };
        TArray<int32> Tris = { 0, 1, 2, 1, 3, 2 };
        TArray<FLinearColor> Colors = { FLinearColor::White, FLinearColor::White, FLinearColor::White, FLinearColor::White };
        TArray<FProcMeshTangent> Tangents = { FProcMeshTangent(Fwd, false), FProcMeshTangent(Fwd, false),
            FProcMeshTangent(Fwd, false), FProcMeshTangent(Fwd, false) };
        auto* Plate = NewObject<UProceduralMeshComponent>(this);
        Plate->SetupAttachment(RootComponent);
        Plate->RegisterComponent();
        AddInstanceComponent(Plate);
        Plate->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UVs, Colors, Tangents, false);
        UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(SignBase, this);
        Mid->SetTextureParameterValue(TEXT("Sign"), SignTexture);
        Plate->SetMaterial(0, Mid);
        Plate->SetCastShadow(false);
    };
    // A sign over the porch of a shop.
    auto ShopSign = [&](const FVector2D& House, const FVector2D& Facing, const TCHAR* Name)
    {
        SignPlate(Name, House + Facing * 376.0f, 352.0f, Facing, 240.0f, 52.0f);
    };

    // --- streets and the market square ----------------------------------------------------------------
    Ribbon(V.MainStreet, V.StreetHalfWidth * 2, TEXT("M_VillageRoad"), 7);
    Ribbon(V.CrossStreet, 520.0f, TEXT("M_VillageRoad"), 7);
    Ribbon(V.PierPath, 220.0f, TEXT("M_Path"), 6);
    Patch(V.Market, V.Axis, V.Side, V.MarketHalfAlong, V.MarketHalfAcross, TEXT("M_Path"));

    // --- the houses, their gardens and fences ---------------------------------------------------------
    int32 Shops = 0;
    for (int32 I = 0; I < V.Plots.Num(); ++I)
    {
        const LakeMap::FVillagePlot& P = V.Plots[I];
        const bool bFishing = P.Kind == 1;
        const bool bStore = P.Kind == 2;
        if (bFishing || bStore) ++Shops;
        const float Yaw = YawOf(P.Facing);

        // The house stands on a stone footing that takes up the slope of the hill.
        float Low = 1e9f, High = -1e9f;
        for (float DX : { -3.8f, 3.8f })
            for (float DY : { -3.4f, 3.4f })
            {
                const FVector2D Q = P.House + FVector2D(DX, DY).GetRotated(Yaw);
                Low = FMath::Min(Low, Ground(Q));
                High = FMath::Max(High, Ground(Q));
            }
        High = FMath::Max(High, Ground(P.House));
        const float Top = High + 2.0f, Bottom = Low - 60.0f;
        Add(Stone, FVector(P.House, (Top + Bottom) * 0.5f), FVector(8.4f, 7.6f, (Top - Bottom) / 100.0f), FRotator(0, Yaw, 0));
        const float HouseScale = 0.96f + Random.FRandRange(0.0f, 0.09f);
        if (UStaticMeshComponent* House = MeshProp(TEXT("/Game/Village/SM_Izba.SM_Izba"), FVector(P.House, Top),
            FRotator(0, Yaw + Random.FRandRange(-2.0f, 2.0f), 0), true))
            House->SetWorldScale3D(FVector(HouseScale));

        // The front and back fences with a gate in the front; the side fences close the plot.
        const FVector2D FrontLine = P.House + P.Facing * P.Front;
        const FVector2D BackLine = P.House - P.Facing * P.Back;
        const FVector2D Left = -V.Axis * P.HalfWidth, Right = V.Axis * P.HalfWidth;
        FenceLine(FrontLine + Left, P.Gate - V.Axis * 130);
        FenceLine(P.Gate + V.Axis * 130, FrontLine + Right);
        FenceLine(BackLine + Left, BackLine + Right);
        FenceLine(FrontLine + Left, BackLine + Left);
        FenceLine(FrontLine + Right, BackLine + Right);
        for (float S : { -130.0f, 130.0f })
        {
            const FVector2D Q = P.Gate + V.Axis * S;
            Add(Bark, FVector(Q, Ground(Q) + 75.0f), FVector(0.13f, 0.13f, 1.5f));
        }
        Add(Bark, FVector(P.Gate, Ground(P.Gate) + 150.0f), FVector(0.09f, 0.09f, 2.6f), Along(V.Axis));
        // Paths: from the street to the gate, and from the gate to the porch.
        Ribbon({ P.Gate + P.Facing * 475.0f, P.Gate }, 170.0f, TEXT("M_Path"), 6);
        Ribbon({ P.Gate, P.House + P.Facing * 620.0f }, 160.0f, TEXT("M_Path"), 6);

        // The garden behind the house: three long beds with cabbages and carrots.
        for (int32 Bed = -1; Bed <= 1; ++Bed)
        {
            const FVector2D BedCentre = P.Garden + P.Facing * (Bed * 220.0f);
            Add(Soil, FVector(BedCentre, Ground(BedCentre) + 7.0f), FVector(12.0f, 1.7f, 0.14f), FRotator(0, YawOf(V.Axis), 0));
            for (int32 K = 0; K < 7; ++K)
                for (int32 R = -1; R <= 1; R += 2)
                {
                    const FVector2D Q = BedCentre + V.Axis * (-480.0f + K * 160.0f + Random.FRandRange(-20.0f, 20.0f))
                        + P.Facing * (R * 35.0f + Random.FRandRange(-8.0f, 8.0f));
                    if (Bed == 1) Add(Tops, FVector(Q, Ground(Q) + 26.0f), FVector(0.15f, 0.15f, 0.2f));
                    else Add(Green, FVector(Q, Ground(Q) + 20.0f), FVector(0.32f, 0.32f, 0.28f));
                }
        }
        // A woodpile and a barrel by the house.
        for (int32 K = 0; K < 6; ++K)
            Add(Wood, FVector(P.House + V.Axis * (400.0f + (K % 3) * 32.0f) + P.Facing * (K / 3) * 34.0f + V.Side * Random.FRandRange(-8.0f, 8.0f),
                Ground(P.House) + 16.0f + (K / 3) * 32.0f), FVector(0.3f, 0.3f, 0.3f), FRotator(Random.FRandRange(0.0f, 20.0f), Random.FRandRange(0.0f, 360.0f), 0));
        Add(Barrels, FVector(P.House + V.Axis * (-500.0f) + P.Facing * (-300.0f), Ground(P.House) + 44.0f), FVector(0.6f, 0.6f, 0.9f));

        // Every plot has a fruit tree or a willow at the side of the garden (real tree meshes).
        if (I % 3 != 2)
        {
            const FVector2D T = P.Garden + V.Axis * 560.0f;
            const float TreeScale = I % 3 == 0 ? 0.42f : 0.5f;
            const TCHAR* TreePath = I % 3 == 0 ? TEXT("/Game/Plants/Trees/SM_Oak.SM_Oak") : TEXT("/Game/Plants/Trees/SM_Willow.SM_Willow");
            if (UStaticMeshComponent* Tree = MeshProp(TreePath, FVector(T, Ground(T)), FRotator(0, Random.FRandRange(0.0f, 360.0f), 0), false))
                Tree->SetWorldScale3D(FVector(TreeScale));
        }

        // The shops: a sign over the porch and the goods outside.
        if (bFishing)
        {
            ShopSign(P.House, P.Facing, TEXT("FishingShop"));
            MeshProp(TEXT("/Game/Village/SM_NetRack.SM_NetRack"), FVector(P.House + V.Axis * 720.0f, Ground(P.House + V.Axis * 720.0f)), FRotator(0, YawOf(V.Axis), 0), false);
            // An old boat turned upside down on two trestles by the shop.
            const FVector2D B = P.House - V.Axis * 760.0f + P.Facing * 60.0f;
            Add(Wood, FVector(B + V.Axis * 120.0f, Ground(B) + 34.0f), FVector(0.2f, 1.2f, 0.2f));
            Add(Wood, FVector(B - V.Axis * 120.0f, Ground(B) + 34.0f), FVector(0.2f, 1.2f, 0.2f));
            MeshProp(TEXT("/Game/Village/SM_Boat_B.SM_Boat_B"), FVector(B, Ground(B) + 92.0f), FRotator(0, YawOf(V.Axis), 180), true);
            for (int32 K = 0; K < 4; ++K)
            {
                const FVector2D R = P.House + P.Facing * 330.0f + V.Axis * (150.0f + K * 28.0f);
                Add(Bark, FVector(R, Ground(R) + 110.0f), FVector(0.035f, 0.035f, 2.2f), FRotator(10, YawOf(V.Axis), 8));
            }
            SignPlate(TEXT("Tackle"), P.House + P.Facing * 600.0f + V.Axis * 700.0f, 150.0f, P.Facing, 130.0f, 56.0f);
        }
        if (bStore)
        {
            ShopSign(P.House, P.Facing, TEXT("Store"));
            for (int32 K = 0; K < 5; ++K)
                Add(Wood, FVector(P.House + V.Axis * (500.0f + K * 42.0f), Ground(P.House) + 45.0f), FVector(0.55f, 0.55f, 0.55f), FRotator(0, Random.FRandRange(0.0f, 90.0f), 0));
            for (int32 K = 0; K < 3; ++K)
                Add(Barrels, FVector(P.House - V.Axis * (520.0f + K * 80.0f), Ground(P.House) + 44.0f), FVector(0.6f, 0.6f, 0.9f));
            for (int32 K = 0; K < 3; ++K)
                Add(Canvas, FVector(P.House + V.Axis * (300.0f + K * 60.0f), Ground(P.House) + 24.0f), FVector(0.6f, 0.45f, 0.9f), FRotator(0, Random.FRandRange(0.0f, 360.0f), 0));
        }
    }

    // The village's name over the entrance from the pier.
    {
        const FVector2D Entrance = V.MainStreet.Last();
        for (float S : { -190.0f, 190.0f })
        {
            const FVector2D Q = Entrance + V.Side * S;
            Add(Bark, FVector(Q, Ground(Q) + 130.0f), FVector(0.15f, 0.15f, 2.6f));
        }
        SignPlate(TEXT("Village"), Entrance, 268.0f, V.Axis, 380.0f, 82.0f);
    }
    // --- the market square: stalls, tables, fish and the well -------------------------------------------
    auto Stall = [&](const FVector2D& C, const FVector2D& Facing)
    {
        const float Yaw = YawOf(Facing);
        const FVector2D Side2(-Facing.Y, Facing.X);
        Add(Wood, FVector(C + Facing * 40.0f, Ground(C) + 95.0f), FVector(0.8f, 2.4f, 0.08f), FRotator(0, Yaw, 0));
        Add(Wood, FVector(C + Facing * 40.0f, Ground(C) + 70.0f), FVector(0.7f, 2.3f, 0.05f), FRotator(0, Yaw, 0));
        for (float S : { -1.05f, 1.05f })
            for (float F : { -0.55f, 0.65f })
            {
                const FVector2D Q = C + Side2 * (S * 100.0f) + Facing * (F * 100.0f);
                Add(Bark, FVector(Q, Ground(Q) + 110.0f), FVector(0.1f, 0.1f, 2.2f));
            }
        Add(Canvas, FVector(C + Facing * 35.0f, Ground(C) + 240.0f), FVector(2.1f, 3.1f, 0.04f), FRotator(-13, Yaw, 0));
        // The awning frame and the back board of the stall.
        for (float S : { -1.55f, 1.55f })
            Add(Bark, FVector(C + Side2 * (S * 100.0f) + Facing * 90.0f, Ground(C) + 232.0f), FVector(0.07f, 0.07f, 0.5f), FRotator(0, Yaw, 0));
        Add(Canvas, FVector(C - Facing * 150.0f, Ground(C) + 175.0f), FVector(0.05f, 2.6f, 1.1f), FRotator(0, Yaw, 0));
        // Sacks of grain and a crate under the counter.
        for (int32 K = 0; K < 2; ++K)
            Add(Canvas, FVector(C + Side2 * (K == 0 ? -60.0f : 60.0f) - Facing * 120.0f, Ground(C) + 26.0f), FVector(0.55f, 0.55f, 0.75f), FRotator(8, Yaw + 20.0f * K, 0));
        Add(Wood, FVector(C - Facing * 40.0f + Side2 * 180.0f, Ground(C) + 22.0f), FVector(0.45f, 0.45f, 0.45f), FRotator(0, Yaw + 30, 0));
        for (int32 K = 0; K < 3; ++K)
            Add(Wood, FVector(C + Facing * 30.0f + Side2 * (-70.0f + K * 70.0f), Ground(C) + 112.0f), FVector(0.34f, 0.34f, 0.28f), FRotator(0, Random.FRandRange(0.0f, 90.0f), 0));
        Add(Barrels, FVector(C - Facing * 130.0f + Side2 * 130.0f, Ground(C) + 44.0f), FVector(0.6f, 0.6f, 0.9f));
    };
    auto Catch = [&MeshProp, &Random](const FVector2D& P, float Up, float Scale)
    {
        const TCHAR* Names[] = { TEXT("Pike"), TEXT("Perch"), TEXT("Bream"), TEXT("Crucian"), TEXT("Roach"), TEXT("Rudd") };
        const TCHAR* Name = Names[Random.RandRange(0, UE_ARRAY_COUNT(Names) - 1)];
        const FString Path = FString::Printf(TEXT("/Game/Fish/SM_Fish_Meshy_%s.SM_Fish_Meshy_%s"), Name, Name);
        const FRotator Rot(0, Random.FRandRange(0.0f, 360.0f), 0);
        if (UStaticMeshComponent* Fish = MeshProp(*Path, FVector(P, Ground(P) + Up), Rot, false))
            Fish->SetWorldScale3D(FVector(Scale));
    };
    for (int32 K = -1; K <= 1; ++K)
    {
        const FVector2D C = V.Market + V.Axis * (V.MarketHalfAlong - 260.0f) + V.Side * (K * 700.0f);
        const FVector2D F = (V.Market - C).GetSafeNormal();
        Stall(C, F);
        if (K == 0)
            SignPlate(TEXT("Fish"), C + F * 30.0f, 272.0f, F, 130.0f, 56.0f);
        Catch(C + F * 30.0f - FVector2D(-F.Y, F.X) * 30.0f, 132.0f, 0.42f);
        Catch(C + F * 30.0f + FVector2D(-F.Y, F.X) * 35.0f, 132.0f, 0.36f);
    }
    for (int32 K = -1; K <= 1; K += 2)
    {
        const FVector2D C = V.Market - V.Axis * (V.MarketHalfAlong - 260.0f) + V.Side * (K * 700.0f);
        Stall(C, (V.Market - C).GetSafeNormal());
    }
    for (int32 T = -1; T <= 1; T += 2)
    {
        const FVector2D C = V.Market + V.Side * (T * 520.0f) - V.Axis * 260.0f;
        Add(Planks, FVector(C, Ground(C) + 78.0f), FVector(1.0f, 2.8f, 0.07f), FRotator(0, YawOf(V.Axis), 0));
        for (float A : { -1.2f, 1.2f })
            for (float S : { -0.42f, 0.42f })
            {
                const FVector2D Q = C + V.Axis * (A * 100.0f) + V.Side * (S * 100.0f);
                Add(Wood, FVector(Q, Ground(Q) + 38.0f), FVector(0.08f, 0.08f, 0.76f));
            }
        for (int32 K = 0; K < 3; ++K)
            Catch(C + V.Axis * ((K - 1) * 70.0f), 96.0f, 0.34f + 0.06f * (K % 2));
        Add(Wood, FVector(C + V.Side * 80.0f, Ground(C) + 25.0f), FVector(0.5f, 0.5f, 0.5f), FRotator(0, 15, 0));
    }
    // The sign over the lane into the square.
    {
        const FVector2D Arch = V.MarketLane[0] + V.Side * 420.0f;
        for (float S : { -180.0f, 180.0f })
        {
            const FVector2D Q = Arch + V.Axis * S;
            Add(Bark, FVector(Q, Ground(Q) + 140.0f), FVector(0.14f, 0.14f, 2.8f));
        }
        SignPlate(TEXT("Market"), Arch, 290.0f, V.Side, 420.0f, 90.0f);
    }
    // The well at the corner of the square.
    {
        const FVector2D W = V.Well;
        const float Z = Ground(W);
        // The log frame of the well: four walls and four corner posts.
        for (int32 S = 0; S < 4; ++S)
        {
            const FVector2D Out = S == 0 ? V.Axis : S == 1 ? -V.Axis : S == 2 ? V.Side : -V.Side;
            const FVector2D Dir = S < 2 ? V.Side : V.Axis;
            const FVector2D C = W + Out * 165.0f;
            Add(Wood, FVector(C, Z + 48.0f), FVector(3.5f, 0.18f, 1.05f), FRotator(0, YawOf(Dir), 0));
        }
        for (float A : { -1.0f, 1.0f }) for (float S : { -1.0f, 1.0f })
        {
            const FVector2D Q = W + V.Axis * (A * 185.0f) + V.Side * (S * 185.0f);
            Add(Bark, FVector(Q, Z + 115.0f), FVector(0.13f, 0.13f, 2.3f));
        }
        Add(Bark, FVector(W, Z + 205.0f), FVector(0.08f, 0.08f, 1.8f), Along(V.Side));
        Add(Planks, FVector(W + V.Axis * 45.0f, Z + 240.0f), FVector(1.5f, 2.1f, 0.06f), FRotator(-20, YawOf(V.Axis), 0));
        Add(Planks, FVector(W - V.Axis * 45.0f, Z + 240.0f), FVector(1.5f, 2.1f, 0.06f), FRotator(20, YawOf(V.Axis), 0));
        Add(Metal, FVector(W, Z + 135.0f), FVector(0.3f, 0.3f, 0.26f));
        Add(Bark, FVector(W, Z + 172.0f), FVector(0.025f, 0.025f, 1.2f));
    }
    // Benches by the square.
    for (int32 K = -1; K <= 1; K += 2)
    {
        const FVector2D C = V.Market + V.Axis * (K * 520.0f) + V.Side * 780.0f;
        Add(Planks, FVector(C, Ground(C) + 44.0f), FVector(0.5f, 2.0f, 0.06f), FRotator(0, YawOf(V.Axis), 0));
        for (float A : { -0.8f, 0.8f })
        {
            const FVector2D Q = C + V.Axis * (A * 100.0f);
            Add(Bark, FVector(Q, Ground(Q) + 22.0f), FVector(0.14f, 0.14f, 0.44f));
        }
    }
    // A few empty barrels and crates along the street.
    for (int32 K = 0; K < 4; ++K)
    {
        const FVector2D Q = V.Centre + V.Axis * Random.FRandRange(-3500.0f, 3000.0f)
            + V.Side * (Random.FRand() < 0.5f ? -1.0f : 1.0f) * Random.FRandRange(520.0f, 900.0f);
        Add(Barrels, FVector(Q, Ground(Q) + 44.0f), FVector(0.6f, 0.6f, 0.9f));
        Add(Wood, FVector(Q + V.Axis * 90.0f, Ground(Q) + 25.0f), FVector(0.5f, 0.5f, 0.5f), FRotator(0, Random.FRandRange(0.0f, 90.0f), 0));
    }

    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_VILLAGE plots %d, shops %d, street %.0f m, market at (%.0f, %.0f)"),
        V.Plots.Num(), Shops, FVector2D::Distance(V.MainStreet[0], V.MainStreet.Last()) / 100.0f, V.Market.X, V.Market.Y);
}

#if !UE_BUILD_SHIPPING
void ALakeVillage::RunPreview()
{
    const LakeMap::FVillage& V = LakeMap::Village();
    if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
    {
        FLakeWeatherState Clear;
        Clear.Cloud = 0.35f; Clear.Fog = 0; Clear.Rain = 0; Clear.Wind = 0.15f; Clear.WindYaw = 40;
        Weather->SetWeatherNow(Clear);
        Weather->SetHours(11.0f);
    }
    auto Shot = [this](const FString& File, const FVector& Eye, const FVector& LookAt, float Fov)
    {
        auto* Cam = GetWorld()->SpawnActor<ACameraActor>(Eye, (LookAt - Eye).Rotation());
        if (!Cam) return;
        Cam->GetCameraComponent()->SetFieldOfView(Fov);
        if (APlayerController* PC = Cast<APlayerController>(GetWorld()->GetFirstPlayerController()))
        {
            PC->SetViewTarget(Cam);
            if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
        }
        FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / File, true, false);
    };
    struct FShot { float At; FString File; FVector Eye; FVector Look; float Fov; };
    TArray<FShot> Shots;
    const FVector Centre(V.Centre, Ground(V.Centre) + 100.0f);
    {
        FVector Eye = Centre - FVector(V.Axis * 5200.0f, 0.0f) + FVector(V.Side * 250.0f, 0.0f);
        Eye.Z = Ground(V.Centre) + 4500.0f;
        Shots.Add({ 4.0f, TEXT("Village_Air.png"), Eye, Centre, 62 });
    }
    {
        const FVector Eye(V.MainStreet[0] - V.Axis * 300.0f, Ground(V.MainStreet[0]) + 175.0f);
        Shots.Add({ 6.0f, TEXT("Village_Street.png"), Eye, FVector(V.Centre + V.Axis * 1500.0f, Ground(V.Centre) + 150.0f), 70 });
    }
    {
        const FVector Eye(V.Centre + V.Axis * 1400.0f, Ground(V.Centre) + 180.0f);
        Shots.Add({ 8.0f, TEXT("Village_StreetNorth.png"), Eye, FVector(V.MainStreet.Last(), Ground(V.MainStreet.Last()) + 140.0f), 70 });
    }
    {
        const FVector Eye = FVector(V.Market + V.Side * 1600.0f - V.Axis * 1600.0f, Ground(V.Market) + 250.0f);
        Shots.Add({ 10.0f, TEXT("Village_Market.png"), Eye, FVector(V.Market + V.Axis * 200.0f, Ground(V.Market) + 100.0f), 72 });
    }
    for (const LakeMap::FVillagePlot& P : V.Plots)
    {
        if (P.Kind == 0) continue;
        const FString File = P.Kind == 1 ? TEXT("Village_Shop.png") : TEXT("Village_Store.png");
        Shots.Add({ P.Kind == 1 ? 12.0f : 14.0f, File,
            FVector(P.House + P.Facing * 1500.0f + V.Axis * 250.0f, Ground(P.House) + 190.0f),
            FVector(P.House, Ground(P.House) + 170.0f), 68 });
    }
    for (const LakeMap::FVillagePlot& P : V.Plots)
    {
        if (P.Kind != 1) continue;
        Shots.Add({ 17.0f, TEXT("Village_Sign.png"),
            FVector(P.House + P.Facing * 760.0f, Ground(P.House) + 355.0f),
            FVector(P.House + P.Facing * 376.0f, Ground(P.House) + 350.0f), 60 });
    }
    for (const LakeMap::FVillagePlot& P : V.Plots)
    {
        if (P.Kind != 0) continue;
        Shots.Add({ 16.0f, TEXT("Village_Garden.png"),
            FVector(P.Garden + P.Facing * 900.0f + V.Axis * 500.0f, Ground(P.Garden) + 220.0f),
            FVector(P.Garden, Ground(P.Garden) + 60.0f), 70 });
        break;
    }
    for (const FShot& S : Shots)
    {
        FTimerHandle Handle;
        const FShot Copy = S;
        GetWorldTimerManager().SetTimer(Handle, [this, Shot, Copy] { Shot(Copy.File, Copy.Eye, Copy.Look, Copy.Fov); }, S.At, false);
    }
    FTimerHandle Exit;
    GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 20.0f, false);
}
#endif
