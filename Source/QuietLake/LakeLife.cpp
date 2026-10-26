#include "LakeLife.h"
#include "LakeGame.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "UnrealClient.h"

namespace
{
    constexpr float GridCell = 400;
    constexpr int32 RingPool = 96;
    constexpr int32 SprayPool = 48;          // three crossed planes per splash
    constexpr int32 FryPerSwarm = 30;
    constexpr float FryDepth = 9;            // cm under the surface
    constexpr float NearPlayer = 6000;       // full detail within 60 m of a player
    FIntPoint CellOf(const FVector& P) { return FIntPoint(FMath::FloorToInt(P.X / GridCell), FMath::FloorToInt(P.Y / GridCell)); }
    const TCHAR* SpeciesNoun(EFishSpecies S)
    {
        switch (S)
        {
        case EFishSpecies::Perch: return TEXT("окунь");
        case EFishSpecies::Roach: return TEXT("плотва");
        case EFishSpecies::Crucian: return TEXT("карась");
        default: return TEXT("щука");
        }
    }
}

ALakeLife::ALakeLife()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;
    bReplicates = true;
    bAlwaysRelevant = true;
    SetNetUpdateFrequency(4);
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Random.Initialize(20261026);
}

ALakeLife* ALakeLife::Get(const UWorld* World)
{
    static TWeakObjectPtr<ALakeLife> Cached;
    if (!World) return nullptr;
    if (ALakeLife* Life = Cached.Get(); Life && Life->GetWorld() == World && !Life->IsActorBeingDestroyed()) return Life;
    for (TActorIterator<ALakeLife> It(const_cast<UWorld*>(World)); It; ++It)
    {
        Cached = *It;
        return *It;
    }
    return nullptr;
}

void ALakeLife::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ALakeLife, Fry);
}

void ALakeLife::BeginPlay()
{
    Super::BeginPlay();
    if (HasAuthority()) SpawnFry();
#if !UE_BUILD_SHIPPING
    if (HasAuthority() && FParse::Param(FCommandLine::Get(), TEXT("LifePreview"))) RunPreview();
#endif
}

void ALakeLife::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority()) Step(DeltaSeconds);
    if (GetNetMode() == NM_DedicatedServer) return;
#if !UE_BUILD_SHIPPING
    // -LifePreview: frame time every 10 s, to see what the pond costs.
    static const bool bMeasure = FParse::Param(FCommandLine::Get(), TEXT("LifePreview"));
    if (bMeasure && GetWorld()->GetTimeSeconds() > 8)
    {
        static float Sum = 0, Worst = 0;
        static int32 Frames = 0;
        Sum += DeltaSeconds; Worst = FMath::Max(Worst, DeltaSeconds); ++Frames;
        if (Sum >= 10)
        {
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FRAME avg %.1f ms, worst %.1f ms"), Sum / Frames * 1000, Worst * 1000);
            Sum = Worst = 0; Frames = 0;
        }
    }
#endif
    if (!bVisualsReady) SetupVisuals();
    UpdateJumps(GetWorld()->GetTimeSeconds());
    UpdateFryVisuals(DeltaSeconds);
}

float ALakeLife::ServerTime() const
{
    const AGameStateBase* State = GetWorld() ? GetWorld()->GetGameState() : nullptr;
    return State ? State->GetServerWorldTimeSeconds() : (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f);
}

// --- the pond, server side ------------------------------------------------------------------

void ALakeLife::Step(float DeltaSeconds)
{
    Time += DeltaSeconds;
    Grid.Reset();
    Schools.Reset();
    AllFish.Reset();
    for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
    {
        ALakeFish* Fish = *It;
        if (!IsValid(Fish) || Fish->IsActorBeingDestroyed()) continue;
        AllFish.Add(Fish);
        Grid.FindOrAdd(CellOf(Fish->GetActorLocation())).Add(Fish);
        if (Fish->GetSchool() >= 0) Schools.FindOrAdd(Fish->GetSchool()).Add(Fish);
    }
    Players.Reset();
    for (TActorIterator<ALakeCharacter> It(GetWorld()); It; ++It) Players.Add(It->GetActorLocation());
    UpdateFry(DeltaSeconds);
    if (Time >= NextDetail) { NextDetail = Time + 0.5f; UpdateDetail(); }
    if (Time >= NextEvent) { NextEvent = Time + Random.FRandRange(6.0f, 14.0f); UpdateRandomEvents(); }
    if (Time >= NextLog) { NextLog = Time + 30; if (Time > 1) LogStats(); }
}

void ALakeLife::ForEachFishNear(const FVector& P, float Radius, TFunctionRef<void(ALakeFish*)> Visit) const
{
    const int32 Reach = FMath::CeilToInt(Radius / GridCell);
    const FIntPoint Centre = CellOf(P);
    const float Radius2 = Radius * Radius;
    for (int32 DX = -Reach; DX <= Reach; ++DX)
        for (int32 DY = -Reach; DY <= Reach; ++DY)
            if (const TArray<ALakeFish*>* List = Grid.Find(Centre + FIntPoint(DX, DY)))
                for (ALakeFish* Fish : *List)
                    if (IsValid(Fish) && !Fish->IsActorBeingDestroyed() && FVector::DistSquared(Fish->GetActorLocation(), P) <= Radius2) Visit(Fish);
}

