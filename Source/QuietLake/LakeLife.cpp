#include "LakeLife.h"
#include "LakeRipples.h"
#include "LakeGame.h"
#include "LakeHUD.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "Algo/BinarySearch.h"
#include "Async/ParallelFor.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/PostProcessVolume.h"
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
#include "UObject/UObjectIterator.h"

namespace
{
    constexpr float GridCell = 400;
    constexpr int32 RingPool = 96;
    constexpr int32 SprayPool = 48;          // three crossed planes per splash
    constexpr int32 FryPerSwarm = 30;
    constexpr float FryDepth = 9;            // cm under the surface
    constexpr float NearPlayer = 6000;       // full detail within 60 m of a player
    FIntPoint CellOf(const FVector& P) { return FIntPoint(FMath::FloorToInt(P.X / GridCell), FMath::FloorToInt(P.Y / GridCell)); }
    const TCHAR* SpeciesNoun(EFishSpecies S) { return *LakeFish::Info(S).Noun; }
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
    if (HasAuthority())
    {
        FoodField.Init();
        SpawnFry();
        // A game (not a test world): Live Fish stocks the pond.
        if (GetWorld()->GetAuthGameMode<ALakeGameMode>()) StockPond();
    }
#if !UE_BUILD_SHIPPING
    if (HasAuthority() && FParse::Param(FCommandLine::Get(), TEXT("LifePreview"))) RunPreview();
    if (HasAuthority() && FParse::Param(FCommandLine::Get(), TEXT("FishPreview"))) RunFishPreview();
    if (HasAuthority() && FParse::Param(FCommandLine::Get(), TEXT("WaterPreview"))) RunWaterPreview();
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
    if (Underwater)
    {
        // (-FishPreview films the swimming from a few metres under water: there the murk stays off.)
        static const bool bClearWater = FParse::Param(FCommandLine::Get(), TEXT("FishPreview"));
        const FVector Eye = ViewLocation();
        Underwater->BlendWeight = !bClearWater && LakeMap::IsWater(Eye.X, Eye.Y) && Eye.Z < LakeFish::SurfaceZ - 1 ? 1.0f : 0.0f;
    }
    if (GetWorld()->GetTimeSeconds() >= NextShownLog)
    {
        NextShownLog = GetWorld()->GetTimeSeconds() + 20;
        LogShownMotion();
    }
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
    if (!FoodField.IsReady()) FoodField.Init();   // test worlds
    FoodField.Step(DeltaSeconds);
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
    if (Time >= NextSteps) { NextSteps = Time + 0.5f; UpdateFootsteps(); }
    if (Time >= NextSample) { SampleMotion(Time - NextSample + 0.2f); NextSample = Time + 0.2f; }
    if (Time >= NextEvent) { NextEvent = Time + Random.FRandRange(6.0f, 14.0f); UpdateRandomEvents(); }
    if (Time >= NextSpotSample) { NextSpotSample = Time + 5; SampleSpots(); SamplePrey(); }
    if (!bStockLogged && Time > 2 && AllFish.Num() > 50) { bStockLogged = true; LogStock(); }
    if (Time >= NextLog) { NextLog = Time + 30; if (Time > 1) LogStats(); }
}

namespace
{
    /** The water a pole reaches from a spot: the bait lands 2-6 m out from the waterline (from the pier: beyond its end). */
    void FishingWater(const LakeMap::FSpot& Spot, FVector2D& A, FVector2D& B)
    {
        if (!Spot.bBoards)
        {
            const LakeMap::FPier& Pier = LakeMap::Pier();
            const FVector2D End = Pier.Root + Pier.Dir * Pier.Length;
            A = End + Pier.Dir * 200;
            B = End + Pier.Dir * 600;
            return;
        }
        A = Spot.Location - Spot.Outward * 200;
        B = Spot.Location - Spot.Outward * 600;
    }
    float ReachDistance(const LakeMap::FSpot& Spot, const FVector& P)
    {
        FVector2D A, B;
        FishingWater(Spot, A, B);
        const FVector2D AB = B - A, Q(P);
        const float T = FMath::Clamp(FVector2D::DotProduct(Q - A, AB) / AB.SizeSquared(), 0.0f, 1.0f);
        return FVector2D::Distance(Q, A + AB * T);
    }
}

void ALakeLife::SampleSpots()
{
    const TConstArrayView<LakeMap::FSpot> Spots = LakeMap::Spots();
    SpotWatch.SetNum(Spots.Num());
    for (int32 I = 0; I < Spots.Num(); ++I)
    {
        const LakeMap::FSpot& Spot = Spots[I];
        FSpotWatch& W = SpotWatch[I];
        int32 Here = 0, Feeding = 0;
        float Nearest = 3000;
        if (W.Fish.Num() < LakeFish::NumSpecies()) W.Fish.SetNumZeroed(LakeFish::NumSpecies());
        FVector2D A, B;
        FishingWater(Spot, A, B);
        // A fish counts when it would sense a bait somewhere in that water (its species' sense radius).
        ForEachFishNear(FVector((A + B) * 0.5f, LakeFish::SurfaceZ), 3200, [&](ALakeFish* Fish)
        {
            if (!Fish->IsFree()) return;
            const float Dist = ReachDistance(Spot, Fish->GetActorLocation());
            Nearest = FMath::Min(Nearest, Dist);
            if (Dist > LakeFish::Info(Fish->Species).SenseRadius) return;
            W.Fish[(int32)Fish->Species] += 1;
            ++Here;
            Feeding += Fish->GetGoal() == EFishGoal::Feed;
        });
        ++W.Samples;
        W.Nearest += Nearest;
        W.Occupied += Here > 0;
        W.Feeding += Feeding > 0;
    }
}

void ALakeLife::LogSpots()
{
    // Per spot: the share of samples with a fish in reach (and with a feeding one), the mean number
    // in reach of perch / roach / crucian / pike, and how far off the nearest fish is (up to 30 m).
    const TConstArrayView<LakeMap::FSpot> Spots = LakeMap::Spots();
    if (SpotWatch.Num() != Spots.Num()) return;
    FString Line;
    int32 Empty = 0;
    for (int32 I = 0; I < Spots.Num(); ++I)
    {
        FSpotWatch& W = SpotWatch[I];
        if (!W.Samples) continue;
        const float N = W.Samples;
        FString Kinds;
        for (int32 Sp = 0; Sp < W.Fish.Num(); ++Sp) Kinds += FString::Printf(TEXT("%s%.1f"), Sp ? TEXT("/") : TEXT(""), W.Fish[Sp] / N);
        Line += FString::Printf(TEXT(" | %s %.0f%% feed %.0f%% %s nearest %.0f m"), Spots[I].Name, W.Occupied * 100 / N, W.Feeding * 100 / N, *Kinds, W.Nearest / N / 100);
        Empty += W.Occupied == 0;
        W = FSpotWatch();
        // Where the spot's own schools are now: along the bank and out from the waterline (m), what they do.
        if (SpotSchools.IsValidIndex(I))
            for (const int32 School : SpotSchools[I])
                if (const ALakeFish* Lead = SchoolLeader(School))
                {
                    const FVector2D Local = FVector2D(Lead->GetActorLocation()) - Spots[I].Location;
                    const FString Doing = Lead->GetGoal() == EFishGoal::Feed ? FString::Printf(TEXT("feed:%s"), LakeFood::Name(Lead->GetFeedOn()))
                                        : Lead->GetGoal() == EFishGoal::Rest ? FString(TEXT("rest")) : FString(TEXT("cruise"));
                    Line += FString::Printf(TEXT(" [%s %.2f kg %+.0f/%.0f %s]"), SpeciesNoun(Lead->Species), Lead->WeightKg, FVector2D::DotProduct(Local, FVector2D(-Spots[I].Outward.Y, Spots[I].Outward.X)) / 100,
                        -FVector2D::DotProduct(Local, Spots[I].Outward) / 100, *Doing);
                }
    }
    const ALakeWeather* Weather = ALakeWeather::Get(GetWorld());
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_SPOTS hour=%.1f empty %d of %d%s"), Weather ? Weather->Hours() : -1.0f, Empty, Spots.Num(), *Line);
}

void ALakeLife::LogStock()
{
    // Where each species lives: depth under it, the share in the weedy shallows, within 15 m of the
    // bank, and in reach of a fishing spot; then the schools and how many fish each spot has in reach.
    const TConstArrayView<LakeMap::FSpot> Spots = LakeMap::Spots();
    for (int32 S = 0; S < LakeFish::NumSpecies(); ++S)
    {
        int32 Count = 0, Shallows = 0, Bank = 0, Reach = 0;
        double Depth = 0;
        TSet<int32> SchoolIds;
        for (const TWeakObjectPtr<ALakeFish>& Weak : AllFish)
        {
            const ALakeFish* Fish = Weak.Get();
            if (!Fish || (int32)Fish->Species != S) continue;
            const FVector P = Fish->GetActorLocation();
            ++Count;
            Depth += LakeMap::Depth(P.X, P.Y);
            Shallows += LakeMap::BackwaterAmount(P.X, P.Y) > 0.5f;
            Bank += LakeMap::ShoreDistance(P.X, P.Y) < 1500;
            bool bReach = false;
            for (const LakeMap::FSpot& Spot : Spots) bReach |= ReachDistance(Spot, P) < LakeFish::Info(Fish->Species).SenseRadius;
            Reach += bReach;
            if (Fish->GetSchool() >= 0) SchoolIds.Add(Fish->GetSchool());
        }
        if (!Count) continue;
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_STOCK %s: %d fish (%d schools), water under them %.2f m, in the weedy shallows %d%%, within 15 m of the bank %d%%, in reach of a spot %d%%"),
            SpeciesNoun((EFishSpecies)S), Count, SchoolIds.Num(), Depth / Count / 100, Shallows * 100 / Count, Bank * 100 / Count, Reach * 100 / Count);
    }
    SpotWatch.Reset();
    SampleSpots();
    FString Line;
    for (int32 I = 0; I < Spots.Num(); ++I)
    {
        Line += FString::Printf(TEXT(" | %s "), Spots[I].Name);
        for (int32 Sp = 0; Sp < SpotWatch[I].Fish.Num(); ++Sp) Line += FString::Printf(TEXT("%s%.0f"), Sp ? TEXT("/") : TEXT(""), SpotWatch[I].Fish[Sp]);
    }
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_STOCK in reach of each spot (perch/roach/crucian/pike)%s"), *Line);
    SpotWatch.Reset();
}

