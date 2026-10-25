#include "LakeGame.h"
#include "ProceduralMeshComponent.h"
#include "KismetProceduralMeshLibrary.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "DynamicWindData.h"
#include "Components/AudioComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/HUD.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundAttenuation.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Misc/App.h"

ALakeEnvironment::ALakeEnvironment()
{
    bReplicates = true;
    bAlwaysRelevant = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("WorldRoot"));
}

static float LakeRadius(float X, float Y)
{
    return FMath::Sqrt(FMath::Square((X - 2200) / 2200) + FMath::Square(Y / 2800));
}
bool ALakeEnvironment::IsWater(FVector P)
{
    return LakeRadius(P.X, P.Y) < 0.97f;
}
float ALakeEnvironment::GroundHeight(float X, float Y)
{
    const float R = LakeRadius(X, Y);
    if (R < 1) return -25 - (1 - R) * 240;
    const float Shore = FMath::Clamp((R - 1) / 0.12f, 0.0f, 1.0f);
    const float Hills = FMath::Max(0.0f, R - 1.35f) * (70 + 55 * FMath::Sin(X * 0.0008f) * FMath::Cos(Y * 0.001f));
    return -25 + Shore * 15 + Hills + Shore * 10 * FMath::Sin(X * 0.003f) * FMath::Cos(Y * 0.002f);
}