ALakeFish* ALakeLife::SchoolLeader(int32 School) const
{
    // The first free member leads; when it is caught or busy on a bait, the next one takes over.
    if (const TArray<TWeakObjectPtr<ALakeFish>>* Members = Schools.Find(School))
        for (const TWeakObjectPtr<ALakeFish>& Member : *Members)
            if (ALakeFish* Fish = Member.Get(); Fish && !Fish->IsActorBeingDestroyed() && Fish->IsFree()) return Fish;
    return nullptr;
}

bool ALakeLife::IsNearPlayer(const FVector& P, float Radius) const
{
    for (const FVector& Player : Players)
        if (FVector::DistSquared2D(Player, P) < Radius * Radius) return true;
    return false;
}

void ALakeLife::UpdateDetail()
{
    for (const TWeakObjectPtr<ALakeFish>& Weak : AllFish)
        if (ALakeFish* Fish = Weak.Get())
        {
            Fish->SetNearPlayers(Fish->IsBusy() || IsNearPlayer(Fish->GetActorLocation(), NearPlayer));
            // The pond steps first each frame, so the fish see this frame's neighbours.
            Fish->AddTickPrerequisiteActor(this);
        }
}

void ALakeLife::EmitSign(ELakeSign Kind, const FVector& Location, float Size, EFishSpecies Species, float WeightKg, float Yaw, bool bQuiet)
{
    ++Counters.Signs[(int32)Kind];
    // Nobody is near enough to see it.
    if (!IsNearPlayer(Location, 15000)) return;
    FLakeSign Sign;
    Sign.Kind = Kind;
    Sign.Location = FVector(Location.X, Location.Y, LakeFish::SurfaceZ);
    Sign.Size = Size;
    Sign.Yaw = Yaw;
    Sign.Species = Species;
    Sign.WeightKg = WeightKg;
    Sign.bQuiet = bQuiet;
    MulticastSign(Sign);
}

void ALakeLife::MulticastSign_Implementation(const FLakeSign& Sign)
{
    if (GetNetMode() != NM_DedicatedServer) ShowSign(Sign);
}

void ALakeLife::Alarm(ALakeFish* Victim, float Radius)
{
    if (!Victim) return;
    const FVector From = Victim->GetActorLocation();
    ForEachFishNear(From, Radius, [&](ALakeFish* Fish)
    {
        if (Fish != Victim && Fish->Species != EFishSpecies::Pike && Fish->IsFree()) Fish->Spook(From, Random.FRandRange(2.0f, 4.0f));
    });
}

void ALakeLife::Devour(ALakeFish* Prey, ALakeFish* Predator)
{
    if (!IsValid(Prey) || Prey->IsBusy()) return;
    ++Counters.Eaten;
    Alarm(Prey, 500);
    const EFishSpecies Species = Prey->Species;
    Prey->Destroy();
    // The pond stays stocked: another fish of the species turns up later somewhere.
    for (TActorIterator<ALakeEnvironment> It(GetWorld()); It; ++It) It->ScheduleRespawn(Species);
}

void ALakeLife::UpdateRandomEvents()
{
    // Now and then a fish leaps clear of the water, most often on calm evenings and mornings.
    const ALakeWeather* Weather = ALakeWeather::Get(GetWorld());
    float Chance = 0.15f;
    if (Weather)
    {
        const FFishConditions C = Weather->FishConditions();
        const bool bCalm = C.Wind < 0.5f && C.Rain < 0.5f;
        Chance = C.SunElevation > -9 && C.SunElevation < 12 ? (bCalm ? 0.55f : 0.2f) : C.SunElevation < -9 ? 0.05f : (bCalm ? 0.15f : 0.07f);
    }
    if (Players.IsEmpty() || Random.FRand() > Chance) return;
    TArray<ALakeFish*> Candidates;
    for (const TWeakObjectPtr<ALakeFish>& Weak : AllFish)
        if (ALakeFish* Fish = Weak.Get())
        {
            const FVector P = Fish->GetActorLocation();
            if ((Fish->Species == EFishSpecies::Roach || Fish->Species == EFishSpecies::Crucian) && Fish->IsFree()
                && P.Z > LakeFish::SurfaceZ - 90 && IsNearPlayer(P, 5500)) Candidates.Add(Fish);
        }
    if (Candidates.IsEmpty()) return;
    const ALakeFish* Fish = Candidates[Random.RandRange(0, Candidates.Num() - 1)];
    EmitSign(ELakeSign::Jump, Fish->GetActorLocation(), 1, Fish->Species, Fish->WeightKg, Fish->GetActorRotation().Yaw);
}

