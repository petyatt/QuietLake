#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PCGSettings.h"
#include "LakeForest.generated.h"

class UPCGGraph;
class UPCGComponent;
class USkinnedAsset;
class UDynamicWindData;

struct FLakeForestTree
{
    FTransform Transform;
    FSoftObjectPath Mesh;
    bool bWind = false;
};

/** Analytic terrain input for the native PCG skinned-mesh spawners. */
UCLASS(BlueprintType, ClassGroup=(Procedural))
class QUIETLAKE_API ULakeForestPointsSettings : public UPCGSettings
{
    GENERATED_BODY()
public:
    ULakeForestPointsSettings();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Forest", meta=(ClampMin="300", ClampMax="1500")) float Spacing = 500;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Forest", meta=(ClampMin="0", ClampMax="1.5")) float Density = 1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Forest") int32 ForestSeed = 20261031;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Clearings", meta=(ClampMin="13000")) float VillageClearance = 13000;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Forest", meta=(ClampMin="0.4", ClampMax="1.2")) float MinimumScale = 0.65f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Forest", meta=(ClampMin="0.4", ClampMax="1.4")) float MaximumScale = 1.05f;
    UPROPERTY(EditAnywhere, Category="Species") TArray<TSoftObjectPtr<USkinnedAsset>> Birches;
    UPROPERTY(EditAnywhere, Category="Species") TArray<TSoftObjectPtr<USkinnedAsset>> Alders;
    UPROPERTY(EditAnywhere, Category="Species") TArray<TSoftObjectPtr<USkinnedAsset>> Pines;
    UPROPERTY(EditAnywhere, Category="Species") TArray<TSoftObjectPtr<USkinnedAsset>> Spruces;
    UPROPERTY() TObjectPtr<UDynamicWindData> WindData;

    void BuildTrees(TArray<FLakeForestTree>& Out) const;
    bool AllowsTree(const FVector2D& P) const;
#if WITH_EDITOR
    virtual FName GetDefaultNodeName() const override { return TEXT("LakeForestZones"); }
    virtual FText GetDefaultNodeTitle() const override { return FText::FromString(TEXT("Lake Forest / Clearings")); }
    virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Spatial; }
#endif
protected:
    virtual TArray<FPCGPinProperties> InputPinProperties() const override { return {}; }
    virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
    virtual FPCGElementPtr CreateElement() const override;
};

UCLASS()
class QUIETLAKE_API ULakeForestLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Lake|Forest") static bool ConfigureForestGraph(UPCGGraph* Graph);
};

UCLASS()
class QUIETLAKE_API ALakeForest : public AActor
{
    GENERATED_BODY()
public:
    ALakeForest();
    virtual void BeginPlay() override;
    static bool HasGraph();
    void UpdateTrunks(const TArray<FLakeForestTree>& Trees);
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<UPCGComponent> ForestPCG;
private:
    void Generated(UPCGComponent* Component);
    UPROPERTY() TObjectPtr<class UHierarchicalInstancedStaticMeshComponent> Trunks;
    int32 ExpectedTrees = 0;
    int32 GenerationCount = 0;
};