// --- stocking ---------------------------------------------------------------------------------

void ALakeLife::BuildStockPlaces()
{
    // The map's lazily built tables (ground, lilies, snags, spots) are made before the parallel loop.
    const int32 Kinds = LakeFish::NumSpecies();
    for (int32 S = 0; S < Kinds; ++S) LakeFish::HabitatScore((EFishSpecies)S, LakeMap::Centre.X, LakeMap::Centre.Y);
    const FBox2D Box = LakeMap::LakeBounds();
    constexpr float Step = 300;
    const int32 NX = FMath::CeilToInt(Box.GetSize().X / Step), NY = FMath::CeilToInt(Box.GetSize().Y / Step);
    TArray<TArray<FStockPlace>> Rows;
    Rows.SetNum(NY);
    ParallelFor(NY, [&](int32 Y)
    {
        FRandomStream Jitter(7919 * Y + 17);
        for (int32 X = 0; X < NX; ++X)
        {
            const FVector2D P = Box.Min + FVector2D(X + Jitter.FRandRange(0.1f, 0.9f), Y + Jitter.FRandRange(0.1f, 0.9f)) * Step;
            // The same rules as a fish's own choice (LakeFish::PickSpot): not hard against the bank, not on a shoal.
            if (LakeMap::ShoreDistance(P.X, P.Y) < 300 || LakeMap::Depth(P.X, P.Y) < 45) continue;
            FStockPlace Place;
            Place.P = P;
            Place.Score.SetNum(Kinds);
            for (int32 S = 0; S < Kinds; ++S) Place.Score[S] = LakeFish::HabitatScore((EFishSpecies)S, P.X, P.Y);
            Rows[Y].Add(Place);
        }
    });
    StockPlaces.Reset();
    for (TArray<FStockPlace>& Row : Rows) StockPlaces.Append(Row);
    StockRef.Init(1.0f, Kinds);
    for (int32 S = 0; S < Kinds; ++S)
    {
        TArray<float> Scores;
        for (const FStockPlace& Place : StockPlaces) Scores.Add(Place.Score[S]);
        Scores.Sort();
        StockRef[S] = Scores.Num() ? FMath::Max(0.01f, Scores[Scores.Num() * 4 / 5]) : 1.0f;
    }
}

bool ALakeLife::DrawStockPlace(EFishSpecies Species, const TArray<int32>& Candidates, const TArray<FVector2D>& Taken, float Spacing, float Away, FVector2D& Out)
{
    // Weighted by the square of the score: most fish live in the water that suits them best, a few
    // in poorer water.
    const int32 Count = Candidates.Num() ? Candidates.Num() : StockPlaces.Num();
    if (!Count) return false;
    auto Place = [&](int32 I) -> const FStockPlace& { return StockPlaces[Candidates.Num() ? Candidates[I] : I]; };
    TArray<double> Sum;
    Sum.SetNumUninitialized(Count);
    double Total = 0;
    for (int32 I = 0; I < Count; ++I) Sum[I] = Total += FMath::Square(Place(I).Score[(int32)Species]);
    if (Total <= 0) return false;
    for (int32 Try = 0; Try < 300; ++Try)
    {
        const int32 I = FMath::Min(Algo::LowerBound(Sum, StockRandom.FRand() * Total), Count - 1);
        const FVector2D P = Place(I).P;
        if (Away > 0 && IsNearPlayer(FVector(P, 0), Away)) continue;
        bool bFree = true;
        for (const FVector2D& Other : Taken) bFree &= FVector2D::DistSquared(Other, P) >= Spacing * Spacing;
        if (!bFree) continue;
        Out = P;
        return true;
    }
    return false;
}

ALakeFish* ALakeLife::SpawnStocked(EFishSpecies Species, FVector2D At, FVector Home, int32 School, float Drift, float Weight)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    // Most fish are small, big ones are rare; each keeps to its species' layer of the water.
    if (Weight < 0) Weight = FMath::Lerp(S.MinWeight, S.MaxWeight, FMath::Pow(StockRandom.FRand(), 2.5f));
    const float Z = FMath::Lerp(LakeFish::SurfaceZ - 10, ALakeFish::BottomZ(At.X, At.Y) + 8, StockRandom.FRandRange(S.DepthMin, S.DepthMax));
    ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(FVector(At, Z), FRotator(0, StockRandom.FRandRange(0, 360), 0));
    if (!Fish) return nullptr;
    Fish->Setup(Species, Weight, School >= 0 ? Home : FVector(At, Z), School);
    Fish->SetHomeDrift(Drift);
    return Fish;
}

void ALakeLife::StockPond()
{
    const double Begin = FPlatformTime::Seconds();
    StockRandom.Initialize(20261027);
    BuildStockPlaces();
    // How many of each species, from the passports (stock.count, in schools of stock.schoolSize).
    const int32 Kinds = LakeFish::NumSpecies();
    TArray<int32> SchoolsLeft, LonersLeft, Resident;
    SchoolsLeft.Init(0, Kinds); LonersLeft.Init(0, Kinds); Resident.Init(0, Kinds);
    TArray<EFishSpecies> Schooling, Ambushers, Loners;
    for (int32 Sp = 0; Sp < Kinds; ++Sp)
    {
        const FFishSpeciesInfo& S = LakeFish::Info((EFishSpecies)Sp);
        if (S.IsSchooling()) { SchoolsLeft[Sp] = S.Stock.Count / S.Stock.SchoolSize; Schooling.Add((EFishSpecies)Sp); }
        else { LonersLeft[Sp] = S.Stock.Count; (S.Habitat.bAmbush ? Ambushers : Loners).Add((EFishSpecies)Sp); }
    }
    // Homes of the schools and the ambushers: schools keep apart, ambushers keep their own patch of water.
    TArray<FVector2D> SchoolHomes, LieHomes;
    int32 NextSchool = 0;
    // Anywhere in the lake, keeping Spacing from the others if the water allows (less if it does not).
    auto DrawSpaced = [&](EFishSpecies Species, const TArray<FVector2D>& Taken, float Spacing, FVector2D& Out)
    {
        for (; Spacing > 50; Spacing *= 0.7f)
            if (DrawStockPlace(Species, {}, Taken, Spacing, 0, Out)) return true;
        return DrawStockPlace(Species, {}, {}, 0, 0, Out);
    };
    auto StockSchool = [&](EFishSpecies Species, FVector2D Home, float Drift)
    {
        const FFishSpeciesInfo& S = LakeFish::Info(Species);
        const FVector Home3(Home, FMath::Lerp(LakeFish::SurfaceZ - 10, ALakeFish::BottomZ(Home.X, Home.Y) + 8, (S.DepthMin + S.DepthMax) * 0.5f));
        const int32 School = NextSchool++;
        // Fish school with fish of their own size: one age, within a fifth of each other's weight.
        const float Age = FMath::Lerp(S.MinWeight, S.MaxWeight, FMath::Pow(StockRandom.FRand(), 2.5f));
        for (int32 I = 0; I < S.Stock.SchoolSize; ++I)
        {
            const FVector2D At = Home + FVector2D(StockRandom.FRandRange(-80, 80), StockRandom.FRandRange(-80, 80));
            const float Weight = FMath::Clamp(Age * StockRandom.FRandRange(0.8f, 1.2f), S.MinWeight, S.MaxWeight);
            SpawnStocked(Species, LakeMap::IsWater(At.X, At.Y) ? At : Home, Home3, School, Drift, Weight);
        }
        SchoolHomes.Add(Home);
        --SchoolsLeft[(int32)Species];
    };
    auto Jitter = [&](FVector2D P) { return P + FVector2D(StockRandom.FRandRange(-100, 100), StockRandom.FRandRange(-100, 100)); };
    // 1. The fish that live at each fishing spot, in the water its pole reaches and a few metres round.
    FString Plan;
    SpotSchools.Reset();
    for (const LakeMap::FSpot& Spot : LakeMap::Spots())
    {
        TArray<int32>& Own = SpotSchools.AddDefaulted_GetRef();
        TArray<int32> Near, Round;
        for (int32 I = 0; I < StockPlaces.Num(); ++I)
        {
            const float Dist = ReachDistance(Spot, FVector(StockPlaces[I].P, 0));
            if (Dist < 450) Near.Add(I);
            if (Dist < 900) Round.Add(I);
        }
        if (Near.IsEmpty()) continue;
        // How well this water suits a species compared with its good water elsewhere in the lake.
        auto Fit = [&](EFishSpecies Species, const TArray<int32>& Places)
        {
            float Sum = 0;
            for (int32 I : Places) Sum += StockPlaces[I].Score[(int32)Species];
            return Places.Num() ? Sum / Places.Num() / StockRef[(int32)Species] : 0.0f;
        };
        Plan += FString::Printf(TEXT(" | %s ("), Spot.Name);
        for (int32 Sp = 0; Sp < Kinds; ++Sp) Plan += FString::Printf(TEXT("%s%s %.2f"), Sp ? TEXT(", ") : TEXT(""), SpeciesNoun((EFishSpecies)Sp), Fit((EFishSpecies)Sp, Sp < Kinds && LakeFish::Info((EFishSpecies)Sp).IsSchooling() ? Near : Round));
        Plan += TEXT("):");
        FVector2D Home;
        // A school of the species that likes the water best, and of every other one that likes it
        // well enough (a bit closer, the strip is narrow).
        TArray<EFishSpecies> ByFit = Schooling;
        ByFit.Sort([&](EFishSpecies A, EFishSpecies B) { return Fit(A, Near) > Fit(B, Near); });
        for (int32 K = 0; K < ByFit.Num(); ++K)
        {
            const EFishSpecies Species = ByFit[K];
            if ((K > 0 && Fit(Species, Near) < 0.6f) || SchoolsLeft[(int32)Species] <= 0) continue;
            if (!DrawStockPlace(Species, Near, SchoolHomes, K == 0 ? 600.0f : 350.0f, 0, Home)) continue;
            Own.Add(NextSchool);
            StockSchool(Species, Home, 500);
            ++Resident[(int32)Species];
            Plan += FString::Printf(TEXT(" %s %d,"), SpeciesNoun(Species), LakeFish::Info(Species).Stock.SchoolSize);
        }
        // An ambusher where there is cover or a weed edge for its lie (at most about half of them live at the spots).
        for (const EFishSpecies Species : Ambushers)
        {
            if (Fit(Species, Round) <= 0.6f || LonersLeft[(int32)Species] <= 0 || Resident[(int32)Species] >= (LakeFish::Info(Species).Stock.Count + 1) * 55 / 100) continue;
            if (!DrawStockPlace(Species, Round, LieHomes, 1500, 0, Home)) continue;
            SpawnStocked(Species, Home, FVector::ZeroVector, -1, 400);
            LieHomes.Add(Home);
            ++Resident[(int32)Species];
            --LonersLeft[(int32)Species];
            Plan += FString::Printf(TEXT(" %s,"), SpeciesNoun(Species));
            break;
        }
        // The others: a shoal of them where the water suits them, a few at the other spots.
        for (const EFishSpecies Species : Loners)
        {
            const float LonerFit = Fit(Species, Round);
            const int32 Want = FMath::Min(LonerFit > 0.7f ? 8 : LonerFit > 0.3f ? 3 : 1, LonersLeft[(int32)Species]);
            int32 Placed = 0;
            for (int32 I = 0; I < Want; ++I)
                if (DrawStockPlace(Species, Round, {}, 0, 0, Home)) { SpawnStocked(Species, Jitter(Home), FVector::ZeroVector, -1, 2500); ++Placed; }
            LonersLeft[(int32)Species] -= Placed;
            Resident[(int32)Species] += Placed;
            Plan += FString::Printf(TEXT(" %s %d"), SpeciesNoun(Species), Placed);
        }
    }
    // 2. The rest of each species over the whole lake, as well as the water suits it; the schooling
    // species in turn, so none takes all the best water.
    for (bool bAny = true; bAny; )
    {
        bAny = false;
        for (const EFishSpecies Species : Schooling)
        {
            if (SchoolsLeft[(int32)Species] <= 0) continue;
            bAny = true;
            FVector2D Home;
            if (DrawSpaced(Species, SchoolHomes, 2500, Home)) StockSchool(Species, Home, 2500);
            else --SchoolsLeft[(int32)Species];
        }
    }
    for (const EFishSpecies Species : Ambushers)
        for (; LonersLeft[(int32)Species] > 0; --LonersLeft[(int32)Species])
        {
            FVector2D Home;
            if (!DrawSpaced(Species, LieHomes, 1500, Home)) continue;
            SpawnStocked(Species, Home, FVector::ZeroVector, -1, 2500);
            LieHomes.Add(Home);
        }
    for (const EFishSpecies Species : Loners)
        for (; LonersLeft[(int32)Species] > 0; --LonersLeft[(int32)Species])
        {
            FVector2D Home;
            if (DrawStockPlace(Species, {}, {}, 0, 0, Home)) SpawnStocked(Species, Jitter(Home), FVector::ZeroVector, -1, 2500);
        }
    FString Residents;
    for (int32 Sp = 0; Sp < Kinds; ++Sp) Residents += FString::Printf(TEXT("%s%s %d"), Sp ? TEXT(", ") : TEXT(""), SpeciesNoun((EFishSpecies)Sp), Resident[Sp]);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_STOCK pond stocked from %d places in %.2f s; living at the spots (schools or fish): %s%s"),
        StockPlaces.Num(), FPlatformTime::Seconds() - Begin, *Residents, *Plan);
}