void ALakeLife::LogStats()
{
    int32 Count[(int32)EFishSpecies::Count][(int32)EFishGoal::Count] = {};
    int32 Near = 0;
    for (const TWeakObjectPtr<ALakeFish>& Weak : AllFish)
        if (const ALakeFish* Fish = Weak.Get())
        {
            ++Count[(int32)Fish->Species][(int32)Fish->GetGoal()];
            Near += IsNearPlayer(Fish->GetActorLocation(), NearPlayer);
        }
    FString Line;
    for (int32 S = 0; S < (int32)EFishSpecies::Count; ++S)
        Line += FString::Printf(TEXT(" | %s feed %d rest %d cruise %d"), SpeciesNoun((EFishSpecies)S),
            Count[S][(int32)EFishGoal::Feed], Count[S][(int32)EFishGoal::Rest], Count[S][(int32)EFishGoal::Cruise]);
    const ALakeWeather* Weather = ALakeWeather::Get(GetWorld());
    const int32* N = Counters.Signs;
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LIFE hour=%.1f fish=%d near=%d%s | signs rise %d bubbles %d swirl %d splash %d scatter %d jump %d | fry attacks %d pike strikes %d eaten %d"),
        Weather ? Weather->Hours() : -1.0f, AllFish.Num(), Near, *Line, N[0], N[1], N[2], N[3], N[4], N[5], Counters.FryAttacks, Counters.PikeStrikes, Counters.Eaten);
}

// --- fry ----------------------------------------------------------------------------------

bool ALakeLife::FryCanBe(const FVector2D& P) const
{
    const float Depth = LakeMap::Depth(P.X, P.Y);
    const float Shore = LakeMap::ShoreDistance(P.X, P.Y);
    return LakeMap::IsWater(P.X, P.Y) && Depth > 25 && Depth < 170 && Shore > 120 && Shore < 2000 && !LakeMap::OnPier(FVector(P, 0));
}

FVector ALakeLife::FrySpotNear(const FVector2D& P, float Range)
{
    // Fry keep to warm shallow water by the reeds and among the lilies.
    FVector2D Best = P;
    float BestScore = FryCanBe(P) ? 0.01f : -1;
    for (int32 Try = 0; Try < 12; ++Try)
    {
        const FVector2D C = P + FVector2D(Random.FRandRange(-Range, Range), Random.FRandRange(-Range, Range));
        if (!FryCanBe(C)) continue;
        const LakeMap::FZones Z = LakeMap::ZonesAt(C.X, C.Y);
        const float Score = (0.3f + Z.Shallows + Z.ReedEdge + LakeMap::LilyAmount(C.X, C.Y)) * Random.FRandRange(0.6f, 1.0f);
        if (Score > BestScore) { BestScore = Score; Best = C; }
    }
    return FVector(Best, LakeFish::SurfaceZ - FryDepth);
}

void ALakeLife::SpawnFry()
{
    Fry.Reset();
    const TArray<LakeMap::FShorePoint>& Shore = LakeMap::Shoreline();
    for (int32 Try = 0; Try < 80 && Fry.Num() < 16; ++Try)
    {
        const LakeMap::FShorePoint& Point = Shore[Random.RandRange(0, Shore.Num() - 1)];
        const FVector Spot = FrySpotNear(Point.Location - Point.Outward * Random.FRandRange(300, 1100), 400);
        if (!FryCanBe(FVector2D(Spot))) continue;
        bool bCrowded = false;
        for (const FFryShoal& Other : Fry) bCrowded |= FVector::Dist2D(Other.Location, Spot) < 1500;
        if (bCrowded) continue;
        FFryShoal Shoal;
        Shoal.Location = Spot;
        Shoal.Target = Spot;
        Shoal.Count = (uint8)Random.RandRange(25, 40);
        Shoal.NextWander = Random.FRandRange(0, 20);
        Fry.Add(Shoal);
    }
}

void ALakeLife::UpdateFry(float DeltaSeconds)
{
    for (FFryShoal& F : Fry)
    {
        FVector L = F.Location;
        if (F.Panic <= 0 && Time >= F.NextWander)
        {
            F.Target = FrySpotNear(FVector2D(L), 600);
            F.NextWander = Time + Random.FRandRange(12, 30);
            F.Speed = 18;
        }
        FVector To = F.Target - L;
        To.Z = 0;
        const float Dist = To.Size();
        if (Dist > 5) L += To / Dist * FMath::Min(Dist, F.Speed * DeltaSeconds);
        else F.Speed = 18;
        L.Z = LakeFish::SurfaceZ - FryDepth;
        F.Location = L;
        F.Panic = FMath::Max(0.0f, F.Panic - DeltaSeconds * 0.04f);
        if (F.Count < 40 && Random.FRand() < DeltaSeconds * 0.04f) ++F.Count;
    }
}

int32 ALakeLife::NearestFry(const FVector& P, float Radius) const
{
    int32 Best = -1;
    float BestDist = Radius;
    for (int32 I = 0; I < Fry.Num(); ++I)
    {
        const float Dist = FVector::Dist2D(Fry[I].Location, P);
        if (Dist < BestDist) { BestDist = Dist; Best = I; }
    }
    return Best;
}

