#include "LakeFish.h"
#include "LakeGame.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

// ---------------------------------------------------------------------------------------------
// Species table. Values are game-tuned but follow the real fish:
//  perch  - schooling predator of drop-offs, snags and piles, bold, prefers worms, head-shaking fight;
//  roach  - schools along the reed and lily edges in mid-water, maggot and bread, light and cautious;
//  crucian- warm weedy shallows and the silty backwater, bottom feeder, very cautious nibbles;
//  pike   - solitary ambush hunter at the edge of reeds and lilies and by snags, explosive bursts.
// Habitat weights: Shallows, ReedEdge, DropOff, Cover, Deep, Open, water depth (m), ambush.
// Mood (last column): Dawn, Day, Dusk, Night; water temperature low/best/high; likes overcast;
// light rain, heavy rain; ripple, strong wind; fog; sensitivity to fronts. Following anglers' lore:
//  perch  - by day, best morning and evening, steady weather and a light ripple, sulks in a downpour;
//  roach  - the dawns, warm settled weather, bites in a light rain, dislikes strong wind;
//  crucian- warmth: dawns, evenings and warm nights, calm, drizzle and mist; very sensitive to fronts;
//  pike   - "pike weather": grey sky, a breeze, drizzle; poor on bright calm days, idle at night.
// ---------------------------------------------------------------------------------------------
namespace
{
    const FFishSpeciesInfo GSpecies[] = {
        // Name           Mesh                                        Len  K     Wmin  Wmax  Habitat                                                    Column       Cruise Burst Turn  Acc  School Sense  Worm Maggot Bread  Nib    Str  Stam  Beat
        { TEXT("Окунь"),  TEXT("/Game/Fish/SM_Fish_Perch.SM_Fish_Perch"),     26, 1.25f, 0.08f, 0.8f, {0.1f, 0.3f, 1.0f, 0.9f, 0.5f, 0.3f, 1.2f, 4.8f, false},  0.3f, 0.75f, 28, 160, 220, 380, 5, 650, {1.0f, 0.55f, 0.08f}, 0, 1, 1.0f, 9,  9 , {1.0f, 0.75f, 0.9f, 0.15f,  6, 18, 27,  0.1f,  1.0f, 0.5f,  1.15f, 0.6f,  0.85f, 0.8f} },
        { TEXT("Плотва"), TEXT("/Game/Fish/SM_Fish_Roach.SM_Fish_Roach"),     22, 1.1f,  0.05f, 0.4f, {0.5f, 1.0f, 0.3f, 0.3f, 0.0f, 0.4f, 0.6f, 2.8f, false},  0.1f, 0.45f, 24, 130, 200, 320, 7, 450, {0.35f, 1.0f, 0.85f}, 1, 3, 0.7f, 6, 10 , {1.0f, 0.55f, 0.95f, 0.3f,  8, 19, 26,  0.2f,  1.2f, 0.6f,  1.1f, 0.5f,  1.0f, 0.7f} },
        { TEXT("Карась"), TEXT("/Game/Fish/SM_Fish_Crucian.SM_Fish_Crucian"), 20, 1.9f,  0.15f, 0.9f, {1.0f, 0.4f, 0.0f, 0.2f, 0.0f, 0.0f, 0.35f, 1.8f, false}, 0.75f, 1.0f, 12,  80, 110, 160, 1, 350, {0.55f, 0.7f, 1.0f},  3, 6, 0.9f, 14, 6 , {1.0f, 0.4f, 0.9f, 0.7f,  12, 23, 29,  0.3f,  1.25f, 0.5f,  0.85f, 0.35f,  1.2f, 1.0f} },
        { TEXT("Щука"),   TEXT("/Game/Fish/SM_Fish_Pike.SM_Fish_Pike"),       62, 0.65f, 0.8f,  3.5f, {0.6f, 1.0f, 0.4f, 1.0f, 0.1f, 0.1f, 0.6f, 2.6f, true},   0.3f, 0.7f,  10, 420, 120, 900, 1, 800, {0.18f, 0.05f, 0.0f}, 0, 0, 1.6f, 12, 7 , {0.95f, 0.6f, 0.9f, 0.12f,  5, 17, 27,  0.6f,  1.3f, 0.7f,  1.3f, 0.75f,  0.8f, 0.8f} },
    };
    const TCHAR* GBaitNames[] = { TEXT("червь"), TEXT("опарыш"), TEXT("хлеб") };

