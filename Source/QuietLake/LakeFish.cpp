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
    // How each species moves (see FFishGait):
    //  perch  - nimble, swims in kicks and glides, a loose school about a body and a half apart;
    //  roach  - steady and smooth, a tight school that turns as one;
    //  crucian- slow and deliberate, wide lazy meanders, turns with its fins at the bottom;
    //  pike   - long still waits, slow gliding turns, but the strongest burst (see Acceleration).
    const FFishGait GGaits[] = {
        // Hover  Radius TurnAcc Resp  Wander Hz     Kick  Period Spacing
        {  45,    0.6f,  420,    0.3f, 22,    0.12f, 0.45f, 1.6f, 1.4f },
        {  50,    0.5f,  480,    0.25f,18,    0.10f, 1.0f,  1.0f, 1.2f },
        {  30,    0.5f,  200,    0.6f, 30,    0.07f, 1.0f,  1.0f, 2.0f },
        {  20,    1.0f,  160,    0.5f, 10,    0.05f, 1.0f,  1.0f, 3.0f },
    };
    const TCHAR* GBaitNames[] = { TEXT("червь"), TEXT("опарыш"), TEXT("хлеб") };

    float Depth01(float Z, float X, float Y)
    {
        const float Bottom = ALakeFish::BottomZ(X, Y);
        return FMath::Clamp((LakeFish::SurfaceZ - Z) / FMath::Max(1.0f, LakeFish::SurfaceZ - Bottom), 0.0f, 1.0f);
    }
}

const FFishSpeciesInfo& LakeFish::Info(EFishSpecies Species) { return GSpecies[FMath::Clamp((int32)Species, 0, 3)]; }
const FFishGait& LakeFish::Gait(EFishSpecies Species) { return GGaits[FMath::Clamp((int32)Species, 0, 3)]; }
const TCHAR* LakeFish::BaitName(EBait Bait) { return GBaitNames[FMath::Clamp((int32)Bait, 0, 2)]; }
float LakeFish::Size01(EFishSpecies Species, float WeightKg)
{
    const FFishSpeciesInfo& S = Info(Species);
    return FMath::Clamp((WeightKg - S.MinWeight) / FMath::Max(0.01f, S.MaxWeight - S.MinWeight), 0.0f, 1.0f);
}
float LakeFish::Girth(EFishSpecies Species, float WeightKg)
{
    // Big fish are deeper and broader for their length (a big crucian is almost round), and no two
    // fish are quite alike: a little individual variation from the weight's digits, the same on
    // every machine.
    const float Own = FMath::Frac(WeightKg * 1000.0f * 0.6180339f) - 0.5f;
    const float Growth = Species == EFishSpecies::Crucian ? 0.18f : Species == EFishSpecies::Pike ? 0.1f : 0.14f;
    return 0.93f + Growth * Size01(Species, WeightKg) + 0.07f * Own;
}

// --- shown motion on clients -----------------------------------------------------------------

namespace
{
    // How far a client runs on with the last update before it stops guessing (s), and how quickly
    // the difference between what is shown and the update dies away (rad/s, critically damped).
    constexpr float PredictLocation = 0.6f, PredictTurn = 0.4f;
    constexpr float SettleLocation = 6.0f, SettleTurn = 8.0f;
    /** Time run on with the last update: one to one at first, easing to a stop after Limit. */
    float SoftAge(float Age, float Limit) { return Limit * (1 - FMath::Exp(-Age / Limit)); }
}

void FFishNetSmoothing::Receive(const FVector& InLocation, const FVector& InVelocity, const FRotator& InRotationRaw, double Now, double RealNow)
{
    // Replicated rotators arrive as 0..360: a pitch of -20 comes as 340.
    const FRotator InRotation = InRotationRaw.GetNormalized();
    if (RealNow < 0) RealNow = Now;
    // This machine stood still (a long frame, e.g. building the world after joining): the engine
    // clamps the game clock (0.4 s a frame), so the fish swam on for longer than the clock shows,
    // and what is shown is stale. Blending that out would race the fish across metres; start
    // afresh from the update instead.
    const bool bStalled = bReady && (RealNow - LastReceiveReal) - (Now - LastReceive) > 0.1;
    if (!bReady || bStalled)
    {
        Location = NetLocation = InLocation;
        Velocity = NetVelocity = InVelocity;
        Rotation = NetRotation = InRotation;
        NetYawRate = YawRate = 0;
        Offset = OffsetVelocity = FVector::ZeroVector;
        YawOffset = YawOffsetRate = 0;
        LastReceive = Now;
        LastReceiveReal = RealNow;
        bRestarted = bReady;
        bReady = true;
        return;
    }
    // Several properties of one update report it more than once.
    if (Now - LastReceive < 0.005 && InLocation == NetLocation && InRotation.Equals(NetRotation, 0.001f)) return;
    // The turn rate between two updates lets the heading run on smoothly until the next one.
    if (Now - LastReceive > 0.02)
        NetYawRate = FMath::Clamp(FMath::FindDeltaAngleDegrees(NetRotation.Yaw, InRotation.Yaw) / float(Now - LastReceive), -360.0f, 360.0f);
    NetLocation = InLocation;
    NetVelocity = InVelocity;
    NetRotation = InRotation;
    LastReceive = Now;
    LastReceiveReal = RealNow;
    // Keep what is shown exactly where it is and moving as it was: the difference from the new
    // update becomes an offset that springs back to zero. Nothing jumps when an update arrives.
    Offset = Location - NetLocation;
    OffsetVelocity = Velocity - NetVelocity;
    if (Offset.SizeSquared() > FMath::Square(400.0f)) { Offset = OffsetVelocity = FVector::ZeroVector; }   // too far off: jump
    YawOffset = FMath::FindDeltaAngleDegrees(NetRotation.Yaw, Rotation.Yaw);
    YawOffsetRate = YawRate - NetYawRate;
}