void ALakeLife::AttackFry(int32 Index, ALakeFish* Hunter)
{
    if (!Fry.IsValidIndex(Index)) return;
    FFryShoal& F = Fry[Index];
    if (Time - F.AttackedAt < 0.7f) return;
    F.AttackedAt = Time;
    F.ScatteredAt = ServerTime();
    ++Counters.FryAttacks;
    F.Panic += 1;
    if (F.Count > 6) F.Count -= (uint8)Random.RandRange(1, 3);
    const FVector Surface(F.Location.X, F.Location.Y, LakeFish::SurfaceZ);
    const EFishSpecies Species = Hunter ? Hunter->Species : EFishSpecies::Perch;
    const float Weight = Hunter ? Hunter->WeightKg : 0.2f;
    EmitSign(ELakeSign::Scatter, Surface, FMath::Clamp(0.6f + F.Count / 60.0f, 0.6f, 1.3f), Species, Weight);
    if (Species == EFishSpecies::Pike || Random.FRand() < 0.3f)
        EmitSign(ELakeSign::Splash, Surface + FVector(Random.FRandRange(-50, 50), Random.FRandRange(-50, 50), 0), FMath::Min(0.35f + 0.3f * Weight, 1.4f), Species, Weight);
    // Every hungry perch close by rushes in and snatches a mouthful.
    ForEachFishNear(F.Location, 700, [&](ALakeFish* Fish)
    {
        if (Fish->Species != EFishSpecies::Perch || Fish->GetGoal() != EFishGoal::Feed || !Fish->IsFree()) return;
        Fish->Dash(FVector(F.Location) + FVector(Random.FRandRange(-70, 70), Random.FRandRange(-70, 70), -10), 0.45f);
        Fish->Eat(0.1f);
    });
    // After a few rushes the shoal bolts along the bank and the hunt moves with it.
    if (F.Panic >= 3 + Random.FRand() * 2)
    {
        const FVector2D Away = FVector2D(Random.FRandRange(-1, 1), Random.FRandRange(-1, 1)).GetSafeNormal() * Random.FRandRange(800, 1500);
        F.Target = FrySpotNear(FVector2D(F.Location) + Away, 400);
        F.Speed = 140;
        F.Panic = 0;
        F.NextWander = Time + Random.FRandRange(15, 30);
    }
}

// --- signs on the water, every machine -----------------------------------------------------

FVector ALakeLife::ViewLocation() const
{
    const APlayerController* PC = GetWorld()->GetFirstPlayerController();
    return PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector(LakeMap::Centre, 200);
}

void ALakeLife::SetupVisuals()
{
    bVisualsReady = true;
    UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    UMaterialInterface* SignMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Life/M_WaterSign.M_WaterSign"));
    UMaterialInterface* SprayMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Life/M_WaterSpray.M_WaterSpray"));
    auto Pool = [this, Plane](UMaterialInterface* Material, int32 Count)
    {
        auto* Component = NewObject<UInstancedStaticMeshComponent>(this);
        Component->SetupAttachment(RootComponent);
        Component->SetStaticMesh(Plane);
        Component->SetMaterial(0, Material);
        Component->NumCustomDataFloats = 4;
        Component->SetCastShadow(false);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->bAffectDistanceFieldLighting = false;
        Component->bAffectDynamicIndirectLighting = false;
        Component->SetMobility(EComponentMobility::Movable);
        Component->RegisterComponent();
        // Parked out of sight, already expired (start time far in the past).
        for (int32 I = 0; I < Count; ++I)
        {
            Component->AddInstance(FTransform(FRotator::ZeroRotator, FVector(LakeMap::Centre, LakeFish::SurfaceZ - 500), FVector(0.001f)), true);
            Component->SetCustomData(I, { -1000.0f, 0.0f, 0.0f, 1.0f }, false);
        }
        Component->MarkRenderStateDirty();
        return Component;
    };
    Rings = Pool(SignMaterial, RingPool);
    Sprays = Pool(SprayMaterial, SprayPool);
    SplashSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/Life/A_Splash.A_Splash"));
    PlopSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/Life/A_Plop.A_Plop"));
    BubbleSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/Life/A_Bubbles.A_Bubbles"));
    ScatterSound = LoadObject<USoundBase>(nullptr, TEXT("/Game/Life/A_Scatter.A_Scatter"));
    Attenuation = NewObject<USoundAttenuation>(this);
    Attenuation->Attenuation.bAttenuate = true;
    Attenuation->Attenuation.bSpatialize = true;
    Attenuation->Attenuation.AttenuationShape = EAttenuationShape::Sphere;
    Attenuation->Attenuation.AttenuationShapeExtents = FVector(500, 0, 0);
    Attenuation->Attenuation.FalloffDistance = 5000;
    Attenuation->Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
}

void ALakeLife::PlaySound(USoundBase* Sound, const FVector& Location, float Volume, float Pitch)
{
    if (Sound) UGameplayStatics::PlaySoundAtLocation(this, Sound, Location, FRotator::ZeroRotator, Volume, Pitch, 0, Attenuation);
}