void ALakeLife::Restock(const ALakeFish* Gone)
{
    if (!Gone || !HasAuthority()) return;
    RestockLater(Gone->Species, Gone->GetSchool(), Gone->GetHomeAnchor(), Gone->GetHomeDrift(), 45, 0);
}

void ALakeLife::RestockLater(EFishSpecies Species, int32 School, FVector Anchor, float Drift, float Delay, int32 Tries)
{
    FTimerHandle Timer;
    GetWorldTimerManager().SetTimer(Timer, FTimerDelegate::CreateWeakLambda(this, [this, Species, School, Anchor, Drift, Tries]
    {
        // Out of the anglers' sight: back into its school beside a free member, or at its school's
        // (its own) place; if an angler is there, a little later; in the end somewhere else its kind likes.
        constexpr float Unseen = 2500;
        FVector2D At(Anchor);
        bool bPlaced = false;
        // A newcomer to a school is of its size.
        float Weight = -1;
        if (const ALakeFish* Mate = School >= 0 ? SchoolLeader(School) : nullptr)
            Weight = FMath::Clamp(Mate->WeightKg * StockRandom.FRandRange(0.8f, 1.2f), LakeFish::Info(Species).MinWeight, LakeFish::Info(Species).MaxWeight);
        if (School >= 0)
            if (const ALakeFish* Lead = SchoolLeader(School); Lead && !IsNearPlayer(Lead->GetActorLocation(), Unseen))
            {
                At = FVector2D(Lead->GetActorLocation()) + FVector2D(StockRandom.FRandRange(-60, 60), StockRandom.FRandRange(-60, 60));
                bPlaced = LakeMap::IsWater(At.X, At.Y);
                if (!bPlaced) At = FVector2D(Anchor);
            }
        if (!bPlaced) bPlaced = LakeMap::IsWater(At.X, At.Y) && !IsNearPlayer(Anchor, Unseen);
        if (!bPlaced && Tries < 5) { RestockLater(Species, School, Anchor, Drift, 20, Tries + 1); return; }
        if (!bPlaced)
        {
            if (StockPlaces.IsEmpty()) BuildStockPlaces();
            FVector2D Elsewhere;
            if (DrawStockPlace(Species, {}, {}, 0, Unseen, Elsewhere)) At = Elsewhere;
        }
        SpawnStocked(Species, At, Anchor, School, Drift, Weight);
    }), Delay, false);
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
    // The nearest bolt; further off they only turn away (see ALakeFish::Startle).
    const FVector From = Victim->GetActorLocation();
    ForEachFishNear(From, Radius, [&](ALakeFish* Fish)
    {
        // (A fish big enough to eat the victim is not frightened by its struggle.)
        if (Fish != Victim && Fish->WeightKg * LakeFish::Info(Fish->Species).Feeding.PreyMaxWeight < Victim->WeightKg && Fish->IsFree())
            Fish->Startle(From, 1.0f - 0.6f * FVector::Dist(From, Fish->GetActorLocation()) / Radius);
    });
}

void ALakeLife::Disturb(const FVector& At, float Radius, float Strength)
{
    ForEachFishNear(At, Radius, [&](ALakeFish* Fish)
    {
        Fish->Startle(At, Strength * (1.0f - FVector::Dist(At, Fish->GetActorLocation()) / Radius));
    });
}

void ALakeLife::UpdateFootsteps()
{
    // Steps on the bank carry into the water: walking by the edge makes the fish close in shy
    // away, running or stamping along the pier makes them bolt. Standing still disturbs nothing.
    for (TActorIterator<ALakeCharacter> It(GetWorld()); It; ++It)
    {
        const FVector P = It->GetActorLocation();
        const float Speed = It->GetVelocity().Size2D();
        if (Speed < 60) continue;
        const bool bPier = LakeMap::OnPier(P);
        const float Radius = (450 + Speed * 1.2f) * (bPier ? 1.2f : 1.0f);
        Disturb(FVector(P.X, P.Y, LakeFish::SurfaceZ), Radius, FMath::Clamp(Speed / 350, 0.0f, 1.0f) * 0.5f * (bPier ? 1.5f : 1.0f));
    }
}

void ALakeLife::SampleMotion(float DeltaSeconds)
{
    for (const TWeakObjectPtr<ALakeFish>& Weak : AllFish)
    {
        ALakeFish* Fish = Weak.Get();
        if (!Fish || !Fish->IsFree()) { LastTurn.Remove(Weak); continue; }
        const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
        FMotionStats& M = MotionOf(Fish->Species);
        const float Speed = Fish->Velocity().Size();
        const float Turn = Fish->TurnRateNow();
        M.Speed += Speed;
        M.Turn += FMath::Abs(Turn);
        M.MaxTurn = FMath::Max(M.MaxTurn, FMath::Abs(Turn));
        if (const float* Before = LastTurn.Find(Weak))
        {
            const float TurnAccel = FMath::Abs(Turn - *Before) / FMath::Max(0.05f, DeltaSeconds);
            M.TurnAccel += TurnAccel;
            M.MaxTurnAccel = FMath::Max(M.MaxTurnAccel, TurnAccel);
        }
        LastTurn.Add(Weak, Turn);
        ++M.Samples;
        if (Speed < 0.15f * S.CruiseSpeed)
        {
            ++M.Still;
            if (FMath::Abs(Turn) > 45) ++M.Spins;
        }
    }
    // Schools: how far each fish is from its nearest mate (in body lengths) and how well they line up.
    for (const TPair<int32, TArray<TWeakObjectPtr<ALakeFish>>>& School : Schools)
    {
        TArray<const ALakeFish*, TInlineAllocator<16>> Free;
        for (const TWeakObjectPtr<ALakeFish>& Member : School.Value)
            if (const ALakeFish* Fish = Member.Get(); Fish && Fish->IsFree()) Free.Add(Fish);
        if (Free.Num() < 3) continue;
        FVector Sum = FVector::ZeroVector;
        float Speed = 0, Spacing = 0;
        for (const ALakeFish* Fish : Free)
        {
            Sum += Fish->Velocity().GetSafeNormal();
            Speed += Fish->Velocity().Size() / Free.Num();
            float Nearest = 1e9f;
            for (const ALakeFish* Other : Free)
                if (Other != Fish) Nearest = FMath::Min(Nearest, (float)FVector::Dist(Fish->GetActorLocation(), Other->GetActorLocation()));
            Spacing += Nearest / LakeFish::LengthCm(Fish->Species, Fish->WeightKg) / Free.Num();
        }
        if (Speed < 3) continue;
        FMotionStats& M = MotionOf(Free[0]->Species);
        M.Spacing += Spacing;
        M.Order += Sum.Size() / Free.Num();
        ++M.SchoolSamples;
    }
}