void ALakeEnvironment::BeginPlay()
{
    Super::BeginPlay();
    auto Material = [](const TCHAR* Name)
    {
        return LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Materials/%s.%s"), Name, Name));
    };
    // One deterministic layout per peer; foliage transforms do not consume network bandwidth.
    FRandomStream Random(20261025);
    auto Group = [this, &Material](const TCHAR* Shape, const TCHAR* Mat, bool Collision)
    {
        auto* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Shape, Shape)));
        Component->SetMaterial(0, Material(Mat));
        Component->SetCollisionEnabled(Collision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
        Component->SetCollisionResponseToAllChannels(ECR_Block);
        Component->SetCullDistances(16000, 22000);
        Component->RegisterComponent();
        AddInstanceComponent(Component);
        return Component;
    };
    auto Add = [](UHierarchicalInstancedStaticMeshComponent* Group, FVector Location, FVector Scale, FRotator Rotation = FRotator::ZeroRotator)
    {
        Group->AddInstance(FTransform(Rotation, Location, Scale));
    };
    auto* Terrain = NewObject<UProceduralMeshComponent>(this, TEXT("Terrain"));
    Terrain->SetupAttachment(RootComponent);
    Terrain->RegisterComponent();
    AddInstanceComponent(Terrain);
    TArray<FVector> Vertices, Normals;
    TArray<FVector2D> UVs;
    TArray<int32> Triangles;
    TArray<FLinearColor> Colors;
    TArray<FProcMeshTangent> Tangents;
    const int32 Cells = 160;
    for (int32 Y = 0; Y <= Cells; ++Y)
        for (int32 X = 0; X <= Cells; ++X)
        {
            const float PX = -10000 + X * 150;
            const float PY = -12000 + Y * 150;
            Vertices.Add(FVector(PX, PY, GroundHeight(PX, PY)));
            UVs.Add(FVector2D(PX / 250, PY / 250));
            const float R = LakeRadius(PX, PY);
            // Layer masks for M_TerrainScanned: R = meadow (away from the shore), G = forest floor.
            const float Meadow = FMath::Clamp((R - 1.06f) * 6, 0.0f, 1.0f);
            const float Forest = FMath::Clamp((R - 1.3f) * 4, 0.0f, 1.0f);
            Colors.Add(FLinearColor(Meadow, Forest, 0.0f));
            if (X < Cells && Y < Cells)
            {
                const int32 A = Y * (Cells + 1) + X;
                Triangles.Append({A, A + Cells + 1, A + 1, A + 1, A + Cells + 1, A + Cells + 2});
            }
        }
    UKismetProceduralMeshLibrary::CalculateTangentsForMesh(Vertices, Triangles, UVs, Normals, Tangents);
    Terrain->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, true);
    Terrain->SetMaterial(0, Material(TEXT("M_TerrainScanned")));

    auto* Water = Group(TEXT("Cube"), TEXT("M_LakeSurface"), true);
    Add(Water, FVector(2200, 0, -31), FVector(44, 56, 0.12));
    auto* Bark = Group(TEXT("Cylinder"), TEXT("M_Bark"), true);       // pier posts and firewood
    auto* Wood = Group(TEXT("Cube"), TEXT("M_Planks"), true);
    auto* Metal = Group(TEXT("Cylinder"), TEXT("M_Metal"), true);
    // Invisible trunk colliders: the Megaplants trees are skinned meshes without per-instance collision.
    auto* Trunks = Group(TEXT("Cylinder"), TEXT("M_Bark"), true);
    Trunks->SetHiddenInGame(true);
    Trunks->SetCastShadow(false);

    // Quixel Megaplants: Nanite skinned meshes animated by the Dynamic Wind plugin.
    // One instanced component per mesh variant draws every copy of that variant in one batch.
    auto* Wind = NewObject<UDynamicWindData>(this);
    auto Plants = [this, Wind](const TCHAR* Folder, const TCHAR* Prefix, const TCHAR* Letters, float CullDistance)
    {
        TArray<UInstancedSkinnedMeshComponent*> Variants;
        for (const TCHAR* Letter = Letters; *Letter; ++Letter)
        {
            const FString Name = FString::Printf(TEXT("%s_%c"), Prefix, *Letter);
            auto* Mesh = LoadObject<USkinnedAsset>(nullptr, *FString::Printf(TEXT("/Game/Megaplant_Library/%s/%s.%s"), Folder, *Name, *Name));
            if (!Mesh)
            {
                UE_LOG(LogTemp, Warning, TEXT("Megaplant mesh missing: %s/%s"), Folder, *Name);
                continue;
            }
            auto* Component = NewObject<UInstancedSkinnedMeshComponent>(this);
            Component->SetupAttachment(RootComponent);
            Component->SetSkinnedAssetAndUpdate(Mesh);
            Component->SetTransformProvider(Wind);
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetCullDistances(CullDistance * 0.8f, CullDistance);
            Component->RegisterComponent();
            AddInstanceComponent(Component);
            Variants.Add(Component);
        }
        return Variants;
    };
    const auto Pines = Plants(TEXT("Tree_Baltic_Pine/Tree_Baltic_Pine_01"), TEXT("Tree_Baltic_Pine_01"), TEXT("ABCD"), 30000);
    const auto Spruces = Plants(TEXT("Tree_Norway_Spruce/Tree_Norway_Spruce_01"), TEXT("Tree_Norway_Spruce_01"), TEXT("ABCD"), 30000);
    const auto Birches = Plants(TEXT("Tree_Silver_Birch/Tree_Silver_Birch_01"), TEXT("Tree_Silver_Birch_01"), TEXT("ABCD"), 30000);
    const auto Alders = Plants(TEXT("Tree_Black_Alder/Tree_Black_Alder_01"), TEXT("Tree_Black_Alder_01"), TEXT("ABCD"), 30000);
    const auto Bulrush = Plants(TEXT("Plant_Bulrush/Plant_Bulrush_01"), TEXT("Plant_Common_Bulrush_01"), TEXT("ABCDE"), 9000);
    const auto DryReed = Plants(TEXT("Plant_Dry_Common_Reeds/Plant_Dry_Common_Reeds_01"), TEXT("Plant_Dry_Common_Reed"), TEXT("ABCD"), 9000);
    auto Place = [&Random](const TArray<UInstancedSkinnedMeshComponent*>& Variants, FVector Location, float Scale)
    {
        if (Variants.Num() == 0) return;
        Variants[Random.RandRange(0, Variants.Num() - 1)]->AddInstance(
            FTransform(FRotator(0, Random.FRandRange(0, 360), 0), Location, FVector(Scale)), 0);
    };

    // Forest: deciduous trees on the first band around the shore, conifers behind them.
    for (int32 I = 0; I < 520; ++I)
    {
        const float X = Random.FRandRange(-8500, 11000), Y = Random.FRandRange(-10000, 10000);
        const float R = LakeRadius(X, Y);
        if (R < 1.3f || (X < 100 && X > -1600 && FMath::Abs(Y) < 1300)) continue;
        // Sink the base slightly so roots never float on slopes.
        const FVector Base(X, Y, GroundHeight(X, Y) - 15);
        const bool bShoreBand = R < 1.75f && Random.FRand() < 0.7f;
        const TArray<UInstancedSkinnedMeshComponent*>& Species =
            bShoreBand ? (Random.FRand() < 0.55f ? Birches : Alders) : (Random.FRand() < 0.55f ? Pines : Spruces);
        Place(Species, Base, Random.FRandRange(0.7f, 1.05f));
        Add(Trunks, Base + FVector(0, 0, 250), FVector(0.45f, 0.45f, 5.0f));
    }
    // Mossy stone scatters along the shore and a few in the forest (Quixel Megascans, Nanite).
    auto* Rocks = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
    Rocks->SetupAttachment(RootComponent);
    Rocks->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Fab/Megascans/3D/Mossy_Rocks_tjmudcfda/High/tjmudcfda_tier_1/StaticMeshes/tjmudcfda_tier_1.tjmudcfda_tier_1")));
    Rocks->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Rocks->SetCollisionResponseToAllChannels(ECR_Block);
    Rocks->RegisterComponent();
    AddInstanceComponent(Rocks);
    for (int32 I = 0; I < 90; ++I)
    {
        const float Angle = Random.FRandRange(-PI, PI);
        const float Radius = I < 60 ? Random.FRandRange(1.0f, 1.12f) : Random.FRandRange(1.3f, 2.2f);
        const float X = 2200 + FMath::Cos(Angle) * 2200 * Radius, Y = FMath::Sin(Angle) * 2800 * Radius;
        if (X < 600 && FMath::Abs(Y + 700) < 260) continue;   // keep the pier approach clear
        const float S = Random.FRandRange(0.35f, 0.9f);
        Add(Rocks, FVector(X, Y, GroundHeight(X, Y) - 4), FVector(S, S, S * Random.FRandRange(0.8f, 1.6f)),
            FRotator(Random.FRandRange(-4, 4), Random.FRandRange(0, 360), Random.FRandRange(-4, 4)));
    }
    // Bulrush and dry reed clumps along the waterline, thicker away from the pier.
    for (int32 I = 0; I < 520; ++I)
    {
        const float Angle = Random.FRandRange(-PI, PI);
        const float R = Random.FRandRange(0.95f, 1.03f);
        const float X = 2200 + FMath::Cos(Angle) * 2200 * R, Y = FMath::Sin(Angle) * 2800 * R;
        if (X < 800 && FMath::Abs(Y) < 1150) continue;
        Place(Random.FRand() < 0.6f ? Bulrush : DryReed, FVector(X, Y, GroundHeight(X, Y) - 8), Random.FRandRange(0.8f, 1.25f));
    }
    // Walkable pier: each plank has collision, the first plank is a small step from the shore.
    for (int32 I = 0; I < 27; ++I)
        Add(Wood, FVector(-420 + I * 38,-700,4), FVector(0.36,2.6,0.13));
    for (int32 I = 0; I < 5; ++I)
        for (float Y : {-815.0f,-585.0f})
            Add(Bark, FVector(-370 + I * 220,Y,-55), FVector(0.14,0.14,1.7));
    // Bench, tackle table, stool, bucket, and a stack of logs.
    for (float Y : {480.0f,620.0f}) Add(Wood, FVector(-850,Y,16), FVector(0.12,0.12,0.6));
    Add(Wood, FVector(-850,550,49), FVector(0.55,2,0.12));
    Add(Wood, FVector(-885,550,85), FVector(0.10,2,0.6));
    Add(Wood, FVector(-1070,-580,65), FVector(1.3,1,0.12));
    for (float X : {-1120.0f,-1020.0f}) for (float Y : {-620.0f,-540.0f}) Add(Wood,FVector(X,Y,25),FVector(0.08,0.08,0.75));
    Add(Metal, FVector(-990,-340,10), FVector(0.28,0.28,0.38));
    Add(Wood, FVector(-610,-430,20), FVector(0.45,0.45,0.5));
    for (int32 I = 0; I < 5; ++I) Add(Bark,FVector(-1360 + (I % 3) * 26,750,15 + (I / 3) * 24),FVector(0.25,0.25,1.6),FRotator(90,0,0));
    Add(Wood,FVector(-1350,-900,65),FVector(0.12,0.12,1.6));
    Add(Wood,FVector(-1350,-900,160),FVector(0.10,1.9,0.65));
    auto* Sign = NewObject<UTextRenderComponent>(this);
    Sign->SetupAttachment(RootComponent);
    Sign->SetText(FText::FromString(TEXT("ТИХАЯ ЗАВОДЬ")));
    Sign->SetHorizontalAlignment(EHTA_Center);
    Sign->SetWorldSize(18);
    Sign->SetTextRenderColor(FColor(235,224,180));
    Sign->RegisterComponent();
    AddInstanceComponent(Sign);
    Sign->SetWorldLocation(FVector(-1343,-900,153));

    if (GetNetMode() != NM_DedicatedServer)
    {
        // Early morning. The sun is low and ahead of the starting view, just above the
        // far tree line, so the mist and foliage are backlit and light shafts form.
        auto* Sun = GetWorld()->SpawnActor<ADirectionalLight>(FVector(0,0,3000), FRotator(-6,205,0));
        Sun->SetMobility(EComponentMobility::Movable);
        auto* SunComponent = Cast<UDirectionalLightComponent>(Sun->GetLightComponent());
        SunComponent->SetAtmosphereSunLight(true);
        SunComponent->SetIntensity(10.0f);
        SunComponent->SetUseTemperature(true);
        SunComponent->SetTemperature(5500.0f);
        SunComponent->SetLightSourceAngle(0.8f);
        SunComponent->SetEnableLightShaftBloom(true);
        SunComponent->SetBloomScale(0.08f);
        auto* Atmosphere = GetWorld()->SpawnActor<ASkyAtmosphere>();
        Atmosphere->GetComponent()->SetMieScatteringScale(0.008f); // humid morning haze
        GetWorld()->SpawnActor<AVolumetricCloud>();
        auto* Sky = GetWorld()->SpawnActor<ASkyLight>();
        Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        Sky->GetLightComponent()->SetIntensity(1.0f);
        Sky->GetLightComponent()->SetRealTimeCapture(true);

        // Two fog layers: haze over the whole valley plus a dense layer hugging the water.
        // Density is extinction per km, falloff halves density every 1000/falloff cm, so for a
        // ~50 m scene the values are far above the engine defaults.
        auto* Fog = GetWorld()->SpawnActor<AExponentialHeightFog>(FVector(0,0,-25), FRotator::ZeroRotator)->GetComponent();
        Fog->SetFogDensity(0.045f);
        Fog->SetFogHeightFalloff(0.5f);
        FExponentialHeightFogData WaterMist;
        WaterMist.FogDensity = 0.3f;
        WaterMist.FogHeightFalloff = 2.0f;   // the engine maximum: halves every 5 m
        WaterMist.FogHeightOffset = 0.0f;
        Fog->SetSecondFogData(WaterMist);
        Fog->SetVolumetricFog(true);
        Fog->SetVolumetricFogScatteringDistribution(0.4f); // glows toward the sun, still visible away from it
        Fog->SetVolumetricFogAlbedo(FColor(250,252,255)); // milky, slightly cool
        Fog->SetVolumetricFogExtinctionScale(1.0f);
        Fog->SetVolumetricFogDistance(12000.0f);
        auto* Post = GetWorld()->SpawnActor<APostProcessVolume>();
        Post->bUnbound = true;
        auto& PP = Post->Settings;
        PP.bOverride_AutoExposureMethod = true;
        PP.AutoExposureMethod = AEM_Histogram;
        PP.bOverride_AutoExposureBias = true;
        PP.AutoExposureBias = 0.3f;
        PP.bOverride_AutoExposureSpeedUp = true;
        PP.AutoExposureSpeedUp = 2.0f;
        PP.bOverride_AutoExposureSpeedDown = true;
        PP.AutoExposureSpeedDown = 1.5f;
        PP.bOverride_BloomIntensity = true;
        PP.BloomIntensity = 0.5f;
        PP.bOverride_VignetteIntensity = true;
        PP.VignetteIntensity = 0.3f;
        PP.bOverride_MotionBlurAmount = true;
        PP.MotionBlurAmount = 0.0f;
        PP.bOverride_ColorSaturation = true;
        PP.ColorSaturation = FVector4(1.0f,1.0f,1.0f,0.92f);
        PP.bOverride_ColorContrast = true;
        PP.ColorContrast = FVector4(1.0f,1.0f,1.0f,1.05f);
        PP.bOverride_ColorGainHighlights = true;
        PP.ColorGainHighlights = FVector4(1.02f,1.0f,0.97f,1.0f); // gently warm sunlit highlights
        PP.bOverride_ColorGainShadows = true;
        PP.ColorGainShadows = FVector4(0.95f,1.0f,1.06f,1.0f);    // cool shade
        PP.bOverride_FilmGrainIntensity = true;
        PP.FilmGrainIntensity = 0.08f;
        PP.bOverride_LumenFinalGatherQuality = true;
        PP.LumenFinalGatherQuality = 2.0f;
        PP.bOverride_LumenReflectionQuality = true;
        PP.LumenReflectionQuality = 2.0f;

        auto Audio = [this](const TCHAR* Name, FVector Location, float Volume, bool Spatial)
        {
            auto* Sound = LoadObject<USoundWave>(nullptr, *FString::Printf(TEXT("/Game/Audio/%s.%s"),Name,Name));
            if (!Sound) return;
            auto* Component = NewObject<UAudioComponent>(this);
            Component->SetupAttachment(RootComponent);
            Component->SetSound(Sound);
            Component->SetVolumeMultiplier(Volume);
            Component->bAutoActivate = false;
            if (Spatial)
            {
                FSoundAttenuationSettings Settings;
                Settings.bAttenuate = true;
                Settings.bSpatialize = true;
                Settings.AttenuationShapeExtents = FVector(800,0,0);
                Settings.FalloffDistance = 4500;
                Component->AdjustAttenuation(Settings);
            }
            Component->RegisterComponent();
            AddInstanceComponent(Component);
            Component->SetWorldLocation(Location);
            Component->Play();
        };
        Audio(TEXT("A_Wind"),FVector::ZeroVector,0.32f,false);
        Audio(TEXT("A_Water"),FVector(1200,0,-20),0.48f,true);
        Audio(TEXT("A_Birds"),FVector(-1800,1200,550),0.3f,true);
    }
