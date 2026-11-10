#include "LakeBiomes.h"
#include "LakeMap.h"
#include "Async/ParallelFor.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "PCGGraph.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#endif

namespace
{
    enum EBiome : int32 { Nothing = -1, Pine, Spruce, Mixed, Birch, Alder, Edge, Meadow, Reed, Cattail, Sedge, VillageLawn, BiomeCount };
    struct FBiomeInfo { const TCHAR* Name; FColor Color; };
    // Colours a half step apart in each channel at least, as Biome Core tells biomes apart by colour.
    const FBiomeInfo Biomes[BiomeCount] = {
        { TEXT("PineForest"), FColor(0, 0, 255) },
        { TEXT("SpruceForest"), FColor(0, 255, 255) },
        { TEXT("MixedForest"), FColor(0, 255, 0) },
        { TEXT("BirchGrove"), FColor(128, 255, 0) },
        { TEXT("AlderBank"), FColor(255, 0, 255) },
        { TEXT("ForestEdge"), FColor(255, 255, 0) },
        { TEXT("Meadow"), FColor(255, 0, 0) },
        { TEXT("ReedBelt"), FColor(255, 128, 0) },
        { TEXT("Cattails"), FColor(128, 0, 255) },
        { TEXT("SedgeTussocks"), FColor(255, 0, 128) },
        { TEXT("VillageLawn"), FColor(255, 128, 128) },
    };

