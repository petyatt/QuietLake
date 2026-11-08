#include "LakeLevel.h"
#include "LakeGame.h"
#include "LakeMap.h"
#include "EngineUtils.h"
#include "Algo/Reverse.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#if WITH_EDITOR
#include "Editor.h"
#include "ActorFactories/ActorFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeSettings.h"
#include "LandscapeEdit.h"
#include "LandscapeBlueprintBrushBase.h"
#include "UObject/Package.h"
#include "Curves/CurveFloat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterBodyIslandActor.h"
#include "WaterBodyRiverComponent.h"
#include "WaterBodyComponent.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#endif

namespace
{
    constexpr int32 SubsectionQuads = 63, Sections = 2, ComponentQuads = SubsectionQuads * Sections;
    constexpr float QuadSize = 100.0f;                     // 1 m between landscape vertices
    /** Landscape heights: Z = (H - 32768) * ScaleZ / 128 with the default ScaleZ of 100 (±256 m). */
    uint16 EncodeHeight(float Z) { return (uint16)FMath::Clamp(FMath::RoundToInt(Z * 128.0f / 100.0f + 32768.0f), 0, 65535); }

#if WITH_EDITOR
    UWorld* EditorWorld() { return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr; }

    /** Spawns an actor of Class through its editor factory, so it gets the class's editor defaults
     *  (water materials, carving curve) and the Water editor sets up its brush manager and zone. */
    template <class T>
    T* SpawnWithFactory(UWorld* World, const FVector& Location)
    {
        FActorSpawnParameters Params;
        UActorFactory* Factory = GEditor->FindActorFactoryForActorClass(T::StaticClass());
        AActor* Actor = Factory ? Factory->CreateActor(T::StaticClass(), World->PersistentLevel, FTransform(Location), Params)
                                : World->SpawnActor<T>(Location, FRotator::ZeroRotator, Params);
        return Cast<T>(Actor);
    }