#if !UE_BUILD_SHIPPING
    // Reproducible visual check; only enabled by an explicit command-line switch.
    // Three fixed viewpoints with input and HUD off, so screenshots are comparable between runs.
    if (FParse::Param(FCommandLine::Get(),TEXT("LakePreview")))
    {
        struct FShot { FVector Location; FRotator Rotation; const TCHAR* File; };
        static const FShot Shots[] = {
            { FVector(-550,-400,130), FRotator(-6,0,0),     TEXT("WorldPreview.png") },   // start, facing the sunrise
            { FVector(450,-700,115),  FRotator(-4,28,0),    TEXT("WorldPreview_2.png") }, // end of the pier
            { FVector(4700,600,115),  FRotator(-5,-164,0),  TEXT("WorldPreview_3.png") }, // far shore, looking back
        };
        for (int32 I = 0; I < UE_ARRAY_COUNT(Shots); ++I)
        {
            const float At = 26.0f + I * 7.0f;
            FTimerHandle PoseTimer, ShotTimer;
            GetWorldTimerManager().SetTimer(PoseTimer, [this, I]
            {
                APlayerController* PC = GetWorld()->GetFirstPlayerController();
                if (!PC || !PC->GetPawn()) return;
                PC->SetIgnoreLookInput(true);
                PC->SetIgnoreMoveInput(true);
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
                PC->GetPawn()->SetActorLocation(Shots[I].Location, false, nullptr, ETeleportType::TeleportPhysics);
                PC->SetControlRotation(Shots[I].Rotation);
            }, At - 5.0f, false);
            GetWorldTimerManager().SetTimer(ShotTimer, [I]
            {
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Shots[I].File, false, false);
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_PREVIEW %s frame=%.1f ms"), Shots[I].File, FApp::GetDeltaTime() * 1000.0);
            }, At, false);
        }
        FTimerHandle ExitTimer;
        GetWorldTimerManager().SetTimer(ExitTimer, [] { FPlatformMisc::RequestExit(false); }, 26.0f + UE_ARRAY_COUNT(Shots) * 7.0f, false);
    }
#endif
    UE_LOG(LogTemp,Display,TEXT("QUIET_LAKE_WORLD_READY: terrain, lake, pier, foliage and props"));
}