    float S(float A, float B, float X) { const float T = FMath::Clamp((X - A) / (B - A), 0.0f, 1.0f); return T * T * (3 - 2 * T); }
    float Noise(const FVector2D& P, float Scale) { return FMath::PerlinNoise2D(P / Scale); }
    float SegmentDistance(const FVector2D& P, const FVector2D& A, const FVector2D& B)
    {
        const FVector2D AB = B - A;
        const float T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1.0), 0.0, 1.0);
        return FVector2D::Distance(P, A + AB * T);
    }

    const FBox2D VillageLawnBounds(FVector2D(-17000, 1400), FVector2D(15500, 21200));

    float VillageLawnCover(const FVector2D& P, float Inland, float Forest)
    {
        if (!VillageLawnBounds.IsInside(P) || Inland <= 0 || Forest >= 0.25f) return 0.0f;
        const auto MeadowPatch = [&P](const FVector2D& Centre, const FVector2D& Radii)
        {
            const FVector2D Offset = P - Centre;
            return 1.0f - S(0.78f, 1.12f, FVector2D(Offset.X / Radii.X, Offset.Y / Radii.Y).Size());
        };
        float Cover = FMath::Max(MeadowPatch({ 5600, 9200 }, { 6200, 5350 }),
            MeadowPatch({ 9050, 14300 }, { 5100, 4450 }));

        const LakeMap::FVillage& Village = LakeMap::Village();
        for (const auto& Plot : Village.Plots)
        {
            const FVector2D Offset = P - Plot.House;
            const float Along = FVector2D::DotProduct(Offset, Plot.Facing);
            const float Across = FMath::Abs(FVector2D::CrossProduct(Plot.Facing, Offset));
            const float Outside = FMath::Max3(Along - Plot.Front, -Along - Plot.Back, Across - Plot.HalfWidth);
            Cover = FMath::Max(Cover, 1.0f - S(100, 1100, Outside));
        }
        for (const auto& Path : Village.Paths)
            for (int32 I = 0; I + 1 < Path.Line.Num(); ++I)
            {
                const float Shoulder = Path.bRoad && P.X < -2500 ? 480.0f : 680.0f;
                const float Distance = SegmentDistance(P, Path.Line[I], Path.Line[I + 1]);
                Cover = FMath::Max(Cover, 1.0f - S(Path.Width * 0.5f + Shoulder, Path.Width * 0.5f + Shoulder + 980, Distance));
            }

        const LakeMap::FPier& Pier = LakeMap::Pier();
        const float Approach = 1.0f - S(550, 1100, SegmentDistance(P, Pier.Root, Pier.Root - Pier.Dir * 1850));
        const float ShoreMargin = FMath::Lerp(480.0f, 170.0f, Approach);
        Cover *= S(ShoreMargin, FMath::Lerp(1750.0f, 500.0f, Approach), Inland);
        return Cover * (1.0f - S(0.04f, 0.23f, Forest));
    }

    /** Places kept clear, or null: the fishing spots and their casting lanes, the camp, the pier and the
     *  landing, the beach, the footpath, the streams and their footbridges, the spring and the lookout. */
    const TCHAR* Clearing(const FVector2D& P, float ShoreDistance)
    {
        if (LakeMap::VillagePaved(P)) return TEXT("village street");
        if (LakeMap::SpotAmount(P) > 0.25f) return TEXT("spot");
        if (FVector2D::Distance(P, LakeMap::StartSpot()) < 1500) return TEXT("camp");
        const LakeMap::FPier& Pier = LakeMap::Pier();
        if (SegmentDistance(P, Pier.Root - Pier.Dir * 600, Pier.Root + Pier.Dir * Pier.Length) < 900) return TEXT("pier");
        if (FVector2D::Distance(P, LakeMap::Landing().Location) < 800) return TEXT("landing");
        if (LakeMap::BeachAmount(P) > 0.3f) return TEXT("beach");
        // The path with 3 m either side, so that the edge of a biome does not reach onto it.
        if (ShoreDistance < 0)
        {
            float Path = LakeMap::PathAmount(P);
            for (float Off : { 100.0f, 200.0f, 300.0f })
                Path += LakeMap::PathAmount(P + FVector2D(Off, 0)) + LakeMap::PathAmount(P - FVector2D(Off, 0))
                      + LakeMap::PathAmount(P + FVector2D(0, Off)) + LakeMap::PathAmount(P - FVector2D(0, Off));
            if (Path > 0.4f) return TEXT("path");
        }
        if (LakeMap::StreamDistance(P) < 400) return TEXT("stream");
        for (const LakeMap::FFootbridge& Bridge : LakeMap::Footbridges())
            if (SegmentDistance(P, Bridge.A, Bridge.B) < 400) return TEXT("footbridge");
        for (const LakeMap::FBankThing& Thing : { LakeMap::Spring(), LakeMap::Lookout(), LakeMap::OldPine() })
            if (FVector2D::Distance(P, Thing.Location) < 500) return TEXT("lookout");
        return nullptr;
    }

    int32 BiomeOf(const FVector2D& P)
    {
        const float SD = LakeMap::ShoreDistance(P.X, P.Y);    // cm, positive in the water
        if (Clearing(P, SD)) return Nothing;
        const float Forest = LakeMap::ForestAmount(P);
        const float Back = LakeMap::BackwaterAmount(P.X, P.Y);
        const bool bLowBank = LakeMap::BluffHeight(P) < 0.7f;
        if (SD > 0)
        {
            // In the water: reeds and reedmace out to about 1.1 m deep along the low banks, wider in
            // the backwater, the bay and the upper end, patchy under the trees; none in the casting
            // lanes, over the lily beds or under the bluffs.
            if (LakeMap::Depth(P.X, P.Y) > 110 || !LakeMap::LilyAllowed(P)) return Nothing;
            if (LakeMap::InCattails(P)) return Cattail;
            if (LakeMap::InSedgeTussocks(P)) return Sedge;
            if (LakeMap::LilyAmount(P.X, P.Y) > 0.3f || !bLowBank) return Nothing;
            if (SD > 260 + 340 * Back) return Nothing;
            const float Patch = 0.55f + 0.35f * Noise(P, 2500) + 0.15f * Noise(P, 600) + 0.4f * Back - 0.2f * Forest;
            return Patch > 0.4f ? Reed : Nothing;
        }
        // Steep faces (clay cliffs, the step of the meadow) stay bare.
        const float Gx = LakeMap::Ground(P.X + 100, P.Y) - LakeMap::Ground(P.X - 100, P.Y);
        const float Gy = LakeMap::Ground(P.X, P.Y + 100) - LakeMap::Ground(P.X, P.Y - 100);
        if (FMath::Sqrt(Gx * Gx + Gy * Gy) / 200 > 1.0f) return Nothing;
        const float Inland = -SD;
        if (Inland < 250 && bLowBank && 0.5f + 0.4f * Noise(P + FVector2D(700, 0), 1800) + 0.4f * Back - 0.2f * Forest > 0.45f) return Reed;
        if (Forest > 0.8f)
        {
            // Alders along the low wooded banks, a ragged strip 2-16 m wide.
            if (bLowBank && Inland < 900 + 700 * Noise(P + FVector2D(0, 900), 2200)) return Alder;
            float GroveRadius;
            const FVector2D Grove = LakeMap::BirchGrove(&GroveRadius);
            if (FVector2D::Distance(P, Grove) < GroveRadius * (1 + 0.2f * Noise(P, 1500))) return Birch;
            // The forest in big patches: pines leaning to the high dry ground, spruces to the low damp
            // ground and the streams, mixed wood between, and stands of birch here and there.
            const float Above = (LakeMap::Ground(P.X, P.Y) - LakeMap::SurfaceZ) / 100;
            const float Wet = 1 - S(1500, 4000, LakeMap::StreamDistance(P));
            const float Big = Noise(P + FVector2D(0, 3000), 9000), Middle = Noise(P + FVector2D(4000, 0), 3000);
            if (Middle > 0.45f && Wet < 0.5f) return Birch;
            if (0.5f * S(3, 10, Above) - 0.6f * Wet + 0.8f * Big > 0.45f) return Pine;
            if (0.7f * Wet + 0.4f * (1 - S(1.5f, 5, Above)) - 0.8f * Big + 0.3f * Middle > 0.45f) return Spruce;
            return Mixed;
        }
        if (Forest > 0.2f) return Edge;
        const float Cover = VillageLawnCover(P, Inland, Forest);
        if (Cover > 0.53f + 0.42f * Noise(P, 490.0f)) return VillageLawn;
        return Meadow;
    }

    FProperty* FindProperty(UObject* Object, const FString& Name)
    {
        if (!Object) return nullptr;
        if (FProperty* Found = FindFProperty<FProperty>(Object->GetClass(), *Name)) return Found;
        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
            if (It->GetAuthoredName() == Name) return *It;
        return nullptr;
    }
}