void ALakeLife::LogMotion()
{
    for (int32 Sp = 0; Sp < Motion.Num(); ++Sp)
    {
        FMotionStats& M = Motion[Sp];
        if (M.Samples == 0) continue;
        const FFishSpeciesInfo& S = LakeFish::Info((EFishSpecies)Sp);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH_MOTION %s samples=%d speed=%.1f cm/s (cruise %.0f) still=%.0f%% turn mean=%.1f max=%.0f deg/s turn-accel mean=%.0f max=%.0f deg/s2 spins=%.2f%% school spacing=%.2f lengths order=%.2f"),
            SpeciesNoun((EFishSpecies)Sp), M.Samples, M.Speed / M.Samples, S.CruiseSpeed, 100.0f * M.Still / M.Samples, M.Turn / M.Samples, M.MaxTurn,
            M.TurnAccel / M.Samples, M.MaxTurnAccel, 100.0f * M.Spins / M.Samples,
            M.SchoolSamples ? M.Spacing / M.SchoolSamples : 0.0, M.SchoolSamples ? M.Order / M.SchoolSamples : 0.0);
        M = FMotionStats();
    }
}

float ALakeLife::Percentile(const int32* Buckets, int32 Count, int32 Total, float P, float Step)
{
    const int32 Want = FMath::CeilToInt(Total * P);
    int32 Seen = 0;
    for (int32 I = 0; I < Count; ++I)
    {
        Seen += Buckets[I];
        if (Seen >= Want) return (I + 0.5f) * Step;
    }
    return Count * Step;
}

void ALakeLife::RecordShownMotion(const FVector& Where, float Accel, float TurnAccel, EFishSpecies Species, bool bBurst)
{
    // Only the fish this player can actually see swimming close by.
    if (FVector::DistSquared(Where, ViewLocation()) > FMath::Square(5000.0f)) return;
    // A burst (a strike, a rush at the fry) is a fast start of the fish's own, sharp on the server
    // too: kept apart, so the measure shows how smoothly the swimming between updates is shown.
    if (bBurst) { ++Shown.BurstSamples; ++Shown.BurstAccel[FMath::Clamp(FMath::FloorToInt(Accel / 50), 0, 63)]; return; }
    ++Shown.Samples;
    ++Shown.Accel[FMath::Clamp(FMath::FloorToInt(Accel / 50), 0, 63)];
    ++Shown.Turn[FMath::Clamp(FMath::FloorToInt(TurnAccel / 50), 0, 63)];
    Shown.MaxAccel = FMath::Max(Shown.MaxAccel, Accel);
    Shown.MaxTurn = FMath::Max(Shown.MaxTurn, TurnAccel);
    const int32 Sp = (int32)Species;
    if (Shown.BySpecies.Num() <= Sp) { Shown.BySpecies.SetNumZeroed(Sp + 1); Shown.JerkyBySpecies.SetNumZeroed(Sp + 1); }
    ++Shown.BySpecies[Sp];
    if (Accel > 600) ++Shown.JerkyBySpecies[Sp];
}

void ALakeLife::LogShownMotion()
{
    if (Shown.Samples < 100) return;
    // Which species the jerks come from.
    FString Jerky;
    for (int32 Sp = 0; Sp < Shown.BySpecies.Num(); ++Sp)
        if (Shown.BySpecies[Sp] > 0)
            Jerky += FString::Printf(TEXT(" %s %.1f%% of %d"), SpeciesNoun((EFishSpecies)Sp), 100.0f * Shown.JerkyBySpecies[Sp] / Shown.BySpecies[Sp], Shown.BySpecies[Sp]);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH_SMOOTH netmode=%d samples=%d accel p50=%.0f p99=%.0f max=%.0f cm/s2 turn-accel p50=%.0f p99=%.0f max=%.0f deg/s2 | over 600 cm/s2:%s | bursts kept apart: %d samples, p99 %.0f cm/s2"),
        (int32)GetNetMode(), Shown.Samples,
        Percentile(Shown.Accel, 64, Shown.Samples, 0.5f, 50), Percentile(Shown.Accel, 64, Shown.Samples, 0.99f, 50), Shown.MaxAccel,
        Percentile(Shown.Turn, 64, Shown.Samples, 0.5f, 50), Percentile(Shown.Turn, 64, Shown.Samples, 0.99f, 50), Shown.MaxTurn, *Jerky,
        Shown.BurstSamples, Shown.BurstSamples ? Percentile(Shown.BurstAccel, 64, Shown.BurstSamples, 0.99f, 50) : 0.0f);
    Shown = FShownStats();
}

void ALakeLife::Devour(ALakeFish* Prey, ALakeFish* Predator)
{
    if (!IsValid(Prey) || Prey->IsBusy()) return;
    ++Counters.Eaten;
    if (Predator)
    {
        const int32 Kinds = LakeFish::NumSpecies();
        if (Kills.Num() < Kinds * Kinds) Kills.SetNumZeroed(Kinds * Kinds);
        ++Kills[(int32)Predator->Species * Kinds + (int32)Prey->Species];
    }
    Alarm(Prey, 500);
    // The pond stays stocked: another fish of the species takes its place later, in its school and its water.
    Restock(Prey);
    Prey->Destroy();
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
            if (LakeFish::Info(Fish->Species).Feeding.bJumps && Fish->IsFree()
                && P.Z > LakeFish::SurfaceZ - 90 && IsNearPlayer(P, 5500)) Candidates.Add(Fish);
        }
    if (Candidates.IsEmpty()) return;
    const ALakeFish* Fish = Candidates[Random.RandRange(0, Candidates.Num() - 1)];
    EmitSign(ELakeSign::Jump, Fish->GetActorLocation(), 1, Fish->Species, Fish->WeightKg, Fish->GetActorRotation().Yaw);
}

void ALakeLife::LogStats()
{
    TArray<int32> Count;
    Count.Init(0, LakeFish::NumSpecies() * (int32)EFishGoal::Count);
    int32 Near = 0;
    for (const TWeakObjectPtr<ALakeFish>& Weak : AllFish)
        if (const ALakeFish* Fish = Weak.Get())
        {
            ++Count[(int32)Fish->Species * (int32)EFishGoal::Count + (int32)Fish->GetGoal()];
            Near += IsNearPlayer(Fish->GetActorLocation(), NearPlayer);
        }
    FString Line;
    auto Of = [&](int32 S, EFishGoal G) { return Count[S * (int32)EFishGoal::Count + (int32)G]; };
    for (int32 S = 0; S < LakeFish::NumSpecies(); ++S)
        Line += FString::Printf(TEXT(" | %s feed %d rest %d cruise %d"), SpeciesNoun((EFishSpecies)S), Of(S, EFishGoal::Feed), Of(S, EFishGoal::Rest), Of(S, EFishGoal::Cruise));
    const ALakeWeather* Weather = ALakeWeather::Get(GetWorld());
    const int32* N = Counters.Signs;
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LIFE hour=%.1f fish=%d near=%d%s | signs rise %d bubbles %d swirl %d splash %d scatter %d jump %d | fry attacks %d pike strikes %d eaten %d"),
        Weather ? Weather->Hours() : -1.0f, AllFish.Num(), Near, *Line, N[0], N[1], N[2], N[3], N[4], N[5], Counters.FryAttacks, Counters.PikeStrikes, Counters.Eaten);
    LogMotion();
    LogSpots();
    LogPrey();
    LogFood();
}

void ALakeLife::LogFood()
{
    const ALakeWeather* Weather = ALakeWeather::Get(GetWorld());
    if (!Weather || !FoodField.IsReady()) return;
    const FFishConditions C = Weather->FishConditions();
    FString Line = FString::Printf(TEXT("QUIET_LAKE_FOOD hour=%.1f plankton at %.1f m, insects %.2f, silt fauna %.2f | patches eaten below 70%%: plankton %d insects %d silt fauna %d weed %d"),
        Weather->Hours(), LakeFood::PlanktonDepthM(C), LakeFood::Season(EFishFood::Insects, C), LakeFood::Season(EFishFood::Benthos, C),
        FoodField.EatenPatches(EFishFood::Plankton), FoodField.EatenPatches(EFishFood::Insects), FoodField.EatenPatches(EFishFood::Benthos), FoodField.EatenPatches(EFishFood::Plants));
    // By species: what the feeding fish are on and how deep they feed; how far from the bank all of them are.
    for (int32 Sp = 0; Sp < LakeFish::NumSpecies(); ++Sp)
    {
        int32 On[7] = {};
        int32 Feeding = 0, All = 0;
        double Depth = 0, Column = 0, Bank = 0, Water = 0;
        for (const TWeakObjectPtr<ALakeFish>& Weak : AllFish)
        {
            const ALakeFish* Fish = Weak.Get();
            if (!Fish || (int32)Fish->Species != Sp || !Fish->IsFree()) continue;
            const FVector P = Fish->GetActorLocation();
            ++All;
            Bank += LakeMap::ShoreDistance(P.X, P.Y) / 100;
            Water += LakeMap::Depth(P.X, P.Y) / 100;
            if (Fish->GetGoal() != EFishGoal::Feed) continue;
            ++Feeding;
            ++On[FMath::Clamp((int32)Fish->GetFeedOn(), 0, 6)];
            Depth += (LakeFish::SurfaceZ - P.Z) / 100;
            Column += FMath::Clamp((LakeFish::SurfaceZ - P.Z) / FMath::Max(1.0f, LakeFish::SurfaceZ - ALakeFish::BottomZ(P.X, P.Y)), 0.0f, 1.0f);
        }
        if (!All) continue;
        Line += FString::Printf(TEXT(" | %s: feeding %d"), SpeciesNoun((EFishSpecies)Sp), Feeding);
        for (int32 F = 0; F < 7; ++F)
            if (On[F]) Line += FString::Printf(TEXT(" %s %.0f%%"), LakeFood::Name((EFishFood)F), 100.0f * On[F] / Feeding);
        if (Feeding) Line += FString::Printf(TEXT(", at %.1f m (%.2f of the depth)"), Depth / Feeding, Column / Feeding);
        Line += FString::Printf(TEXT("; all %d: %.1f m from the bank over %.1f m of water"), All, Bank / All, Water / All);
    }
    UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
}