void ALakeLife::AddRing(ELakeSign Kind, const FVector& Location, float Size)
{
    if (!Rings) return;
    // How far each kind spreads (cm): the material animates rings, bubbles and foam inside this square.
    float Radius = Size * (Kind == ELakeSign::Bubbles ? 70.0f : Kind == ELakeSign::Rise ? 110.0f : Kind == ELakeSign::Scatter ? 210.0f : 180.0f);
    // Rings stop at the bank (the quad would otherwise draw them on the sand).
    Radius = FMath::Min(Radius, FMath::Max(25.0f, LakeMap::ShoreDistance(Location.X, Location.Y) + 30));
    const int32 Index = NextRing++ % RingPool;
    const FTransform T(FRotator(0, Random.FRandRange(0, 360), 0), FVector(Location.X, Location.Y, LakeFish::SurfaceZ + 0.6f), FVector(Radius / 50.0f));
    Rings->UpdateInstanceTransform(Index, T, true, false, true);
    Rings->SetCustomData(Index, { (float)GetWorld()->GetTimeSeconds(), (float)(int32)Kind, FMath::FRand() * 100.0f, Size }, true);
}

void ALakeLife::AddSpray(const FVector& Location, float Size)
{
    if (!Sprays) return;
    // A crown of drops: three crossed vertical planes, the material throws the drops up and lets them fall.
    const float Height = 70.0f * Size;
    const float Yaw = FMath::FRandRange(0.0f, 60.0f);
    const float Seed = FMath::FRand() * 100.0f;
    for (int32 K = 0; K < 3; ++K)
    {
        const int32 Index = NextSpray++ % SprayPool;
        const FTransform T(FRotator(0, Yaw + K * 60, 90), FVector(Location.X, Location.Y, LakeFish::SurfaceZ + Height * 0.5f), FVector(Height / 100.0f));
        Sprays->UpdateInstanceTransform(Index, T, true, false, true);
        Sprays->SetCustomData(Index, { (float)GetWorld()->GetTimeSeconds(), Size, Seed + K * 17.0f, 0 }, K == 2);
    }
    Sprays->MarkRenderStateDirty();
}