    float Depth01(float Z, float X, float Y)
    {
        const float Bottom = ALakeFish::BottomZ(X, Y);
        return FMath::Clamp((LakeFish::SurfaceZ - Z) / FMath::Max(1.0f, LakeFish::SurfaceZ - Bottom), 0.0f, 1.0f);
    }
}

const FFishSpeciesInfo& LakeFish::Info(EFishSpecies Species) { return GSpecies[FMath::Clamp((int32)Species, 0, 3)]; }
const TCHAR* LakeFish::BaitName(EBait Bait) { return GBaitNames[FMath::Clamp((int32)Bait, 0, 2)]; }
float LakeFish::LengthCm(EFishSpecies Species, float WeightKg)
{
    return FMath::Pow(WeightKg * 1000.0f * 100.0f / Info(Species).Condition, 1.0f / 3.0f);
}
float LakeFish::BaitInterest(EFishSpecies Species, EBait Bait, float DepthFraction)
{
    const FFishSpeciesInfo& S = Info(Species);
    // Bait outside the species' depth band is found less often (a bottom feeder rarely rises to it).
    const float Mid = (S.DepthMin + S.DepthMax) * 0.5f, Half = (S.DepthMax - S.DepthMin) * 0.5f + 0.15f;
    const float DepthMatch = FMath::Clamp(1.0f - FMath::Max(0.0f, FMath::Abs(DepthFraction - Mid) - Half) * 2.5f, 0.15f, 1.0f);
    return S.BaitLike[FMath::Clamp((int32)Bait, 0, 2)] * DepthMatch;
}
float LakeFish::Activity(EFishSpecies Species, const FFishConditions& C)
{
    const FFishMood& M = Info(Species).Mood;
    auto S = [](float A, float B, float X) { return FMath::SmoothStep(A, B, X); };
    // Time of day from the sun: night below -10 degrees, full day above 18, twilight in between.
    const float Night = 1 - S(-10.0f, -4.0f, C.SunElevation);
    const float Day = S(8.0f, 18.0f, C.SunElevation);
    const float Twilight = FMath::Max(0.0f, 1 - Night - Day);
    const float Time = Night * M.Night + Day * M.Day + Twilight * (C.bMorning ? M.Dawn : M.Dusk);
    // Water temperature: sluggish towards both ends of the species' range.
    const float T = C.WaterTempC;
    const float TempFit = T < M.TempBest ? S(M.TempLow, M.TempBest, T) : 1 - S(M.TempBest, M.TempHigh, T);
    const float Temp = 0.2f + 0.8f * TempFit;
    // Sun and cloud matter only by day.
    const float Light = S(0.0f, 15.0f, C.SunElevation);
    const float Sky = 1 + M.Overcast * (C.Cloud - 0.5f) * 0.8f * Light;
    const float LightRain = S(0.05f, 0.2f, C.Rain) * (1 - S(0.4f, 0.7f, C.Rain)), HeavyRain = S(0.5f, 0.9f, C.Rain);
    const float Rain = FMath::Lerp(1.0f, M.LightRain, LightRain) * FMath::Lerp(1.0f, M.HeavyRain, HeavyRain);
    const float Ripple = S(0.08f, 0.2f, C.Wind) * (1 - S(0.45f, 0.65f, C.Wind)), Strong = S(0.55f, 0.85f, C.Wind);
    const float Wind = FMath::Lerp(1.0f, M.Ripple, Ripple) * FMath::Lerp(1.0f, M.StrongWind, Strong);
    const float Fog = FMath::Lerp(1.0f, M.Fog, S(0.4f, 0.8f, C.Fog));
    // Falling pressure before a front: a feeding spell. Rising sharply or right after a sharp change:
    // the fish sulk. Days of steady weather are the best.
    const float Falling = S(0.3f, 1.2f, -C.PressureTrend), Rising = S(0.3f, 1.2f, C.PressureTrend);
    const float Pressure = (1 + 0.4f * M.Front * Falling) * (1 - 0.25f * M.Front * Rising) * (1 - 0.45f * M.Front * C.Shock)
        * (1 + 0.12f * S(6.0f, 24.0f, C.StableHours));
    return FMath::Clamp(Time * Temp * Sky * Rain * Wind * Fog * Pressure, 0.05f, 1.6f);
}
float LakeFish::DepthShiftM(EFishSpecies Species, const FFishConditions& C)
{
    const FFishMood& M = Info(Species).Mood;
    const float Light = FMath::SmoothStep(0.0f, 15.0f, C.SunElevation);
    const float Heat = FMath::SmoothStep(M.TempBest, M.TempBest + 5, C.WaterTempC);
    const float Bright = Light * (1 - C.Cloud) * (1 - FMath::SmoothStep(0.15f, 0.4f, C.Wind));
    const float Night = 1 - FMath::SmoothStep(-10.0f, -4.0f, C.SunElevation);
    // Crucian (warm-water fish) come up into the shallows to feed at night.
    const float NightShallows = Species == EFishSpecies::Crucian ? 0.4f * Night : 0.0f;
    return FMath::Clamp(0.8f * Heat + 0.5f * Bright - 0.3f * C.Cloud * Light - NightShallows, -0.6f, 1.2f);
}
bool LakeFish::IsBrightCalm(const FFishConditions& C)
{
    return C.SunElevation > 12 && C.Cloud < 0.35f && C.Wind < 0.12f && C.Rain < 0.05f;
}
float LakeFish::HabitatScore(EFishSpecies Species, float X, float Y, const FFishConditions* Conditions)
{
    const FFishHabitat& H = Info(Species).Habitat;
    if (!LakeMap::IsWater(X, Y)) return 0;
    const LakeMap::FZones Z = LakeMap::ZonesAt(X, Y);
    const float Sum = H.Shallows + H.ReedEdge + H.DropOff + H.Cover + H.Deep + H.Open;
    const float Like = (H.Shallows * Z.Shallows + H.ReedEdge * Z.ReedEdge + H.DropOff * Z.DropOff + H.Cover * Z.Cover
        + H.Deep * Z.Deep + H.Open * Z.Open) / FMath::Max(0.01f, Sum);
    const float D = LakeMap::Depth(X, Y) / 100;
    // Warm water and bright sun push the depth band down, a grey sky lets it up.
    const float Shift = Conditions ? DepthShiftM(Species, *Conditions) : 0.0f;
    const float Min = FMath::Max(0.25f, H.MinDepthM + Shift), Max = FMath::Max(Min + 0.4f, H.MaxDepthM + FMath::Max(0.0f, Shift));
    const float DepthFit = FMath::SmoothStep(Min - 0.4f, Min, D) * (1 - FMath::SmoothStep(Max, Max + 0.6f, D));
    // The wind drives food to the bank it blows onto, and the fish follow.
    float Downwind = 1;
    if (Conditions && Conditions->Wind > 0.15f)
    {
        const float SD = LakeMap::ShoreDistance(X, Y) / 100;
        const FVector2D Out = (FVector2D(X, Y) - LakeMap::Centre).GetSafeNormal();
        Downwind = 1 + 0.8f * Conditions->Wind * FMath::Max(0.0f, FVector2D::DotProduct(Out, Conditions->WindDir)) * (1 - FMath::SmoothStep(10.0f, 25.0f, SD));
    }
    return (0.03f + Like) * DepthFit * Downwind;
}
FVector LakeFish::PickSpot(EFishSpecies Species, FRandomStream& Random, FVector2D Near, float Range, const FFishConditions* Conditions)
{
    // Best of a few random candidates, with some chance so fish do not all crowd one spot.
    static const FBox2D Lake = [] { FBox2D Box(ForceInit); for (const LakeMap::FShorePoint& P : LakeMap::Shoreline()) Box += P.Location; return Box; }();
    const FFishSpeciesInfo& S = Info(Species);
    FVector2D Best = Range > 0 ? Near : LakeMap::Centre;
    float BestScore = -1;
    for (int32 Try = 0; Try < (Range > 0 ? 10 : 60); ++Try)
    {
        const FVector2D P = Range > 0 ? Near + FVector2D(Random.FRandRange(-Range, Range), Random.FRandRange(-Range, Range))
                                      : FVector2D(Random.FRandRange(Lake.Min.X, Lake.Max.X), Random.FRandRange(Lake.Min.Y, Lake.Max.Y));
        if (LakeMap::Depth(P.X, P.Y) < 35) continue;
        const float Score = HabitatScore(Species, P.X, P.Y, Conditions) * Random.FRandRange(0.6f, 1.0f);
        if (Score > BestScore) { BestScore = Score; Best = P; }
    }
    if (BestScore < 0 && LakeMap::Depth(Best.X, Best.Y) < 35) Best = LakeMap::Centre;
    const float Bottom = ALakeFish::BottomZ(Best.X, Best.Y);
    // Heat and bright sun also press the fish towards the bottom of the water column.
    const float Lower = Conditions ? FMath::Clamp(DepthShiftM(Species, *Conditions) * 0.25f, -0.2f, 0.3f) : 0.0f;
    const float Column = FMath::Clamp(Random.FRandRange(S.DepthMin, S.DepthMax) + Lower, 0.0f, 1.0f);
    return FVector(Best, FMath::Lerp(SurfaceZ - 10, Bottom + 8, Column));
}
float ALakeFish::BottomZ(float X, float Y) { return ALakeEnvironment::GroundHeight(X, Y); }

