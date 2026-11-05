#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeRodCase.generated.h"

/**
 * The rod case, only for the look (its state is the angler's, replicated): a canvas bag 150 x 22 x 10 cm
 * with three compartments and a lid hinged along one long edge. On the back it hangs on its strap; on
 * the ground the lid opens and the folded rods lie in their compartments.
 * Meshes: Tools/GenerateTackleModels.py (RodCase, RodCaseLid, RodCaseStrap, RodPacked_*).
 */
UCLASS()
class QUIETLAKE_API ALakeRodCase : public AActor
{
    GENERATED_BODY()
public:
    ALakeRodCase();
    /**
     * Where (origin: the middle of the bottom, X along the case, Z up through the lid), whether it hangs
     * on the back (the strap shows), whether the lid is open, and the compartment of each rod (-1: out).
     */
    void Show(const FTransform& Where, bool bOnBack, bool bOpen, const TArray<int32>& SlotOfRod, float DeltaSeconds);
    /** 0 closed .. 1 open. */
    float LidOpen() const { return Lid01; }
    static constexpr float Length = 150.0f;
    static constexpr float Width = 22.0f;
    static constexpr float Height = 10.0f;
    /** A compartment's middle in the case's frame. */
    static FVector SlotLocation(int32 Slot);
private:
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Body;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Lid;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Strap;
    UPROPERTY() TArray<TObjectPtr<class UStaticMeshComponent>> Packed;
    float Lid01 = 0;
};

/** The blue see-through copy that shows where the case or a rod will be laid (B, or R held). */
UCLASS()
class QUIETLAKE_API ALakeGhost : public AActor
{
    GENERATED_BODY()
public:
    ALakeGhost();
    /** Mode 0 hidden, 1 the case, 2 a rod; bValid blue, otherwise red. */
    void Show(int32 Mode, const FTransform& Where, bool bValid);
private:
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> CaseBody;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> CaseLid;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> RodBlank;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> RodHandle;
    UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> Tint;
    int32 ShownMode = -1;
    int32 ShownValid = -1;
};
