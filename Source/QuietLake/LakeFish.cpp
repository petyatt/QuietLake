#include "LakeFish.h"
#include "LakeGame.h"
#include "LakeLife.h"
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
// light rain, heavy rain; ripple, strong wind; fog; sensitivity to fronts. Then where each species
// feeds and where it rests (same habitat columns) - see the daily life in ALakeLife. Following anglers' lore:
//  perch  - by day, best morning and evening, steady weather and a light ripple, sulks in a downpour;
//  roach  - the dawns, warm settled weather, bites in a light rain, dislikes strong wind;
//  crucian- warmth: dawns, evenings and warm nights, calm, drizzle and mist; very sensitive to fronts;
//  pike   - "pike weather": grey sky, a breeze, drizzle; poor on bright calm days, idle at night.
// ---------------------------------------------------------------------------------------------
namespace
{
    const FFishSpeciesInfo GSpecies[] = {
        // Name           Mesh                                        Len  K     Wmin  Wmax  Habitat                                                    Column       Cruise Burst Turn  Acc  School Sense  Worm Maggot Bread  Nib    Str  Stam  Beat
        { TEXT("Окунь"),  TEXT("/Game/Fish/SM_Fish_Perch.SM_Fish_Perch"),     26, 1.25f, 0.08f, 0.8f, {0.1f, 0.3f, 1.0f, 0.9f, 0.5f, 0.3f, 1.2f, 4.8f, false},  0.3f, 0.75f, 28, 160, 220, 380, 5, 650, {1.0f, 0.55f, 0.08f}, 0, 1, 1.0f, 9,  9 , {1.0f, 0.75f, 0.9f, 0.15f,  6, 18, 27,  0.1f,  1.0f, 0.5f,  1.15f, 0.6f,  0.85f, 0.8f},
          {0.4f, 1.0f, 0.8f, 0.4f, 0.1f, 0.2f, 0.6f, 3.5f, false}, {0.0f, 0.1f, 0.5f, 1.0f, 0.6f, 0.0f, 1.5f, 5.0f, false} },
        { TEXT("Плотва"), TEXT("/Game/Fish/SM_Fish_Roach.SM_Fish_Roach"),     22, 1.1f,  0.05f, 0.4f, {0.5f, 1.0f, 0.3f, 0.3f, 0.0f, 0.4f, 0.6f, 2.8f, false},  0.1f, 0.45f, 24, 130, 200, 320, 7, 450, {0.35f, 1.0f, 0.85f}, 1, 3, 0.7f, 6, 10 , {1.0f, 0.55f, 0.95f, 0.3f,  8, 19, 26,  0.2f,  1.2f, 0.6f,  1.1f, 0.5f,  1.0f, 0.7f},
          {0.8f, 1.0f, 0.2f, 0.1f, 0.0f, 0.3f, 0.4f, 2.2f, false}, {0.1f, 0.3f, 0.6f, 0.2f, 0.3f, 1.0f, 1.2f, 3.5f, false} },
        { TEXT("Карась"), TEXT("/Game/Fish/SM_Fish_Crucian.SM_Fish_Crucian"), 20, 1.9f,  0.15f, 0.9f, {1.0f, 0.4f, 0.0f, 0.2f, 0.0f, 0.0f, 0.35f, 1.8f, false}, 0.75f, 1.0f, 12,  80, 110, 160, 1, 350, {0.55f, 0.7f, 1.0f},  3, 6, 0.9f, 14, 6 , {1.0f, 0.4f, 0.9f, 0.7f,  12, 23, 29,  0.3f,  1.25f, 0.5f,  0.85f, 0.35f,  1.2f, 1.0f},
          {1.0f, 0.5f, 0.0f, 0.1f, 0.0f, 0.0f, 0.3f, 1.6f, false}, {0.8f, 0.3f, 0.0f, 0.6f, 0.0f, 0.0f, 0.6f, 2.2f, false} },
        { TEXT("Щука"),   TEXT("/Game/Fish/SM_Fish_Pike.SM_Fish_Pike"),       62, 0.65f, 0.8f,  3.5f, {0.6f, 1.0f, 0.4f, 1.0f, 0.1f, 0.1f, 0.6f, 2.6f, true},   0.3f, 0.7f,  10, 420, 120, 900, 1, 800, {0.18f, 0.05f, 0.0f}, 0, 0, 1.6f, 12, 7 , {0.95f, 0.6f, 0.9f, 0.12f,  5, 17, 27,  0.6f,  1.3f, 0.7f,  1.3f, 0.75f,  0.8f, 0.8f},
          {0.5f, 1.0f, 0.3f, 1.0f, 0.0f, 0.1f, 0.5f, 2.2f, true}, {0.1f, 0.2f, 0.6f, 0.7f, 0.7f, 0.0f, 1.5f, 4.0f, true} },
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
const FFishHabitat& LakeFish::HabitatFor(EFishSpecies Species, EFishGoal Goal)
{
    const FFishSpeciesInfo& S = Info(Species);
    return Goal == EFishGoal::Feed ? S.FeedHabitat : Goal == EFishGoal::Rest ? S.RestHabitat : S.Habitat;
}
const TCHAR* LakeFish::GoalName(EFishSpecies Species, EFishGoal Goal)
{
    const bool bPredator = Species == EFishSpecies::Perch || Species == EFishSpecies::Pike;
    return Goal == EFishGoal::Feed ? (bPredator ? TEXT("охотятся") : TEXT("кормятся")) : Goal == EFishGoal::Rest ? TEXT("отдыхают") : TEXT("гуляют");
}
namespace
{
    float ScoreHabitat(const FFishHabitat& H, EFishSpecies Species, float X, float Y, const FFishConditions* Conditions);
}
float LakeFish::HabitatScore(EFishSpecies Species, float X, float Y, const FFishConditions* Conditions)
{
    return ScoreHabitat(Info(Species).Habitat, Species, X, Y, Conditions);
}
namespace
{
float ScoreHabitat(const FFishHabitat& H, EFishSpecies Species, float X, float Y, const FFishConditions* Conditions)
{
    using namespace LakeFish;
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
}
FVector LakeFish::PickSpot(EFishSpecies Species, FRandomStream& Random, FVector2D Near, float Range, const FFishConditions* Conditions, EFishGoal Goal)
{
    const FFishHabitat& Habitat = HabitatFor(Species, Goal);
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
        const float Score = ScoreHabitat(Habitat, Species, P.X, P.Y, Conditions) * Random.FRandRange(0.6f, 1.0f);
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
    // Under the water a fish is lost from sight long before this.
    Body->LDMaxDrawDistance = 9000;
    Body->CachedMaxDrawDistance = 9000;
}

void ALakeFish::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ALakeFish, Species);
    DOREPLIFETIME(ALakeFish, WeightKg);
    DOREPLIFETIME(ALakeFish, NetLocation);
    DOREPLIFETIME(ALakeFish, NetRotation);
    DOREPLIFETIME(ALakeFish, NetEffort);
    DOREPLIFETIME(ALakeFish, Goal);
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
    Energy = Random.FRandRange(0.5f, 1.0f);
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    // Bigger fish of a species hold out longer.
    MaxStamina = S.Stamina * FMath::Lerp(0.6f, 1.8f, FMath::Clamp((WeightKg - S.MinWeight) / (S.MaxWeight - S.MinWeight), 0.0f, 1.0f));
    Stamina = MaxStamina;
    NetLocation = GetActorLocation();
    NetRotation = GetActorRotation();
    // Spread the first decisions so the pond does not change its mind all at once.
    GoalUntil = Random.FRandRange(2.0f, 25.0f);
    NextSign = Random.FRandRange(1.0f, 10.0f);
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

namespace
{
    /** Dawn or dusk: the sun low or just below the horizon. */
    bool IsTwilight(const FFishConditions& C) { return C.SunElevation > -9 && C.SunElevation < 12; }
}

const ALakeFish* ALakeFish::Leader() const
{
    if (School < 0) return this;
    const ALakeLife* Life = ALakeLife::Get(GetWorld());
    const ALakeFish* Lead = Life ? Life->SchoolLeader(School) : nullptr;
    return Lead ? Lead : this;
}

void ALakeFish::ChooseRoamTarget(bool bLocal)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    FFishConditions Conditions;
    const bool bWeather = GetConditions(Conditions);
    const FFishConditions* C = bWeather ? &Conditions : nullptr;
    // School members go where their leader goes, each a little to the side.
    const ALakeFish* Lead = Leader();
    if (Lead != this)
    {
        RoamTarget = Lead->RoamTarget + FVector(Random.FRandRange(-130, 130), Random.FRandRange(-130, 130), Random.FRandRange(-15, 15));
        RoamTarget.Z = FMath::Clamp(RoamTarget.Z, BottomZ(RoamTarget.X, RoamTarget.Y) + 8, LakeFish::SurfaceZ - 10);
        if (!LakeMap::IsWater(RoamTarget.X, RoamTarget.Y)) RoamTarget = Lead->RoamTarget;
        return;
    }
    const bool bHomeBound = S.SchoolSize > 1 || S.Habitat.bAmbush;
    if (Goal == EFishGoal::Cruise)
    {
        // Schools and ambushers keep to their home area; loners wander through good water nearby.
        // Now and then a home moves to better water for the conditions (deeper in the heat, to the
        // bank the wind blows onto), so schools drift with the weather.
        if (bHomeBound && bWeather && Random.FRand() < 0.08f) HomePoint = LakeFish::PickSpot(Species, Random, FVector2D(HomePoint), 2500, C);
        const float Range = S.Habitat.bAmbush ? 350.0f : S.SchoolSize > 1 ? 900.0f : 2500.0f;
        RoamTarget = LakeFish::PickSpot(Species, Random, FVector2D(bHomeBound ? HomePoint : GetActorLocation()), Range, C);
        return;
    }
    // Hungry perch go after the nearest fry.
    PreyFry = -1;
    if (Goal == EFishGoal::Feed && Species == EFishSpecies::Perch)
        if (const ALakeLife* Life = ALakeLife::Get(GetWorld()))
        {
            PreyFry = Life->NearestFry(GetActorLocation(), 4500);
            if (PreyFry >= 0) { RoamTarget = Life->FryLocation(PreyFry) - FVector(0, 0, 20); return; }
        }
    // Feeding and resting places: near home for schools and ambushers, anywhere near for loners;
    // once there, a fish browses along a few metres at a time.
    const float Range = bLocal ? (Species == EFishSpecies::Crucian ? 250.0f : 500.0f) : bHomeBound ? 1500.0f : 1200.0f;
    const FVector2D Near = bLocal || !bHomeBound ? FVector2D(GetActorLocation()) : FVector2D(HomePoint);
    RoamTarget = LakeFish::PickSpot(Species, Random, Near, Range, C, Goal);
    const float Bottom = BottomZ(RoamTarget.X, RoamTarget.Y);
    auto Column = [&](float Fraction) { return FMath::Lerp(LakeFish::SurfaceZ - 10, Bottom + 8, Fraction); };
    if (Goal == EFishGoal::Feed)
    {
        // Crucian root in the silt; roach sip insects off the top on calm dawns and evenings.
        if (Species == EFishSpecies::Crucian) RoamTarget.Z = Bottom + 7;
        else if (Species == EFishSpecies::Roach && bWeather && IsTwilight(Conditions) && Conditions.Wind < 0.5f && Conditions.Rain < 0.6f && Random.FRand() < 0.65f)
            RoamTarget.Z = LakeFish::SurfaceZ - 12;
        else if (Species == EFishSpecies::Perch) RoamTarget.Z = Column(Random.FRandRange(0.6f, 0.95f));
    }
    else
        RoamTarget.Z = Column(Species == EFishSpecies::Crucian ? 0.75f : Species == EFishSpecies::Roach ? 0.5f : 0.8f);
}

void ALakeFish::SetGoal(EFishGoal NewGoal, float Seconds)
{
    GoalUntil = SimTime + Seconds;
    if (NewGoal == Goal) return;
    Goal = NewGoal;
    bAtSpot = false;
    StrikeEnd = -1;
    Prey = nullptr;
    StateTime = 0;
    ChooseRoamTarget();
}

void ALakeFish::Decide()
{
    // School members follow their leader's choice.
    const ALakeFish* Lead = Leader();
    if (Lead != this)
    {
        if (Lead->Goal != Goal) { Goal = Lead->Goal; bAtSpot = false; ChooseRoamTarget(); }
        GoalUntil = SimTime + Random.FRandRange(3.0f, 6.0f);
        return;
    }
    // Hunger and the daily rhythm (Activity: the time of day and the weather for this species)
    // call for food; tiredness and a sluggish hour (heat, cold, the wrong time of day) for rest;
    // otherwise the fish patrols its water. A little chance and some stickiness keep it natural.
    const float A = FMath::Clamp(Activity, 0.0f, 1.3f);
    float Score[(int32)EFishGoal::Count];
    Score[(int32)EFishGoal::Feed] = FMath::Pow(Hunger, 1.3f) * A * 1.4f;
    Score[(int32)EFishGoal::Rest] = (1 - Energy) * 1.4f + FMath::Max(0.0f, 0.55f - A) * 1.5f;
    Score[(int32)EFishGoal::Cruise] = 0.3f + 0.15f * Energy;
    Score[(int32)Goal] *= 1.15f;
    int32 Best = 0;
    for (int32 G = 0; G < (int32)EFishGoal::Count; ++G)
    {
        Score[G] *= Random.FRandRange(0.85f, 1.15f);
        if (Score[G] > Score[Best]) Best = G;
    }
    const EFishGoal Pick = (EFishGoal)Best;
    SetGoal(Pick, Pick == EFishGoal::Feed ? Random.FRandRange(60, 160) : Pick == EFishGoal::Rest ? Random.FRandRange(45, 120) : Random.FRandRange(25, 70));
}

void ALakeFish::SetNearPlayers(bool bInNear)
{
    if (bNear == bInNear) return;
    bNear = bInNear;
    // Far from everyone: a few coarse steps a second and rare network updates are plenty.
    SetActorTickInterval(bNear ? 0.0f : 0.2f);
    SetNetUpdateFrequency(bNear ? 12.0f : 2.0f);
}

void ALakeFish::Dash(FVector Target, float Seconds)
{
    if (State != EState::Roam) return;
    BurstTime = FMath::Max(BurstTime, Seconds);
    RoamTarget = Target;
}

void ALakeFish::Eat(float Meal)
{
    Hunger = FMath::Max(0.0f, Hunger - Meal);
}

FVector ALakeFish::SteerRoam() const
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FVector To = RoamTarget - GetActorLocation();
    // Pike hang almost still at their ambush spot; others cruise and slow down on arrival.
    const float Lively = FMath::Lerp(0.45f, 1.0f, FMath::Clamp(Activity, 0.0f, 1.0f));
    // An ambusher moving to a new lie swims steadily and glides the last metre.
    float Speed = S.Habitat.bAmbush ? FMath::Min(FMath::Max(S.CruiseSpeed, 30.0f), To.Size() * 0.25f)
                                    : S.CruiseSpeed * FMath::Clamp(To.Size() / 120.0f, 0.3f, 1.0f) * Lively;
    // Resting fish barely move; browsing ones potter about.
    if (Goal == EFishGoal::Rest && bAtSpot) Speed *= 0.3f;
    else if (Goal == EFishGoal::Feed && bAtSpot && Species != EFishSpecies::Perch) Speed *= 0.55f;
    else if (Goal != EFishGoal::Cruise && !bAtSpot && !S.Habitat.bAmbush) Speed *= 1.5f;   // on its way somewhere
    return To.GetSafeNormal() * Speed;
}