ALakeFish::ALakeFish()
{
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;
    bAlwaysRelevant = true;   // every angler sees every fish
    SetReplicatingMovement(false);
    SetNetUpdateFrequency(12);
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
    Body->SetupAttachment(RootComponent);
    Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Body->SetCastShadow(true);
}

void ALakeFish::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ALakeFish, Species);
    DOREPLIFETIME(ALakeFish, WeightKg);
    DOREPLIFETIME(ALakeFish, NetLocation);
    DOREPLIFETIME(ALakeFish, NetRotation);
    DOREPLIFETIME(ALakeFish, NetEffort);
}

void ALakeFish::Setup(EFishSpecies InSpecies, float InWeight, FVector Home, int32 InSchool)
{
    Species = InSpecies;
    WeightKg = InWeight;
    HomePoint = Home;
    School = InSchool;
    Random.Initialize(GetUniqueID() * 7919 + (int32)(InWeight * 1000));
    Phase = Random.FRandRange(0, 2 * PI);
    Hunger = Random.FRandRange(0.55f, 1.0f);
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    // Bigger fish of a species hold out longer.
    MaxStamina = S.Stamina * FMath::Lerp(0.6f, 1.8f, FMath::Clamp((WeightKg - S.MinWeight) / (S.MaxWeight - S.MinWeight), 0.0f, 1.0f));
    Stamina = MaxStamina;
    NetLocation = GetActorLocation();
    NetRotation = GetActorRotation();
    ChooseRoamTarget();
    UpdateVisual(0);
}