void FFishNetSmoothing::Advance(float Dt, double Now)
{
    if (!bReady || Dt <= 0) return;
    // The offsets settle like a critically damped spring: no overshoot, and the correction starts
    // gently (its rate is continuous), so the shown fish never jerks.
    const int32 Steps = FMath::Clamp(FMath::CeilToInt(Dt / 0.01f), 1, 20);
    const float H = Dt / Steps;
    for (int32 I = 0; I < Steps; ++I)
    {
        OffsetVelocity += (-2 * SettleLocation * OffsetVelocity - SettleLocation * SettleLocation * Offset) * H;
        Offset += OffsetVelocity * H;
        YawOffsetRate += (-2 * SettleTurn * YawOffsetRate - SettleTurn * SettleTurn * YawOffset) * H;
        YawOffset += YawOffsetRate * H;
    }
    // Where the fish is now: the last update run on with its velocity and turn, easing off if the
    // next one is late.
    const float Age = (float)FMath::Max(0.0, Now - LastReceive);
    Location = NetLocation + NetVelocity * SoftAge(Age, PredictLocation) + Offset;
    Velocity = NetVelocity * FMath::Exp(-Age / PredictLocation) + OffsetVelocity;
    Rotation.Yaw = FRotator::NormalizeAxis(NetRotation.Yaw + NetYawRate * SoftAge(Age, PredictTurn) + YawOffset);
    YawRate = NetYawRate * FMath::Exp(-Age / PredictTurn) + YawOffsetRate;
    const float Ease = 1 - FMath::Exp(-Dt * 5.0f);
    Rotation.Pitch = FRotator::NormalizeAxis(Rotation.Pitch + FMath::FindDeltaAngleDegrees(Rotation.Pitch, NetRotation.Pitch) * Ease);
    Rotation.Roll = FRotator::NormalizeAxis(Rotation.Roll + FMath::FindDeltaAngleDegrees(Rotation.Roll, NetRotation.Roll) * Ease);
}
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
TArray<FBiteStep> LakeFish::MakeBite(EFishSpecies Species, float Size01, float A, bool bOnBottom, bool bWary, FRandomStream& R)
{
    TArray<FBiteStep> Out;
    // Sluggish (poor appetite, bright calm water): more testing, and the fish lets go sooner.
    const float Lazy = FMath::Clamp(1.0f - A, 0.0f, 0.9f) + (bWary ? 0.3f : 0.0f);
    const float Pace = FMath::Lerp(1.1f, 0.65f, FMath::Clamp(Lazy, 0.0f, 1.0f));
    auto Add = [&Out](EBiteMotion Motion, float Duration, float Amount, bool bTake) { Out.Add({ Motion, Duration, Amount, bTake }); };
    auto Pause = [&](float Lo, float Hi) { Add(EBiteMotion::None, R.FRandRange(Lo, Hi), 0, false); };
    switch (Species)
    {
    case EFishSpecies::Crucian:
    {
        const int32 Trembles = R.RandRange(1, 3) + (Size01 < 0.3f ? R.RandRange(1, 3) : 0) + (Lazy > 0.4f ? 1 : 0);
        for (int32 I = 0; I < Trembles; ++I)
        {
            Add(EBiteMotion::Tremble, R.FRandRange(0.6f, 1.8f), R.FRandRange(0.3f, 0.9f) * (1 - 0.4f * Size01), false);
            Pause(0.8f, 3.5f);
        }
        if (bOnBottom && R.FRand() < 0.8f)
        {
            const float Lift = R.FRandRange(1.2f, 3.6f) * (0.6f + 0.6f * Size01);
            Add(EBiteMotion::Lift, R.FRandRange(1.2f, 2.4f), Lift, true);
            Add(EBiteMotion::Drift, R.FRandRange(1.5f, 3.0f) * Pace, R.FRandRange(3.0f, 8.0f), true);
        }
        else
        {
            Add(EBiteMotion::Peck, 0.35f, R.FRandRange(0.8f, 1.5f), false);
            Pause(0.4f, 1.2f);
            Add(EBiteMotion::Drift, R.FRandRange(2.0f, 3.2f) * Pace, R.FRandRange(6.0f, 12.0f), true);
        }
        break;
    }
    case EFishSpecies::Roach:
    {
        const int32 Pecks = R.RandRange(1, 3) + (Lazy > 0.4f ? 1 : 0);
        for (int32 I = 0; I < Pecks; ++I)
        {
            if (R.FRand() < 0.3f) Add(EBiteMotion::Tremble, R.FRandRange(0.4f, 0.8f), R.FRandRange(0.4f, 0.7f), false);
            Add(EBiteMotion::Peck, R.FRandRange(0.18f, 0.3f), R.FRandRange(0.8f, 2.0f), false);
            Pause(0.3f, 1.2f);
        }
        if (bOnBottom && R.FRand() < 0.25f)
        {
            Add(EBiteMotion::Lift, R.FRandRange(0.8f, 1.4f), R.FRandRange(1.0f, 2.0f), true);
            Add(EBiteMotion::Drift, R.FRandRange(1.0f, 1.6f) * Pace, R.FRandRange(5.0f, 9.0f), true);
        }
        else Add(EBiteMotion::Sink, R.FRandRange(1.2f, 2.0f) * Pace, R.FRandRange(20.0f, 35.0f), true);
        break;
    }
    case EFishSpecies::Perch:
    {
        const int32 Taps = R.RandRange(0, 2) + (Lazy > 0.5f ? 1 : 0);
        for (int32 I = 0; I < Taps; ++I)
        {
            Add(EBiteMotion::Peck, 0.2f, R.FRandRange(1.5f, 3.0f), false);
            Pause(0.4f, 1.5f);
        }
        Add(EBiteMotion::Sink, R.FRandRange(2.0f, 3.0f) * FMath::Max(Pace, 0.85f), R.FRandRange(45.0f, 70.0f), true);
        break;
    }
    default:
        Add(EBiteMotion::Peck, 0.3f, R.FRandRange(2.0f, 4.0f), false);
        Pause(0.5f, 2.0f);
        Add(EBiteMotion::Sink, R.FRandRange(1.3f, 1.8f), R.FRandRange(60.0f, 90.0f), true);
        break;
    }
    return Out;
}
const TCHAR* LakeFish::BiteText(EBiteMotion Motion, float Amount)
{
    switch (Motion)
    {
    case EBiteMotion::Tremble: return TEXT("Поплавок подрагивает…");
    case EBiteMotion::Peck: return TEXT("Поплавок коротко притопило…");
    case EBiteMotion::Lift: return Amount >= 2.5f ? TEXT("Поплавок поднимается и ложится на бок!") : TEXT("Поплавок поднимается!");
    case EBiteMotion::Drift: return TEXT("Поплавок медленно пошёл в сторону!");
    case EBiteMotion::Sink: return TEXT("Поплавок ушёл под воду!");
    default: return TEXT("");
    }
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
        const FVector2D Out = LakeMap::NearestShore(FVector2D(X, Y)).Outward;
        Downwind = 1 + 0.8f * Conditions->Wind * FMath::Max(0.0f, FVector2D::DotProduct(Out, Conditions->WindDir)) * (1 - FMath::SmoothStep(10.0f, 25.0f, SD));
    }
    return (0.03f + Like) * DepthFit * Downwind;
}
}
FVector LakeFish::PickSpot(EFishSpecies Species, FRandomStream& Random, FVector2D Near, float Range, const FFishConditions* Conditions, EFishGoal Goal)
{
    const FFishHabitat& Habitat = HabitatFor(Species, Goal);
    // Best of a few random candidates, with some chance so fish do not all crowd one spot.
    const FBox2D Lake = LakeMap::LakeBounds();
    const FFishSpeciesInfo& S = Info(Species);
    FVector2D Best = Range > 0 ? Near : LakeMap::Centre;
    float BestScore = -1;
    for (int32 Try = 0; Try < (Range > 0 ? 10 : 120); ++Try)
    {
        const FVector2D P = Range > 0 ? Near + FVector2D(Random.FRandRange(-Range, Range), Random.FRandRange(-Range, Range))
                                      : FVector2D(Random.FRandRange(Lake.Min.X, Lake.Max.X), Random.FRandRange(Lake.Min.Y, Lake.Max.Y));
        // Not so close to the bank that the fish would be pushed back before reaching it (SteerBounds).
        if (LakeMap::Depth(P.X, P.Y) < 45 || LakeMap::ShoreDistance(P.X, P.Y) < 300) continue;
        // Somewhere it can swim to straight: not behind a point of land or the island.
        if (Range > 0)
        {
            bool bOpen = true;
            const int32 Checks = FMath::CeilToInt(FVector2D::Distance(Near, P) / 200);
            for (int32 K = 1; K < Checks && bOpen; ++K)
            {
                const FVector2D Q = FMath::Lerp(Near, P, float(K) / Checks);
                bOpen = LakeMap::ShoreDistance(Q.X, Q.Y) > 60 && LakeMap::Depth(Q.X, Q.Y) > 35;   // no land and no shoal on the way
            }
            if (!bOpen) continue;
        }
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
    // Each angler gets the fish within 150 m; the lake holds hundreds.
    bAlwaysRelevant = false;
    SetNetCullDistanceSquared(FMath::Square(15000.0));
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
    DOREPLIFETIME(ALakeFish, NetVelocity);
    DOREPLIFETIME(ALakeFish, NetRotation);
    DOREPLIFETIME(ALakeFish, NetEffort);
    DOREPLIFETIME(ALakeFish, Goal);
    DOREPLIFETIME(ALakeFish, bLifted);
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
    // Bigger fish of a species hold out longer, and are calmer: slower to turn and to speed up.
    Size01 = LakeFish::Size01(Species, WeightKg);
    Agility = FMath::Lerp(1.25f, 0.7f, Size01);
    MaxStamina = S.Stamina * FMath::Lerp(0.6f, 1.8f, Size01);
    Stamina = MaxStamina;
    WanderSeed = Random.FRandRange(0, 100);
    Yaw = GetActorRotation().Yaw;
    Pitch = 0;
    if (School >= 0)
    {
        // Its own place in the school, beside and behind the leader.
        const float Reach = LakeFish::LengthCm(Species, WeightKg) * LakeFish::Gait(Species).Spacing * 2.2f;
        SlotOffset = FVector(-Reach * Random.FRandRange(0.15f, 1.0f), Reach * Random.FRandRange(-0.8f, 0.8f), Random.FRandRange(-12, 12));
    }
    NetLocation = GetActorLocation();
    NetRotation = GetActorRotation();
    NetVelocity = FVector::ZeroVector;
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
    // School members go where their leader goes; how they keep with it is SteerSchool's business.
    const ALakeFish* Lead = Leader();
    if (Lead != this)
    {
        RoamTarget = Lead->RoamTarget;
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
        // A cruising fish carries on more or less the way it is going rather than doubling back.
        const FVector Ahead = GetActorForwardVector().GetSafeNormal2D();
        for (int32 Try = 0; Try < 4; ++Try)
        {
            RoamTarget = LakeFish::PickSpot(Species, Random, FVector2D(bHomeBound ? HomePoint : GetActorLocation()), Range, C);
            if (FVector::DotProduct((RoamTarget - GetActorLocation()).GetSafeNormal2D(), Ahead) > -0.2f) break;
        }
        // Warm still sunny water: roach and crucian bask just under the surface, a pike lies up there
        // in the shallows - fish the angler can see.
        if (bWeather && Conditions.WaterTempC > 17 && Conditions.SunElevation > 15 && Conditions.Wind < 0.3f && Conditions.Rain < 0.15f)
        {
            const float Chance = Species == EFishSpecies::Roach ? 0.35f : Species == EFishSpecies::Crucian ? 0.3f : Species == EFishSpecies::Pike ? 0.12f : 0.0f;
            if (Random.FRand() < Chance)
                RoamTarget.Z = FMath::Max(LakeFish::SurfaceZ - Random.FRandRange(9.0f, 20.0f), BottomZ(RoamTarget.X, RoamTarget.Y) + 10);
        }
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
    // A pike hunts from a lie at the edge of cover.
    if (Goal == EFishGoal::Feed && S.Habitat.bAmbush && !bLocal)
    {
        ChooseLie();
        return;
    }
    // Feeding and resting places: near home for schools and ambushers, anywhere near for loners;
    // once there, a fish browses along a few metres at a time (a crucian a step at a time).
    const float Range = bLocal ? (Species == EFishSpecies::Crucian ? 160.0f : S.Habitat.bAmbush ? 700.0f : 500.0f) : bHomeBound ? 1500.0f : 1200.0f;
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

void ALakeFish::ChooseLie()
{
    FFishConditions Conditions;
    const FFishConditions* C = GetConditions(Conditions) ? &Conditions : nullptr;
    // A lie near home (the first one) or a short move along the weed edge (the next ones).
    const bool bMove = bAtSpot && Goal == EFishGoal::Feed;
    RoamTarget = LakeFish::PickSpot(Species, Random, FVector2D(bMove ? GetActorLocation() : HomePoint), bMove ? 800.0f : 1500.0f, C, EFishGoal::Feed);
    const float Bottom = BottomZ(RoamTarget.X, RoamTarget.Y);
    RoamTarget.Z = FMath::Lerp(LakeFish::SurfaceZ - 10, Bottom + 8, Random.FRandRange(0.45f, 0.8f));
    // It faces out from its cover into the open water, where prey swims past.
    const FVector2D Out = LakeMap::DeeperDirection(RoamTarget.X, RoamTarget.Y);
    LieFacing = FVector(Out.IsNearlyZero() ? FVector2D(GetActorForwardVector()) : Out, 0).RotateAngleAxis(Random.FRandRange(-35.0f, 35.0f), FVector::UpVector);
    LieUntil = SimTime + Random.FRandRange(50.0f, 140.0f);
    bAtSpot = false;
}

void ALakeFish::SetGoal(EFishGoal NewGoal, float Seconds)
{
    GoalUntil = SimTime + Seconds;
    if (NewGoal == Goal) return;
    Goal = NewGoal;
    bAtSpot = false;
    bRooting = false;
    PitchBias = 0;
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
    DashTarget = Target;
    DashUntil = SimTime + Seconds;
}

void ALakeFish::Eat(float Meal)
{
    Hunger = FMath::Max(0.0f, Hunger - Meal);
}

void ALakeFish::Startle(FVector From, float Strength)
{
    if (State == EState::Hooked || bLifted || Strength < 0.08f) return;
    // A pike in its lie lets small disturbances pass; a fish at the bait ignores a mild one.
    if ((Species == EFishSpecies::Pike && Strength < 0.4f) || (IsBusy() && Strength < 0.7f)) return;
    if (Strength >= 0.65f) { Spook(From, 1.0f + 2.5f * Strength); return; }
    if (State != EState::Roam) return;
    // Shy: turn and swim calmly away for a few seconds; the school follows it round.
    const bool bActive = SimTime < AvoidUntil;
    AvoidFrom = From;
    AvoidStrength = bActive ? FMath::Max(AvoidStrength, Strength) : Strength;
    AvoidUntil = FMath::Max(AvoidUntil, SimTime + 2.0f + 5.0f * Strength);
}

FVector ALakeFish::SteerRoam() const
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FVector To = RoamTarget - GetActorLocation();
    const float Dist = To.Size();
    // Lively in its good hours, sluggish in its bad ones; big fish swim more slowly than small.
    const float Lively = FMath::Lerp(0.45f, 1.0f, FMath::Clamp(Activity, 0.0f, 1.0f));
    const float Cruise = S.CruiseSpeed * Lively * FMath::Lerp(1.08f, 0.88f, Size01);
    float Speed;
    // An ambusher moving to a new lie swims steadily and glides the last metre.
    if (S.Habitat.bAmbush && Goal == EFishGoal::Feed) Speed = FMath::Min(FMath::Max(S.CruiseSpeed, 30.0f) * FMath::Lerp(1.0f, 0.8f, Size01), Dist * 0.25f + 2.0f);
    else
    {
        // On a cruise the next waypoint is picked before the fish would slow (see Live); feeding
        // and resting spots it reaches gently.
        const float SlowRadius = Goal == EFishGoal::Cruise ? 60.0f : 150.0f;
        Speed = Cruise * FMath::Clamp(Dist / SlowRadius, 0.25f, 1.0f);
    }
    // Resting fish barely move; browsing ones potter about.
    if (Goal == EFishGoal::Rest && bAtSpot) Speed *= 0.3f;
    else if (Goal == EFishGoal::Feed && bAtSpot && Species != EFishSpecies::Perch) Speed *= 0.55f;
    else if (Goal != EFishGoal::Cruise && !bAtSpot && !S.Habitat.bAmbush) Speed *= 1.4f;   // on its way somewhere
    return Dist > 1 ? To / Dist * Speed : FVector::ZeroVector;
}

FVector ALakeFish::Meander(const FVector& Desired) const
{
    // A fish never swims a ruler-straight line: its course swings slowly from side to side and up
    // and down. Small fish meander more and quicker than big ones.
    const float Speed = Desired.Size();
    if (Speed < 3) return Desired;
    const FFishGait& G = LakeFish::Gait(Species);
    const float W = 2 * PI * G.WanderHz * FMath::Lerp(1.3f, 0.75f, Size01);
    const float Swing = G.WanderDeg * FMath::Lerp(1.2f, 0.7f, Size01)
        * (0.7f * FMath::Sin(SimTime * W + WanderSeed) + 0.3f * FMath::Sin(SimTime * W * 2.3f + WanderSeed * 3.1f));
    FVector Out = Desired.RotateAngleAxis(Swing, FVector::UpVector);
    Out.Z += Speed * 0.08f * FMath::Sin(SimTime * W * 0.7f + WanderSeed * 1.7f);
    return Out;
}

FVector ALakeFish::SteerSchool(bool bFollow) const
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FFishGait& G = LakeFish::Gait(Species);
    const float Len = LakeFish::LengthCm(Species, WeightKg);
    const float Personal = FMath::Max(18.0f, Len * G.Spacing);
    const float Look = FMath::Max(200.0f, Personal * 4);
    const FVector P = GetActorLocation();
    FVector Centre = FVector::ZeroVector, Heading = FVector::ZeroVector, Separation = FVector::ZeroVector;
    int32 Count = 0;
    auto Visit = [&](const ALakeFish* Other)
    {
        if (Other == this) return;
        const FVector Offset = P - Other->GetActorLocation();
        const float Dist = Offset.Size();
        // Keep out of each other's way (any fish); school mates keep their spacing.
        const float Keep = Other->School == School && School >= 0 ? Personal : FMath::Max(30.0f, Len * 0.9f);
        if (Dist < Keep && Dist > 0.1f) Separation += Offset / Dist * (1 - Dist / Keep);
        if (School < 0 || Other->School != School || Dist > Look) return;
        Centre += Other->GetActorLocation();
        Heading += Other->Vel;
        ++Count;
    };
    if (const ALakeLife* Life = ALakeLife::Get(GetWorld())) Life->ForEachFishNear(P, Look, [&](ALakeFish* Other) { Visit(Other); });
    else for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It) Visit(*It);
    const float Cruise = S.CruiseSpeed;
    if (!bFollow) return Separation * Cruise * 1.2f;
    // A follower: line up with its mates, close in on the group and take its own place beside the
    // leader. A resting school hangs loose; a moving one keeps tight.
    const ALakeFish* Lead = Leader();
    const FVector LeadVel = Lead->Vel;
    FVector Out = Count ? (Heading / Count) * 0.75f + LeadVel * 0.25f : LeadVel;
    if (Count) Out += (Centre / Count - P) * (Goal == EFishGoal::Rest ? 0.08f : 0.15f);
    const FVector Slot = Lead->GetActorLocation() + FRotator(0, Lead->Yaw, 0).RotateVector(SlotOffset);
    const FVector ToSlot = Slot - P;
    Out += (ToSlot * (Goal == EFishGoal::Cruise ? 0.4f : 0.25f)).GetClampedToMaxSize(Cruise * 1.3f);
    Out += Separation * Cruise * 1.8f;
    return Out;
}