void ALakeLife::SamplePrey()
{
    const int32 Kinds = LakeFish::NumSpecies();
    if (PreyWatch.Num() < Kinds) PreyWatch.SetNum(Kinds);
    for (const TWeakObjectPtr<ALakeFish>& Weak : AllFish)
    {
        const ALakeFish* Fish = Weak.Get();
        if (!Fish || !Fish->IsFree()) continue;
        FPreyWatch& W = PreyWatch[(int32)Fish->Species];
        ++W.Samples;
        // The nearest fish that could eat this one (big enough, and a fish-eater).
        const FVector P = Fish->GetActorLocation();
        float Threat = 1e9f, Mate = 1e9f, Seen = 1e9f;
        ForEachFishNear(P, 800, [&](ALakeFish* Other)
        {
            if (Other == Fish) return;
            const float Dist = FVector::Dist(P, Other->GetActorLocation());
            if (Other->GetSchool() >= 0 && Other->GetSchool() == Fish->GetSchool()) { Mate = FMath::Min(Mate, Dist); return; }
            if (Fish->WeightKg > Other->WeightKg * LakeFish::Info(Other->Species).Feeding.PreyMaxWeight) return;
            Threat = FMath::Min(Threat, Dist);
            // One it can tell is there: on the move or hunting (not lying still in ambush).
            if (Other->Velocity().SizeSquared() > FMath::Square(8.0f) || Other->GetGoal() == EFishGoal::Feed && !LakeFish::Info(Other->Species).Habitat.bAmbush) Seen = FMath::Min(Seen, Dist);
        });
        W.Seen += Seen < 800;
        W.SeenWithin150 += Seen < 150;
        const bool bThreat = Threat < 800;
        W.Threatened += bThreat;
        W.Within150 += Threat < 150;
        W.Within300 += Threat < 300;
        // School mates: how tight the school is (body lengths to the nearest mate), the fish afraid of a predator and not.
        if (Fish->GetSchool() >= 0 && Mate < 800)
        {
            const float Lengths = Mate / LakeFish::LengthCm(Fish->Species, Fish->WeightKg);
            if (Fish->FearNow() > 0.3f) { W.SpacingAfraid += Lengths; ++W.MatesAfraid; }
            else { W.Spacing += Lengths; ++W.Mates; }
        }
    }
}