TArray<FString> ULakeBiomeLibrary::BiomeNames()
{
    TArray<FString> Names;
    for (const FBiomeInfo& Info : Biomes) Names.Add(Info.Name);
    return Names;
}

FLinearColor ULakeBiomeLibrary::BiomeColor(const FString& Name)
{
    for (const FBiomeInfo& Info : Biomes)
        if (Name == Info.Name) return FLinearColor(Info.Color.R / 255.0f, Info.Color.G / 255.0f, Info.Color.B / 255.0f, 0.0f);
    return FLinearColor::Black;
}

FString ULakeBiomeLibrary::BiomeAt(float X, float Y)
{
    const int32 Biome = BiomeOf(FVector2D(X, Y));
    return Biome == Nothing ? FString() : FString(Biomes[Biome].Name);
}

FString ULakeBiomeLibrary::ClearingAt(float X, float Y)
{
    const FVector2D P(X, Y);
    const float SD = LakeMap::ShoreDistance(X, Y);
    if (const TCHAR* Reason = Clearing(P, SD)) return Reason;
    if (SD > 0) return LakeMap::Depth(X, Y) > 110 ? TEXT("deep water") : TEXT("open water");
    const float Gx = LakeMap::Ground(X + 100, Y) - LakeMap::Ground(X - 100, Y);
    const float Gy = LakeMap::Ground(X, Y + 100) - LakeMap::Ground(X, Y - 100);
    if (FMath::Sqrt(Gx * Gx + Gy * Gy) / 200 > 1.0f) return TEXT("steep");
    return FString();
}

FVector4 ULakeBiomeLibrary::BiomeMapRect(bool bShore)
{
    const FBox2D B = bShore ? LakeMap::LakeBounds().ExpandBy(4000) : LakeMap::TerrainBounds;
    return FVector4(B.Min.X, B.Min.Y, B.Max.X, B.Max.Y);
}

FVector4 ULakeBiomeLibrary::VillageLawnMapRect()
{
    return FVector4(VillageLawnBounds.Min.X, VillageLawnBounds.Min.Y, VillageLawnBounds.Max.X, VillageLawnBounds.Max.Y);
}

