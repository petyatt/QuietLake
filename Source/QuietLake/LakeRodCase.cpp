#include "LakeRodCase.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
    UStaticMeshComponent* CasePart(AActor* Owner, const TCHAR* Name, const TCHAR* Mesh, USceneComponent* Parent)
    {
        auto* C = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
        if (Parent) C->SetupAttachment(Parent);
        C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Mesh));
        C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        C->SetCastShadow(true);
        return C;
    }
    constexpr float LidOpenDegrees = 118.0f;
    constexpr float PackedLength = 140.0f;
    /** The lid turns about the hinge (along X) so that its free edge rises: the sign that does that. */
    FQuat LidTurn(float Degrees)
    {
        static const float Sign = FQuat(FVector::ForwardVector, FMath::DegreesToRadians(90.0f)).RotateVector(FVector(0, 1, 0)).Z > 0 ? 1.0f : -1.0f;
        return FQuat(FVector::ForwardVector, FMath::DegreesToRadians(Degrees * Sign));
    }
}

ALakeRodCase::ALakeRodCase()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;
    Body = CasePart(this, TEXT("Body"), TEXT("/Game/Tackle/SM_RodCase.SM_RodCase"), nullptr);
    RootComponent = Body;
    // The lid's origin is on its hinge: the long edge at -Y, half way up.
    Lid = CasePart(this, TEXT("Lid"), TEXT("/Game/Tackle/SM_RodCaseLid.SM_RodCaseLid"), Body);
    Lid->SetRelativeLocation(FVector(0, -Width / 2, Height / 2));
    Strap = CasePart(this, TEXT("Strap"), TEXT("/Game/Tackle/SM_RodCaseStrap.SM_RodCaseStrap"), Body);
    for (const TCHAR* Kind : { TEXT("Light"), TEXT("Medium"), TEXT("Heavy") })
    {
        auto* Rod = CasePart(this, *FString::Printf(TEXT("Packed%s"), Kind), *FString::Printf(TEXT("/Game/Tackle/SM_RodPacked_%s.SM_RodPacked_%s"), Kind, Kind), Body);
        Rod->SetVisibility(false);
        Packed.Add(Rod);
    }
}

FVector ALakeRodCase::SlotLocation(int32 Slot)
{
    // Three lanes across the case (the dividers are 7.3 cm apart), the rods resting on the bottom.
    return FVector(0, (FMath::Clamp(Slot, 0, 2) - 1) * 7.33f, 2.3f);
}

void ALakeRodCase::Show(const FTransform& Where, bool bOnBack, bool bOpen, const TArray<int32>& SlotOfRod, float DeltaSeconds)
{
    SetActorTransform(Where);
    // The lid swings open or shut in about 0.4 s (only the lid: the rods simply appear and go).
    Lid01 = FMath::FInterpConstantTo(Lid01, bOpen && !bOnBack ? 1.0f : 0.0f, DeltaSeconds, 2.5f);
    Lid->SetRelativeRotation(LidTurn(LidOpenDegrees * FMath::InterpEaseInOut(0.0f, 1.0f, Lid01, 2.0f)));
    Strap->SetVisibility(bOnBack);
    for (int32 Rod = 0; Rod < Packed.Num(); ++Rod)
    {
        const int32 Slot = SlotOfRod.IsValidIndex(Rod) ? SlotOfRod[Rod] : -1;
        Packed[Rod]->SetVisibility(Slot >= 0);
        if (Slot >= 0) Packed[Rod]->SetRelativeLocation(SlotLocation(Slot) - FVector(PackedLength / 2, 0, 0));
    }
}

ALakeGhost::ALakeGhost()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    CaseBody = CasePart(this, TEXT("CaseBody"), TEXT("/Game/Tackle/SM_RodCase.SM_RodCase"), RootComponent);
    CaseLid = CasePart(this, TEXT("CaseLid"), TEXT("/Game/Tackle/SM_RodCaseLid.SM_RodCaseLid"), CaseBody);
    CaseLid->SetRelativeLocation(FVector(0, -ALakeRodCase::Width / 2, ALakeRodCase::Height / 2));
    // A 6 m pole: the engine cylinder (100 units tall, radius 50) along X, and the cork handle.
    RodBlank = CasePart(this, TEXT("RodBlank"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), RootComponent);
    RodBlank->SetRelativeLocationAndRotation(FVector(300, 0, 0), FRotator(-90, 0, 0));
    RodBlank->SetRelativeScale3D(FVector(0.03f, 0.03f, 5.6f));
    RodHandle = CasePart(this, TEXT("RodHandle"), TEXT("/Game/Tackle/SM_RodHandle.SM_RodHandle"), RootComponent);
    for (UStaticMeshComponent* Part : { CaseBody.Get(), CaseLid.Get(), RodBlank.Get(), RodHandle.Get() })
    {
        Part->SetCastShadow(false);
        Part->SetVisibility(false);
    }
}

void ALakeGhost::Show(int32 Mode, const FTransform& Where, bool bValid)
{
    if (!Tint)
        if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Tackle/M_Ghost.M_Ghost")))
        {
            Tint = UMaterialInstanceDynamic::Create(Base, this);
            for (UStaticMeshComponent* Part : { CaseBody.Get(), CaseLid.Get(), RodBlank.Get(), RodHandle.Get() })
                for (int32 I = 0; I < Part->GetNumMaterials(); ++I) Part->SetMaterial(I, Tint);
        }
    if (Mode != 0) SetActorTransform(Where);
    // The cylinder stands along Z: laid along the rod (X).
    RodBlank->SetRelativeLocationAndRotation(FVector(300, 0, 0), FQuat::FindBetweenNormals(FVector::UpVector, FVector::ForwardVector));
    if (Mode == ShownMode && (int32)bValid == ShownValid) return;
    ShownMode = Mode;
    ShownValid = bValid;
    CaseBody->SetVisibility(Mode == 1);
    CaseLid->SetVisibility(Mode == 1);
    RodBlank->SetVisibility(Mode == 2);
    RodHandle->SetVisibility(Mode == 2);
    // Dim: a pale see-through blue (red where it cannot go) that does not glare at noon.
    if (Tint) Tint->SetVectorParameterValue(TEXT("Tint"), bValid ? FLinearColor(0.05f, 0.13f, 0.32f) : FLinearColor(0.34f, 0.05f, 0.04f));
}