FVector ALakeFish::SteerBounds() const
{
    const FVector P = GetActorLocation();
    const float Shore = LakeMap::ShoreDistance(P.X, P.Y);
    const float Depth = LakeFish::SurfaceZ - BottomZ(P.X, P.Y);
    FVector Push = FVector::ZeroVector;
    if (Shore < 200 || Depth < 30)
    {
        // Back towards deeper water.
        Push += FVector(LakeMap::DeeperDirection(P.X, P.Y), 0) * 60 * FMath::Max(1.0f, (200 - Shore) / 60);
    }
    if (P.Z > LakeFish::SurfaceZ - 8) Push.Z -= 25;
    if (P.Z < BottomZ(P.X, P.Y) + 6) Push.Z += 25;
    return Push;
}

// --- daily life ------------------------------------------------------------------------------

void ALakeFish::UpdateRooting(FVector& Desired, float& MaxSpeed)
{
    // A feeding crucian works the silt: nose down, tail fanning, a cloud of bubbles; then a short
    // slow shuffle to the next patch, and down again.
    const FVector P = GetActorLocation();
    const float Bottom = BottomZ(P.X, P.Y);
    if (bRooting && SimTime >= PhaseUntil)
    {
        bRooting = false;
        ChooseRoamTarget(true);
        PhaseUntil = SimTime + Random.FRandRange(4.0f, 8.0f);
    }
    else if (!bRooting && (FVector::Dist2D(P, RoamTarget) < 30 || SimTime >= PhaseUntil))
    {
        bRooting = true;
        PhaseUntil = SimTime + Random.FRandRange(3.0f, 9.0f) * FMath::Lerp(0.8f, 1.3f, Size01);
        RoamTarget = FVector(P.X, P.Y, Bottom + 6);
    }
    if (bRooting)
    {
        // Hold the spot, swaying slowly as it digs.
        PitchBias = -32.0f + 6.0f * FMath::Sin(SimTime * 1.3f + WanderSeed);
        Desired = ((RoamTarget - P) * 0.6f).GetClampedToMaxSize(5.0f);
        FaceDir = FRotator(0, Yaw + 18.0f * FMath::Sin(SimTime * 0.5f + WanderSeed), 0).Vector();
        MaxSpeed = 6;
    }
    else
    {
        PitchBias = -6.0f;
        Desired = SteerRoam() * 0.8f;
    }
}