void ALakeLife::LogPrey()
{
    FString Line, Eaten;
    for (int32 Sp = 0; Sp < PreyWatch.Num(); ++Sp)
    {
        FPreyWatch& W = PreyWatch[Sp];
        if (!W.Samples) continue;
        Line += FString::Printf(TEXT(" | %s: a predator within 8 m %.0f%%, of those within 3 m %.0f%%, within 1.5 m %.0f%%"), SpeciesNoun((EFishSpecies)Sp),
            100.0f * W.Threatened / W.Samples, W.Threatened ? 100.0f * W.Within300 / W.Threatened : 0.0f, W.Threatened ? 100.0f * W.Within150 / W.Threatened : 0.0f);
        Line += FString::Printf(TEXT(", within 1.5 m of one on the move %.0f%%"), W.Seen ? 100.0f * W.SeenWithin150 / W.Seen : 0.0f);
        if (W.Mates) Line += FString::Printf(TEXT(", school %.2f lengths (%.2f afraid, %d)"), W.Spacing / W.Mates, W.MatesAfraid ? W.SpacingAfraid / W.MatesAfraid : 0.0, W.MatesAfraid);
        W = FPreyWatch();
    }
    const int32 Kinds = LakeFish::NumSpecies();
    for (int32 I = 0; I < Kills.Num(); ++I)
        if (Kills[I]) Eaten += FString::Printf(TEXT(" %s>%s %d"), SpeciesNoun((EFishSpecies)(I / Kinds)), SpeciesNoun((EFishSpecies)(I % Kinds)), Kills[I]);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_PREY%s | eaten so far:%s | fry: %d rushes at them, about %.0f caught, %d times they darted off before a strike"),
        *Line, Eaten.IsEmpty() ? TEXT(" none") : *Eaten, Counters.FryAttacks, Counters.FryEaten, Counters.FryFlights);
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
    for (int32 Try = 0; Try < 600 && Fry.Num() < 40; ++Try)
    {
        const LakeMap::FShorePoint& Point = Shore[Random.RandRange(0, Shore.Num() - 1)];
        const FVector Spot = FrySpotNear(Point.Location - Point.Outward * Random.FRandRange(300, 1100), 400);
        if (!FryCanBe(FVector2D(Spot))) continue;
        bool bCrowded = false;
        for (const FFryShoal& Other : Fry) bCrowded |= FVector::Dist2D(Other.Location, Spot) < 2500;
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
        // Fry keep watch: a hunting perch or pike within a few metres sends the shoal darting off,
        // away from it and into the weeds.
        F.Alert = FMath::Max(0.0f, F.Alert - DeltaSeconds * 0.15f);
        if (Time >= F.NextLook && F.Panic < 2)
        {
            F.NextLook = Time + Random.FRandRange(0.4f, 0.8f);
            const ALakeFish* Danger = nullptr;
            float Nearest = 250;
            ForEachFishNear(L, 250, [&](ALakeFish* Fish)
            {
                const FFishFeeding& Eats = LakeFish::Info(Fish->Species).Feeding;
                if (!(Eats.bHuntsFry || Eats.PreyMaxWeight > 0) || !Fish->IsFree()) return;
                // A hunting one from a few metres (a pike lying still only close by), any other only when it comes right up.
                const float Dist = FVector::Dist(L, Fish->GetActorLocation());
                const bool bMoving = Fish->Velocity().SizeSquared() > FMath::Square(8.0f);
                const float Range = Fish->GetGoal() == EFishGoal::Feed ? (bMoving ? 250.0f : 250.0f * (1 - Eats.Camouflage)) : 100.0f;
                if (Dist < Range && Dist < Nearest) { Nearest = Dist; Danger = Fish; }
            });
            if (Danger && F.Alert < 0.5f)
            {
                const FVector2D Away = FVector2D(L - Danger->GetActorLocation()).GetSafeNormal() * Random.FRandRange(300.0f, 600.0f);
                F.Target = FrySpotNear(FVector2D(L) + Away, 250);
                F.Speed = 70;
                F.NextWander = Time + Random.FRandRange(10.0f, 20.0f);
                F.Alert = 1;
                ++Counters.FryFlights;
            }
        }
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
    // Among the lilies and the reeds, and on the alert, fry are harder to catch.
    const LakeMap::FZones Zones = LakeMap::ZonesAt(F.Location.X, F.Location.Y);
    const float Cover = FMath::Max(LakeMap::LilyAmount(F.Location.X, F.Location.Y), 0.6f * Zones.ReedEdge);
    const float Catch = (1 - 0.6f * Cover) * (1 - 0.4f * F.Alert);
    if (F.Count > 6 && Random.FRand() < Catch) { const int32 Lost = Random.RandRange(1, 3); F.Count -= (uint8)Lost; Counters.FryEaten += Lost; }
    const FVector Surface(F.Location.X, F.Location.Y, LakeFish::SurfaceZ);
    const EFishSpecies Species = Hunter ? Hunter->Species : EFishSpecies::Perch;
    const float Weight = Hunter ? Hunter->WeightKg : 0.2f;
    EmitSign(ELakeSign::Scatter, Surface, FMath::Clamp(0.6f + F.Count / 60.0f, 0.6f, 1.3f), Species, Weight);
    if (LakeFish::Info(Species).Habitat.bAmbush || Random.FRand() < 0.3f)
        EmitSign(ELakeSign::Splash, Surface + FVector(Random.FRandRange(-50, 50), Random.FRandRange(-50, 50), 0), FMath::Min(0.35f + 0.3f * Weight, 1.4f), Species, Weight);
    // Every hungry perch close by rushes in and snatches a mouthful (a full one only keeps with the school).
    ForEachFishNear(F.Location, 700, [&](ALakeFish* Fish)
    {
        if (!LakeFish::Info(Fish->Species).Feeding.bHuntsFry || Fish->GetGoal() != EFishGoal::Feed || !Fish->IsFree()) return;
        if (Fish != Hunter && Fish->Hunger < 0.15f) return;
        Fish->Dash(FVector(F.Location) + FVector(Random.FRandRange(-70, 70), Random.FRandRange(-70, 70), -10), 0.45f);
        Fish->Eat(0.1f * (0.5f + 0.5f * Catch));
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
    // Waves near the viewer: fish under the surface, floats and rain disturb the water.
    // Only where something is drawn: a machine without rendering (-NullRHI test clients) would
    // spend its cores on waves no one sees. QUIET_LAKE_NO_RIPPLES=1 leaves them out, to compare.
    Ripples = ALakeRipples::Get(GetWorld());
    if (!Ripples && FApp::CanEverRender() && FPlatformMisc::GetEnvironmentVariable(TEXT("QUIET_LAKE_NO_RIPPLES")).IsEmpty())
        Ripples = GetWorld()->SpawnActor<ALakeRipples>();
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
    // Under the water the view fades into murk within a couple of metres (M_Underwater); the volume
    // is local to this machine and switched on only while its camera is below the surface.
    if (UMaterialInterface* Murk = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Underwater.M_Underwater")))
    {
        FActorSpawnParameters Params;
        Params.ObjectFlags |= RF_Transient;
        Underwater = GetWorld()->SpawnActor<APostProcessVolume>(Params);
        if (Underwater)
        {
            Underwater->bUnbound = true;
            Underwater->Priority = 10;
            Underwater->BlendWeight = 0;
            Underwater->Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1, Murk));
        }
    }
}

void ALakeLife::PlaySound(USoundBase* Sound, const FVector& Location, float Volume, float Pitch)
{
    if (Sound) UGameplayStatics::PlaySoundAtLocation(this, Sound, Location, FRotator::ZeroRotator, Volume, Pitch, 0, Attenuation);
}

void ALakeLife::AddRing(ELakeSign Kind, const FVector& Location, float Size)
{
    // Near the viewer the water itself shows the sign: a push into the live waves (ALakeRipples)
    // instead of a drawn ring. Bubbles stay drawn (they are bubbles, not waves); farther signs too.
    if (Ripples && Kind != ELakeSign::Bubbles && Ripples->Covers(Location))
    {
        const float S = FMath::Clamp(Size, 0.1f, 2.5f);
        auto Spray = [&](int32 Count, float Spread, float MaxDelay)
        {
            for (int32 I = 0; I < Count; ++I)
            {
                const float A = Random.FRandRange(0, 2 * PI), D = Spread * FMath::Sqrt(Random.FRand());
                Ripples->Splash(Location + FVector(FMath::Cos(A) * D, FMath::Sin(A) * D, 0), Random.FRandRange(0.8f, 1.6f),
                    Random.FRandRange(12.0f, 30.0f), Random.FRandRange(0.15f, MaxDelay));
            }
        };
        switch (Kind)
        {
        case ELakeSign::Rise:       // a sip: a small dimple, then rings
            Ripples->Splash(Location, 2.0f + 2.0f * S, 45.0f + 60.0f * S);
            break;
        case ELakeSign::Swirl:      // a heave of water from below, then wide rings
            Ripples->Splash(Location, 5.0f + 8.0f * S, -(50.0f + 70.0f * S));
            Ripples->Splash(Location + FVector(Random.FRandRange(-8, 8) * S, Random.FRandRange(-8, 8) * S, 0), 4.0f + 4.0f * S, 30.0f * S, 0.12f);
            break;
        case ELakeSign::Splash:     // a hole punched in the water, and the spray falling back around it
            Ripples->Splash(Location, 4.0f + 5.0f * S, 110.0f + 120.0f * S);
            Spray(FMath::RoundToInt(5 + 8 * S), 15.0f + 40.0f * S, 0.6f);
            break;
        case ELakeSign::Scatter:    // fry spraying out of the water over a metre or two
            Ripples->Splash(Location, 6.0f, 60.0f * S);
            Spray(FMath::RoundToInt(18 + 12 * S), 60.0f + 50.0f * S, 0.9f);
            break;
        default:
            Ripples->Splash(Location, 2.0f + 2.0f * S, 30.0f * S);
            break;
        }
        return;
    }
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
    // What the species' passport says of this sign (or a general note).
    const FFishSigns& Says = LakeFish::Info(Sign.Species).Signs;
    auto Note = [](const FString& Own, const TCHAR* General) { return Own.IsEmpty() ? FString(General) : Own; };
    FString Text;
    switch (Sign.Kind)
    {
    case ELakeSign::Rise: Text = Note(Says.Rise, TEXT("Круги на воде")); break;
    case ELakeSign::Bubbles: Text = Note(Says.Bubbles, TEXT("Пузыри на воде")); break;
    case ELakeSign::Swirl: Text = Note(Says.Swirl, TEXT("Бурун на воде")); break;
    case ELakeSign::Splash: Text = Note(Says.Splash, TEXT("Всплеск")); break;
    case ELakeSign::Scatter: Text = Note(Says.Scatter, TEXT("Малёк брызнул из воды")); break;
    case ELakeSign::Jump: Text = Note(Says.Jump, TEXT("Из воды выпрыгнула рыба")); break;
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
    if (!FryMesh) FryMesh = LoadObject<UStaticMesh>(nullptr, *LakeFish::Info(EFishSpecies::Roach).Mesh);
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

// --- fish preview ----------------------------------------------------------------------------

void ALakeLife::RunFishPreview()
{
#if !UE_BUILD_SHIPPING
    // -FishPreview: how the fish swim. First the paths of the fish near the start drawn from high
    // above for 40 s (smooth arcs, schools moving as one); then a showcase school of roach, one of
    // perch, a feeding crucian and a pike in ambush filmed close up under the water in short
    // sequences, the pike's strike, a school shying away from a splash, fish of different sizes
    // side by side and roach basking at the top; the motion statistics go to the log.
    struct FState
    {
        TWeakObjectPtr<ACameraActor> Camera;
        TFunction<void(float)> Follow;
        FVector CamAt = FVector::ZeroVector;
        bool bCamSet = false;
        TMap<TWeakObjectPtr<ALakeFish>, FVector> Trails;
        FVector TrailCentre = FVector::ZeroVector;
        float TrailFrom = 4, TrailTo = 44, TrailRadius = 3200;
        TArray<TWeakObjectPtr<ALakeFish>> Roach, Perch, Lineup;
        TWeakObjectPtr<ALakeFish> Crucian, Pike, Bait;
    };
    TSharedRef<FState> St = MakeShared<FState>();
    FVector2D Facing;
    const FVector2D Start = LakeMap::StartSpot(&Facing);
    const FVector2D Right(-Facing.Y, Facing.X);
    // The showcase: the first water 1.6 m deep or more straight out from the start.
    FVector2D Show = Start + Facing * 900;
    for (float Out = 300; Out < 4000; Out += 50)
    {
        const FVector2D P = Start + Facing * Out;
        if (LakeMap::IsWater(P.X, P.Y) && LakeMap::Depth(P.X, P.Y) > 160 && LakeMap::ShoreDistance(P.X, P.Y) > 350) { Show = P; break; }
    }
    auto At = [this](float Seconds, TFunction<void()> Do)
    {
        FTimerHandle Handle;
        GetWorldTimerManager().SetTimer(Handle, [Do] { Do(); }, Seconds, false);
    };
    auto Shot = [this](const TCHAR* File)
    {
        FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / File, false, false);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH_PREVIEW shot %s at %.2f s"), File, GetWorld()->GetTimeSeconds());
    };
    auto Place = [this, St](const FVector& From, const FVector& Target, float Fov)
    {
        ACameraActor* Cam = St->Camera.Get();
        if (!Cam)
        {
            Cam = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation());
            St->Camera = Cam;
            if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
            {
                PC->SetViewTarget(Cam);
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
        }
        Cam->GetCameraComponent()->SetFieldOfView(Fov);
        Cam->SetActorLocationAndRotation(From, (Target - From).Rotation());
    };
    auto Centroid = [](const TArray<TWeakObjectPtr<ALakeFish>>& Fish, FVector& OutVel)
    {
        FVector C = FVector::ZeroVector;
        OutVel = FVector::ZeroVector;
        int32 N = 0;
        for (const TWeakObjectPtr<ALakeFish>& F : Fish)
            if (const ALakeFish* Fi = F.Get()) { C += Fi->GetActorLocation(); OutVel += Fi->Velocity(); ++N; }
        if (N) { C /= N; OutVel /= N; }
        return C;
    };
    // The camera follows a moving subject smoothly, every frame.
    FTimerHandle FollowTimer;
    GetWorldTimerManager().SetTimer(FollowTimer, [this, St]
    {
        if (St->Follow) St->Follow(GetWorld()->GetDeltaSeconds());
    }, 0.01f, true);
    // Side view under the water: the camera keeps beside the subject, level with it, a few metres off.
    auto FollowSide = [St, Place](TFunction<FVector(FVector&)> Subject, float Distance, float Height, float Fov)
    {
        St->bCamSet = false;
        St->Follow = [St, Place, Subject, Distance, Height, Fov](float Dt)
        {
            FVector Vel;
            const FVector C = Subject(Vel);
            FVector Side = FVector::CrossProduct(Vel.GetSafeNormal2D(), FVector::UpVector);
            if (Side.IsNearlyZero()) Side = FVector(0, 1, 0);
            FVector Want = C + Side * Distance;
            Want.Z = FMath::Clamp(C.Z + Height, ALakeFish::BottomZ(Want.X, Want.Y) + 12, LakeFish::SurfaceZ - 6);
            St->CamAt = St->bCamSet ? FMath::VInterpTo(St->CamAt, Want, Dt, 1.5f) : Want;
            St->bCamSet = true;
            Place(St->CamAt, C, Fov);
        };
    };
    auto Weather = [this](float Hours)
    {
        if (ALakeWeather* W = ALakeWeather::Get(GetWorld()))
        {
            FLakeWeatherState Calm;
            Calm.Cloud = 0.2f; Calm.Fog = 0.0f; Calm.Rain = 0; Calm.Wind = 0.05f; Calm.WindYaw = 40;
            W->SetWeather(Calm, 1);
            W->SetHours(Hours);
        }
    };

    // 1. Paths from above.
    At(2, [this, Weather, St, Start, Place]
    {
        Weather(10.5f);
        // The busiest patch of water within 45 m of the start (the fish there are fully simulated).
        int32 Best = -1;
        for (const TWeakObjectPtr<ALakeFish>& Weak : AllFish)
            if (const ALakeFish* Fish = Weak.Get(); Fish && FVector2D::Distance(FVector2D(Fish->GetActorLocation()), Start) < 4500)
            {
                int32 Count = 0;
                ForEachFishNear(Fish->GetActorLocation(), 2200, [&](ALakeFish*) { ++Count; });
                if (Count > Best) { Best = Count; St->TrailCentre = Fish->GetActorLocation(); }
            }
        if (Best < 0) St->TrailCentre = FVector(Start, LakeFish::SurfaceZ);
        St->TrailCentre.Z = LakeFish::SurfaceZ;
        Place(St->TrailCentre + FVector(0, 0, 3600), St->TrailCentre + FVector(1, 0, 0), 70);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH_PREVIEW trails around %s, %d fish within 22 m"), *St->TrailCentre.ToCompactString(), Best);
    });
    FTimerHandle TrailTimer;
    GetWorldTimerManager().SetTimer(TrailTimer, [this, St]
    {
        const float Now = GetWorld()->GetTimeSeconds();
        if (Now < St->TrailFrom || Now > St->TrailTo) return;
        static const FColor Colours[] = { FColor(255, 120, 0), FColor(90, 220, 255), FColor(255, 215, 40), FColor(60, 255, 60) };
        ForEachFishNear(St->TrailCentre, St->TrailRadius, [&](ALakeFish* Fish)
        {
            const FVector P = Fish->GetActorLocation();
            if (const FVector* Before = St->Trails.Find(Fish); Before && FVector::DistSquared(*Before, P) < FMath::Square(300.0f))
                DrawDebugLine(GetWorld(), FVector(Before->X, Before->Y, LakeFish::SurfaceZ + 2), FVector(P.X, P.Y, LakeFish::SurfaceZ + 2),
                              Colours[(int32)Fish->Species], true, -1, SDPG_Foreground, St->TrailRadius > 1000 ? 7.0f : 2.0f);
            St->Trails.Add(Fish, P);
        });
    }, 0.1f, true);
    At(44, [Shot] { Shot(TEXT("Fish_Trails.png")); });
    At(45, [this] { FlushPersistentDebugLines(GetWorld()); LogMotion(); });

    // 2. The showcase fish.
    At(45.5f, [this, St, Show, Facing, Right]
    {
        auto Spawn = [this](EFishSpecies Species, float Weight, FVector P, int32 School, float Yaw)
        {
            P.Z = FMath::Clamp(P.Z, ALakeFish::BottomZ(P.X, P.Y) + 12, LakeFish::SurfaceZ - 15);
            ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(P, FRotator(0, Yaw, 0));
            if (Fish) Fish->Setup(Species, Weight, P, School);
            return Fish;
        };
        const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Right.Y, Right.X));
        const float Mid = (LakeFish::SurfaceZ + ALakeFish::BottomZ(Show.X, Show.Y)) * 0.5f;
        const float RoachWeights[] = { 0.06f, 0.08f, 0.1f, 0.12f, 0.15f, 0.18f, 0.22f, 0.28f, 0.35f };
        for (int32 I = 0; I < 9; ++I)
            St->Roach.Add(Spawn(EFishSpecies::Roach, RoachWeights[I], FVector(Show + Right * (I % 3) * 35 - Facing * (I / 3) * 35, Mid), 9000, Yaw));
        const FVector2D PerchAt = Show + Facing * 500;
        const float PerchWeights[] = { 0.1f, 0.15f, 0.22f, 0.3f, 0.45f, 0.7f };
        for (int32 I = 0; I < 6; ++I)
            St->Perch.Add(Spawn(EFishSpecies::Perch, PerchWeights[I], FVector(PerchAt + Right * (I % 3) * 45 + Facing * (I / 3) * 45, Mid + 30), 9001, Yaw));
        // A crucian already at its feeding patch, nose in the silt.
        const FVector2D CrucianAt = Show - Right * 350;
        if (ALakeFish* Crucian = Spawn(EFishSpecies::Crucian, 0.6f, FVector(CrucianAt, ALakeFish::BottomZ(CrucianAt.X, CrucianAt.Y) + 8), -1, Yaw))
        {
            Crucian->Hunger = 1;
            Crucian->Energy = 1;
            Crucian->SetGoal(EFishGoal::Feed, 1000);
            Crucian->RoamTarget = Crucian->GetActorLocation();
            Crucian->bAtSpot = true;
            St->Crucian = Crucian;
        }
        // A pike lying in wait, facing the open water where the roach school swims.
        const FVector2D PikeAt = Show + Right * 420 + Facing * 150;
        if (ALakeFish* Pike = Spawn(EFishSpecies::Pike, 2.5f, FVector(PikeAt, Mid + 10), -1, Yaw + 180))
        {
            Pike->Hunger = 0.2f;   // not hungry yet: it only watches
            Pike->SetGoal(EFishGoal::Feed, 1000);
            Pike->RoamTarget = Pike->GetActorLocation();
            Pike->LieFacing = FVector(-Right, 0);
            Pike->LieUntil = 1e6f;
            Pike->bAtSpot = true;
            St->Pike = Pike;
        }
        for (const TWeakObjectPtr<ALakeFish>& F : St->Roach) if (F.IsValid()) F->SetGoal(EFishGoal::Cruise, 1000);
        for (const TWeakObjectPtr<ALakeFish>& F : St->Perch) if (F.IsValid()) F->SetGoal(EFishGoal::Cruise, 1000);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH_PREVIEW showcase at %s, water %.0f cm"), *FVector(Show, 0).ToCompactString(), LakeMap::Depth(Show.X, Show.Y));
    });

    // 3. The roach school close up.
    At(47, [St, FollowSide, Centroid] { FollowSide([St, Centroid](FVector& Vel) { return Centroid(St->Roach, Vel); }, 320, 30, 60); });
    At(52.0f, [Shot] { Shot(TEXT("Fish_Roach_1.png")); });
    At(52.3f, [Shot] { Shot(TEXT("Fish_Roach_2.png")); });
    At(52.6f, [Shot] { Shot(TEXT("Fish_Roach_3.png")); });
    At(52.9f, [Shot] { Shot(TEXT("Fish_Roach_4.png")); });
    At(54.0f, [St, Place, Centroid]
    {
        St->Follow = [St, Place, Centroid](float)
        {
            FVector Vel;
            const FVector C = Centroid(St->Roach, Vel);
            Place(FVector(C.X, C.Y, LakeFish::SurfaceZ) + FVector(-180, -180, 260), C, 60);
        };
    });
    At(56.0f, [Shot] { Shot(TEXT("Fish_Roach_Above.png")); });

    // 4. The perch school.
    At(57, [St, FollowSide, Centroid] { FollowSide([St, Centroid](FVector& Vel) { return Centroid(St->Perch, Vel); }, 340, 30, 60); });
    At(61.0f, [Shot] { Shot(TEXT("Fish_Perch_1.png")); });
    At(61.4f, [Shot] { Shot(TEXT("Fish_Perch_2.png")); });
    At(61.8f, [Shot] { Shot(TEXT("Fish_Perch_3.png")); });

    // 5. The crucian rooting at the bottom.
    At(63, [St, FollowSide] { FollowSide([St](FVector& Vel) { const ALakeFish* F = St->Crucian.Get(); Vel = F ? FRotator(0, F->GetActorRotation().Yaw, 0).Vector() : FVector::ForwardVector; return F ? F->GetActorLocation() : FVector::ZeroVector; }, 130, 25, 55); });
    At(66.0f, [Shot] { Shot(TEXT("Fish_Crucian_1.png")); });
    At(69.0f, [Shot] { Shot(TEXT("Fish_Crucian_2.png")); });
    At(72.0f, [St, Shot] { if (const ALakeFish* F = St->Crucian.Get()) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH_PREVIEW crucian rooting=%d pitch=%.0f height over bottom=%.0f cm"), F->bRooting, F->GetActorRotation().Pitch, F->GetActorLocation().Z - ALakeFish::BottomZ(F->GetActorLocation().X, F->GetActorLocation().Y)); Shot(TEXT("Fish_Crucian_3.png")); });

    // 6. The pike in its lie, then its strike at a small roach passing the snout.
    At(73, [St, FollowSide] { FollowSide([St](FVector& Vel) { const ALakeFish* F = St->Pike.Get(); Vel = F ? F->GetActorForwardVector() : FVector::ForwardVector; return F ? F->GetActorLocation() : FVector::ZeroVector; }, 300, 0, 60); });
    At(76.0f, [St, Shot, Place]
    {
        Shot(TEXT("Fish_Pike_Lie.png"));
        ALakeFish* Pike = St->Pike.Get();
        if (!Pike) return;
        // Hold the camera still, a little ahead of the pike, so its dash crosses the picture.
        St->Follow = nullptr;
        const FVector Ahead = Pike->GetActorLocation() + Pike->GetActorForwardVector() * 90;
        FVector Eye = Ahead - Pike->GetActorRightVector() * 330;
        Eye.Z = FMath::Clamp(Pike->GetActorLocation().Z + 25, ALakeFish::BottomZ(Eye.X, Eye.Y) + 12, LakeFish::SurfaceZ - 6);
        Place(Eye, Ahead, 60);
        const FVector Front = Pike->MouthLocation() + Pike->GetActorForwardVector() * 110 + Pike->GetActorRightVector() * 25;
        if (ALakeFish* Prey = Pike->GetWorld()->SpawnActor<ALakeFish>(Front, FRotator(0, Pike->GetActorRotation().Yaw + 90, 0)))
        {
            Prey->Setup(EFishSpecies::Roach, 0.1f, Front, -1);
            St->Bait = Prey;
        }
    });
    At(76.3f, [this, St]
    {
        ALakeFish* Pike = St->Pike.Get();
        ALakeFish* Prey = St->Bait.Get();
        if (!Pike || !Prey) return;
        Pike->Hunger = 1;
        Pike->Prey = Prey;
        Pike->StrikeEnd = Pike->SimTime + 0.8f;
        Pike->BurstTime = 0.8f;
        CountPikeStrike();
    });
    const float StrikeShots[] = { 76.25f, 76.4f, 76.5f, 76.6f, 76.75f, 77.1f };
    for (int32 I = 0; I < 6; ++I) At(StrikeShots[I], [Shot, I] { Shot(*FString::Printf(TEXT("Fish_Pike_Strike_%d.png"), I + 1)); });

    // 7. A splash beside the roach school: they turn away together.
    At(80, [this, St, Place, Centroid]
    {
        FVector Vel;
        const FVector C = Centroid(St->Roach, Vel);
        const FVector Eye = FVector(C.X, C.Y, LakeFish::SurfaceZ + 900);
        St->Follow = nullptr;
        Place(Eye, Eye - FVector(0.01f, 0, 1), 70);
        FlushPersistentDebugLines(GetWorld());
        St->Trails.Reset();
        St->TrailCentre = C;
        St->TrailRadius = 600;
        St->TrailFrom = 80;
        St->TrailTo = 86;
    });
    At(81.0f, [Shot] { Shot(TEXT("Fish_Shy_0.png")); });
    At(81.2f, [this, St, Centroid]
    {
        FVector Vel;
        const FVector C = Centroid(St->Roach, Vel);
        const FVector Splash = FVector(C.X, C.Y, LakeFish::SurfaceZ) + FVector::CrossProduct(Vel.GetSafeNormal2D(), FVector::UpVector) * 160;
        EmitSign(ELakeSign::Rise, Splash, 0.7f, EFishSpecies::Roach, 0.1f, 0, true);
        Disturb(Splash, 700, 0.6f);
        DrawDebugSphere(GetWorld(), Splash, 12, 8, FColor::Red, true, -1, SDPG_Foreground, 2);
        St->TrailCentre = Splash;
    });
    At(82.0f, [Shot] { Shot(TEXT("Fish_Shy_1.png")); });
    At(83.0f, [Shot] { Shot(TEXT("Fish_Shy_2.png")); });
    At(85.5f, [Shot] { Shot(TEXT("Fish_Shy_3.png")); });

    // 8. Fish of one species, small to big, side by side.
    At(86, [this, St, Show, Facing, Right, Place]
    {
        St->Follow = nullptr;
        FlushPersistentDebugLines(GetWorld());
        const FVector2D Row = Show - Facing * 250;
        const float Z = FMath::Max(ALakeFish::BottomZ(Row.X, Row.Y) + 40, LakeFish::SurfaceZ - 90);
        const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Right.Y, Right.X));
        const float Perch[] = { 0.08f, 0.3f, 0.8f };
        const float Crucian[] = { 0.15f, 0.5f, 0.9f };
        // Two rows nose to tail across the view (perch above, crucian below), the camera on the deep side.
        for (int32 I = 0; I < 3; ++I)
        {
            const FVector A(Row + Right * (I * 55 - 55), Z + 30), B(Row + Right * (I * 55 - 55), Z - 15);
            for (int32 K = 0; K < 2; ++K)
                if (ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(K ? B : A, FRotator(0, Yaw, 0)))
                {
                    Fish->Setup(K ? EFishSpecies::Crucian : EFishSpecies::Perch, K ? Crucian[I] : Perch[I], K ? B : A, -1);
                    Fish->SetGoal(EFishGoal::Rest, 1000);
                    Fish->RoamTarget = Fish->GetActorLocation();
                    Fish->bAtSpot = true;
                    St->Lineup.Add(Fish);
                }
        }
        const FVector Mid(Row, Z + 8);
        Place(Mid + FVector(Facing, 0) * 260, Mid, 55);
    });
    At(86.4f, [Shot] { Shot(TEXT("Fish_Sizes.png")); });

    // 9. Roach basking just under the surface on a warm still afternoon, seen from above the water.
    At(88, [Weather] { Weather(14.0f); });
    FTimerHandle BaskTimer;
    GetWorldTimerManager().SetTimer(BaskTimer, [this, St, Show, Facing]
    {
        const float Now = GetWorld()->GetTimeSeconds();
        if (Now < 88 || Now > 98) return;
        // Warm still afternoon: the school drifts in circles just under the top.
        if (ALakeFish* Lead = SchoolLeader(9000))
        {
            const FVector2D Circle = Show + Facing * 250 + FVector2D(FMath::Cos(Now * 0.25f), FMath::Sin(Now * 0.25f)) * 220;
            Lead->RoamTarget = FVector(Circle, LakeFish::SurfaceZ - 12);
            Lead->StateTime = 0;
        }
    }, 0.5f, true);
    At(95, [St, Place, Centroid]
    {
        St->Follow = [St, Place, Centroid](float)
        {
            FVector Vel;
            const FVector C = Centroid(St->Roach, Vel);
            Place(FVector(C.X, C.Y, LakeFish::SurfaceZ) + FVector(-200, 90, 170), FVector(C.X, C.Y, C.Z), 55);
        };
    });
    At(96.0f, [Shot] { Shot(TEXT("Fish_Surface_Above.png")); });
    At(96.2f, [St, FollowSide, Centroid] { FollowSide([St, Centroid](FVector& Vel) { return Centroid(St->Roach, Vel); }, 300, -40, 65); });
    At(97.0f, [St, Shot, Centroid]
    {
        FVector Vel;
        const FVector C = Centroid(St->Roach, Vel);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FISH_PREVIEW roach school %.0f cm under the surface"), LakeFish::SurfaceZ - C.Z);
        Shot(TEXT("Fish_Surface_Below.png"));
    });
    At(98, [this] { LogMotion(); LogShownMotion(); });
    At(100, [] { FPlatformMisc::RequestExit(false); });
#endif
}

