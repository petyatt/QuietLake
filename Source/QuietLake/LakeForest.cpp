#include "LakeForest.h"
#include "LakeMap.h"
#include "PCGComponent.h"
#include "PCGContext.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "Data/PCGPointData.h"
#include "Elements/PCGSkinnedMeshSpawner.h"
#include "MeshSelectors/PCGSkinnedMeshSelector.h"
#include "Metadata/PCGMetadata.h"
#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "DynamicWindData.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
    constexpr const TCHAR* GraphPath = TEXT("/Game/Forest/PCG_LakeForest.PCG_LakeForest");
    float EdgeStart(const FVector2D& P) { return 7 + 7 * (0.5f + 0.5f * FMath::PerlinNoise2D(P / 4000)); }
    float SegmentDistance(const FVector2D& P, const FVector2D& A, const FVector2D& B)
    {
        const FVector2D AB = B - A;
        return FVector2D::Distance(P, A + AB * FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(1.0, AB.SizeSquared()), 0.0, 1.0));
    }
}

ULakeForestPointsSettings::ULakeForestPointsSettings()
{
    WindData = CreateDefaultSubobject<UDynamicWindData>(TEXT("ForestWind"));
    auto Family = [](const TCHAR* Folder, const TCHAR* Prefix, TArray<TSoftObjectPtr<USkinnedAsset>>& Out)
    {
        for (TCHAR Variant : { TCHAR('A'), TCHAR('B'), TCHAR('C'), TCHAR('D') })
        {
            const FString Name = FString::Printf(TEXT("%s_%c"), Prefix, Variant);
            Out.Add(TSoftObjectPtr<USkinnedAsset>(FSoftObjectPath(FString::Printf(TEXT("/Game/Megaplant_Library/%s/%s.%s"), Folder, *Name, *Name))));
        }
    };
    Family(TEXT("Tree_Silver_Birch/Tree_Silver_Birch_01"), TEXT("Tree_Silver_Birch_01"), Birches);
    Family(TEXT("Tree_Black_Alder/Tree_Black_Alder_01"), TEXT("Tree_Black_Alder_01"), Alders);
    Family(TEXT("Tree_Baltic_Pine/Tree_Baltic_Pine_01"), TEXT("Tree_Baltic_Pine_01"), Pines);
    Family(TEXT("Tree_Norway_Spruce/Tree_Norway_Spruce_01"), TEXT("Tree_Norway_Spruce_01"), Spruces);
    Spruces.Add(TSoftObjectPtr<USkinnedAsset>(FSoftObjectPath(TEXT("/Game/Plants/PV/SKM_PV_Spruce.SKM_PV_Spruce"))));
}

bool ULakeForestPointsSettings::AllowsTree(const FVector2D& P) const
{
    const float Land = -LakeMap::ShoreDistance(P.X, P.Y);
    if (Land < 300 || FVector2D::Distance(P, LakeMap::VillageCentre()) < FMath::Max(13000.0f, VillageClearance)
        || FVector2D::Distance(P, LakeMap::StartSpot()) < 1600 || LakeMap::SpotAmount(P) > 0.03f) return false;
    // The footpath round the lake, the streams, the old oak and the willows stay clear of the forest.
    if (LakeMap::PathAmount(P) > 0.15f || LakeMap::StreamDistance(P) < 450) return false;
    if (FVector2D::Distance(P, LakeMap::OldOak().Location) < 900) return false;
    for (const LakeMap::FBankThing& Willow : LakeMap::Willows()) if (FVector2D::Distance(P, Willow.Location) < 700) return false;
    const auto& Pier = LakeMap::Pier();
    if (SegmentDistance(P, Pier.Root - Pier.Dir * 1700, Pier.Root + Pier.Dir * Pier.Length) < 650) return false;
    if (LakeMap::ForestAmount(P) < 0.55f) return false;
    const float DX = LakeMap::Ground(P.X + 100, P.Y) - LakeMap::Ground(P.X - 100, P.Y);
    const float DY = LakeMap::Ground(P.X, P.Y + 100) - LakeMap::Ground(P.X, P.Y - 100);
    return FVector2D(DX, DY).Size() / 200 < 0.75f;
}