void ALakeLife::ShowSign(const FLakeSign& Sign)
{
    if (!bVisualsReady) SetupVisuals();
    const FVector View = ViewLocation();
    const float Dist = FVector::Dist(View, Sign.Location);
    if (Dist > 12000) return;
    const FVector P = Sign.Location;
    const float S = Sign.Size;
    switch (Sign.Kind)
    {
    case ELakeSign::Rise:
        AddRing(Sign.Kind, P, S);
        PlaySound(PlopSound, P, FMath::Min(0.25f + 0.35f * S, 0.8f), FMath::Clamp(1.35f - 0.35f * S, 0.8f, 1.4f));
        break;
    case ELakeSign::Bubbles:
        AddRing(Sign.Kind, P, S);
        PlaySound(BubbleSound, P, 0.45f, FMath::FRandRange(0.9f, 1.15f));
        break;
    case ELakeSign::Swirl:
        AddRing(Sign.Kind, P, S);
        PlaySound(PlopSound, P, FMath::Min(0.5f + 0.3f * S, 1.0f), 0.65f);
        break;
    case ELakeSign::Splash:
        AddRing(Sign.Kind, P, S);
        AddSpray(P, S);
        PlaySound(SplashSound, P, FMath::Min(0.4f + 0.4f * S, 1.0f), FMath::Clamp(1.25f - 0.25f * S, 0.75f, 1.3f));
        break;
    case ELakeSign::Scatter:
        AddRing(Sign.Kind, P, S);
        PlaySound(ScatterSound, P, 0.6f, FMath::FRandRange(0.9f, 1.1f));
        break;
    case ELakeSign::Jump:
    {
        // The fish flies along an arc; drops and rings where it leaves the water and where it falls back.
        const float Length = LakeFish::LengthCm(Sign.Species, Sign.WeightKg);
        FJump Jump;
        Jump.Start = GetWorld()->GetTimeSeconds();
        Jump.Size = FMath::Clamp(Length / 25.0f, 0.5f, 2.0f);
        Jump.FishLength = Length;
        Jump.Height = 25 + 25 * Jump.Size + FMath::FRandRange(0.0f, 15.0f);
        Jump.Length = 50 + 45 * Jump.Size;
        Jump.Duration = 0.45f + 0.15f * Jump.Size;
        Jump.Dir = FRotator(0, Sign.Yaw, 0).Vector();
        Jump.From = P - Jump.Dir * Jump.Length * 0.5f;
        Jump.bLanded = false;
        int32 Slot = Jumps.IndexOfByPredicate([&](const FJump& J) { return J.bLanded; });
        if (Slot == INDEX_NONE) { if (Jumps.Num() >= 4) break; Slot = Jumps.Add(FJump()); }
        Jumps[Slot] = Jump;
        if (!Jumpers.IsValidIndex(Slot))
        {
            auto* Component = NewObject<UStaticMeshComponent>(this);
            Component->SetupAttachment(RootComponent);
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetMobility(EComponentMobility::Movable);
            Component->RegisterComponent();
            Jumpers.SetNum(Slot + 1);
            Jumpers[Slot] = Component;
        }
        const FFishSpeciesInfo& Info = LakeFish::Info(Sign.Species);
        UStaticMeshComponent* Body = Jumpers[Slot];
        Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Info.Mesh));
        Body->SetWorldScale3D(FVector(Length / Info.MeshLength));
        Body->SetCustomPrimitiveDataFloat(0, Info.TailBeat * 2.5f);
        Body->SetCustomPrimitiveDataFloat(1, 0.08f);
        Body->SetCustomPrimitiveDataFloat(2, 0);
        Body->SetCustomPrimitiveDataFloat(3, Info.MeshLength);
        Body->SetVisibility(true);
        AddRing(ELakeSign::Rise, Jump.From, 0.5f * Jump.Size);
        AddSpray(Jump.From, 0.35f * Jump.Size);
        PlaySound(PlopSound, Jump.From, 0.35f, 1.2f);
        break;
    }
    default: break;
    }
    // A note for the HUD panel, if it happened within sight.
    if (Sign.bQuiet || Dist > 6000) return;
    FString Text;
    const bool bPike = Sign.Species == EFishSpecies::Pike;
    switch (Sign.Kind)
    {
    case ELakeSign::Rise: Text = Sign.Species == EFishSpecies::Roach ? TEXT("Плотва берёт с поверхности — круги") : TEXT("Круги на воде"); break;
    case ELakeSign::Bubbles: Text = TEXT("Карась роется в иле — пузыри"); break;
    case ELakeSign::Swirl: Text = bPike ? TEXT("Бурун — щука ударила под водой") : TEXT("Бурун на воде"); break;
    case ELakeSign::Splash: Text = bPike ? TEXT("Всплеск — удар щуки") : Sign.Species == EFishSpecies::Perch ? TEXT("Окунь бьёт малька") : TEXT("Всплеск"); break;
    case ELakeSign::Scatter: Text = bPike ? TEXT("Щука влетела в стайку малька") : TEXT("Окуни гоняют малька — «котёл»"); break;
    case ELakeSign::Jump: Text = FString::Printf(TEXT("Из воды выпрыгнул%s %s"), Sign.Species == EFishSpecies::Roach || bPike ? TEXT("а") : TEXT(""), SpeciesNoun(Sign.Species)); break;
    default: break;
    }
    RecentNotes.Insert({ (float)GetWorld()->GetRealTimeSeconds(), FString::Printf(TEXT("%s · %.0f м"), *Text, Dist / 100) }, 0);
    if (RecentNotes.Num() > 6) RecentNotes.SetNum(6);
}

void ALakeLife::UpdateJumps(float Now)
{
    for (int32 I = 0; I < Jumps.Num(); ++I)
    {
        FJump& J = Jumps[I];
        if (J.bLanded || !Jumpers.IsValidIndex(I)) continue;
        const float T = (Now - J.Start) / J.Duration;
        UStaticMeshComponent* Body = Jumpers[I];
        if (T >= 1)
        {
            J.bLanded = true;
            Body->SetVisibility(false);
            const FVector To = J.From + J.Dir * J.Length;
            AddRing(ELakeSign::Splash, To, 0.55f * J.Size);
            AddSpray(To, 0.5f * J.Size);
            PlaySound(SplashSound, To, FMath::Min(0.35f + 0.25f * J.Size, 0.9f), FMath::Clamp(1.4f - 0.25f * J.Size, 0.9f, 1.4f));
            continue;
        }
        // Up and over: the body follows the arc, nose up on the way out and down on the way in.
        const float Rise = J.Height * 4 * T * (1 - T);
        const FVector Centre = J.From + J.Dir * J.Length * T + FVector(0, 0, LakeFish::SurfaceZ - J.From.Z + Rise - 6);
        const float Pitch = FMath::RadiansToDegrees(FMath::Atan2(J.Height * 4 * (1 - 2 * T), J.Length)) * 0.8f;
        const FQuat Facing = FRotator(Pitch, J.Dir.Rotation().Yaw, 0).Quaternion();
        // The mesh has its snout at the origin and the tail towards +X (see ALakeFish::UpdateVisual).
        Body->SetWorldLocationAndRotation(Centre + Facing.GetForwardVector() * J.FishLength * 0.4f, Facing * FRotator(0, 180, 0).Quaternion());
    }
}