FString ULakeBiomeLibrary::BuildBiomeMap(const FString& PackagePath, FVector4 Rect, int32 Width, int32 Height, bool bLawnCoverage)
{
#if WITH_EDITOR
    const FBox2D Bounds(FVector2D(Rect.X, Rect.Y), FVector2D(Rect.Z, Rect.W));
    const FVector2D Extent = Bounds.GetSize();
    TArray<FColor> Pixels;
    Pixels.SetNumZeroed(Width * Height);
    TArray<TArray<int32>> RowCounts;
    RowCounts.SetNum(Height);
    ParallelFor(Height, [&](int32 Row)
    {
        TArray<int32>& Counts = RowCounts[Row];
        Counts.SetNumZeroed(BiomeCount + 1);
        for (int32 Column = 0; Column < Width; ++Column)
        {
            const FVector2D P(Bounds.Min.X + (Column + 0.5) / Width * Extent.X, Bounds.Min.Y + (Row + 0.5) / Height * Extent.Y);
            if (bLawnCoverage)
            {
                const float Shore = LakeMap::ShoreDistance(P.X, P.Y);
                const float Cover = Clearing(P, Shore) ? 0.0f : VillageLawnCover(P, -Shore, LakeMap::ForestAmount(P));
                const uint8 Value = FMath::RoundToInt(FMath::Clamp(Cover, 0.0f, 1.0f) * 255.0f);
                Pixels[Row * Width + Column] = FColor(Value, Value, Value, 255);
                continue;
            }
            const int32 Biome = BiomeOf(P);
            FColor Colour = Biome == Nothing ? FColor::Black : Biomes[Biome].Color;
            Colour.A = 255;
            Pixels[Row * Width + Column] = Colour;
            ++Counts[Biome + 1];
        }
    });

    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    TArray64<uint8> Png;
    FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
    FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / (AssetName + TEXT(".png"))));

    UPackage* Package = CreatePackage(*PackagePath);
    Package->FullyLoad();
    UTexture2D* Texture = FindObject<UTexture2D>(Package, *AssetName);
    const bool bNew = !Texture;
    if (bNew) Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    Texture->PreEditChange(nullptr);
    Texture->Source.Init(Width, Height, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
    Texture->SRGB = false;
    Texture->CompressionSettings = TC_VectorDisplacementmap;     // exact colours
    Texture->MipGenSettings = TMGS_NoMipmaps;
    Texture->Filter = bLawnCoverage ? TF_Bilinear : TF_Nearest;
    Texture->AddressX = Texture->AddressY = TA_Clamp;
    Texture->PostEditChange();
    if (bNew) FAssetRegistryModule::AssetCreated(Texture);
    Package->MarkPackageDirty();

    if (bLawnCoverage) return FString::Printf(TEXT("%s %dx%d village lawn coverage"), *AssetName, Width, Height);

    TArray<int32> Total;
    Total.SetNumZeroed(BiomeCount + 1);
    for (const TArray<int32>& Counts : RowCounts)
        for (int32 I = 0; I <= BiomeCount; ++I) Total[I] += Counts[I];
    FString Out = FString::Printf(TEXT("%s %dx%d over %.0f x %.0f m, nothing %.1f%%"), *AssetName, Width, Height, Extent.X / 100, Extent.Y / 100, 100.0 * Total[0] / Pixels.Num());
    for (int32 I = 0; I < BiomeCount; ++I)
        Out += FString::Printf(TEXT("\n%s %.2f%% (%.2f ha)"), Biomes[I].Name, 100.0 * Total[I + 1] / Pixels.Num(), Total[I + 1] * (Extent.X / Width) * (Extent.Y / Height) / 1e8);
    return Out;
#else
    return FString();
#endif
}