void ULakeForestPointsSettings::BuildTrees(TArray<FLakeForestTree>& Out) const
{
    Out.Reset();
    if (Density <= 0) return;
    FRandomStream Random(ForestSeed);
    const FBox2D Bounds = LakeMap::TerrainBounds.ExpandBy(-500);
    const float Cell = FMath::Clamp(Spacing, 300.0f, 1500.0f);
    for (float Y = Bounds.Min.Y; Y < Bounds.Max.Y; Y += Cell)
        for (float X = Bounds.Min.X; X < Bounds.Max.X; X += Cell)
        {
            const FVector2D P(X + Random.FRandRange(0, Cell), Y + Random.FRandRange(0, Cell));
            if (!AllowsTree(P)) continue;
            const float Land = -LakeMap::ShoreDistance(P.X, P.Y) / 100;
            const float Edge = EdgeStart(P);
            const float EdgeDensity = FMath::SmoothStep(Edge - 3, Edge + 8, Land);
            // Broad gaps and denser groves break the uniform wall of trees along the lake.
            const float Groves = 0.78f + 0.32f * FMath::PerlinNoise2D(P / 2100);
            const float Chance = (Land < 70 ? 0.5f : Land < 180 ? 0.24f : 0.13f)
                * EdgeDensity * Groves * FMath::Clamp(Density, 0.0f, 1.5f);
            if (Random.FRand() > Chance) continue;
            const float Pick = Random.FRand();
            const auto& Species = LakeMap::BluffHeight(P) > 2.5f ? Pines : Land < Edge + 20
                ? (Pick < 0.5f ? Birches : Pick < 0.85f ? Alders : Pines) : (Pick < 0.5f ? Pines : Spruces);
            if (Species.IsEmpty()) continue;
            FLakeForestTree& Tree = Out.AddDefaulted_GetRef();
            const float Size = Random.FRandRange(FMath::Clamp(MinimumScale, 0.4f, 1.2f), FMath::Clamp(FMath::Max(MinimumScale, MaximumScale), 0.4f, 1.4f));
            Tree.Transform = FTransform(FRotator(0, Random.FRandRange(0, 360), 0), FVector(P, LakeMap::Ground(P.X, P.Y) - 15), FVector(Size));
            Tree.Mesh = Species[Random.RandRange(0, Species.Num() - 1)].ToSoftObjectPath();
            Tree.bWind = Land < Edge + 14;
        }
}

TArray<FPCGPinProperties> ULakeForestPointsSettings::OutputPinProperties() const
{
    return { FPCGPinProperties(TEXT("ForestEdge"), EPCGDataType::Point), FPCGPinProperties(TEXT("ForestInterior"), EPCGDataType::Point) };
}

class FLakeForestPointsElement : public IPCGElement
{
public:
    virtual bool CanExecuteOnlyOnMainThread(FPCGContext*) const override { return true; }
    virtual bool IsCacheable(const UPCGSettings*) const override { return false; }
protected:
    virtual bool ExecuteInternal(FPCGContext* Context) const override
    {
        const auto* Settings = Context->GetInputSettings<ULakeForestPointsSettings>();
        TArray<FLakeForestTree> Trees;
        Settings->BuildTrees(Trees);
        for (bool Wind : { true, false })
        {
            auto* Data = FPCGContext::NewObject_AnyThread<UPCGPointData>(Context);
            auto* MeshPath = Data->Metadata->CreateAttribute<FString>(TEXT("Mesh"), FString(), false, true);
            auto& Points = Data->GetMutablePoints();
            for (const auto& Tree : Trees)
            {
                if (Tree.bWind != Wind) continue;
                FPCGPoint& Point = Points.AddDefaulted_GetRef();
                Point.Transform = Tree.Transform;
                Point.Density = 1;
                Point.Seed = GetTypeHash(Tree.Transform.GetLocation());
                Point.MetadataEntry = Data->Metadata->AddEntry();
                MeshPath->SetValue(Point.MetadataEntry, Tree.Mesh.ToString());
            }
            FPCGTaggedData& Output = Context->OutputData.TaggedData.AddDefaulted_GetRef();
            Output.Pin = Wind ? TEXT("ForestEdge") : TEXT("ForestInterior");
            Output.Data = Data;
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_PCG_POINTS %s=%d"), Wind ? TEXT("edge") : TEXT("interior"), Points.Num());
        }
        if (auto* Forest = Context->GetTypedExecutionTarget<ALakeForest>()) Forest->UpdateTrunks(Trees);
        return true;
    }
};

FPCGElementPtr ULakeForestPointsSettings::CreateElement() const { return MakeShared<FLakeForestPointsElement>(); }