FVector ALakeFish::MouthLocation() const
{
    return GetActorLocation() + GetActorForwardVector() * LakeFish::LengthCm(Species, WeightKg) * 0.42f;
}

// --- habitat -------------------------------------------------------------------------------

bool ALakeFish::GetConditions(FFishConditions& Out) const
{
    const ALakeWeather* Weather = ALakeWeather::Get(GetWorld());
    if (!Weather) { Out = FFishConditions(); return false; }
    Out = Weather->FishConditions();
    return true;
}

void ALakeFish::ChooseRoamTarget()
{
    // Schools and ambushers keep to their home area; loners wander through good water nearby.
    // Now and then a home moves to better water for the conditions (deeper in the heat, to the
    // bank the wind blows onto), so schools drift with the weather.
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    FFishConditions Conditions;
    const bool bWeather = GetConditions(Conditions);
    const FFishConditions* C = bWeather ? &Conditions : nullptr;
    const bool bHomeBound = S.SchoolSize > 1 || S.Habitat.bAmbush;
    if (bHomeBound && bWeather && Random.FRand() < 0.08f) HomePoint = LakeFish::PickSpot(Species, Random, FVector2D(HomePoint), 2500, C);
    const float Range = S.Habitat.bAmbush ? 350.0f : S.SchoolSize > 1 ? 900.0f : 2500.0f;
    RoamTarget = LakeFish::PickSpot(Species, Random, FVector2D(bHomeBound ? HomePoint : GetActorLocation()), Range, C);
}

FVector ALakeFish::SteerRoam() const
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FVector To = RoamTarget - GetActorLocation();
    // Pike hang almost still at their ambush spot; others cruise and slow down on arrival.
    const float Lively = FMath::Lerp(0.45f, 1.0f, FMath::Clamp(Activity, 0.0f, 1.0f));
    const float Speed = S.Habitat.bAmbush ? FMath::Min(S.CruiseSpeed, To.Size() * 0.2f)
                                          : S.CruiseSpeed * FMath::Clamp(To.Size() / 120.0f, 0.3f, 1.0f) * Lively;
    return To.GetSafeNormal() * Speed;
}