// --- water preview ---------------------------------------------------------------------------

void ALakeLife::RunWaterPreview()
{
#if !UE_BUILD_SHIPPING
    // -WaterPreview: how the water looks, at four kinds of light (sunny morning, overcast noon, low
    // evening sun, night): from the bank at standing height, from a pier looking down, straight
    // down from above and under the water. Fish hang still in a row at 15, 40 and 80 cm under the
    // surface over 1.6 m or more of water, so it shows how deep one can see.
    struct FState
    {
        TWeakObjectPtr<ACameraActor> Camera;
        TArray<TPair<TWeakObjectPtr<ALakeFish>, FVector>> Held;
    };
    TSharedRef<FState> St = MakeShared<FState>();
    FVector2D Facing;
    const FVector2D Start = LakeMap::StartSpot(&Facing);
    const FVector2D Right(-Facing.Y, Facing.X);
    FVector2D Show = Start + Facing * 900;
    for (float Out = 300; Out < 4000; Out += 50)
    {
        const FVector2D P = Start + Facing * Out;
        if (LakeMap::IsWater(P.X, P.Y) && LakeMap::Depth(P.X, P.Y) > 160 && LakeMap::ShoreDistance(P.X, P.Y) > 350) { Show = P; break; }
    }
    auto At = [this](float Seconds, TFunction<void()> Do)
    {
        FTimerHandle Handle;
        GetWorldTimerManager().SetTimer(Handle, [Do] { Do(); }, Seconds, false);
    };
    auto Look = [this, St](const FVector& From, const FVector& Target, float Fov)
    {
        ACameraActor* Cam = St->Camera.Get();
        if (!Cam)
        {
            Cam = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation());
            St->Camera = Cam;
            if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
            {
                PC->SetViewTarget(Cam);
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
        }
        Cam->GetCameraComponent()->SetFieldOfView(Fov);
        Cam->SetActorLocationAndRotation(From, (Target - From).Rotation());
    };
    // The fish row: across the view, a roach, a perch and a crucian at each depth.
    At(2, [this, St, Show, Right, Facing]
    {
        const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Right.Y, Right.X));
        const float Depths[] = { 15, 40, 80 };
        const EFishSpecies Kinds[] = { EFishSpecies::Roach, EFishSpecies::Perch, EFishSpecies::Crucian };
        const float Weights[] = { 0.25f, 0.4f, 0.6f };
        for (int32 D = 0; D < 3; ++D)
            for (int32 K = 0; K < 3; ++K)
            {
                const FVector P(Show + Right * (K * 45 - 45) + Facing * (D * 60 - 60), LakeFish::SurfaceZ - Depths[D]);
                if (ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(P, FRotator(0, Yaw, 0)))
                {
                    Fish->Setup(Kinds[K], Weights[K], P, -1);
                    St->Held.Add({ Fish, P });
                }
            }
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_WATER_PREVIEW fish row at %s, water %.0f cm"), *FVector(Show, 0).ToCompactString(), LakeMap::Depth(Show.X, Show.Y));
    });
    // Keep them hanging where they were put (only the tail moves).
    FTimerHandle HoldTimer;
    GetWorldTimerManager().SetTimer(HoldTimer, [St]
    {
        for (const TPair<TWeakObjectPtr<ALakeFish>, FVector>& H : St->Held)
            if (ALakeFish* Fish = H.Key.Get())
            {
                Fish->SetActorLocation(H.Value);
                Fish->Vel = FVector::ZeroVector;
                Fish->Effort = 0.15f;
            }
    }, 0.02f, true);
    struct FLight { const TCHAR* Name; float Hours, Cloud; };
    const FLight Lights[] = { { TEXT("Sun"), 10.5f, 0.15f }, { TEXT("Grey"), 13.0f, 0.95f }, { TEXT("Evening"), 19.6f, 0.2f }, { TEXT("Night"), 23.5f, 0.2f } };
    const FVector Mid(Show, LakeFish::SurfaceZ - 40);
    const FVector2D Bank = Start - Facing * 120;
    // -WaterQuick: only the sunny morning.
    const int32 Count = FParse::Param(FCommandLine::Get(), TEXT("WaterQuick")) ? 1 : 4;
    for (int32 L = 0; L < Count; ++L)
    {
        const float T0 = 4 + L * 20.0f;
        const FLight Light = Lights[L];
        At(T0, [this, Light]
        {
            if (ALakeWeather* W = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState State;
                State.Cloud = Light.Cloud; State.Fog = 0; State.Rain = 0; State.Wind = 0.05f; State.WindYaw = 40;
                W->SetWeather(State, 1);
                W->SetHours(Light.Hours);
            }
        });
        auto Shot = [this, Light](const TCHAR* View)
        {
            const FString File = FString::Printf(TEXT("Water_%s_%s.png"), View, Light.Name);
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / File, false, false);
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_WATER_PREVIEW shot %s"), *File);
        };
        // From the bank at standing height, looking out past the fish row.
        At(T0 + 5, [Look, Bank, Mid] { Look(FVector(Bank, LakeMap::Ground(Bank.X, Bank.Y) + 165), Mid, 70); });
        At(T0 + 8, [Shot] { Shot(TEXT("Bank")); });
        // From a pier: 1.5 m over the water, 2.5 m back, looking down at about 30 degrees.
        At(T0 + 9, [Look, Mid, Facing] { Look(Mid - FVector(Facing, 0) * 250 + FVector(0, 0, 40 + 150), Mid, 60); });
        At(T0 + 12, [Shot] { Shot(TEXT("Pier")); });
        // Straight down from 3 m.
        At(T0 + 13, [Look, Mid] { Look(FVector(Mid.X, Mid.Y, LakeFish::SurfaceZ + 300), FVector(Mid.X + 0.01f, Mid.Y, Mid.Z), 60); });
        At(T0 + 15, [Shot] { Shot(TEXT("Top")); });
        // Under the water, 3 m in front of the rows (the near one 15 cm down, the far one 80 cm).
        At(T0 + 16, [Look, Mid, Facing]
        {
            FVector Eye = Mid - FVector(Facing, 0) * 320;
            Eye.Z = FMath::Clamp(LakeFish::SurfaceZ - 45.0f, ALakeFish::BottomZ(Eye.X, Eye.Y) + 10, LakeFish::SurfaceZ - 8.0f);
            Look(Eye, Mid, 65);
        });
        At(T0 + 18.5f, [Shot] { Shot(TEXT("Under")); });
    }
    // -WaterQuick also shows the pier view with the water surface hidden: what lies under it.
    if (Count == 1)
    {
        At(25, [this, Look, Mid, Facing]
        {
            for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
                if (It->GetWorld() == GetWorld() && It->GetNumMaterials() > 0 && It->GetMaterial(0) && It->GetMaterial(0)->GetBaseMaterial() && It->GetMaterial(0)->GetBaseMaterial()->GetName() == TEXT("M_LakeSurface"))
                    It->SetVisibility(false);
            Look(Mid - FVector(Facing, 0) * 250 + FVector(0, 0, 40 + 150), Mid, 60);
        });
        At(28, [] { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Water_Pier_Hidden.png"), false, false); });
    }
    At(FMath::Max(4 + Count * 20.0f + 1, 30.0f), [] { FPlatformMisc::RequestExit(false); });
#endif
}