bool ULakeForestLibrary::ConfigureForestGraph(UPCGGraph* Graph)
{
#if WITH_EDITOR
    if (!Graph || !Graph->GetNodes().IsEmpty()) return false;
    ULakeForestPointsSettings* Points = nullptr;
    UPCGNode* Source = Graph->AddNodeOfType<ULakeForestPointsSettings>(Points);
    Source->SetNodeTitle(TEXT("Lake zones / village and fishing clearings"));
    Source->SetNodePosition(0, 0);
    for (bool Wind : { true, false })
    {
        UPCGSkinnedMeshSpawnerSettings* Settings = nullptr;
        UPCGNode* Spawn = Graph->AddNodeOfType<UPCGSkinnedMeshSpawnerSettings>(Settings);
        Spawn->SetNodeTitle(Wind ? TEXT("Forest edge / wind") : TEXT("Forest interior / distant"));
        Spawn->SetNodePosition(420, Wind ? -140 : 160);
        Settings->MeshSelectorParameters->MeshAttribute.SetAttributeName(TEXT("Mesh"));
        auto& Desc = Settings->MeshSelectorParameters->TemplateDescriptor;
        Desc.InstanceStartCullDistance = Wind ? 32000 : 88000;
        Desc.InstanceEndCullDistance = Wind ? 40000 : 110000;
        Desc.bCastShadow = Wind;
        Desc.TransformProvider = Wind ? Points->WindData.Get() : nullptr;
        Settings->bSynchronousLoad = true;
        Graph->AddEdge(Source, Wind ? TEXT("ForestEdge") : TEXT("ForestInterior"), Spawn, PCGPinConstants::DefaultInputLabel);
        Graph->AddEdge(Spawn, PCGPinConstants::DefaultOutputLabel, Graph->GetOutputNode(), PCGPinConstants::DefaultOutputLabel);
    }
    Graph->MarkPackageDirty();
    return true;
#else
    return false;
#endif
}

ALakeForest::ALakeForest()
{
    auto* Bounds = CreateDefaultSubobject<UBoxComponent>(TEXT("ForestBounds"));
    RootComponent = Bounds;
    Bounds->SetBoxExtent(FVector(LakeMap::TerrainBounds.GetExtent(), 6000));
    Bounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ForestPCG = CreateDefaultSubobject<UPCGComponent>(TEXT("ForestPCG"));
    ForestPCG->GenerationTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
    Trunks = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("ForestTrunks"));
    Trunks->SetupAttachment(RootComponent);
    Trunks->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Trunks->SetCollisionResponseToAllChannels(ECR_Block);
    Trunks->SetHiddenInGame(true);
    Trunks->SetCastShadow(false);
}

bool ALakeForest::HasGraph()
{
    return !FParse::Param(FCommandLine::Get(), TEXT("LegacyForest")) && LoadObject<UPCGGraph>(nullptr, GraphPath) != nullptr;
}

void ALakeForest::BeginPlay()
{
    Super::BeginPlay();
    auto* Graph = LoadObject<UPCGGraph>(nullptr, GraphPath);
    if (!Graph) return;
    Trunks->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
    ForestPCG->OnPCGGraphGeneratedDelegate.AddUObject(this, &ALakeForest::Generated);
    ForestPCG->SetGraph(Graph);
    ForestPCG->GenerateLocal(true);
}

void ALakeForest::UpdateTrunks(const TArray<FLakeForestTree>& Trees)
{
    ExpectedTrees = Trees.Num();
    Trunks->ClearInstances();
    TArray<FTransform> Instances;
    for (const auto& Tree : Trees)
    {
        const double Size = Tree.Transform.GetScale3D().X;
        Instances.Emplace(FRotator::ZeroRotator, Tree.Transform.GetLocation() + FVector(0, 0, 250 * Size), FVector(0.4 * Size, 0.4 * Size, 5 * Size));
    }
    Trunks->AddInstances(Instances, false, true);
}

void ALakeForest::Generated(UPCGComponent*)
{
    TInlineComponentArray<UInstancedSkinnedMeshComponent*> Components(this);
    int32 Instances = 0;
    for (const auto* Component : Components) Instances += Component->GetInstanceCount();
    ++GenerationCount;
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_PCG_FOREST_READY pass=%d instances=%d expected=%d components=%d trunks=%d"),
        GenerationCount, Instances, ExpectedTrees, Components.Num(), Trunks->GetInstanceCount());
    ensureMsgf(Instances == ExpectedTrees, TEXT("PCG forest instance count does not match its terrain points"));
    if (GenerationCount == 1 && FParse::Param(FCommandLine::Get(), TEXT("ForestRegenCheck")))
    {
        FTimerHandle Regenerate;
        GetWorldTimerManager().SetTimer(Regenerate, [this] { ForestPCG->GenerateLocal(true); }, 1.0f, false);
    }
}
