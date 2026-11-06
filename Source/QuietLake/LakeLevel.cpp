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
#include "WaterBodyLakeActor.h"
#include "WaterBodyRiverActor.h"
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
    TArray<uint8> Masks[3];
    for (TArray<uint8>& Mask : Masks) Mask.SetNumZeroed(SizeX * SizeY);
    for (int32 Y = 0; Y < SizeY; ++Y)
        for (int32 X = 0; X < SizeX; ++X)
        {
            const FVector2D P = Bounds.Min + FVector2D(X, Y) * QuadSize;
            const int32 I = Y * SizeX + X;
            // Inside the waterline the ground is flat just under the water: the Water lake carves the
            // bowl into it (its brush only lowers, see SpawnLake).
            const float Z = LakeMap::Ground(P.X, P.Y);
            Heights[I] = EncodeHeight(LakeMap::ShoreDistance(P.X, P.Y) > 0 ? FMath::Max(Z, LakeMap::SurfaceZ - 5) : Z);
            const FLinearColor L = ALakeEnvironment::TerrainLayers(P);
            Masks[0][I] = (uint8)FMath::RoundToInt(FMath::Clamp(L.R, 0.0f, 1.0f) * 255);
            Masks[1][I] = (uint8)FMath::RoundToInt(FMath::Clamp(L.G, 0.0f, 1.0f) * 255);
            Masks[2][I] = (uint8)FMath::RoundToInt(FMath::Clamp(L.B, 0.0f, 1.0f) * 255);
        }

    TMap<FGuid, TArray<uint16>> HeightData;
    HeightData.Add(FGuid(), MoveTemp(Heights));
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

TArray<FVector> ULakeLevelLibrary::LakeOutline(float Spacing)
{
    // Marching squares on the signed distance to the waterline (positive in the water), 1 m cells;
    // the crossings are chained into loops and the longest loop is the lake.
    const FBox2D Box = LakeMap::LakeBounds().ExpandBy(1500);
    constexpr float Cell = 100.0f;
    const int32 NX = FMath::CeilToInt(Box.GetSize().X / Cell), NY = FMath::CeilToInt(Box.GetSize().Y / Cell);
    TArray<float> D;
    D.SetNumUninitialized((NX + 1) * (NY + 1));
    for (int32 Y = 0; Y <= NY; ++Y)
        for (int32 X = 0; X <= NX; ++X)
            D[Y * (NX + 1) + X] = LakeMap::ShoreDistance(Box.Min.X + X * Cell, Box.Min.Y + Y * Cell);
    auto Value = [&](int32 X, int32 Y) { return D[Y * (NX + 1) + X]; };
    const int32 HCount = NX * (NY + 1);
    auto HEdge = [&](int32 X, int32 Y) { return Y * NX + X; };
    auto VEdge = [&](int32 X, int32 Y) { return HCount + Y * (NX + 1) + X; };
    auto EdgePoint = [&](int32 Edge)
    {
        int32 X0, Y0, X1, Y1;
        if (Edge < HCount) { Y0 = Y1 = Edge / NX; X0 = Edge % NX; X1 = X0 + 1; }
        else { const int32 E = Edge - HCount; Y0 = E / (NX + 1); X0 = X1 = E % (NX + 1); Y1 = Y0 + 1; }
        const float A = Value(X0, Y0), B = Value(X1, Y1);
        const float T = FMath::Clamp(A / (A - B), 0.0f, 1.0f);
        return Box.Min + FMath::Lerp(FVector2D(X0, Y0), FVector2D(X1, Y1), T) * Cell;
    };
    TMap<int32, TArray<int32, TInlineAllocator<2>>> Links;
    auto Link = [&](int32 A, int32 B) { Links.FindOrAdd(A).Add(B); Links.FindOrAdd(B).Add(A); };
    for (int32 Y = 0; Y < NY; ++Y)
        for (int32 X = 0; X < NX; ++X)
        {
            const bool A = Value(X, Y) > 0, B = Value(X + 1, Y) > 0, C = Value(X + 1, Y + 1) > 0, Dd = Value(X, Y + 1) > 0;
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
    TArray<FVector2D> Best;
    for (const auto& Pair : Links)
    {
        if (Seen.Contains(Pair.Key)) continue;
        TArray<FVector2D> Loop;
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
        if (Loop.Num() > Best.Num()) Best = MoveTemp(Loop);
    }
    // Even spacing along the loop.
    TArray<FVector> Out;
    if (Best.Num() < 3) return Out;
    float Walked = 0, Next = 0;
    for (int32 I = 0; I < Best.Num(); ++I)
    {
        const FVector2D A = Best[I], B = Best[(I + 1) % Best.Num()];
        const float Len = FVector2D::Distance(A, B);
        while (Next <= Walked + Len)
        {
            Out.Add(FVector(FMath::Lerp(A, B, Len > 0 ? (Next - Walked) / Len : 0.0f), LakeMap::SurfaceZ));
            Next += FMath::Max(100.0f, Spacing);
        }
        Walked += Len;
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL waterline %d crossings, %.0f m long, %d spline points"), Best.Num(), Walked / 100, Out.Num());
    return Out;
}

AActor* ULakeLevelLibrary::SpawnLake(float ChannelDepth, float RampWidth, float Spacing)
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
    double Area = 0;
    for (int32 I = 0; I < Local.Num(); ++I)
    {
        const FVector& A = Local[I];
        const FVector& B = Local[(I + 1) % Local.Num()];
        Area += A.X * B.Y - B.X * A.Y;
    }
    if (Area < 0) Algo::Reverse(Local);
    Lake->GetWaterSpline()->ResetSpline(Local);
    UWaterBodyComponent* Body = Lake->GetWaterBodyComponent();
    Body->bAffectsLandscape = true;
    Body->CurveSettings.bUseCurveChannel = true;
    Body->CurveSettings.ChannelDepth = ChannelDepth;
    Body->CurveSettings.CurveRampWidth = RampWidth;
    // Only lowering: the brush digs the bowl and cuts the banks no flatter than 70 degrees, but never
    // raises or flattens the land around (its default blend laid a flat shelf at the water level).
    FWaterBodyHeightmapSettings& Carve = Body->WaterHeightmapSettings;
    Carve.BlendMode = EWaterBrushBlendType::Min;
    Carve.FalloffSettings.FalloffMode = EWaterBrushFalloffMode::Angle;
    Carve.FalloffSettings.FalloffAngle = 70.0f;
    Carve.FalloffSettings.EdgeOffset = 0.0f;
    Carve.FalloffSettings.ZOffset = 0.0f;
    Lake->SetActorLabel(TEXT("Lake"));
    RefreshWaterBody(Lake);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEVEL lake: %d spline points, channel %.0f cm, ramp %.0f cm, outline reversed %d"), Local.Num(), ChannelDepth, RampWidth, Area < 0);
    return Lake;
#else
    return nullptr;
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