void ALakeFish::Live(float DeltaSeconds, FVector& Desired, float& MaxSpeed)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    ALakeLife* Life = ALakeLife::Get(GetWorld());
    // Needs: swimming tires, resting restores; hunger grows with the appetite of the hour.
    const float Recovery = Goal != EFishGoal::Rest ? 0.0f : bAtSpot ? 0.014f : 0.006f;   // already easing off on the way
    Energy = FMath::Clamp(Energy - DeltaSeconds * (0.0012f + 0.004f * Effort) + DeltaSeconds * Recovery, 0.0f, 1.0f);
    Hunger = FMath::Min(1.0f, Hunger + DeltaSeconds * 0.0035f * Activity);
    {
        const ALakeFish* Lead = Leader();
        if (Lead != this)
        {
            // School members share their leader's destination and arrive with it.
            RoamTarget = Lead->RoamTarget;
            bAtSpot = Lead->bAtSpot;
        }
    }
    const float Dist = FVector::Dist(GetActorLocation(), RoamTarget);
    if (!bAtSpot && Dist < 150)
    {
        bAtSpot = true;
        if (S.Habitat.bAmbush && Goal == EFishGoal::Feed) LieUntil = SimTime + Random.FRandRange(50.0f, 140.0f);
    }
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
    // Decide may have handed the lead to another fish.
    const ALakeFish* Lead = Leader();
    const bool bFollower = Lead != this;
    PitchBias = 0;
    FaceDir = FVector::ZeroVector;
    const bool bRoot = Goal == EFishGoal::Feed && bAtSpot && Species == EFishSpecies::Crucian;
    if (!bRoot) bRooting = false;
    const bool bLie = Goal == EFishGoal::Feed && S.Habitat.bAmbush && bAtSpot;

    // Perch drive the fry: follow the shoal, the leader leads each rush into it.
    if (Goal == EFishGoal::Feed && Species == EFishSpecies::Perch && PreyFry >= 0 && Life)
    {
        if (!bFollower)
        {
            RoamTarget = Life->FryLocation(PreyFry) - FVector(0, 0, 20);
            if (FVector::Dist2D(GetActorLocation(), RoamTarget) < 320 && SimTime >= NextFryAttack)
            {
                NextFryAttack = SimTime + Random.FRandRange(1.0f, 2.4f);
                Life->AttackFry(PreyFry, this);
            }
        }
    }
    else if (!bFollower && !bRoot)
    {
        if (bLie)
        {
            // A pike keeps its lie for a minute or two, then moves along the weed edge.
            if (SimTime >= LieUntil) ChooseLie();
        }
        else if (Goal == EFishGoal::Cruise ? (Dist < 150 || StateTime > 45)
                                           : (bAtSpot && (Dist < 50 || StateTime > 25)) || StateTime > (S.Habitat.bAmbush ? 90 : 60))
        {
            // Cruising, the next waypoint is chosen before the fish arrives, so it never stops;
            // at its feeding or resting spot it browses along a few metres at a time.
            ChooseRoamTarget(bAtSpot && Goal != EFishGoal::Cruise);
            StateTime = 0;
        }
    }

    if (bRoot) UpdateRooting(Desired, MaxSpeed);
    else if (bLie)
    {
        // In the lie: hang almost still, fins holding the spot, facing the open water.
        const FVector Hold = RoamTarget - GetActorLocation();
        Desired = (Hold * 0.3f).GetClampedToMaxSize(Hold.Size() > 120 ? 25.0f : 6.0f);
        FaceDir = LieFacing;
        MaxSpeed = FMath::Max(6.0f, (float)Desired.Size());
    }
    else if (bFollower)
    {
        const FVector Flock = SteerSchool(true);
        Desired = Meander(Flock) * 0.5f + Flock * 0.5f;
        MaxSpeed = FMath::Max(MaxSpeed, S.CruiseSpeed * 1.35f);
    }
    else
    {
        const FVector Roam = SteerRoam();
        Desired = Meander(Roam) + SteerSchool(false);
        // The leader of a school swims a little slower so the school keeps up with it.
        if (School >= 0) Desired *= 0.85f;
        // Moving to a new place a fish may swim faster than it cruises (SteerRoam sets how fast).
        MaxSpeed = FMath::Max(MaxSpeed, (float)Roam.Size());
    }
    // Shying away from a disturbance: a smooth turn and a quicker swim away from it.
    if (SimTime < AvoidUntil)
    {
        const float Fade = FMath::Clamp((AvoidUntil - SimTime) / 1.5f, 0.0f, 1.0f);
        const FVector Away = (GetActorLocation() - AvoidFrom).GetSafeNormal2D();
        Desired += Away * S.CruiseSpeed * (1.0f + 1.4f * AvoidStrength) * Fade;
        MaxSpeed = FMath::Max(MaxSpeed, S.CruiseSpeed * (1.2f + AvoidStrength));
        if (bRooting) { bRooting = false; PhaseUntil = SimTime + 5; }
    }
    // Rushing at the fry with the school.
    if (SimTime < DashUntil)
    {
        Desired = (DashTarget - GetActorLocation()).GetSafeNormal() * S.BurstSpeed;
        MaxSpeed = S.BurstSpeed;
    }
    if (Goal == EFishGoal::Feed && Species == EFishSpecies::Pike) UpdatePikeAmbush(DeltaSeconds, Desired, MaxSpeed);
    if (!Life) return;

    const FVector P = GetActorLocation();
    const FVector Surface(P.X, P.Y, LakeFish::SurfaceZ);
    // Just under the surface a fish now and then shows itself: a dimple, a fin, a little bulge.
    // (Roach sipping at the top already show their own rings.)
    if (P.Z > LakeFish::SurfaceZ - 18 && SimTime >= NextSurfaceSign && !(Goal == EFishGoal::Feed && Species == EFishSpecies::Roach))
    {
        if (NextSurfaceSign > 0)
        {
            const float Size = FMath::Min(0.12f + WeightKg * 0.35f + 0.25f * FMath::Clamp((float)Vel.Size() / S.BurstSpeed, 0.0f, 1.0f), 0.6f);
            Life->EmitSign(FMath::Abs(YawRate) > 45 && Vel.Size() > S.CruiseSpeed ? ELakeSign::Swirl : ELakeSign::Rise, Surface, Size, Species, WeightKg, 0, Size < 0.3f);
        }
        NextSurfaceSign = SimTime + Random.FRandRange(5.0f, 16.0f);
    }
    if (SimTime < NextSign) return;

    // Signs of feeding on the water.
    const float Bottom = BottomZ(P.X, P.Y);
    const float Pace = FMath::Clamp(Activity, 0.4f, 1.3f);
    if (bRooting && P.Z < Bottom + 30 && LakeFish::SurfaceZ - Bottom < 300)
    {
        // Rooting in the silt sends up chains of bubbles.
        Life->EmitSign(ELakeSign::Bubbles, Surface, 0.4f + WeightKg * 0.8f, Species, WeightKg);
        NextSign = SimTime + Random.FRandRange(4.0f, 9.0f) / Pace;
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
    if (!bAtSpot || Hunger < 0.3f) return;
    // Stalking: a fish nearby but not yet in front of the snout - the pike turns slowly towards it
    // with its fins and edges a little closer, the body stiff and the tail still.
    if (const ALakeFish* Stalked = Prey.Get(); Stalked && SimTime < NextLook + 0.4f)
    {
        const FVector To = Stalked->GetActorLocation() - GetActorLocation();
        FaceDir = To.GetSafeNormal2D();
        Desired += To.GetSafeNormal() * 4.0f;
    }
    // Waiting in ambush: a fish or fry passing in front of the snout is struck.
    if (SimTime < NextLook) return;
    NextLook = SimTime + 0.4f;
    const FVector Mouth = MouthLocation(), Forward = GetActorForwardVector();
    ALakeFish* Best = nullptr;
    ALakeFish* Near = nullptr;
    float BestDist = 260, NearDist = 450;
    Life->ForEachFishNear(Mouth, 450, [&](ALakeFish* Other)
    {
        if (Other == this || Other->Species == EFishSpecies::Pike || !Other->IsFree() || Other->WeightKg > WeightKg * 0.3f) return;
        const FVector To = Other->GetActorLocation() - Mouth;
        const float Dist = To.Size();
        if (Dist < BestDist && FVector::DotProduct(Forward, To / FMath::Max(1.0f, Dist)) > 0.5f) { Best = Other; BestDist = Dist; }
        if (Dist < NearDist) { Near = Other; NearDist = Dist; }
    });
    Prey = Best ? nullptr : Near;
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
            // How this fish will take it (wary in bright sun on a glassy lake, more so with a poor appetite).
            FFishConditions Conditions;
            const bool bBright = GetConditions(Conditions) && LakeFish::IsBrightCalm(Conditions);
            const FVector BaitAt = Angler->BaitLocation();
            const bool bOnBottom = BaitAt.Z <= BottomZ(BaitAt.X, BaitAt.Y) + 8;
            Bite = LakeFish::MakeBite(Species, Size01, Activity, bOnBottom, bBright, Random);
            BiteIndex = -1;
            BiteStepEnd = SimTime + Random.FRandRange(0.3f, 1.0f);
            // A crucian rooting at the bait sends bubbles up beside the float.
            if (Species == EFishSpecies::Crucian)
                if (ALakeLife* Life = ALakeLife::Get(GetWorld()))
                    Life->EmitSign(ELakeSign::Bubbles, FVector(BaitAt.X + Random.FRandRange(-40, 40), BaitAt.Y + Random.FRandRange(-40, 40), LakeFish::SurfaceZ), 0.4f + WeightKg * 0.6f, Species, WeightKg);
        }
        return;
    }
    if ((State == EState::Nibble || State == EState::Holding) && Angler && SimTime >= BiteStepEnd) NextBiteStep(Angler);
}

