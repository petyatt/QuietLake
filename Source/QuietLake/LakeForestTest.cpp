#if WITH_DEV_AUTOMATION_TESTS
#include "LakeForest.h"
#include "LakeMap.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "Elements/PCGSkinnedMeshSpawner.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeForestZonesTest, "QuietLake.Forest.Zones", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeForestZonesTest::RunTest(const FString&)
{
    auto* Settings = NewObject<ULakeForestPointsSettings>();
    TArray<FLakeForestTree> First, Again;
    Settings->BuildTrees(First);
    Settings->BuildTrees(Again);
    TestTrue(TEXT("The lake has a substantial forest"), First.Num() > 1000 && First.Num() < 15000);
    TestEqual(TEXT("Regenerating with the same seed keeps the number of trees"), Again.Num(), First.Num());
    int32 Edge = 0, Interior = 0;
    bool Clearings = true, DryGround = true, Repeatable = First.Num() == Again.Num();
    for (int32 I = 0; I < First.Num(); ++I)
    {
        const auto& Tree = First[I];
        const FVector P = Tree.Transform.GetLocation();
        Clearings &= FVector2D::Distance(FVector2D(P), LakeMap::VillageCentre()) >= 13000
            && LakeMap::SpotAmount(FVector2D(P)) <= 0.03f;
        DryGround &= !LakeMap::IsWater(P.X, P.Y) && LakeMap::ShoreDistance(P.X, P.Y) <= -300
            && FMath::Abs(P.Z - (LakeMap::Ground(P.X, P.Y) - 15)) < 0.1f;
        if (Again.IsValidIndex(I)) Repeatable &= Tree.Transform.Equals(Again[I].Transform, 0.001) && Tree.Mesh == Again[I].Mesh;
        if (Tree.bWind) ++Edge; else ++Interior;
    }
    TestTrue(TEXT("Village reserve and fishing clearings contain no generated trees"), Clearings);
    TestTrue(TEXT("Trees follow actual dry terrain instead of the water plane"), DryGround);
    TestTrue(TEXT("Seed reproduces every transform and species for peers"), Repeatable);
    TestTrue(TEXT("Both a moving forest edge and a cheaper interior are generated"), Edge > 0 && Interior > Edge);
    Settings->Density = 0;
    Settings->BuildTrees(Again);
    TestEqual(TEXT("Zero density is an empty forest"), Again.Num(), 0);
    auto* Graph = NewObject<UPCGGraph>();
    TestTrue(TEXT("An editable graph is built"), ULakeForestLibrary::ConfigureForestGraph(Graph));
    int32 Spawners = 0;
    for (const auto* Node : Graph->GetNodes()) Spawners += Node->GetSettings()->IsA<UPCGSkinnedMeshSpawnerSettings>();
    TestEqual(TEXT("The graph uses two native skinned mesh spawners"), Spawners, 2);
    TestFalse(TEXT("Running setup again preserves an authored graph"), ULakeForestLibrary::ConfigureForestGraph(Graph));
    AddInfo(FString::Printf(TEXT("Forest: %d trees, %d edge, %d interior; clearings and seed verified"), First.Num(), Edge, Interior));
    return true;
}
#endif