FVector ALakeFish::SteerSchool() const
{
    if (School < 0) return FVector::ZeroVector;
    FVector Centre = FVector::ZeroVector, Heading = FVector::ZeroVector, Separation = FVector::ZeroVector;
    int32 Count = 0;
    auto Visit = [&](const ALakeFish* Other)
    {
        if (Other == this) return;
        const FVector Offset = GetActorLocation() - Other->GetActorLocation();
        const float Dist = Offset.Size();
        if (Dist < 35 && Dist > 0.1f) Separation += Offset / (Dist * Dist) * 35;   // any species
        if (Other->School != School || Dist > 260) return;
        Centre += Other->GetActorLocation();
        Heading += Other->Vel;
        ++Count;
    };
    if (const ALakeLife* Life = ALakeLife::Get(GetWorld())) Life->ForEachFishNear(GetActorLocation(), 260, [&](ALakeFish* Other) { Visit(Other); });
    else for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It) Visit(*It);
    if (Count == 0) return Separation * 20;
    Centre /= Count;
    Heading /= Count;
    // A resting school hangs loosely; a hunting one keeps tight.
    const float Pull = Goal == EFishGoal::Rest ? 0.15f : 0.25f;
    return (Centre - GetActorLocation()) * Pull + Heading * 0.45f + Separation * 20;
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