void ALakeFish::NextBiteStep(ALakeCharacter* Angler)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    ++BiteIndex;
    if (!Bite.IsValidIndex(BiteIndex))
    {
        // Done: a fish still only testing loses interest; one holding the bait spits it (the angler's timer).
        BiteStepEnd = TNumericLimits<float>::Max();
        if (State == EState::Nibble)
        {
            Angler->OnBiteMotion(this, FBiteStep());
            Angler->ReleaseBaitClaim(this);
            IgnoreBaitUntil.Add(Angler, SimTime + 30);
            State = EState::Roam;
            ChooseRoamTarget();
        }
        return;
    }
    const FBiteStep Step = Bite[BiteIndex];
    BiteStepEnd = SimTime + Step.Duration;
    if (Step.bTake && State == EState::Nibble)
    {
        // It takes: the angler may strike for as long as it keeps the bait in its mouth.
        float Window = 0;
        for (int32 I = BiteIndex; I < Bite.Num(); ++I) if (Bite[I].bTake) Window += Bite[I].Duration;
        Angler->OnFishTake(this, Window);
        State = EState::Holding;
        StateTime = 0;
    }
    Angler->OnBiteMotion(this, Step);
    if (State == EState::Holding)
    {
        // Where it goes with the bait: up (lifting it), aside, or down and away.
        const FVector P = GetActorLocation();
        const FVector Away = (P - Angler->GetActorLocation()).GetSafeNormal2D();
        const FVector Side = FVector::CrossProduct(Away, FVector::UpVector) * (Random.FRand() < 0.5f ? 1 : -1);
        switch (Step.Motion)
        {
        case EBiteMotion::Lift:
        {
            // Rising with the bait high enough to lift the shot off it: the float loses its load.
            const float Rise = FMath::Clamp(LakeFish::SurfaceZ - 15 - P.Z, 0.0f, 8 + 9 * Step.Amount);
            RoamTarget = P + Side * 10 + FVector(0, 0, Rise);
            HoldSpeed = FMath::Max(0.25f, Rise / FMath::Max(Step.Duration * 0.8f, 0.3f) / S.CruiseSpeed);
            break;
        }
        case EBiteMotion::Drift:
            RoamTarget = P + (Side * 0.8f + Away * 0.4f).GetSafeNormal() * 250;
            HoldSpeed = 0.6f;
            break;
        default:
            RoamTarget = P + (Away + Side * Random.FRandRange(-0.5f, 0.5f)).GetSafeNormal() * 400;
            RoamTarget.Z = FMath::Max(BottomZ(RoamTarget.X, RoamTarget.Y) + 10, P.Z - 60);
            HoldSpeed = FMath::Clamp(Step.Amount / S.CruiseSpeed * 0.6f, 0.6f, 2.5f);
            break;
        }
        return;
    }
    // A fish that does not like the bait much often gives up after tasting it.
    if (Step.Motion != EBiteMotion::None)
    {
        const float Like = LakeFish::BaitInterest(Species, Angler->Bait, 0.5f);
        if (Random.FRand() > FMath::Min(1.0f, (0.55f + Like * 0.45f) * FMath::Lerp(0.75f, 1.1f, FMath::Clamp(Activity, 0.0f, 1.0f))))
        {
            Angler->OnBiteMotion(this, FBiteStep());
            Angler->ReleaseBaitClaim(this);
            IgnoreBaitUntil.Add(Angler, SimTime + 30);
            State = EState::Roam;
            ChooseRoamTarget();
        }
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
    // A bolt: a quick start, then a fast swim that eases off (see Simulate).
    BurstTime = FMath::Min(Seconds * 0.4f, 1.0f);
    StrikeEnd = -1;
    Prey = nullptr;
    bRooting = false;
    DashUntil = -1;
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

void ALakeFish::DropBait()
{
    if (State != EState::Nibble && State != EState::Holding) return;
    if (ALakeCharacter* Angler = TargetAngler.Get())
    {
        IgnoreBaitUntil.Add(Angler, SimTime + 40);
        Angler->ReleaseBaitClaim(this);
    }
    TargetAngler = nullptr;
    Bite.Reset();
    BiteIndex = -1;
    BiteStepEnd = 0;
    State = EState::Roam;
    StateTime = 0;
    ChooseRoamTarget();
}

void ALakeFish::LiftOut()
{
    bLifted = true;
    HangTime = 0;
    HangForward = GetActorForwardVector();
    Vel = FVector::ZeroVector;
    PendingLineForce = FVector::ZeroVector;
}

void ALakeFish::HangOnLine(FVector Mouth, FVector LineUp, float DeltaSeconds)
{
    LastHangFrame = GFrameCounter;
    const float Dt = FMath::Min(DeltaSeconds, 0.05f);
    HangTime += Dt;
    const float Len = LakeFish::LengthCm(Species, WeightKg);
    FVector Up = LineUp.GetSafeNormal();
    if (Up.IsNearlyZero()) Up = FVector::UpVector;
    // In the water it is drawn mouth first along the line, lying flat; clear of the water its
    // weight hangs below the hook, head up along the line.
    const float Out = FMath::Clamp((Mouth.Z - (LakeFish::SurfaceZ - 6)) / (Len * 0.8f), 0.0f, 1.0f);
    const FVector Drawn = Up.GetSafeNormal2D();
    if (!Drawn.IsNearlyZero()) HangForward = FMath::VInterpNormalRotationTo(HangForward, Drawn, Dt, 90.0f);
    FVector Flat = HangForward.GetSafeNormal2D();
    if (Flat.IsNearlyZero()) Flat = FVector::ForwardVector;
    const FVector Axis = FMath::Lerp(Flat + FVector(0, 0, 0.25f), Up * 1.25f, Out).GetSafeNormal();
    // Out of the water it fights in bouts: hard thrashing, then a moment hanging still.
    const float Bout = FMath::Sin(HangTime * 2.3f + Phase) * FMath::Sin(HangTime * 0.9f + 1.0f);
    const float Want = Out < 0.3f ? 0.85f : Bout > -0.15f ? 1.0f : 0.25f;
    HangEffort = FMath::FInterpTo(HangEffort, Want, Dt, 6.0f);
    // The body bends from side to side around the hook and slowly turns on the line.
    const float Bend = FMath::DegreesToRadians((5 + 18 * HangEffort) * Out * FMath::Sin(HangTime * (8 + 6 * HangEffort) + Phase));
    const float Twist = FMath::DegreesToRadians(50 * Out * FMath::Sin(HangTime * 0.7f + Phase));
    const FQuat Base = FRotationMatrix::MakeFromXZ(Axis, FMath::Lerp(FVector::UpVector, -Flat, Out)).ToQuat();
    const FQuat Pose = Base * FQuat(FVector::ForwardVector, Twist) * FQuat(FVector::UpVector, Bend);
    SetActorLocationAndRotation(Mouth - Pose.GetForwardVector() * Len * 0.42f, Pose);
}

void ALakeFish::Release(float SpookSeconds)
{
    if (bLifted)
    {
        // Dropped back: it falls where it was drawn out.
        bLifted = false;
        SetActorLocation(FVector(NetLocation));
    }
    // Swim on from the pose it was left in, level again.
    Yaw = GetActorRotation().Yaw;
    Pitch = FMath::Clamp(GetActorRotation().Pitch, -30.0, 30.0);
    Roll = 0;
    YawRate = 0;
    Wish = Vel;
    WishSpeedNow = Vel.Size();
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
    if (State == EState::Hooked) { MoveHooked(Desired, MaxSpeed, DeltaSeconds); return; }
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FFishGait& G = LakeFish::Gait(Species);
    const float Dt = DeltaSeconds;
    const float Len = LakeFish::LengthCm(Species, WeightKg);
    const bool bBurst = BurstTime > 0;
    Desired = Desired.GetClampedToMaxSize(MaxSpeed);
    // The wish is smoothed: a fish eases into each new intention instead of twitching from one to
    // the next. A burst (a strike, a bolt) is near instant; at the bait it aims quickly.
    const float Response = bBurst ? 0.05f : bPrecise ? 0.12f : G.Response / Agility;
    const float Blend = 1 - FMath::Exp(-Dt / Response);
    Wish += (Desired - Wish) * Blend;
    // The speed is smoothed apart from the direction, so a wish swinging right round does not
    // pass through a standstill on the way (the fish swims on round the turn).
    WishSpeedNow += ((float)Desired.Size() - WishSpeedNow) * Blend;
    const float WishSpeed = WishSpeedNow;
    const FVector W = (Wish.Size() > 0.3f * WishSpeed ? Wish : Desired).GetSafeNormal() * WishSpeed;
    const float ForwardSpeed = FVector::DotProduct(Vel, FRotator(Pitch, Yaw, 0).Vector());

    // Turning. The fins turn a fish slowly on the spot; under way the body cannot turn tighter
    // than its arc (a few tenths of its length), so a turn at speed is a curve. The turn rate
    // builds up and dies away (no instant snap), and eases off as the body comes round.
    float MaxTurn = G.HoverTurn * Agility + FMath::RadiansToDegrees(FMath::Max(0.0f, ForwardSpeed) / (G.TurnRadius * Len));
    MaxTurn = FMath::Min(MaxTurn, S.TurnRate * Agility * (bBurst ? 1.8f : 1.0f));
    const FVector Aim = W.Size2D() > 1.5f ? W : FaceDir;
    // A fish that barely wants to move does not bother to turn hard (no fidgeting on the spot).
    const float Urge = bBurst || bPrecise ? 1.0f : W.Size2D() > 1.5f ? FMath::Clamp(WishSpeed / (0.35f * S.CruiseSpeed), 0.25f, 1.0f) : 0.5f;
    float TargetRate = 0;
    if (Aim.Size2D() > 0.01f)
    {
        const float Error = FMath::FindDeltaAngleDegrees(Yaw, FMath::RadiansToDegrees(FMath::Atan2(Aim.Y, Aim.X)));
        TargetRate = FMath::Clamp(Error * (bBurst ? 6.0f : 2.2f), -MaxTurn * Urge, MaxTurn * Urge);
    }
    const float TurnAccel = G.TurnAccel * Agility * (bBurst ? 4.0f : 1.0f);
    YawRate += FMath::Clamp(TargetRate - YawRate, -TurnAccel * Dt, TurnAccel * Dt);
    Yaw = FRotator::NormalizeAxis(Yaw + YawRate * Dt);
    // Climbing and diving: the nose follows the wish up or down, gently; the back stays up.
    // A fish rarely tips more than a quarter of a right angle; rooting it stands on its nose.
    float WantPitch = 0;
    if (WishSpeed > 1.5f) WantPitch = FMath::Clamp(FMath::RadiansToDegrees(FMath::Atan2(W.Z, FMath::Max((float)W.Size2D(), 1.0f))), bBurst ? -35.0f : -22.0f, bBurst ? 35.0f : 22.0f);
    Pitch = FMath::FInterpTo(Pitch, FMath::Clamp(WantPitch + PitchBias, -45.0f, 35.0f), Dt, bBurst ? 6.0f : 2.0f);
    const FVector Heading = FRotator(Pitch, Yaw, 0).Vector();

    // Thrust only along the body, like a real tail. Asked to go somewhere behind it, a fish swims
    // on round the turn instead of stopping to spin (unless it must aim its mouth exactly).
    const float Align = WishSpeed > 0.01f ? FVector::DotProduct(Heading, W / WishSpeed) : 0.0f;
    float TargetSpeed = WishSpeed * FMath::Lerp(bPrecise ? 0.0f : 0.65f, 1.0f, FMath::Clamp(Align, 0.0f, 1.0f));
    // Kick and glide (perch): a few strong beats, then coasting on the momentum.
    bool bKicking = true;
    if (G.Kick < 1 && !bBurst && !bPrecise && WishSpeed > 4)
    {
        bKicking = FMath::Frac(SimTime / G.KickPeriod + WanderSeed) < G.Kick;
        if (bKicking) TargetSpeed *= 1 + 0.7f * (1 - G.Kick);
    }
    const float Accel = S.Acceleration * Agility * (bBurst ? 2.5f : 1.0f);
    // A burst is a fast start: the tail drives hard until the speed is reached.
    float Thrust = FMath::Clamp((TargetSpeed - ForwardSpeed) * (bBurst ? 8.0f : 3.0f), -Accel * 0.35f, Accel);
    if (!bKicking) Thrust = FMath::Min(Thrust, 0.0f) * 0.3f;
    // Coming round a turn it carries its way on rather than braking.
    if (Align < 0.5f && !bPrecise) Thrust = FMath::Max(Thrust, -Accel * 0.08f);
    Effort = FMath::Clamp(FMath::Abs(Thrust) / FMath::Max(1.0f, S.Acceleration * 2.5f) + ForwardSpeed / S.BurstSpeed * 0.5f, 0.0f, 1.0f);
    Vel += Heading * Thrust * Dt;
    PendingLineForce = FVector::ZeroVector;
    // Water drag: low along the streamlined body, high sideways, so the motion follows the body
    // round each turn.
    const FVector Forward = Heading * FVector::DotProduct(Vel, Heading);
    const FVector Side = Vel - Forward;
    const float ForwardDrag = 1.2f / S.BurstSpeed * Forward.Size();
    Vel = Forward / (1 + ForwardDrag * Dt) + Side / (1 + 5.0f * Dt);
    // Rising and sinking on the spot (swim bladder and fins), for a fish hanging still.
    Vel.Z += (W.Z - Vel.Z) * FMath::Min(1.0f, Dt * 1.2f) * (1 - FMath::Abs(Align));
    // Banking a little into turns.
    Roll = FMath::FInterpTo(Roll, FMath::Clamp(YawRate * FMath::Max(0.0f, ForwardSpeed) / 400.0f, -14.0f, 14.0f), Dt, 3.0f);

    // Integrate and keep the fish inside the water volume.
    FVector P = GetActorLocation() + Vel * Dt;
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
    SetActorLocationAndRotation(P, FRotator(Pitch, Yaw, Roll));
}

void ALakeFish::MoveHooked(FVector Desired, float MaxSpeed, float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    Desired = Desired.GetClampedToMaxSize(MaxSpeed);
    FVector Heading = GetActorForwardVector();
    const bool bBurst = BurstTime > 0;
    const float TurnRate = S.TurnRate * (bBurst ? 1.8f : 1.0f) * 0.8f;
    // On the line the fish throws itself about: the body turns straight at the wanted heading,
    // limited by the species' turn rate. Fish keep their back up: pitch is limited.
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
    Accel *= S.Strength * FMath::Lerp(0.25f, 1.0f, StaminaFraction());
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

    FVector P = GetActorLocation() + Vel * DeltaSeconds;
    const float Bottom = BottomZ(P.X, P.Y);
    const float Top = LakeFish::SurfaceZ - 4;
    if (P.Z > Top) { P.Z = Top; Vel.Z = FMath::Min(0.0, Vel.Z); }
    if (P.Z < Bottom + 4) { P.Z = Bottom + 4; Vel.Z = FMath::Max(0.0, Vel.Z); }
    if (!LakeMap::IsWater(P.X, P.Y) || LakeFish::SurfaceZ - Bottom < 10)
    {
        const FVector In(LakeMap::DeeperDirection(P.X, P.Y), 0);
        P = GetActorLocation();
        Vel -= In * FMath::Min(0.0, FVector::DotProduct(Vel, In)) * 1.0;
    }
    const FRotator Rotation = Heading.Rotation();
    YawRate = DeltaSeconds > 0 ? FMath::FindDeltaAngleDegrees(Yaw, Rotation.Yaw) / DeltaSeconds : 0.0f;
    Yaw = Rotation.Yaw;
    Pitch = Rotation.Pitch;
    Roll = 0;
    Wish = Vel;
    WishSpeedNow = Vel.Size();
    SetActorLocationAndRotation(P, Rotation);
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
    bPrecise = false;
    if (State != EState::Roam) { PitchBias = 0; FaceDir = FVector::ZeroVector; }
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
        bPrecise = To.Size() < 80;
        // Pike close the last metre in one strike.
        if (S.Habitat.bAmbush && To.Size() < 120) { BurstTime = 0.4f; Desired = To.GetSafeNormal() * S.BurstSpeed; MaxSpeed = S.BurstSpeed; }
        break;
    }
    case EState::Nibble:
    {
        // Hover with the mouth at the bait, tiny pecks forward.
        const FVector To = Angler->BaitLocation() - MouthLocation();
        Desired = To * 1.5f + GetActorForwardVector() * 6 * FMath::Sin(SimTime * 9);
        FaceDir = To;
        MaxSpeed = 20;
        bPrecise = true;
        break;
    }
    case EState::Holding:
        Desired = (RoamTarget - GetActorLocation()).GetSafeNormal() * S.CruiseSpeed * HoldSpeed;
        MaxSpeed = FMath::Max(S.CruiseSpeed, S.CruiseSpeed * HoldSpeed);
        bPrecise = Bite.IsValidIndex(BiteIndex) && Bite[BiteIndex].Motion == EBiteMotion::Lift;
        break;
    case EState::Hooked:
    {
        if (!Angler) { State = EState::Roam; break; }
        // The fish "leads": runs along the bank, away from the angler or down to the bottom, with
        // bursts while it is fresh (longer for a heavy fish). A tired fish comes up and gives in.
        const float Fresh = StaminaFraction();
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
    {
        // A bolt away from the fright, then a fast swim that eases back to a cruise; school mates
        // flee together.
        FleeTime -= DeltaSeconds;
        const float Speed = BurstTime > 0 ? S.BurstSpeed : S.CruiseSpeed * (1.5f + 2.5f * FMath::Clamp(FleeTime / 3.0f, 0.0f, 1.0f));
        const FVector Away = (GetActorLocation() - FleeFrom).GetSafeNormal();
        Desired = Away * Speed + (School >= 0 ? SteerSchool(true) * 0.3f : FVector::ZeroVector);
        MaxSpeed = S.BurstSpeed;
        if (FleeTime <= 0) { State = EState::Roam; ChooseRoamTarget(); StateTime = 0; }
        break;
    }
    }
    if (State == EState::Flee || State == EState::Hooked) MaxSpeed = FMath::Max(MaxSpeed, S.CruiseSpeed);
    if (BurstTime > 0) MaxSpeed = S.BurstSpeed;
    Desired += SteerBounds();
    Move(Desired, MaxSpeed, DeltaSeconds);
}