void ALakeLife::UpdateFryVisuals(float DeltaSeconds)
{
    UWorld* World = GetWorld();
    const FVector View = ViewLocation();
    const float Now = World->GetTimeSeconds();
    const float ServerNow = ServerTime();
    static UStaticMesh* FryMesh = nullptr;
    if (!FryMesh) FryMesh = LoadObject<UStaticMesh>(nullptr, LakeFish::Info(EFishSpecies::Roach).Mesh);
    const float MeshLength = LakeFish::Info(EFishSpecies::Roach).MeshLength;
    SwarmViews.SetNum(Fry.Num());
    FrySwarms.SetNum(Fry.Num());
    TArray<FTransform> Transforms;
    for (int32 I = 0; I < Fry.Num(); ++I)
    {
        const FFryShoal& Shoal = Fry[I];
        FSwarmView& V = SwarmViews[I];
        TObjectPtr<UInstancedStaticMeshComponent>& Swarm = FrySwarms[I];
        const bool bVisible = FVector::Dist(View, Shoal.Location) < 3500;
        if (!bVisible)
        {
            if (Swarm && Swarm->IsVisible()) Swarm->SetVisibility(false);
            V.Seen = -1;
            continue;
        }
        if (!Swarm)
        {
            Swarm = NewObject<UInstancedStaticMeshComponent>(this);
            Swarm->SetupAttachment(RootComponent);
            Swarm->SetStaticMesh(FryMesh);
            Swarm->SetCastShadow(false);
            Swarm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Swarm->bAffectDistanceFieldLighting = false;
            Swarm->SetMobility(EComponentMobility::Movable);
            Swarm->RegisterComponent();
            // The fish material wiggles the body: a quick beat, small fry.
            Swarm->SetCustomPrimitiveDataFloat(0, 26);
            Swarm->SetCustomPrimitiveDataFloat(1, 0.07f);
            Swarm->SetCustomPrimitiveDataFloat(2, 0);
            Swarm->SetCustomPrimitiveDataFloat(3, MeshLength);
            FRandomStream R(1000 + I);
            V.Offsets.Reset();
            for (int32 K = 0; K < FryPerSwarm; ++K)
            {
                const float A = R.FRandRange(0, 2 * PI), D = FMath::Sqrt(R.FRand());
                V.Offsets.Add(FVector(FMath::Cos(A) * D * 95, FMath::Sin(A) * D * 60, R.FRandRange(-8, 8)));
                Swarm->AddInstance(FTransform::Identity, true);
            }
        }
        if (!Swarm->IsVisible()) Swarm->SetVisibility(true);
        // Drift with the replicated shoal; face the way it goes.
        if (V.Seen < 0) V.Location = Shoal.Location;
        const FVector Before = V.Location;
        V.Location = FMath::VInterpTo(V.Location, FVector(Shoal.Location), DeltaSeconds, 1.5f);
        const FVector Move = (V.Location - Before) / FMath::Max(DeltaSeconds, 0.001f);
        if (Move.Size2D() > 3) V.Heading = FMath::VInterpNormalRotationTo(V.Heading, Move.GetSafeNormal2D(), DeltaSeconds, 90);
        V.Seen = Now;
        // Right after an attack the fry burst outwards and slowly gather again.
        const float Age = ServerNow - Shoal.ScatteredAt;
        const float Burst = Age >= 0 && Age < 3 ? FMath::Exp(-Age * 1.6f) : 0.0f;
        const FQuat Heading = V.Heading.Rotation().Quaternion();
        const float FryScale = 6.0f / MeshLength;
        Transforms.Reset();
        for (int32 K = 0; K < FryPerSwarm; ++K)
        {
            if (K >= Shoal.Count) { Transforms.Add(FTransform(FQuat::Identity, V.Location, FVector(0.0001f))); continue; }
            const FVector O = V.Offsets[K];
            const FVector Wobble(FMath::Sin(Now * 0.9f + K * 1.7f) * 12, FMath::Cos(Now * 0.7f + K * 2.3f) * 10, FMath::Sin(Now * 1.3f + K) * 3);
            const FVector Local = (O + Wobble) * (1 + 2.5f * Burst);
            const FVector P = V.Location + Heading.RotateVector(Local);
            float Yaw = V.Heading.Rotation().Yaw + FMath::Sin(Now * 1.1f + K * 2.0f) * 25;
            if (Burst > 0.2f) Yaw = FMath::RadiansToDegrees(FMath::Atan2(P.Y - V.Location.Y, P.X - V.Location.X));
            Transforms.Add(FTransform(FRotator(0, Yaw + 180, 0), P, FVector(FryScale)));
        }
        Swarm->BatchUpdateInstancesTransforms(0, Transforms, true, true, true);
        // Fry sip at the surface all the time: tiny dimples show where a shoal is.
        if (Now >= V.NextDimple && Shoal.Count > 0 && Transforms.Num())
        {
            V.NextDimple = Now + FMath::FRandRange(0.25f, 0.9f);
            AddRing(ELakeSign::Rise, Transforms[FMath::RandRange(0, FMath::Min<int32>(Shoal.Count, Transforms.Num()) - 1)].GetLocation(), FMath::FRandRange(0.08f, 0.14f));
        }
    }
}

// --- preview ---------------------------------------------------------------------------------