FVector ALakeFish::SteerSchool() const
{
    if (School < 0) return FVector::ZeroVector;
    FVector Centre = FVector::ZeroVector, Heading = FVector::ZeroVector, Separation = FVector::ZeroVector;
    int32 Count = 0;
    for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
    {
        const ALakeFish* Other = *It;
        if (Other == this) continue;
        const FVector Offset = GetActorLocation() - Other->GetActorLocation();
        const float Dist = Offset.Size();
        if (Dist < 35 && Dist > 0.1f) Separation += Offset / (Dist * Dist) * 35;   // any species
        if (Other->School != School || Dist > 260) continue;
        Centre += Other->GetActorLocation();
        Heading += Other->Vel;
        ++Count;
    }
    if (Count == 0) return Separation * 20;
    Centre /= Count;
    Heading /= Count;
    return (Centre - GetActorLocation()) * 0.25f + Heading * 0.45f + Separation * 20;
}

FVector ALakeFish::SteerBounds() const
{
    const FVector P = GetActorLocation();
    const float Shore = LakeMap::ShoreDistance(P.X, P.Y);
    const float Depth = LakeFish::SurfaceZ - BottomZ(P.X, P.Y);
    FVector Push = FVector::ZeroVector;
    if (Shore < 200 || Depth < 35)
    {
        // Back towards deeper water.
        Push += FVector(LakeMap::DeeperDirection(P.X, P.Y), 0) * 60 * FMath::Max(1.0f, (200 - Shore) / 60);
    }
    if (P.Z > LakeFish::SurfaceZ - 12) Push.Z -= 40;
    if (P.Z < BottomZ(P.X, P.Y) + 10) Push.Z += 40;
    return Push;
}

// --- reaction to bait ----------------------------------------------------------------------

void ALakeFish::UpdateBait(float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    ALakeCharacter* Angler = TargetAngler.Get();
    // The bait was pulled out of the water, or another fish got there first.
    if ((State == EState::Investigate || State == EState::Nibble) && (!Angler || !Angler->HasBaitInWater()))
    {
        State = EState::Roam;
        TargetAngler = nullptr;
        return;
    }
    if (SimTime >= NextMoodCheck)
    {
        NextMoodCheck = SimTime + 2.0f;
        FFishConditions Conditions;
        Activity = GetConditions(Conditions) ? LakeFish::Activity(Species, Conditions) : 1.0f;
    }
    if (State == EState::Roam && SimTime >= NextThink)
    {
        NextThink = SimTime + Random.FRandRange(0.6f, 1.4f);
        for (TActorIterator<ALakeCharacter> It(GetWorld()); It; ++It)
        {
            ALakeCharacter* Candidate = *It;
            if (!Candidate->HasBaitInWater()) continue;
            const float* Until = IgnoreBaitUntil.Find(Candidate);
            if (Until && *Until > SimTime) continue;
            const FVector Bait = Candidate->BaitLocation();
            const float Dist = FVector::Dist(Bait, GetActorLocation());
            if (Dist > S.SenseRadius) continue;
            const float Interest = LakeFish::BaitInterest(Species, Candidate->Bait, Depth01(Bait.Z, Bait.X, Bait.Y))
                * Hunger * Activity * FMath::Sqrt(1 - Dist / S.SenseRadius);
            if (Random.FRand() < Interest * 0.7f)
            {
                State = EState::Investigate;
                StateTime = 0;
                TargetAngler = Candidate;
                return;
            }
        }
        return;
    }
    if (State == EState::Investigate)
    {
        // Lose interest if it takes too long to get there.
        if (StateTime > 25)
        {
            IgnoreBaitUntil.Add(Angler, SimTime + 20);
            State = EState::Roam;
            return;
        }
        if (FVector::Dist(MouthLocation(), Angler->BaitLocation()) < 14 && Angler->ClaimBait(this))
        {
            State = EState::Nibble;
            StateTime = 0;
            // Wary fish test the bait more often: low appetite, or bright sun on a glassy lake.
            FFishConditions Conditions;
            const bool bBright = GetConditions(Conditions) && LakeFish::IsBrightCalm(Conditions);
            const int32 Extra = (Activity < 0.6f) + (Activity < 0.35f) + bBright - (Activity > 1.2f);
            NibblesLeft = FMath::Max(0, Random.RandRange(S.MinNibbles, S.MaxNibbles) + Extra);
            NextThink = SimTime + Random.FRandRange(0.5f, 1.2f);
        }
        return;
    }
    if (State == EState::Nibble && SimTime >= NextThink)
    {
        NextThink = SimTime + Random.FRandRange(0.7f, 1.6f);
        const float Like = LakeFish::BaitInterest(Species, Angler->Bait, 0.5f);
        if (NibblesLeft > 0)
        {
            Angler->OnFishNibble(this);
            --NibblesLeft;
            // A fish that does not like the bait much often gives up after tasting it.
            if (Random.FRand() > FMath::Min(1.0f, (0.55f + Like * 0.45f) * FMath::Lerp(0.75f, 1.1f, FMath::Clamp(Activity, 0.0f, 1.0f))))
            {
                Angler->ReleaseBaitClaim(this);
                IgnoreBaitUntil.Add(Angler, SimTime + 30);
                State = EState::Roam;
                ChooseRoamTarget();
            }
            return;
        }
        Angler->OnFishTake(this);
        State = EState::Holding;
        StateTime = 0;
        // Swim off with the bait: away from the angler, slowly.
        const FVector Away = (GetActorLocation() - Angler->GetActorLocation()).GetSafeNormal2D();
        RoamTarget = GetActorLocation() + (Away + FVector(Random.FRandRange(-0.6f, 0.6f), Random.FRandRange(-0.6f, 0.6f), 0)).GetSafeNormal() * 300;
        RoamTarget.Z = GetActorLocation().Z;
    }
}