// --- per-frame -----------------------------------------------------------------------------

void ALakeFish::OnRep_NetLocation()
{
    if (UWorld* World = GetWorld()) Smooth.Receive(FVector(NetLocation), FVector(NetVelocity), NetRotation, World->GetTimeSeconds(), World->GetRealTimeSeconds());
}

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
    // Size: the model is scaled to the fish's length, and its depth and width to its build.
    const float Len = LakeFish::LengthCm(Species, WeightKg);
    const FVector Scale(Len / S.MeshLength);
    if (!GetActorScale3D().Equals(Scale, 0.001f)) SetActorScale3D(Scale);
    const float Girth = LakeFish::Girth(Species, WeightKg);
    if (Girth != LastGirth) { LastGirth = Girth; Body->SetRelativeScale3D(FVector(1, Girth, Girth)); }

    // The motion as this machine shows it.
    const bool bHanging = bLifted || GFrameCounter - LastHangFrame < 3;
    const FVector Here = GetActorLocation();
    const float CurrentYaw = GetActorRotation().Yaw;
    FVector ShownVel = HasAuthority() ? Vel : Smooth.Velocity;
    float Turn = HasAuthority() ? YawRate : Smooth.YawRate;
    ShownWindow += DeltaSeconds;
    // After this machine stood still the fish starts afresh where the server has it: a new
    // stretch of motion, not a step within one.
    const bool bRestarted = !HasAuthority() && Smooth.bRestarted;
    Smooth.bRestarted = false;
    if (bHanging || DeltaSeconds <= 0)
    {
        ShownVel = FVector::ZeroVector;
        Turn = 0;
        bShownValid = false;
    }
    else if (!bShownValid || bRestarted)
    {
        bShownValid = true;
        ShownWindow = 0;
        ShownPrevVelocity = FVector::ZeroVector;
        ShownPrevLocation = Here;
        ShownPrevYaw = CurrentYaw;
    }
    else if (ShownWindow >= 1.0f / 60)
    {
        // How smooth the shown motion is (for the log), seen as a 60 fps eye would: acceleration
        // and change of turn between one sixtieth of a second and the next.
        const FVector WindowVel = (Here - ShownPrevLocation) / ShownWindow;
        const float WindowTurn = FMath::FindDeltaAngleDegrees(ShownPrevYaw, CurrentYaw) / ShownWindow;
        if (ALakeLife* Life = ALakeLife::Get(GetWorld()); Life && ShownPrevVelocity != FVector::ZeroVector)
            Life->RecordShownMotion(Here, (float)((WindowVel - ShownPrevVelocity).Size() / ShownWindow),
                                    FMath::Abs(WindowTurn - ShownPrevFrameTurn) / ShownWindow, Species);
        ShownPrevVelocity = WindowVel;
        ShownPrevFrameTurn = WindowTurn;
        ShownPrevLocation = Here;
        ShownPrevYaw = CurrentYaw;
        ShownWindow = 0;
    }
    const float Dt = FMath::Min(DeltaSeconds, 0.1f);
    ShownSpeed = FMath::FInterpTo(ShownSpeed, (float)ShownVel.Size(), Dt, 4.0f);
    ShownTurn = FMath::FInterpTo(ShownTurn, Turn, Dt, 6.0f);

    // Tail beat: faster the faster it swims (about one beat per 0.7 body lengths travelled), harder
    // with effort; gliding, the tail barely moves. On the line: the hanging fish's thrashing.
    const float E = bHanging ? HangEffort : HasAuthority() ? Effort : NetEffort;
    float Beat, Amp;
    if (bHanging)
    {
        Beat = S.TailBeat * (0.6f + 2.2f * E);
        Amp = 0.03f + 0.06f * E;
    }
    else
    {
        const float Hz = FMath::Min(7.0f, (0.35f + ShownSpeed / (0.7f * Len)) * (0.75f + 0.6f * E));
        Beat = 2 * PI * Hz;
        Amp = 0.022f + 0.07f * E + 0.015f * FMath::Clamp(ShownSpeed / S.CruiseSpeed, 0.0f, 1.0f);
    }
    // Keep the wave running on smoothly when the rate changes: shift the phase so the tail stays
    // where it was at this moment (the material bends by sin(u - Time * Rate + Phase)).
    Beat = FMath::RoundToFloat(Beat * 4) / 4;
    Amp = FMath::RoundToFloat(Amp * 400) / 400;
    if (Beat != LastBeat)
    {
        const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
        if (LastBeat >= 0) Phase = (float)FMath::Fmod(Phase + Now * (Beat - LastBeat), 2.0 * PI);
        LastBeat = Beat;
        Body->SetCustomPrimitiveDataFloat(0, Beat);
        Body->SetCustomPrimitiveDataFloat(2, Phase);
    }
    if (Amp != LastAmp) { LastAmp = Amp; Body->SetCustomPrimitiveDataFloat(1, Amp); }
    // The body curves into a turn (a C, head and tail towards the inside): curvature is the turn
    // rate over the speed, and a fish turning on the spot curls hardest.
    const float Curl = FMath::DegreesToRadians(ShownTurn) * 0.5f * Len / FMath::Max(ShownSpeed, 0.6f * Len);
    const float WantBend = bHanging ? 0.0f : FMath::Clamp(Curl, -0.3f, 0.3f);
    ShownBend = FMath::FInterpTo(ShownBend, WantBend, Dt, 5.0f);
    const float Bend = FMath::RoundToFloat(ShownBend * 200) / 200;
    // The mesh is turned round (its +Y is the fish's left), so a bend to the right is negative.
    if (Bend != LastBend) { LastBend = Bend; Body->SetCustomPrimitiveDataFloat(4, -Bend); }
}