AActor* ULakeBiomeLibrary::BakeInstances(AActor* Source, const FString& Label)
{
#if WITH_EDITOR
    if (!Source || !Source->GetWorld()) return nullptr;
    UWorld* World = Source->GetWorld();
    AActor* Baked = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
    if (!Baked) return nullptr;
    Baked->SetActorLabel(Label);
    USceneComponent* Root = NewObject<USceneComponent>(Baked, TEXT("Root"), RF_Transactional);
    Root->SetMobility(EComponentMobility::Static);
    Baked->SetRootComponent(Root);
    Baked->AddInstanceComponent(Root);
    Root->RegisterComponent();
    // One component per mesh and settings: Biome Core spreads a mesh over several components.
    TMap<FString, UHierarchicalInstancedStaticMeshComponent*> Groups;
    TInlineComponentArray<UInstancedStaticMeshComponent*> Sources(Source);
    for (UInstancedStaticMeshComponent* From : Sources)
    {
        UStaticMesh* Mesh = From->GetStaticMesh();
        if (!Mesh || From->GetInstanceCount() == 0) continue;
        const FString Key = FString::Printf(TEXT("%s_%d_%d_%d_%d"), *Mesh->GetName(), (int32)From->GetCollisionEnabled(), From->InstanceEndCullDistance,
                                            From->CastShadow, From->bAffectDistanceFieldLighting);
        UHierarchicalInstancedStaticMeshComponent*& To = Groups.FindOrAdd(Key);
        if (!To)
        {
            To = NewObject<UHierarchicalInstancedStaticMeshComponent>(Baked, *FString::Printf(TEXT("HISM_%s_%d"), *Mesh->GetName(), Groups.Num()), RF_Transactional);
            To->SetMobility(EComponentMobility::Static);
            To->SetStaticMesh(Mesh);
            for (int32 Slot = 0; Slot < From->GetNumOverrideMaterials(); ++Slot) To->SetMaterial(Slot, From->GetMaterial(Slot));
            To->SetCollisionProfileName(From->GetCollisionProfileName());
            To->SetCollisionEnabled(From->GetCollisionEnabled());
            To->SetCullDistances(From->InstanceStartCullDistance, From->InstanceEndCullDistance);
            To->SetCastShadow(From->CastShadow);
            To->bAffectDistanceFieldLighting = From->bAffectDistanceFieldLighting;
            To->bAffectDynamicIndirectLighting = From->bAffectDynamicIndirectLighting;
            To->SetEvaluateWorldPositionOffset(true);
            To->SetupAttachment(Root);
            Baked->AddInstanceComponent(To);
            To->RegisterComponent();
        }
        TArray<FTransform> Transforms;
        Transforms.Reserve(From->GetInstanceCount());
        for (int32 I = 0; I < From->GetInstanceCount(); ++I)
        {
            FTransform T;
            From->GetInstanceTransform(I, T, true);
            Transforms.Add(T);
        }
        To->AddInstances(Transforms, false, true);
    }
    Baked->MarkPackageDirty();
    return Baked;
#else
    return nullptr;
#endif
}

UObject* ULakeBiomeLibrary::MakeGraphInstance(const FString& PackagePath, UObject* Graph)
{
#if WITH_EDITOR
    UPCGGraphInterface* Source = Cast<UPCGGraphInterface>(Graph);
    if (!Source) return nullptr;
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    UPackage* Package = CreatePackage(*PackagePath);
    Package->FullyLoad();
    UPCGGraphInstance* Instance = FindObject<UPCGGraphInstance>(Package, *AssetName);
    const bool bNew = !Instance;
    if (bNew) Instance = NewObject<UPCGGraphInstance>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    Instance->SetGraph(Source);
    if (bNew) FAssetRegistryModule::AssetCreated(Instance);
    Package->MarkPackageDirty();
    return Instance;
#else
    return nullptr;
#endif
}

FString ULakeBiomeLibrary::DescribeProperties(UObject* Object, bool bBlueprintOnly)
{
    FString Out;
    if (!Object) return Out;
    for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
    {
        if (bBlueprintOnly && !Cast<UBlueprintGeneratedClass>(It->GetOwnerClass())) continue;
        FString Value;
        It->ExportTextItem_InContainer(Value, Object, nullptr, Object, PPF_None);
        Out += FString::Printf(TEXT("%s (%s) [%s] = %s\n"), *It->GetName(), *It->GetAuthoredName(), *It->GetCPPType(), *Value.Left(2000));
    }
    return Out;
}

FString ULakeBiomeLibrary::GetPropertyText(UObject* Object, const FString& Name)
{
    FString Value;
    if (FProperty* Property = FindProperty(Object, Name)) Property->ExportTextItem_InContainer(Value, Object, nullptr, Object, PPF_None);
    return Value;
}

bool ULakeBiomeLibrary::SetPropertyText(UObject* Object, const FString& Name, const FString& Text)
{
    FProperty* Property = FindProperty(Object, Name);
    if (!Property) return false;
#if WITH_EDITOR
    Object->Modify();
    Object->PreEditChange(Property);
#endif
    const TCHAR* Rest = Property->ImportText_InContainer(*Text, Object, Object, PPF_None);
#if WITH_EDITOR
    FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
    Object->PostEditChangeProperty(Event);
#endif
    Object->MarkPackageDirty();
    return Rest != nullptr;
}