// --- interaction ---------------------------------------------------------------------------

void ALakeFish::Spook(FVector From, float Seconds)
{
    if (State == EState::Hooked) return;
    if (ALakeCharacter* Angler = TargetAngler.Get()) Angler->ReleaseBaitClaim(this);
    TargetAngler = nullptr;
    State = EState::Flee;
    FleeFrom = From;
    FleeTime = Seconds;
    BurstTime = FMath::Min(Seconds, 1.2f);
}

void ALakeFish::Hook(ALakeCharacter* Angler)
{
    State = EState::Hooked;
    StateTime = 0;
    TargetAngler = Angler;
    BurstTime = 2.0f;   // the first run after the strike
}

void ALakeFish::ApplyLineForce(FVector Force) { PendingLineForce = Force; }

void ALakeFish::Release(float SpookSeconds)
{
    const FVector From = TargetAngler.IsValid() ? TargetAngler->GetActorLocation() : GetActorLocation();
    if (TargetAngler.IsValid()) IgnoreBaitUntil.Add(TargetAngler, SimTime + 60);
    State = EState::Roam;
    PendingLineForce = FVector::ZeroVector;
    Hunger *= 0.5f;
    Spook(From, SpookSeconds);
}

// --- motion --------------------------------------------------------------------------------

void ALakeFish::Move(FVector Desired, float MaxSpeed, float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    Desired = Desired.GetClampedToMaxSize(MaxSpeed);
    FVector Heading = GetActorForwardVector();
    const bool bBurst = BurstTime > 0;
    const float TurnRate = S.TurnRate * (bBurst ? 1.8f : 1.0f) * (State == EState::Hooked ? 0.8f : 1.0f);
    // Turn the body towards the desired direction, limited by the species' turn rate.
    // Fish keep their back up: pitch is limited, roll is not simulated.
    if (Desired.SizeSquared() > 4)
    {
        FVector Want = Desired.GetSafeNormal();
        Want.Z = FMath::Clamp(Want.Z, -0.5f, 0.5f);
        Want.Normalize();
        const FQuat From = Heading.ToOrientationQuat(), To = Want.ToOrientationQuat();
        const float Angle = FMath::RadiansToDegrees(From.AngularDistance(To));
        const float Step = Angle > KINDA_SMALL_NUMBER ? FMath::Min(1.0f, TurnRate * DeltaSeconds / Angle) : 1.0f;
        Heading = FQuat::Slerp(From, To, Step).GetForwardVector();
    }
    // Thrust only along the body axis, like a real tail; reverse thrust is weak.
    float Accel = S.Acceleration * (bBurst ? 2.5f : 1.0f);
    if (State == EState::Hooked) Accel *= S.Strength * FMath::Lerp(0.25f, 1.0f, StaminaFraction());
    const float ForwardSpeed = FVector::DotProduct(Vel, Heading);
    const float TargetSpeed = Desired.Size() * FMath::Max(0.0f, FVector::DotProduct(Heading, Desired.GetSafeNormal()));
    const float Thrust = FMath::Clamp((TargetSpeed - ForwardSpeed) * 3.0f, -Accel * 0.3f, Accel);
    Effort = FMath::Clamp(FMath::Abs(Thrust) / FMath::Max(1.0f, S.Acceleration * 2.5f) + ForwardSpeed / S.BurstSpeed * 0.5f, 0.0f, 1.0f);

    Vel += Heading * Thrust * DeltaSeconds;
    // External pull from the fishing line, divided by the fish's mass.
    Vel += PendingLineForce / FMath::Max(0.03f, WeightKg) * DeltaSeconds;
    PendingLineForce = FVector::ZeroVector;
    // Water drag: low along the streamlined body, high sideways and vertically.
    const FVector Forward = Heading * FVector::DotProduct(Vel, Heading);
    const FVector Side = Vel - Forward;
    const float ForwardDrag = 1.2f / S.BurstSpeed * Forward.Size();
    Vel = Forward / (1 + ForwardDrag * DeltaSeconds) + Side / (1 + 5.0f * DeltaSeconds);

    // Integrate and keep the fish inside the water volume.
    FVector P = GetActorLocation() + Vel * DeltaSeconds;
    const float Bottom = BottomZ(P.X, P.Y);
    const float Top = LakeFish::SurfaceZ - 4;
    if (P.Z > Top) { P.Z = Top; Vel.Z = FMath::Min(0.0, Vel.Z); }
    if (P.Z < Bottom + 4) { P.Z = Bottom + 4; Vel.Z = FMath::Max(0.0, Vel.Z); }
    if (!LakeMap::IsWater(P.X, P.Y) || LakeFish::SurfaceZ - Bottom < 10)
    {
        // Hit the shore: stop the motion towards it.
        const FVector In(LakeMap::DeeperDirection(P.X, P.Y), 0);
        P = GetActorLocation();
        Vel -= In * FMath::Min(0.0, FVector::DotProduct(Vel, In)) * 1.0;
    }
    SetActorLocationAndRotation(P, Heading.Rotation());
}