void ALakeFish::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    // Out of the water on a line the angler's rod places the fish (HangOnLine).
    const bool bHanging = bLifted || GFrameCounter - LastHangFrame < 3;
    if (HasAuthority() && !bHanging)
    {
        // Fixed sub-steps keep the swimming model stable at low frame rates; far from every
        // player a few coarse steps are enough.
        const float StepLength = bNear ? 0.02f : 0.1f;
        const int32 Steps = FMath::Clamp(FMath::CeilToInt(DeltaSeconds / StepLength), 1, bNear ? 5 : 3);
        for (int32 I = 0; I < Steps; ++I) Simulate(DeltaSeconds / Steps);
        NetLocation = GetActorLocation();
        NetVelocity = Vel;
        NetRotation = GetActorRotation();
        NetEffort = Effort;
    }
    else if (!HasAuthority() && !bHanging)
    {
        // Between network updates: swim on with the replicated velocity, easing out the error.
        if (!Smooth.bReady) OnRep_NetLocation();
        Smooth.Advance(DeltaSeconds, GetWorld()->GetTimeSeconds());
        FVector P = Smooth.Location;
        P.Z = FMath::Clamp(P.Z, BottomZ(P.X, P.Y) + 3, LakeFish::SurfaceZ - 3);
        SetActorLocationAndRotation(P, Smooth.Rotation);
    }
    if (GetNetMode() != NM_DedicatedServer) UpdateVisual(DeltaSeconds);
}