    /** Our lake water (M_LakeSurface: the peaty colour and murk, the foam, the wind's ripple and the
     *  simulated rings of ALakeRipples) instead of the engine's; and no engine underwater effect,
     *  the game has its own murk (ALakeLife). */
    void UseLakeWater(UWaterBodyComponent* Body)
    {
        if (UMaterialInterface* Surface = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_LakeSurface.M_LakeSurface")))
        {
            Body->SetWaterMaterial(Surface);
            Body->SetWaterStaticMeshMaterial(Surface);
            if (UWaterBodyRiverComponent* River = Cast<UWaterBodyRiverComponent>(Body)) River->SetLakeTransitionMaterial(Surface);
        }
        Body->SetUnderwaterPostProcessMaterial(nullptr);
    }

    void RefreshWaterBody(AWaterBody* Body)
    {
        UWaterSplineComponent* Spline = Body->GetWaterSpline();
        Spline->SynchronizeWaterProperties();
        Spline->PostEditChange();
        Body->GetWaterBodyComponent()->PostEditChange();
        Body->PostEditChange();
    }
#endif
}

UObject* ULakeLevelLibrary::CreateLayerInfo(const FString& Folder, FName LayerName)
{
#if WITH_EDITOR
    const FString Name = FString::Printf(TEXT("LI_%s"), *LayerName.ToString());
    const FString PackageName = Folder / Name;
    if (ULandscapeLayerInfoObject* Existing = LoadObject<ULandscapeLayerInfoObject>(nullptr, *(PackageName + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet))
        return Existing;
    UPackage* Package = CreatePackage(*PackageName);
    auto* Info = NewObject<ULandscapeLayerInfoObject>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
    Info->SetLayerName(LayerName, false);
    Info->SetBlendMethod(ELandscapeTargetLayerBlendMethod::None, false);
    FAssetRegistryModule::AssetCreated(Info);
    Package->MarkPackageDirty();
    return Info;
#else
    return nullptr;
#endif
}

AActor* ULakeLevelLibrary::BuildLandscape(UMaterialInterface* Material, const TArray<UObject*>& LayerInfos)
{
#if WITH_EDITOR
    UWorld* World = EditorWorld();
    if (!World) return nullptr;
    // The water brush manager adds its own edit layer "Water"; by default the editor then opens a modal
    // dialog to place it, and the build script ran on (and saved) under the dialog before the brush
    // was attached. Without the dialog the layer goes on top, as wanted.
    GetMutableDefault<ULandscapeSettings>()->bShowDialogForAutomaticLayerCreation = false;
    for (TActorIterator<ALandscapeProxy> It(World); It; ++It) It->Destroy();

    const FBox2D Bounds = LakeMap::TerrainBounds;
    const int32 CountX = FMath::CeilToInt((Bounds.Max.X - Bounds.Min.X) / QuadSize / ComponentQuads);
    const int32 CountY = FMath::CeilToInt((Bounds.Max.Y - Bounds.Min.Y) / QuadSize / ComponentQuads);
    const int32 SizeX = CountX * ComponentQuads + 1, SizeY = CountY * ComponentQuads + 1;
    TArray<uint16> Heights;
    Heights.SetNumUninitialized(SizeX * SizeY);
    // The designed ground (not a bake of an earlier build), in parallel.
    LakeMap::ShoreDistance(0, 0);
    LakeMap::Ground(0, 0);
    ParallelFor(SizeY, [&](int32 Y)
    {
        for (int32 X = 0; X < SizeX; ++X)
        {
            const FVector2D P = Bounds.Min + FVector2D(X, Y) * QuadSize;
            // Inside the waterline the ground is flat just under the water: the Water lake carves the
            // bowl into it (its brush only lowers, see SpawnLake).
            const float Z = LakeMap::DesignGround(P.X, P.Y);
            Heights[Y * SizeX + X] = EncodeHeight(LakeMap::ShoreDistance(P.X, P.Y) > 0 ? FMath::Max(Z, LakeMap::SurfaceZ - 5) : Z);
        }
    });
    return CreateLandscape(World, Material, LayerInfos, Heights);
#else
    return nullptr;
#endif
}

ALandscape* ULakeLevelLibrary::CreateLandscape(UWorld* World, UMaterialInterface* Material, const TArray<UObject*>& LayerInfos, const TArray<uint16>& Heights)
{
#if WITH_EDITOR
    const FBox2D Bounds = LakeMap::TerrainBounds;
    const int32 CountX = FMath::CeilToInt((Bounds.Max.X - Bounds.Min.X) / QuadSize / ComponentQuads);
    const int32 CountY = FMath::CeilToInt((Bounds.Max.Y - Bounds.Min.Y) / QuadSize / ComponentQuads);
    const int32 SizeX = CountX * ComponentQuads + 1, SizeY = CountY * ComponentQuads + 1;
    if (!World || Heights.Num() != SizeX * SizeY) return nullptr;
    TArray<uint8> Masks[3];
    for (TArray<uint8>& Mask : Masks) Mask.SetNumZeroed(SizeX * SizeY);
    for (int32 Y = 0; Y < SizeY; ++Y)
        for (int32 X = 0; X < SizeX; ++X)
        {
            const FVector2D P = Bounds.Min + FVector2D(X, Y) * QuadSize;
            const int32 I = Y * SizeX + X;
            const FLinearColor L = ALakeEnvironment::TerrainLayers(P);
            Masks[0][I] = (uint8)FMath::RoundToInt(FMath::Clamp(L.R, 0.0f, 1.0f) * 255);
            Masks[1][I] = (uint8)FMath::RoundToInt(FMath::Clamp(L.G, 0.0f, 1.0f) * 255);
            Masks[2][I] = (uint8)FMath::RoundToInt(FMath::Clamp(L.B, 0.0f, 1.0f) * 255);
        }
    TMap<FGuid, TArray<uint16>> HeightData;
    HeightData.Add(FGuid(), Heights);
    TArray<FLandscapeImportLayerInfo> Layers;
    for (int32 L = 0; L < FMath::Min(3, LayerInfos.Num()); ++L)
        if (auto* Info = Cast<ULandscapeLayerInfoObject>(LayerInfos[L]))
        {
            FLandscapeImportLayerInfo& Layer = Layers.Emplace_GetRef(Info->GetLayerName());
            Layer.LayerInfo = Info;
            Layer.LayerData = MoveTemp(Masks[L]);
        }
    TMap<FGuid, TArray<FLandscapeImportLayerInfo>> LayerData;
    LayerData.Add(FGuid(), Layers);
    ALandscape* Landscape = World->SpawnActor<ALandscape>(FVector(Bounds.Min, 0), FRotator::ZeroRotator);
    Landscape->LandscapeMaterial = Material;
    Landscape->SetActorRelativeScale3D(FVector(QuadSize, QuadSize, 100));
    Landscape->Import(FGuid::NewGuid(), 0, 0, SizeX - 1, SizeY - 1, Sections, SubsectionQuads, HeightData, TEXT(""), LayerData,
        ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
    Landscape->SetActorLabel(TEXT("LakeLandscape"));
    ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
    Info->UpdateLayerInfoMap(Landscape);
    for (const FLandscapeImportLayerInfo& Layer : Layers)
    {
        Landscape->AddTargetLayer(Layer.LayerName, FLandscapeTargetLayerSettings(Layer.LayerInfo));
        const int32 Index = Info->GetLayerInfoIndex(Layer.LayerName);
        if (Index != INDEX_NONE) Info->Layers[Index].LayerInfoObj = Layer.LayerInfo;
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL landscape %dx%d vertices, %dx%d components, %d layers"), SizeX, SizeY, CountX, CountY, Layers.Num());
    return Landscape;
#else
    return nullptr;
#endif
}

int32 ULakeLevelLibrary::RebuildLandWithoutBrush()
{
#if WITH_EDITOR
    UWorld* World = EditorWorld();
    if (!World) return 0;
    ALandscape* Old = nullptr;
    for (TActorIterator<ALandscape> It(World); It; ++It) { Old = *It; break; }
    if (!Old) return 0;
    ULandscapeInfo* OldInfo = Old->GetLandscapeInfo();
    int32 MinX, MinY, MaxX, MaxY;
    if (!OldInfo || !OldInfo->GetLandscapeExtent(MinX, MinY, MaxX, MaxY)) return 0;
    const int32 W = MaxX - MinX + 1, H = MaxY - MinY + 1;
    TArray<uint16> Carved;
    Carved.SetNumZeroed(W * H);
    {
        FLandscapeEditDataInterface Data(OldInfo);
        Data.GetHeightData(MinX, MinY, MaxX, MaxY, Carved.GetData(), W);
    }
    const FTransform T = Old->GetActorTransform();
    const float ScaleZ = T.GetScale3D().Z / 128.0f, OffsetZ = T.GetLocation().Z - 32768.0f * ScaleZ;
    // The carved bowl stays under the water; the land above the waterline goes back to the designed
    // ground, with a smooth join a metre either side of the waterline.
    TArray<uint16> Hybrid;
    Hybrid.SetNumUninitialized(W * H);
    int32 Changed = 0;
    float Worst = 0;
    for (int32 Y = 0; Y < H; ++Y)
        for (int32 X = 0; X < W; ++X)
        {
            const FVector P = T.TransformPosition(FVector(MinX + X, MinY + Y, 0));
            const float SD = LakeMap::ShoreDistance(P.X, P.Y);          // positive in the water
            const float CarvedZ = Carved[Y * W + X] * ScaleZ + OffsetZ;
            float Z = CarvedZ;
            if (SD < 100.0f)
            {
                const float Design = LakeMap::DesignGround(P.X, P.Y);
                const float Land = 1.0f - FMath::SmoothStep(-100.0f, 100.0f, SD);
                Z = FMath::Lerp(CarvedZ, Design, Land);
                Worst = FMath::Max(Worst, FMath::Abs(Z - CarvedZ));
                if (FMath::Abs(Z - CarvedZ) > 0.5f) ++Changed;
            }
            Hybrid[Y * W + X] = (uint16)FMath::Clamp(FMath::RoundToInt((Z - T.GetLocation().Z) / ScaleZ + 32768.0f), 0, 65535);
        }
    // Nothing may carve the new landscape: the water stays as the surface only.
    for (TActorIterator<AWaterBody> It(World); It; ++It)
        if (UWaterBodyComponent* Body = It->GetWaterBodyComponent()) Body->bAffectsLandscape = false;
    TArray<AActor*> Brushes;
    for (TActorIterator<AActor> It(World); It; ++It)
        if (It->GetClass()->GetName().Contains(TEXT("WaterBrushManager"))) Brushes.Add(*It);
    for (AActor* Brush : Brushes) Brush->Destroy();
    UMaterialInterface* Material = Old->LandscapeMaterial;
    TArray<UObject*> LayerInfos;
    for (const FLandscapeInfoLayerSettings& Layer : OldInfo->Layers)
        if (Layer.LayerInfoObj) LayerInfos.Add(Layer.LayerInfoObj);
    for (TActorIterator<ALandscapeProxy> It(World); It; ++It) It->Destroy();
    ALandscape* New = CreateLandscape(World, Material, LayerInfos, Hybrid);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL land rebuilt without the brush: %d of %d vertices set back to the design, up to %.0f cm, %d layer infos, landscape %s"),
        Changed, W * H, Worst, LayerInfos.Num(), New ? TEXT("ok") : TEXT("failed"));
    return New ? Changed : 0;
#else
    return 0;
#endif
}

namespace
{
    /** Closed loops where Value crosses zero (positive inside), by marching squares on a Cell grid. */
    TArray<TArray<FVector2D>> Contours(const FBox2D& Box, float Cell, TFunctionRef<float(float, float)> Value)
    {
        const int32 NX = FMath::CeilToInt(Box.GetSize().X / Cell), NY = FMath::CeilToInt(Box.GetSize().Y / Cell);
        TArray<float> D;
        D.SetNumUninitialized((NX + 1) * (NY + 1));
        for (int32 Y = 0; Y <= NY; ++Y)
            for (int32 X = 0; X <= NX; ++X)
                D[Y * (NX + 1) + X] = Value(Box.Min.X + X * Cell, Box.Min.Y + Y * Cell);
        auto At = [&](int32 X, int32 Y) { return D[Y * (NX + 1) + X]; };
        const int32 HCount = NX * (NY + 1);
        auto HEdge = [&](int32 X, int32 Y) { return Y * NX + X; };
        auto VEdge = [&](int32 X, int32 Y) { return HCount + Y * (NX + 1) + X; };
        auto EdgePoint = [&](int32 Edge)
        {
            int32 X0, Y0, X1, Y1;
            if (Edge < HCount) { Y0 = Y1 = Edge / NX; X0 = Edge % NX; X1 = X0 + 1; }
            else { const int32 E = Edge - HCount; Y0 = E / (NX + 1); X0 = X1 = E % (NX + 1); Y1 = Y0 + 1; }
            const float A = At(X0, Y0), B = At(X1, Y1);
            const float T = FMath::Clamp(A / (A - B), 0.0f, 1.0f);
            return Box.Min + FMath::Lerp(FVector2D(X0, Y0), FVector2D(X1, Y1), T) * Cell;
        };
        TMap<int32, TArray<int32, TInlineAllocator<2>>> Links;
        auto Link = [&](int32 A, int32 B) { Links.FindOrAdd(A).Add(B); Links.FindOrAdd(B).Add(A); };
        for (int32 Y = 0; Y < NY; ++Y)
            for (int32 X = 0; X < NX; ++X)
            {
                const bool A = At(X, Y) > 0, B = At(X + 1, Y) > 0, C = At(X + 1, Y + 1) > 0, Dd = At(X, Y + 1) > 0;
                const int32 Bottom = HEdge(X, Y), Right = VEdge(X + 1, Y), Top = HEdge(X, Y + 1), Left = VEdge(X, Y);
                TArray<int32, TInlineAllocator<4>> Cross;
                if (A != B) Cross.Add(Bottom);
                if (B != C) Cross.Add(Right);
                if (Dd != C) Cross.Add(Top);
                if (A != Dd) Cross.Add(Left);
                if (Cross.Num() == 2) Link(Cross[0], Cross[1]);
                else if (Cross.Num() == 4) { Link(Bottom, Right); Link(Top, Left); }   // a saddle: either pairing closes the loops
            }
        TSet<int32> Seen;
        TArray<TArray<FVector2D>> Loops;
        for (const auto& Pair : Links)
        {
            if (Seen.Contains(Pair.Key)) continue;
            TArray<FVector2D>& Loop = Loops.AddDefaulted_GetRef();
            int32 Prev = INDEX_NONE, Cur = Pair.Key;
            while (Cur != INDEX_NONE && !Seen.Contains(Cur))
            {
                Seen.Add(Cur);
                Loop.Add(EdgePoint(Cur));
                int32 Next = INDEX_NONE;
                for (int32 N : Links[Cur]) if (N != Prev && !Seen.Contains(N)) { Next = N; break; }
                Prev = Cur;
                Cur = Next;
            }
        }
        return Loops;
    }

    /** A point every Spacing along a closed loop, at height Z. */
    TArray<FVector> Resample(const TArray<FVector2D>& Loop, float Spacing, float Z, float* OutLength = nullptr)
    {
        TArray<FVector> Out;
        float Walked = 0, Next = 0;
        for (int32 I = 0; I < Loop.Num(); ++I)
        {
            const FVector2D A = Loop[I], B = Loop[(I + 1) % Loop.Num()];
            const float Len = FVector2D::Distance(A, B);
            while (Next <= Walked + Len)
            {
                Out.Add(FVector(FMath::Lerp(A, B, Len > 0 ? (Next - Walked) / Len : 0.0f), Z));
                Next += FMath::Max(100.0f, Spacing);
            }
            Walked += Len;
        }
        if (OutLength) *OutLength = Walked;
        return Out;
    }

    /** Twice the signed area of a loop (X east, Y south). */
    double SignedArea(const TArray<FVector>& Points)
    {
        double Area = 0;
        for (int32 I = 0; I < Points.Num(); ++I)
        {
            const FVector& A = Points[I];
            const FVector& B = Points[(I + 1) % Points.Num()];
            Area += A.X * B.Y - B.X * A.Y;
        }
        return Area;
    }
}

TArray<FVector> ULakeLevelLibrary::LakeOutline(float Spacing)
{
    // The zero line of the signed distance to the waterline (positive in the water), 1 m cells; the
    // longest loop is the lake.
    TArray<TArray<FVector2D>> Loops = Contours(LakeMap::LakeBounds().ExpandBy(1500), 100.0f, [](float X, float Y) { return LakeMap::ShoreDistance(X, Y); });
    const TArray<FVector2D>* Best = nullptr;
    for (const TArray<FVector2D>& Loop : Loops) if (!Best || Loop.Num() > Best->Num()) Best = &Loop;
    if (!Best || Best->Num() < 3) return {};
    float Length = 0;
    TArray<FVector> Out = Resample(*Best, Spacing, LakeMap::SurfaceZ, &Length);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL waterline %d crossings, %.0f m long, %d spline points"), Best->Num(), Length / 100, Out.Num());
    return Out;
}

UObject* ULakeLevelLibrary::CreateBedProfile(const FString& Folder, const FString& Name)
{
#if WITH_EDITOR
    const FString PackageName = Folder / Name;
    UCurveFloat* Curve = LoadObject<UCurveFloat>(nullptr, *(PackageName + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
    if (!Curve)
    {
        UPackage* Package = CreatePackage(*PackageName);
        Curve = NewObject<UCurveFloat>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
        FAssetRegistryModule::AssetCreated(Curve);
    }
    // Across the ramp (0 at the waterline, 1 where the full channel depth is reached) how much of the
    // depth is reached. With a 6 m channel over a 60 m ramp: about 1.5 m of water a couple of metres
    // out (in a float fisher's reach), 3 m at 7 m, then a broad shelf of open water at 3.5-4 m out to
    // ~35 m from the bank, and only the middle of the lake falls to the full depth. (A plain bowl was
    // deep everywhere past 25 m: no open midwater for roach and perch, only the deep zone.)
    FRichCurve& Keys = Curve->FloatCurve;
    Keys.Reset();
    for (const FVector2f Key : { FVector2f(0, 0), FVector2f(0.04f, 0.25f), FVector2f(0.12f, 0.5f), FVector2f(0.25f, 0.6f), FVector2f(0.6f, 0.68f), FVector2f(1, 1) })
        Keys.SetKeyInterpMode(Keys.AddKey(Key.X, Key.Y), RCIM_Cubic);
    Keys.AutoSetTangents();
    Curve->MarkPackageDirty();
    return Curve;
#else
    return nullptr;
#endif
}

AActor* ULakeLevelLibrary::SpawnLake(float ChannelDepth, float RampWidth, float Spacing, UObject* Profile)
{
#if WITH_EDITOR
    UWorld* World = EditorWorld();
    if (!World) return nullptr;
    for (TActorIterator<AWaterBodyLake> It(World); It; ++It) It->Destroy();
    const TArray<FVector> Outline = LakeOutline(Spacing);
    if (Outline.Num() < 3) return nullptr;
    FVector Centre = FVector::ZeroVector;
    for (const FVector& P : Outline) Centre += P;
    Centre /= Outline.Num();
    AWaterBodyLake* Lake = SpawnWithFactory<AWaterBodyLake>(World, Centre);
    if (!Lake) return nullptr;
    TArray<FVector> Local;
    for (const FVector& P : Outline) Local.Add(P - Centre);
    // The water brush fills the lake's shape as a triangle fan, and with the other winding every
    // triangle is culled: the lake carved nothing at all. Keep the outline's signed area positive.
    const double Area = SignedArea(Local);
    if (Area < 0) Algo::Reverse(Local);
    Lake->GetWaterSpline()->ResetSpline(Local);
    UWaterBodyComponent* Body = Lake->GetWaterBodyComponent();
    Body->bAffectsLandscape = true;
    Body->CurveSettings.bUseCurveChannel = true;
    Body->CurveSettings.ChannelDepth = ChannelDepth;
    Body->CurveSettings.CurveRampWidth = RampWidth;
    if (UCurveFloat* Curve = Cast<UCurveFloat>(Profile)) Body->CurveSettings.ElevationCurveAsset = Curve;
    // Only lowering: the brush digs the bowl and cuts the banks no flatter than 70 degrees, but never
    // raises or flattens the land around (its default blend laid a flat shelf at the water level).
    FWaterBodyHeightmapSettings& Carve = Body->WaterHeightmapSettings;
    Carve.BlendMode = EWaterBrushBlendType::Min;
    Carve.FalloffSettings.FalloffMode = EWaterBrushFalloffMode::Angle;
    Carve.FalloffSettings.FalloffAngle = 70.0f;
    Carve.FalloffSettings.EdgeOffset = 0.0f;
    Carve.FalloffSettings.ZOffset = 0.0f;
    Lake->SetActorLabel(TEXT("Lake"));
    // The surface is flat to the engine: the wind's ripple and the rings are in our material only.
    Lake->SetWaterWaves(nullptr);
    UseLakeWater(Body);
    RefreshWaterBody(Lake);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL lake: %d spline points, channel %.0f cm, ramp %.0f cm, outline reversed %d"), Local.Num(), ChannelDepth, RampWidth, Area < 0);
    return Lake;
#else
    return nullptr;
#endif
}

int32 ULakeLevelLibrary::SpawnShallows(float MaxDesignDepth, float MinBankDistance, float MinAreaM2, float DepthCm, float FalloffAngle)
{
#if WITH_EDITOR
    UWorld* World = EditorWorld();
    if (!World) return 0;
    for (TActorIterator<AWaterBodyIsland> It(World); It; ++It) It->Destroy();
    // The flats of the designed bed: shallower than MaxDesignDepth and at least MinBankDistance out.
    const TArray<TArray<FVector2D>> Loops = Contours(LakeMap::LakeBounds().ExpandBy(500), 200.0f, [=](float X, float Y)
    {
        const float SD = LakeMap::ShoreDistance(X, Y);
        if (SD <= 0) return -1000.0f;
        return FMath::Min(SD - MinBankDistance, MaxDesignDepth - (LakeMap::SurfaceZ - LakeMap::DesignGround(X, Y)));
    });
    int32 Count = 0;
    FString Sizes;
    for (const TArray<FVector2D>& Loop : Loops)
    {
        if (Loop.Num() < 8) continue;
        TArray<FVector> Points = Resample(Loop, 500.0f, LakeMap::SurfaceZ - DepthCm);
        const double Area = SignedArea(Points);
        if (FMath::Abs(Area) * 0.5 / 10000 < MinAreaM2 || Points.Num() < 4) continue;
        if (Area < 0) Algo::Reverse(Points);         // the same winding the brush needs for the lake
        FVector Centre = FVector::ZeroVector;
        for (const FVector& P : Points) Centre += P;
        Centre /= Points.Num();
        AWaterBodyIsland* Shelf = SpawnWithFactory<AWaterBodyIsland>(World, Centre);
        if (!Shelf) continue;
        TArray<FVector> Local;
        for (const FVector& P : Points) Local.Add(P - Centre);
        Shelf->GetWaterSpline()->ResetSpline(Local);
        // A flat top at the spline (DepthCm under the water), sloping off outside it; it only raises
        // the carved bowl, never lowers it.
        Shelf->WaterCurveSettings.bUseCurveChannel = false;
        Shelf->WaterCurveSettings.ChannelDepth = 0;
        FWaterBodyHeightmapSettings& Raise = Shelf->WaterHeightmapSettings;
        Raise.BlendMode = EWaterBrushBlendType::Max;
        Raise.FalloffSettings.FalloffMode = EWaterBrushFalloffMode::Angle;
        Raise.FalloffSettings.FalloffAngle = FalloffAngle;
        Raise.FalloffSettings.EdgeOffset = 0;
        Raise.FalloffSettings.ZOffset = 0;
        Shelf->SetActorLabel(FString::Printf(TEXT("Shallow_%d"), Count));
        Shelf->GetWaterSpline()->SynchronizeWaterProperties();
        Shelf->GetWaterSpline()->PostEditChange();
        Shelf->PostEditChange();
        Sizes += FString::Printf(TEXT(" %.0f m2"), FMath::Abs(Area) * 0.5 / 10000);
        ++Count;
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL shallows: %d flats at %.0f cm (%s), from %d loops"), Count, DepthCm, *Sizes, Loops.Num());
    return Count;
#else
    return 0;
#endif
}

void ULakeLevelLibrary::WaterAlignmentLog()
{
#if WITH_EDITOR
    UWorld* World = EditorWorld();
    if (!World) return;
    ALandscape* Landscape = nullptr;
    for (TActorIterator<ALandscape> It(World); It; ++It) { Landscape = *It; break; }
    AWaterBody* Lake = nullptr;
    for (TActorIterator<AWaterBody> It(World); It; ++It)
        if (It->IsA<AWaterBodyLake>()) { Lake = *It; break; }
    if (!Landscape || !Lake) { UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_ALIGN no landscape or lake")); return; }
    auto Height = [Landscape](float X, float Y) -> TOptional<float>
    {
        return Landscape->GetHeightAtLocation(FVector(X, Y, 0.0f), EHeightfieldSource::Editor);
    };
    // The ground the game rules use (the baked heights) must be the landscape the player sees.
    const FBox2D Box = LakeMap::LakeBounds();
    int32 Samples = 0, Bad = 0;
    float Sum = 0, Worst = 0;
    for (float Y = Box.Min.Y; Y <= Box.Max.Y; Y += 400.0f)
        for (float X = Box.Min.X; X <= Box.Max.X; X += 400.0f)
        {
            const TOptional<float> Z = Height(X, Y);
            if (!Z.IsSet()) continue;
            const float Diff = FMath::Abs(LakeMap::Ground(X, Y) - Z.GetValue());
            Sum += Diff;
            ++Samples;
            if (Diff > 30.0f) ++Bad;
            Worst = FMath::Max(Worst, Diff);
        }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_ALIGN baked ground vs landscape: %d samples, mean %.1f cm, %d over 30 cm, worst %.1f cm (baked %d)"),
        Samples, Sum / FMath::Max(1, Samples), Bad, Worst, LakeMap::HasBakedGround() ? 1 : 0);
    // The bank across the shore: the designed ground on land, the carved bed in the water.
    UWaterSplineComponent* Spline = Lake->GetWaterSpline();
    const int32 Count = Spline->GetNumberOfSplinePoints();
    for (float Frac : { 0.08f, 0.33f, 0.62f })
    {
        const int32 I = FMath::Clamp(FMath::RoundToInt(Count * Frac), 0, Count - 1);
        const FVector P = Spline->GetLocationAtSplinePoint(I, ESplineCoordinateSpace::World);
        const FVector2D Out = LakeMap::NearestShore(FVector2D(P.X, P.Y)).Outward;
        FString Line = FString::Printf(TEXT("QUIET_LAKE_ALIGN transect %s:"), *FVector2D(P.X, P.Y).ToString());
        for (float D = -200.0f; D <= 600.0f; D += 50.0f)
        {
            const float X = P.X + Out.X * D, Y = P.Y + Out.Y * D;
            const TOptional<float> Z = Height(X, Y);
            Line += FString::Printf(TEXT(" %+.1fm:%+.1f(design %+.1f)"), D / 100.0f, Z.IsSet() ? Z.GetValue() : -999.0f, LakeMap::Ground(X, Y));
        }
        UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
    }
    // The pier: the ground from the bank out along it, to check the deck is above the bank.
    {
        const LakeMap::FPier& Pier = LakeMap::Pier();
        FString Line = FString::Printf(TEXT("QUIET_LAKE_ALIGN pier root (%.0f, %.0f) dir (%.3f, %.3f) length %.0f cm, along from the root:"),
            Pier.Root.X, Pier.Root.Y, Pier.Dir.X, Pier.Dir.Y, Pier.Length);
        for (float A = -400.0f; A <= Pier.Length + 400.0f; A += 200.0f)
        {
            const FVector2D P = Pier.Root + Pier.Dir * A;
            Line += FString::Printf(TEXT(" %.0fm:%+.0f(design d %+.0f)"), A / 100.0f, LakeMap::Ground(P.X, P.Y) - LakeMap::SurfaceZ, LakeMap::Depth(P.X, P.Y));
        }
        UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
    }
    // The fishing spots: the water 1.5 m off the bank should be as deep as the design puts it.
    int32 BadSpots = 0;
    for (const LakeMap::FSpot& Spot : LakeMap::Spots())
    {
        const FVector2D OnWater = Spot.Location - Spot.Outward * 150.0f;
        const TOptional<float> Z = Height(OnWater.X, OnWater.Y);
        const float DesignDepth = LakeMap::Depth(OnWater.X, OnWater.Y) / 100.0f;
        const float RealDepth = Z.IsSet() ? (LakeMap::SurfaceZ - Z.GetValue()) / 100.0f : -99.0f;
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_ALIGN spot %s: 1.5 m off the bank design %.2f m, landscape %.2f m"), Spot.Name, DesignDepth, RealDepth);
        if (RealDepth < 0.05f || FMath::Abs(RealDepth - DesignDepth) > 0.8f) ++BadSpots;
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_ALIGN spots off the design depth: %d of %d"), BadSpots, LakeMap::Spots().Num());
#endif
}

float ULakeLevelLibrary::UpdateLandscape()
{
#if WITH_EDITOR
    UWorld* World = EditorWorld();
    if (!World) return 0;
    float Depth = 0;
    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        It->ForceUpdateLayersContent();
        const TOptional<float> Z = It->GetHeightAtLocation(FVector(LakeMap::Centre, 0), EHeightfieldSource::Editor);
        if (Z.IsSet()) Depth = LakeMap::SurfaceZ - Z.GetValue();
        // The stored heights themselves (what the level saves), next to the collision above.
        float Stored = -1;
        if (ULandscapeInfo* Info = It->GetLandscapeInfo())
        {
            const FVector Local = It->GetActorTransform().InverseTransformPosition(FVector(LakeMap::Centre, 0));
            int32 X1 = FMath::RoundToInt(Local.X), Y1 = FMath::RoundToInt(Local.Y), X2 = X1, Y2 = Y1;
            uint16 H = 0;
            FLandscapeEditDataInterface Data(Info);
            Data.GetHeightData(X1, Y1, X2, Y2, &H, 0);
            Stored = LakeMap::SurfaceZ - (H - 32768.0f) * 100.0f / 128.0f;
            // The deepest stored point over the whole lake and how much of it is deeper than 50 cm.
            const FBox2D Box = LakeMap::LakeBounds();
            const FVector A = It->GetActorTransform().InverseTransformPosition(FVector(Box.Min, 0));
            const FVector B = It->GetActorTransform().InverseTransformPosition(FVector(Box.Max, 0));
            int32 BX1 = FMath::Max(0, FMath::FloorToInt(A.X)), BY1 = FMath::Max(0, FMath::FloorToInt(A.Y)), BX2 = FMath::CeilToInt(B.X), BY2 = FMath::CeilToInt(B.Y);
            TArray<uint16> All;
            All.SetNumZeroed((BX2 - BX1 + 1) * (BY2 - BY1 + 1));
            const int32 Stride = BX2 - BX1 + 1;
            Data.GetHeightData(BX1, BY1, BX2, BY2, All.GetData(), Stride);
            uint16 Lowest = 65535; int32 LowX = 0, LowY = 0, Deep = 0;
            for (int32 Y = 0; Y <= BY2 - BY1; ++Y)
                for (int32 X = 0; X < Stride; ++X)
                {
                    const uint16 V = All[Y * Stride + X];
                    if (V < Lowest) { Lowest = V; LowX = X; LowY = Y; }
                    if (LakeMap::SurfaceZ - (V - 32768.0f) * 100.0f / 128.0f > 50) ++Deep;
                }
            const FVector LowAt = It->GetActorTransform().TransformPosition(FVector(BX1 + LowX, BY1 + LowY, 0));
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL deepest stored point %.0f cm at %s, %d m2 deeper than 50 cm"),
                LakeMap::SurfaceZ - (Lowest - 32768.0f) * 100.0f / 128.0f, *FVector2D(LowAt).ToString(), Deep);
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL landscape up to date %d, depth at the lake centre %.0f cm by collision, %.0f cm stored"), It->IsUpToDate(), Depth, Stored);
        const int32 Layers = It->GetLayersConst().Num();
        for (int32 L = 0; L < Layers; ++L)
            for (ALandscapeBlueprintBrushBase* Brush : It->GetBrushesForLayer(L))
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL layer %d/%d brush %s heightmap %d"), L, Layers, *GetNameSafe(Brush), Brush ? Brush->CanAffectHeightmap() : -1);
    }
    for (TActorIterator<AWaterBody> It(World); It; ++It)
    {
        UWaterBodyComponent* Body = It->GetWaterBodyComponent();
        UWaterSplineComponent* Spline = It->GetWaterSpline();
        float Under = 0;
        for (TActorIterator<ALandscape> L(World); L; ++L)
        {
            const FVector Mid = Spline->GetLocationAtSplineInputKey(Spline->GetNumberOfSplinePoints() * 0.5f, ESplineCoordinateSpace::World);
            const TOptional<float> Z = L->GetHeightAtLocation(Mid, EHeightfieldSource::Editor);
            if (Z.IsSet() && It->IsA<AWaterBodyRiver>()) Under = Mid.Z - Z.GetValue();
            if (It->IsA<AWaterBodyRiver>())
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL %s middle: surface %.0f, landscape %.0f, LakeMap bed %.0f"), *It->GetActorLabel(), Mid.Z, Z.IsSet() ? Z.GetValue() : 0.0, LakeMap::Ground(Mid.X, Mid.Y));
        }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL water body %s affects landscape %d, landscape %s, points %d closed %d, bounds %s, ground under its middle %.0f cm"),
            *It->GetActorLabel(), Body->AffectsLandscape(), *GetNameSafe(Body->FindLandscape()), Spline->GetNumberOfSplinePoints(),
            Spline->IsClosedLoop(), *Body->Bounds.GetBox().ToString(), Under);
    }
    return Depth;
#else
    return 0;
#endif
}

int32 ULakeLevelLibrary::BakeGround()
{
#if WITH_EDITOR
    UWorld* World = EditorWorld();
    TActorIterator<ALandscape> It(World);
    if (!World || !It) return 0;
    ULandscapeInfo* Info = It->GetLandscapeInfo();
    int32 MinX, MinY, MaxX, MaxY;
    if (!Info || !Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY)) return 0;
    const int32 W = MaxX - MinX + 1, Hh = MaxY - MinY + 1;
    TArray<uint16> Heights;
    Heights.SetNumZeroed(W * Hh);
    FLandscapeEditDataInterface Data(Info);
    Data.GetHeightData(MinX, MinY, MaxX, MaxY, Heights.GetData(), W);
    const FTransform T = It->GetActorTransform();
    struct FHeader { char Magic[4]; int32 W, H; float MinX, MinY, Step, ZScale, ZOffset; } Header;
    FMemory::Memcpy(Header.Magic, "QLG1", 4);
    Header.W = W; Header.H = Hh;
    const FVector Origin = T.TransformPosition(FVector(MinX, MinY, 0));
    Header.MinX = Origin.X; Header.MinY = Origin.Y;
    Header.Step = T.GetScale3D().X;
    Header.ZScale = T.GetScale3D().Z / 128.0f;
    Header.ZOffset = T.GetLocation().Z - 32768.0f * Header.ZScale;
    TArray<uint8> Bytes;
    Bytes.Append((const uint8*)&Header, sizeof(Header));
    Bytes.Append((const uint8*)Heights.GetData(), Heights.Num() * sizeof(uint16));
    const FString Path = FPaths::ProjectContentDir() / LakeMap::BakedGroundFile;
    const bool bSaved = FFileHelper::SaveArrayToFile(Bytes, *Path);
    // The bed across the bank in front of the camp, as the fish and the float will find it.
    FVector2D Facing;
    const FVector2D Start = LakeMap::StartSpot(&Facing);
    const FVector2D Into = -LakeMap::NearestShore(Start).Outward;
    const FVector2D Edge = LakeMap::NearestShore(Start).Location;
    FString Transect;
    for (float M : { 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 10.0f, 15.0f, 20.0f, 30.0f })
    {
        const FVector Local = T.InverseTransformPosition(FVector(Edge + Into * M * 100, 0));
        const int32 X = FMath::Clamp(FMath::RoundToInt(Local.X) - MinX, 0, W - 1), Y = FMath::Clamp(FMath::RoundToInt(Local.Y) - MinY, 0, Hh - 1);
        Transect += FString::Printf(TEXT(" %.0fm:%.0fcm"), M, LakeMap::SurfaceZ - (Heights[Y * W + X] * Header.ZScale + Header.ZOffset));
    }
    // And in the middle of each raised flat.
    FString Flats;
    for (TActorIterator<AWaterBodyIsland> Island(World); Island; ++Island)
    {
        const FVector Local = T.InverseTransformPosition(Island->GetActorLocation());
        const int32 X = FMath::Clamp(FMath::RoundToInt(Local.X) - MinX, 0, W - 1), Y = FMath::Clamp(FMath::RoundToInt(Local.Y) - MinY, 0, Hh - 1);
        Flats += FString::Printf(TEXT(" %s:%.0fcm"), *Island->GetActorLabel(), LakeMap::SurfaceZ - (Heights[Y * W + X] * Header.ZScale + Header.ZOffset));
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL baked %dx%d heights to %s (%d); camp bank:%s; flats:%s"), W, Hh, *Path, bSaved, *Transect, *Flats);
    return bSaved ? W * Hh : 0;
#else
    return 0;
#endif
}

int32 ULakeLevelLibrary::SpawnStreams(float Depth)
{
#if WITH_EDITOR
    UWorld* World = EditorWorld();
    if (!World) return 0;
    for (TActorIterator<AWaterBodyRiver> It(World); It; ++It) It->Destroy();
    int32 Count = 0;
    for (const LakeMap::FStream& Stream : LakeMap::Streams())
    {
        if (Stream.Surface.Num() < 2) continue;
        const FVector Origin = Stream.Surface[0];
        AWaterBodyRiver* River = SpawnWithFactory<AWaterBodyRiver>(World, Origin);
        if (!River) continue;
        TArray<FVector> Local;
        for (const FVector& P : Stream.Surface) Local.Add(P - Origin);
        River->GetWaterSpline()->ResetSpline(Local);
        if (UWaterSplineMetadata* Meta = River->GetWaterSplineMetadata())
        {
            auto Fill = [&](FInterpCurveFloat& Curve, float Value)
            {
                Curve.Points.Reset();
                for (int32 I = 0; I < Local.Num(); ++I) Curve.Points.Emplace((float)I, Value);
            };
            Fill(Meta->RiverWidth, Stream.Width);
            Fill(Meta->Depth, Depth);
            Fill(Meta->WaterVelocityScalar, Stream.bOutflow ? 1.0f : 0.6f);
        }
        // A narrow channel cut into the existing stream bed, only lowering the ground.
        UWaterBodyComponent* Body = River->GetWaterBodyComponent();
        Body->CurveSettings.ChannelDepth = Depth;
        Body->CurveSettings.CurveRampWidth = Stream.Width * 0.4f;
        FWaterBodyHeightmapSettings& Carve = Body->WaterHeightmapSettings;
        Carve.BlendMode = EWaterBrushBlendType::Min;
        Carve.FalloffSettings.FalloffMode = EWaterBrushFalloffMode::Width;
        Carve.FalloffSettings.FalloffWidth = 300.0f;
        Carve.FalloffSettings.EdgeOffset = 30.0f;
        Carve.FalloffSettings.ZOffset = 0.0f;
        UseLakeWater(Body);
        River->SetActorLabel(FString::Printf(TEXT("Stream_%d"), Count));
        RefreshWaterBody(River);
        ++Count;
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL streams: %d rivers"), Count);
    return Count;
#else
    return 0;
#endif
}