void ALakeFish::Simulate(float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    SimTime += DeltaSeconds;
    StateTime += DeltaSeconds;
    BurstTime = FMath::Max(0.0f, BurstTime - DeltaSeconds);
    UpdateBait(DeltaSeconds);

    FVector Desired = FVector::ZeroVector;
    float MaxSpeed = S.CruiseSpeed;
    ALakeCharacter* Angler = TargetAngler.Get();
    switch (State)
    {
    case EState::Roam:
        if (FVector::Dist(GetActorLocation(), RoamTarget) < 60 || StateTime > (S.Habitat.bAmbush ? 70 : 20))
        {
            ChooseRoamTarget();
            StateTime = 0;
        }
        Desired = SteerRoam() + SteerSchool();
        Hunger = FMath::Min(1.0f, Hunger + DeltaSeconds * 0.004f * Activity);
        break;
    case EState::Investigate:
    {
        // Approach slowly, ending with the mouth at the bait.
        const FVector To = Angler->BaitLocation() - MouthLocation();
        Desired = To.GetSafeNormal() * FMath::Min(S.CruiseSpeed * 0.8f, To.Size() * 0.8f + 4);
        // Pike close the last metre in one strike.
        if (S.Habitat.bAmbush && To.Size() < 120) { BurstTime = 0.4f; Desired = To.GetSafeNormal() * S.BurstSpeed; MaxSpeed = S.BurstSpeed; }
        break;
    }
    case EState::Nibble:
    {
        // Hover with the mouth at the bait, tiny pecks forward.
        const FVector To = Angler->BaitLocation() - MouthLocation();
        Desired = To * 1.5f + GetActorForwardVector() * 6 * FMath::Sin(SimTime * 9);
        MaxSpeed = 20;
        break;
    }
    case EState::Holding:
        Desired = (RoamTarget - GetActorLocation()).GetSafeNormal() * S.CruiseSpeed * 0.6f;
        break;
    case EState::Hooked:
    {
        if (!Angler) { State = EState::Roam; break; }
        // The fish "leads": runs along the bank, away from the angler or down to the bottom, with
        // bursts while it is fresh (longer for a heavy fish). A tired fish comes up and gives in.
        const float Fresh = StaminaFraction();
        const float Size01 = FMath::Clamp((WeightKg - S.MinWeight) / (S.MaxWeight - S.MinWeight), 0.0f, 1.0f);
        if (StateTime > NextRun)
        {
            StateTime = 0;
            NextRun = Random.FRandRange(1.5f, 3.5f) * FMath::Lerp(1.5f, 1.0f, Fresh);
            const FVector Away = (GetActorLocation() - Angler->GetActorLocation()).GetSafeNormal2D();
            const FVector Along = FVector::CrossProduct(Away, FVector::UpVector) * (Random.FRand() < 0.5f ? 1 : -1);
            const float Pick = Random.FRand();
            const FVector Dir = Pick < 0.45f ? (Along + Away * 0.3f).GetSafeNormal() : (Away + Along * Random.FRandRange(-0.5f, 0.5f)).GetSafeNormal();
            RoamTarget = GetActorLocation() + Dir * Random.FRandRange(300, 700) * (0.4f + Fresh);
            const float Bottom = BottomZ(RoamTarget.X, RoamTarget.Y);
            RoamTarget.Z = Fresh < 0.3f ? LakeFish::SurfaceZ - 12                                 // tired: up to the surface
                         : Pick > 0.75f ? Bottom + 10                                                 // dive
                         : FMath::Lerp(LakeFish::SurfaceZ - 10, Bottom + 8, S.DepthMax);
            if (Random.FRand() < 0.55f * Fresh) BurstTime = (Species == EFishSpecies::Pike ? 1.2f : 0.5f) + 1.5f * Size01;
        }
        Desired = (RoamTarget - GetActorLocation()).GetSafeNormal() * S.BurstSpeed;
        MaxSpeed = FMath::Lerp(S.CruiseSpeed * 0.5f, S.BurstSpeed, Fresh);
        // Pulling against the line costs stamina; with a slack line the fish gets its breath back.
        Stamina = FMath::Clamp(Stamina - DeltaSeconds * (0.15f + Effort * 0.9f) + (Effort < 0.2f ? DeltaSeconds * 0.12f : 0.0f), 0.0f, MaxStamina);
        break;
    }
    case EState::Flee:
        FleeTime -= DeltaSeconds;
        Desired = (GetActorLocation() - FleeFrom).GetSafeNormal() * S.BurstSpeed + SteerSchool();
        MaxSpeed = S.BurstSpeed;
        if (FleeTime <= 0) { State = EState::Roam; ChooseRoamTarget(); StateTime = 0; }
        break;
    }
    if (State == EState::Flee || State == EState::Hooked) MaxSpeed = FMath::Max(MaxSpeed, S.CruiseSpeed);
    if (BurstTime > 0) MaxSpeed = S.BurstSpeed;
    Desired += SteerBounds();
    Move(Desired, MaxSpeed, DeltaSeconds);
}