// --- daily life ------------------------------------------------------------------------------

void ALakeFish::Live(float DeltaSeconds, FVector& Desired, float& MaxSpeed)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    ALakeLife* Life = ALakeLife::Get(GetWorld());
    // Needs: swimming tires, resting restores; hunger grows with the appetite of the hour.
    const float Recovery = Goal != EFishGoal::Rest ? 0.0f : bAtSpot ? 0.014f : 0.006f;   // already easing off on the way
    Energy = FMath::Clamp(Energy - DeltaSeconds * (0.0012f + 0.004f * Effort) + DeltaSeconds * Recovery, 0.0f, 1.0f);
    Hunger = FMath::Min(1.0f, Hunger + DeltaSeconds * 0.0035f * Activity);
    const float Dist = FVector::Dist(GetActorLocation(), RoamTarget);
    if (Dist < 90) bAtSpot = true;
    if (Goal == EFishGoal::Feed && bAtSpot)
    {
        // Grazers find food where they browse; perch without fry pick at the bottom.
        if (Species == EFishSpecies::Crucian || Species == EFishSpecies::Roach) Hunger -= DeltaSeconds * 0.012f;
        else if (Species == EFishSpecies::Perch && PreyFry < 0) Hunger -= DeltaSeconds * 0.005f;
        Hunger = FMath::Max(0.0f, Hunger);
    }
    if (SimTime >= GoalUntil || (Goal == EFishGoal::Feed && Hunger < 0.1f) || (Goal == EFishGoal::Rest && Energy > 0.97f)) Decide();
    else if (School >= 0)
    {
        // A school turns together: follow the leader's change at once.
        const ALakeFish* Lead = Leader();
        if (Lead != this && Lead->Goal != Goal) Decide();
    }

    // Perch drive the fry: follow the shoal, the leader leads each rush into it.
    if (Goal == EFishGoal::Feed && Species == EFishSpecies::Perch && PreyFry >= 0 && Life)
    {
        if (Leader() == this)
        {
            RoamTarget = Life->FryLocation(PreyFry) - FVector(0, 0, 20);
            if (FVector::Dist2D(GetActorLocation(), RoamTarget) < 320 && SimTime >= NextFryAttack)
            {
                NextFryAttack = SimTime + Random.FRandRange(1.0f, 2.4f);
                Life->AttackFry(PreyFry, this);
            }
        }
    }
    else if (Dist < 60 || StateTime > (S.Habitat.bAmbush ? 70 : 25) || (School >= 0 && StateTime > 3 && Leader() != this))
    {
        ChooseRoamTarget(bAtSpot && Goal != EFishGoal::Cruise);
        StateTime = 0;
    }
    Desired = SteerRoam() + SteerSchool();
    if (Goal == EFishGoal::Feed && Species == EFishSpecies::Pike) UpdatePikeAmbush(DeltaSeconds, Desired, MaxSpeed);
    if (!Life || SimTime < NextSign) return;

    // Signs of feeding on the water.
    const FVector P = GetActorLocation();
    const float Bottom = BottomZ(P.X, P.Y);
    const FVector Surface(P.X, P.Y, LakeFish::SurfaceZ);
    const float Pace = FMath::Clamp(Activity, 0.4f, 1.3f);
    if (Goal == EFishGoal::Feed && Species == EFishSpecies::Crucian && bAtSpot && P.Z < Bottom + 30 && LakeFish::SurfaceZ - Bottom < 300)
    {
        // Rooting in the silt sends up chains of bubbles.
        Life->EmitSign(ELakeSign::Bubbles, Surface, 0.4f + WeightKg * 0.8f, Species, WeightKg);
        NextSign = SimTime + Random.FRandRange(5.0f, 12.0f) / Pace;
    }
    else if (Goal == EFishGoal::Feed && Species == EFishSpecies::Roach && P.Z > LakeFish::SurfaceZ - 30)
    {
        // Sipping an insect off the top: a small ring.
        Life->EmitSign(ELakeSign::Rise, Surface, 0.3f + WeightKg * 1.5f, Species, WeightKg);
        NextSign = SimTime + Random.FRandRange(9.0f, 25.0f) / Pace;
    }
    else NextSign = SimTime + Random.FRandRange(2.0f, 5.0f);
}