void ALakeLife::RunPreview()
{
#if !UE_BUILD_SHIPPING
    // -LifePreview: each kind of sign in front of the start spot on a calm evening, the fry, the
    // HUD panel; then the pond left to itself at four times of day, logging what the fish do.
    FVector2D Facing;
    const FVector2D Start = LakeMap::StartSpot(&Facing);
    const FVector2D Right(-Facing.Y, Facing.X);
    auto Spot = [Start, Facing, Right](float Out, float Side) { return FVector(Start + Facing * Out + Right * Side, LakeFish::SurfaceZ); };
    auto At = [this](float Seconds, TFunction<void()> Do)
    {
        FTimerHandle Handle;
        GetWorldTimerManager().SetTimer(Handle, [Do] { Do(); }, Seconds, false);
    };
    auto Shot = [](const TCHAR* File, bool bUI) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / File, bUI, false); };
    auto Weather = [this](float Hours)
    {
        if (ALakeWeather* W = ALakeWeather::Get(GetWorld()))
        {
            FLakeWeatherState Calm;
            Calm.Cloud = 0.35f; Calm.Fog = 0.05f; Calm.Rain = 0; Calm.Wind = 0.06f; Calm.WindYaw = 40;
            W->SetWeather(Calm, 1);
            W->SetHours(Hours);
        }
    };
    At(3, [this, Weather, Start, Facing]
    {
        Weather(19.6f);
        if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
            if (APawn* Pawn = PC->GetPawn())
            {
                const FVector2D Stand = Start - Facing * 150;
                Pawn->SetActorLocation(FVector(Stand, LakeMap::Ground(Stand.X, Stand.Y) + 100));
                PC->SetControlRotation(FRotator(-16, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
            }
    });
    At(10, [this, Spot] { EmitSign(ELakeSign::Rise, Spot(650, -200), 0.8f, EFishSpecies::Roach); EmitSign(ELakeSign::Bubbles, Spot(500, 120), 0.8f, EFishSpecies::Crucian);
                          EmitSign(ELakeSign::Swirl, Spot(850, 250), 1.2f, EFishSpecies::Pike); });
    At(11.2f, [Shot] { Shot(TEXT("Life_Rings.png"), false); });
    At(14, [this, Spot] { EmitSign(ELakeSign::Splash, Spot(600, 0), 1.4f, EFishSpecies::Pike, 2.0f); });
    At(14.25f, [Shot] { Shot(TEXT("Life_Splash.png"), false); });
    At(17, [this, Spot] { EmitSign(ELakeSign::Scatter, Spot(600, -150), 1.1f, EFishSpecies::Perch); });
    At(17.5f, [Shot] { Shot(TEXT("Life_Scatter.png"), false); });
    At(20, [this, Spot, Facing] { EmitSign(ELakeSign::Jump, Spot(550, 0), 1, EFishSpecies::Crucian, 0.6f, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)) + 90); });
    At(20.33f, [Shot] { Shot(TEXT("Life_Jump.png"), false); });
    At(22, [this, Spot]
    {
        if (Fry.Num())
        {
            Fry[0].Location = Spot(380, 60) - FVector(0, 0, FryDepth);
            Fry[0].Target = Fry[0].Location;
            Fry[0].Count = 30;
            Fry[0].NextWander = Time + 100;
        }
    });
    At(23, [this, Spot]
    {
        if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
            if (const APawn* Pawn = PC->GetPawn())
                PC->SetControlRotation((Spot(380, 60) - Pawn->GetActorLocation() - FVector(0, 0, 60)).Rotation());
    });
    At(26, [Shot] { Shot(TEXT("Life_Fry.png"), false); });
    At(27, [this] { if (Fry.Num()) { Fry[0].ScatteredAt = ServerTime(); EmitSign(ELakeSign::Scatter, Fry[0].Location, 1, EFishSpecies::Perch); } });
    At(27.4f, [Shot] { Shot(TEXT("Life_FryScatter.png"), false); });
    At(28.5f, [this, Start, Facing]
    {
        if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
            PC->SetControlRotation(FRotator(-16, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
    });
    At(29, [this]
    {
        if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
            if (ALakeHUD* HUD = Cast<ALakeHUD>(PC->GetHUD())) HUD->bShowLife = true;
    });
    At(31, [Shot] { Shot(TEXT("Life_Panel.png"), true); });
    // -LifeShotsOnly: just the pictures.
    if (FParse::Param(FCommandLine::Get(), TEXT("LifeShotsOnly"))) { At(33, [] { FPlatformMisc::RequestExit(false); }); return; }
    // The pond on its own: how the fish spend each part of the day.
    const float Hours[] = { 5.5f, 13.0f, 21.0f, 1.0f };
    for (int32 I = 0; I < 4; ++I)
    {
        const float Begin = 34 + I * 90.0f;
        const float H = Hours[I];
        At(Begin, [this, Weather, H] { Weather(H); UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LIFE_PHASE %.1f h"), H); });
        At(Begin + 88, [this] { LogStats(); });
    }
    At(34 + 4 * 90.0f + 2, [] { FPlatformMisc::RequestExit(false); });
#endif
}
