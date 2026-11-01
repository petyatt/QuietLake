#include "LakeBirds.h"
#include "LakeMap.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"

ALakeBirds::ALakeBirds()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = 0.05f;
    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("BirdFlocks"));
    RootComponent = Mesh;
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mesh->SetCastShadow(false);
    Mesh->bAffectDistanceFieldLighting = false;
    Mesh->bAffectDynamicIndirectLighting = false;
}

void ALakeBirds::BeginPlay()
{
    Super::BeginPlay();
    FVector2D Facing;
    const FVector2D Camp = LakeMap::StartSpot(&Facing);
    const FVector2D Side(-Facing.Y, Facing.X);
    Centres = { FVector(Camp + Facing * 5500 + Side * 3500, LakeMap::SurfaceZ + 2300),
        FVector(LakeMap::IslandCentre(), LakeMap::SurfaceZ + 3400),
        FVector(LakeMap::WestBay(), LakeMap::SurfaceZ + 2900) };
    Mesh->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Bark.M_Bark")));
    Tick(0);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BIRDS 3 flocks, 12 birds, cosmetic 20 Hz"));
}

void ALakeBirds::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (Centres.IsEmpty()) return;
    const double Time = GetWorld()->GetTimeSeconds();
    TArray<FVector> Vertices, Normals;
    TArray<int32> Indices;
    TArray<FVector2D> UV;
    TArray<FLinearColor> Colours;
    TArray<FProcMeshTangent> Tangents;
    for (int32 Bird = 0; Bird < 12; ++Bird)
    {
        const int32 Flock = Bird / 4;
        const float Phase = Time * (0.18f + Flock * 0.025f) + (Bird % 4) * 0.12f;
        const float Radius = 2200 + 120 * (Bird % 4);
        FVector Position = Centres[Flock] + FVector(FMath::Cos(Phase) * Radius, FMath::Sin(Phase) * Radius * 0.55f,
            90 * FMath::Sin(Phase * 2 + Bird));
        Position.Z = FMath::Max(Position.Z, double(LakeMap::Ground(Position.X, Position.Y) + 2300));
        const FVector Forward = FVector(-FMath::Sin(Phase), FMath::Cos(Phase) * 0.55f, 0).GetSafeNormal();
        const FVector Side(-Forward.Y, Forward.X, 0);
        // A short burst of wingbeats alternates with a glide, separately for every bird.
        const float Beat = FMath::Fmod(float(Time) + Bird * 0.73f, 5.0f) < 2.0f
            ? FMath::Sin(Time * 24 + Bird * 1.7f) : 0.12f;
        auto At = [&](float X, float Y, float Z) { return Position + Forward * X + Side * Y + FVector(0, 0, Z); };
        auto Triangle = [&](FVector A, FVector B, FVector C)
        {
            const int32 Base = Vertices.Num();
            Vertices.Append({ A, B, C });
            const FVector Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
            for (int32 I = 0; I < 3; ++I)
            {
                Normals.Add(Normal); UV.Add(FVector2D::ZeroVector);
                Colours.Add(FLinearColor(0.1f, 0.09f, 0.07f)); Tangents.Add(FProcMeshTangent(Forward, false));
            }
            Indices.Append({ Base, Base + 1, Base + 2, Base + 2, Base + 1, Base });
        };
        for (float Sign : { -1.0f, 1.0f })
        {
            const FVector Shoulder = At(3, Sign * 2.5f, 1);
            const FVector Elbow = At(-2, Sign * 16, Beat * 9);
            const FVector Tip = At(-14, Sign * 32, Beat * 18);
            Triangle(Shoulder, Elbow, At(-10, Sign * 8, Beat * 3));
            Triangle(Elbow, Tip, At(-12, Sign * 8, Beat * 3));
            Triangle(At(12, 0, 0), At(-13, 0, 0), At(-4, Sign * 4, 3));
            Triangle(At(-12, 0, 0), At(-24, Sign * 6, 1), At(-19, 0, 1));
        }
    }
    if (!bReady)
    {
        Mesh->CreateMeshSection_LinearColor(0, Vertices, Indices, Normals, UV, Colours, Tangents, false);
        bReady = true;
    }
    else Mesh->UpdateMeshSection_LinearColor(0, Vertices, Normals, UV, Colours, Tangents);
}