void ALakeFish::UpdatePikeAmbush(float DeltaSeconds, FVector& Desired, float& MaxSpeed)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    ALakeLife* Life = ALakeLife::Get(GetWorld());
    if (!Life) return;
    if (StrikeEnd > 0)
    {
        // The strike: a short explosive dash at the prey.
        ALakeFish* Target = Prey.Get();
        const FVector At = Target ? Target->GetActorLocation() : Life->FryLocation(PreyFry);
        const FVector To = At - MouthLocation();
        Desired = To.GetSafeNormal() * S.BurstSpeed;
        MaxSpeed = S.BurstSpeed;
        BurstTime = FMath::Max(BurstTime, 0.1f);
        if (To.Size() > 35 && SimTime < StrikeEnd) return;
        // Resolved: caught or missed. Near the top it breaks the surface, a little deeper it shows as a swirl.
        const float Depth = LakeFish::SurfaceZ - At.Z;
        const FVector Surface(At.X, At.Y, LakeFish::SurfaceZ);
        const float Size = FMath::Min(0.6f + 0.3f * WeightKg, 1.8f);
        if (Depth < 200) Life->EmitSign(Depth < 90 ? ELakeSign::Splash : ELakeSign::Swirl, Surface, Size, Species, WeightKg);
        if (Target)
        {
            const bool bCaught = To.Size() < 60 && Target->IsFree() && Random.FRand() < 0.3f + 0.35f * Hunger;
            if (bCaught) { Life->Devour(Target, this); Eat(0.9f); }
            else
            {
                Target->Spook(GetActorLocation(), 4);
                Life->Alarm(Target, 450);
                // A fish that escapes near the top often leaps clear.
                if (Depth < 90 && Random.FRand() < 0.35f)
                {
                    const FVector Away = (At - GetActorLocation()).GetSafeNormal2D();
                    Life->EmitSign(ELakeSign::Jump, Surface, 1, Target->Species, Target->WeightKg, FMath::RadiansToDegrees(FMath::Atan2(Away.Y, Away.X)));
                }
            }
        }
        else if (PreyFry >= 0) Life->AttackFry(PreyFry, this);
        StrikeEnd = -1;
        Prey = nullptr;
        PreyFry = -1;
        NextLook = SimTime + Random.FRandRange(4.0f, 10.0f);
        return;
    }
    // Waiting in ambush: a fish or fry passing in front of the snout is struck.
    if (!bAtSpot || SimTime < NextLook || Hunger < 0.3f) return;
    NextLook = SimTime + 0.4f;
    const FVector Mouth = MouthLocation(), Forward = GetActorForwardVector();
    ALakeFish* Best = nullptr;
    float BestDist = 260;
    Life->ForEachFishNear(Mouth, 260, [&](ALakeFish* Other)
    {
        if (Other == this || Other->Species == EFishSpecies::Pike || !Other->IsFree() || Other->WeightKg > WeightKg * 0.3f) return;
        const FVector To = Other->GetActorLocation() - Mouth;
        const float Dist = To.Size();
        if (Dist < BestDist && FVector::DotProduct(Forward, To / FMath::Max(1.0f, Dist)) > 0.2f) { Best = Other; BestDist = Dist; }
    });
    const int32 Fry = Best ? -1 : Life->NearestFry(Mouth, 350);
    if (!Best && Fry < 0) return;
    if (Random.FRand() > 0.35f * Hunger * FMath::Clamp(Activity, 0.3f, 1.3f)) return;
    Prey = Best;
    PreyFry = Fry;
    StrikeEnd = SimTime + 0.8f;
    BurstTime = 0.8f;
    Life->CountPikeStrike();
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
    if (State == EState::Roam && SimTime >= NextThink && StrikeEnd < 0)
    {
        NextThink = SimTime + Random.FRandRange(0.6f, 1.4f);
        // A feeding fish is quick to find bait, a resting one rarely bothers.
        const float Mind = Goal == EFishGoal::Feed ? 1.4f : Goal == EFishGoal::Rest ? 0.3f : 0.85f;
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
                * Hunger * Activity * Mind * FMath::Sqrt(1 - Dist / S.SenseRadius);
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
    StrikeEnd = -1;
    Prey = nullptr;
}