// --- per-frame -----------------------------------------------------------------------------

void ALakeFish::UpdateVisual(float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    if (!Body->GetStaticMesh())
    {
        Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, S.Mesh));
        // The mesh's snout is at X = 0 and the tail towards +X: turn it to face the actor's forward
        // and put the centre of mass (about 40% back from the snout) on the actor's origin.
        Body->SetRelativeRotation(FRotator(0, 180, 0));
        Body->SetRelativeLocation(FVector(S.MeshLength * 0.4f, 0, 0));
        Body->SetCustomPrimitiveDataFloat(2, Phase);
        Body->SetCustomPrimitiveDataFloat(3, S.MeshLength);
    }
    SetActorScale3D(FVector(LakeFish::LengthCm(Species, WeightKg) / S.MeshLength));
    const float E = HasAuthority() ? Effort : NetEffort;
    Body->SetCustomPrimitiveDataFloat(0, S.TailBeat * (0.6f + 2.2f * E));
    Body->SetCustomPrimitiveDataFloat(1, 0.03f + 0.06f * E);
}

void ALakeFish::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority())
    {
        // Fixed sub-steps keep the swimming model stable at low frame rates.
        const int32 Steps = FMath::Clamp(FMath::CeilToInt(DeltaSeconds / 0.02f), 1, 5);
        for (int32 I = 0; I < Steps; ++I) Simulate(DeltaSeconds / Steps);
        NetLocation = GetActorLocation();
        NetRotation = GetActorRotation();
        NetEffort = Effort;
    }
    else
    {
        // Smooth between network updates.
        SetActorLocation(FMath::VInterpTo(GetActorLocation(), FVector(NetLocation), DeltaSeconds, 6));
        SetActorRotation(FMath::RInterpTo(GetActorRotation(), NetRotation, DeltaSeconds, 6));
    }
    if (GetNetMode() != NM_DedicatedServer) UpdateVisual(DeltaSeconds);
}