void ALakeFish::Hook(ALakeCharacter* Angler)
{
    State = EState::Hooked;
    StateTime = 0;
    TargetAngler = Angler;
    BurstTime = 2.0f;   // the first run after the strike
    StrikeEnd = -1;
    // The thrashing scares the school mates nearby.
    if (ALakeLife* Life = ALakeLife::Get(GetWorld())) Life->Alarm(this, 600);
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
        Live(DeltaSeconds, Desired, MaxSpeed);
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
        // Thrashing at the top: splashes the angler can see and hear.
        if (GetActorLocation().Z > LakeFish::SurfaceZ - 30 && Effort > 0.35f && SimTime >= NextSign)
            if (ALakeLife* Life = ALakeLife::Get(GetWorld()))
            {
                const FVector P = GetActorLocation();
                Life->EmitSign(ELakeSign::Splash, FVector(P.X, P.Y, LakeFish::SurfaceZ), FMath::Min(0.35f + WeightKg * 0.4f, 1.4f), Species, WeightKg, 0, true);
                NextSign = SimTime + Random.FRandRange(0.9f, 2.2f);
            }
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
        Body->SetCustomPrimitiveDataFloat(3, S.MeshLength);
    }
    const FVector Scale(LakeFish::LengthCm(Species, WeightKg) / S.MeshLength);
    if (!GetActorScale3D().Equals(Scale, 0.001f)) SetActorScale3D(Scale);
    // The material bends the body by sin(u - Time * Rate + Phase). Every change of these values
    // rebuilds the fish's render state, so they change only in steps of effort, and the phase is
    // shifted so the tail carries on smoothly at the new rate.
    const float E = FMath::RoundToFloat((HasAuthority() ? Effort : NetEffort) * 8) / 8;
    const float Beat = S.TailBeat * (0.6f + 2.2f * E), Amp = 0.03f + 0.06f * E;
    if (Beat != LastBeat)
    {
        const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
        if (LastBeat >= 0) Phase = (float)FMath::Fmod(Phase + Now * (Beat - LastBeat), 2.0 * PI);
        LastBeat = Beat;
        Body->SetCustomPrimitiveDataFloat(0, Beat);
        Body->SetCustomPrimitiveDataFloat(2, Phase);
    }
    if (Amp != LastAmp) { LastAmp = Amp; Body->SetCustomPrimitiveDataFloat(1, Amp); }
}

void ALakeFish::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority())
    {
        // Fixed sub-steps keep the swimming model stable at low frame rates; far from every
        // player a few coarse steps are enough.
        const float StepLength = bNear ? 0.02f : 0.1f;
        const int32 Steps = FMath::Clamp(FMath::CeilToInt(DeltaSeconds / StepLength), 1, bNear ? 5 : 3);
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
