#include "LakeFish.h"
#include "LakeFood.h"
#include "LakeWater.h"
#include "LakeGame.h"
#include "LakeLife.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------------------------
// The species. Each has its passport in Content/LiveFish/Species/<id>.json (see the README.md
// there): its body, where it lives, how it swims, feeds, bites and fights, its mood with the time
// and the weather, how many the pond holds. The passports are read once, sorted by "order" (the
// EFishSpecies number); the four first ones are perch, roach, crucian and pike.
// ---------------------------------------------------------------------------------------------
namespace
{
    const TCHAR* GBaitNames[] = { TEXT("червь"), TEXT("опарыш"), TEXT("хлеб") };
    const TCHAR* GBaitKeys[] = { TEXT("worm"), TEXT("maggot"), TEXT("bread") };

    float Depth01(float Z, float X, float Y)
    {
        const float Bottom = ALakeFish::BottomZ(X, Y);
        return FMath::Clamp((LakeFish::SurfaceZ - Z) / FMath::Max(1.0f, LakeFish::SurfaceZ - Bottom), 0.0f, 1.0f);
    }

    TArray<FString> GProblems;

    /** Fields in a file the passport does not have (a typo would silently be ignored otherwise). */
    void CheckFields(const UStruct* Struct, const TSharedPtr<FJsonObject>& Json, const FString& Where, TArray<FString>& Problems)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Json->Values)
        {
            const FProperty* Found = nullptr;
            for (TFieldIterator<FProperty> It(Struct); It && !Found; ++It)
                if (FJsonObjectConverter::StandardizeCase(It->GetName()).Equals(Field.Key, ESearchCase::IgnoreCase)) Found = *It;
            if (!Found) { Problems.Add(FString::Printf(TEXT("%s: unknown field \"%s\""), *Where, *Field.Key)); continue; }
            if (const FStructProperty* Inner = CastField<FStructProperty>(Found))
            {
                if (Field.Value->Type == EJson::Object) CheckFields(Inner->Struct, Field.Value->AsObject(), Where + TEXT(".") + Field.Key, Problems);
            }
            else if (const FArrayProperty* List = CastField<FArrayProperty>(Found))
            {
                if (const FStructProperty* Item = CastField<FStructProperty>(List->Inner); Item && Field.Value->Type == EJson::Array)
                    for (int32 I = 0; I < Field.Value->AsArray().Num(); ++I)
                        if (Field.Value->AsArray()[I]->Type == EJson::Object)
                            CheckFields(Item->Struct, Field.Value->AsArray()[I]->AsObject(), FString::Printf(TEXT("%s.%s[%d]"), *Where, *Field.Key, I), Problems);
            }
        }
    }

    TArray<FFishSpeciesInfo> LoadSpecies()
    {
        TArray<FFishSpeciesInfo> Out;
        const FString Dir = FPaths::ProjectContentDir() / TEXT("LiveFish/Species");
        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), true, false);
        Files.Sort();
        for (const FString& File : Files)
        {
            FString Text;
            FFishSpeciesInfo Info;
            if (!FFileHelper::LoadFileToString(Text, *(Dir / File))) { GProblems.Add(FString::Printf(TEXT("%s: cannot be read"), *File)); continue; }
            if (LakeFish::ReadSpecies(Text, File, Info, GProblems)) Out.Add(Info);
        }
        Out.Sort([](const FFishSpeciesInfo& A, const FFishSpeciesInfo& B) { return A.Order < B.Order; });
        for (int32 I = 0; I < Out.Num(); ++I)
            if (Out[I].Order != I) GProblems.Add(FString::Printf(TEXT("%s: \"order\" is %d, expected %d (0, 1, 2, ... without gaps)"), *Out[I].Id, Out[I].Order, I));
        for (const FString& Problem : GProblems) UE_LOG(LogTemp, Error, TEXT("LIVE_FISH species file %s"), *Problem);
        checkf(Out.Num() >= 4, TEXT("Live Fish needs the species files in %s (perch, roach, crucian, pike); found %d"), *Dir, Out.Num());
        FString Names;
        for (const FFishSpeciesInfo& Info : Out) Names += TEXT(" ") + Info.Id;
        UE_LOG(LogTemp, Display, TEXT("LIVE_FISH %d species:%s"), Out.Num(), *Names);
        return Out;
    }
    const TArray<FFishSpeciesInfo>& AllSpecies()
    {
        static const TArray<FFishSpeciesInfo> Species = LoadSpecies();
        return Species;
    }
}

float FFishDiet::Of(EFishFood Food) const
{
    switch (Food)
    {
    case EFishFood::Plankton: return Plankton;
    case EFishFood::Insects: return Insects;
    case EFishFood::Benthos: return Benthos;
    case EFishFood::Plants: return Plants;
    case EFishFood::Fry: return Fry;
    case EFishFood::Fish: return Fish;
    case EFishFood::Groundbait: return Groundbait;
    default: return 0;
    }
}
FFishDiet LakeFish::DietOf(EFishSpecies Species, float Size01)
{
    const FFishSpeciesInfo& S = Info(Species);
    if (S.DietBig.IsEmpty()) return S.Diet;
    const float T = FMath::Clamp(Size01, 0.0f, 1.0f);
    FFishDiet D;
    D.Plankton = FMath::Lerp(S.Diet.Plankton, S.DietBig.Plankton, T);
    D.Insects = FMath::Lerp(S.Diet.Insects, S.DietBig.Insects, T);
    D.Benthos = FMath::Lerp(S.Diet.Benthos, S.DietBig.Benthos, T);
    D.Plants = FMath::Lerp(S.Diet.Plants, S.DietBig.Plants, T);
    D.Fry = FMath::Lerp(S.Diet.Fry, S.DietBig.Fry, T);
    D.Fish = FMath::Lerp(S.Diet.Fish, S.DietBig.Fish, T);
    D.Groundbait = FMath::Lerp(S.Diet.Groundbait, S.DietBig.Groundbait, T);
    return D;
}

float FFishSpeciesInfo::BaitLike(EBait Bait) const
{
    const float* Like = Baits.Find(GBaitKeys[FMath::Clamp((int32)Bait, 0, 2)]);
    return Like ? *Like : 0.0f;
}
const FFishSpeciesInfo& LakeFish::Info(EFishSpecies Species)
{
    const TArray<FFishSpeciesInfo>& All = AllSpecies();
    return All[FMath::Clamp((int32)Species, 0, All.Num() - 1)];
}
int32 LakeFish::NumSpecies() { return AllSpecies().Num(); }
const TArray<FString>& LakeFish::SpeciesProblems() { AllSpecies(); return GProblems; }
bool LakeFish::ReadSpecies(const FString& Text, const FString& File, FFishSpeciesInfo& Info, TArray<FString>& Problems)
{
    TSharedPtr<FJsonObject> Json;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
    {
        Problems.Add(FString::Printf(TEXT("%s: not a valid JSON file"), *File));
        return false;
    }
    if (!FJsonObjectConverter::JsonObjectToUStruct(Json.ToSharedRef(), &Info))
    {
        Problems.Add(FString::Printf(TEXT("%s: could not be read into a species passport"), *File));
        return false;
    }
    Info.Id = FPaths::GetBaseFilename(File);
    CheckFields(FFishSpeciesInfo::StaticStruct(), Json, File, Problems);
    // Numbers that would break the fish.
    if (Info.MaxWeight <= Info.MinWeight || Info.MinWeight <= 0) Problems.Add(FString::Printf(TEXT("%s: minWeight must be above 0 and below maxWeight"), *File));
    if (Info.CruiseSpeed <= 0 || Info.BurstSpeed < Info.CruiseSpeed) Problems.Add(FString::Printf(TEXT("%s: cruiseSpeed must be above 0 and burstSpeed at least as fast"), *File));
    if (Info.SurfaceClearance < 0 || Info.SurfaceClearance > 150) Problems.Add(FString::Printf(TEXT("%s: surfaceClearance goes from 0 to 150 cm"), *File));
    if (Info.Bite.Take.IsEmpty()) Problems.Add(FString::Printf(TEXT("%s: bite.take is empty - how does it take the bait?"), *File));
    if (Info.Mesh.IsEmpty()) Problems.Add(FString::Printf(TEXT("%s: no mesh"), *File));
    if (Info.Stock.SchoolSize < 1) Problems.Add(FString::Printf(TEXT("%s: stock.schoolSize must be 1 or more"), *File));
    if (Info.Stock.Count < 0) Problems.Add(FString::Printf(TEXT("%s: stock.count must be 0 (not in the pond) or more"), *File));
    {
        const FFishStock& St = Info.Stock;
        float Below = Info.MinWeight, Shares = 0;
        for (const FFishSizeBand& B : St.Sizes)
        {
            if (B.UpTo <= Below || B.Share < 0) Problems.Add(FString::Printf(TEXT("%s: stock.sizes: upTo must grow from minWeight, share must be 0 or more"), *File));
            Below = B.UpTo;
            Shares += B.Share;
        }
        if (St.Sizes.Num() && (Shares <= 0 || !FMath::IsNearlyEqual(Below, Info.MaxWeight, 0.01f)))
            Problems.Add(FString::Printf(TEXT("%s: stock.sizes: shares must add up to more than 0 and the last upTo be maxWeight"), *File));
        Below = 0;
        for (const FFishGroupSize& G : St.Groups)
        {
            if (G.UpTo <= Below || G.Min < 1 || G.Max < G.Min || G.Drift <= 0 || G.DeeperM < 0 || G.Apart < 0)
                Problems.Add(FString::Printf(TEXT("%s: stock.groups: upTo must grow, 1 <= min <= max, drift above 0, deeperM and apart 0 or more"), *File));
            Below = G.UpTo;
        }
        if (St.Spots.Num() && St.Groups.IsEmpty()) Problems.Add(FString::Printf(TEXT("%s: stock.spots need stock.groups"), *File));
        for (const TPair<FString, int32>& Spot : St.Spots)
        {
            bool bKnown = false;
            for (const LakeMap::FSpot& Known : LakeMap::Spots()) bKnown |= Spot.Key == Known.Name;
            if (!bKnown || Spot.Value < 0) Problems.Add(FString::Printf(TEXT("%s: stock.spots: no fishing spot \"%s\" (or a count below 0)"), *File, *Spot.Key));
        }
    }
    if (Info.Name.IsEmpty() || Info.Noun.IsEmpty()) Problems.Add(FString::Printf(TEXT("%s: name and noun are needed"), *File));
    if (Info.Diet.IsEmpty()) Problems.Add(FString::Printf(TEXT("%s: diet is empty - what does it eat? (plankton, insects, benthos, plants, fry, fish)"), *File));
    const FFishFightStyle& F = Info.Fight;
    auto FightProblem = [&](const TCHAR* Detail) { Problems.Add(FString::Printf(TEXT("%s: fight.%s"), *File, Detail)); };
    for (float V : { F.PullMin, F.PullMax, F.PauseMin, F.PauseMax, F.BurstMin, F.BurstMax, F.SeriesGap,
        F.Recovery, F.EffortCost, F.Variation, F.JumpSpeed, F.JumpCooldown, F.ShoreDistance, Info.Stamina, Info.Strength, Info.RunBurst })
        if (!FMath::IsFinite(V)) FightProblem(TEXT("parameters must be finite"));
    for (float V : { F.AlongWeight, F.AwayWeight, F.DiveWeight, F.SurfaceWeight })
        if (!FMath::IsFinite(V) || V < 0) FightProblem(TEXT("direction weights must be finite and nonnegative"));
    if (F.AlongWeight + F.AwayWeight + F.DiveWeight + F.SurfaceWeight <= 0) FightProblem(TEXT("direction weights must have a positive sum"));
    for (float V : { F.PullSpeed, F.PauseSpeed, F.BurstChance, F.JumpChance, F.ShoreRunChance })
        if (!FMath::IsFinite(V) || V < 0 || V > 1) FightProblem(TEXT("speeds and probabilities must be in 0..1"));
    if (F.Variation < 0 || F.Variation > 0.5f) FightProblem(TEXT("variation must be in 0..0.5"));
    if (F.PullMin <= 0 || F.PullMax < F.PullMin || F.PauseMin <= 0 || F.PauseMax < F.PauseMin
        || F.BurstMin < 0 || F.BurstMax < F.BurstMin || (F.BurstMin == 0 && F.BurstMax != 0)) FightProblem(TEXT("duration ranges are invalid"));
    if (F.SeriesMin < 1 || F.SeriesMax < F.SeriesMin || F.SeriesMax > 12 || F.SeriesGap <= 0) FightProblem(TEXT("series needs 1..12 pulses and a positive gap"));
    if (F.Recovery < 0 || F.EffortCost <= 0 || F.JumpSpeed <= 0 || F.JumpCooldown < 1 || F.ShoreDistance <= 0
        || Info.Stamina <= 0 || Info.Strength <= 0 || Info.RunBurst <= 0) FightProblem(TEXT("reserves, forces and special manoeuvre parameters are invalid"));
    for (const FFishDiet* Diet : { &Info.Diet, &Info.DietBig })
        for (float V : { Diet->Plankton, Diet->Insects, Diet->Benthos, Diet->Plants, Diet->Fry, Diet->Fish, Diet->Groundbait })
            if (V < 0 || V > 1.5f) { Problems.Add(FString::Printf(TEXT("%s: diet values go from 0 to 1"), *File)); break; }
    return true;
}
const FFishGait& LakeFish::Gait(EFishSpecies Species) { return Info(Species).Gait; }
const TCHAR* LakeFish::BaitName(EBait Bait) { return GBaitNames[FMath::Clamp((int32)Bait, 0, 2)]; }
bool LakeFish::SchoolsTogether(EFishSpecies A, EFishSpecies B)
{
    if (A == B) return true;
    const FFishSpeciesInfo& SA = Info(A);
    const FFishSpeciesInfo& SB = Info(B);
    return SA.Stock.SchoolsWith.Contains(SB.Id) || SB.Stock.SchoolsWith.Contains(SA.Id);
}
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
    const float Growth = Info(Species).GirthGrowth;
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
float LakeFish::BaitInterest(EFishSpecies Species, EBait Bait, float DepthFraction, float FeedColumn)
{
    const FFishSpeciesInfo& S = Info(Species);
    float DepthMatch;
    if (FeedColumn >= 0)
        // Feeding in one layer (on the bottom, at the top, with the plankton): a bait there is found
        // at once, one far off it seldom.
        DepthMatch = FMath::Clamp(1.0f - FMath::Max(0.0f, FMath::Abs(DepthFraction - FeedColumn) - 0.15f) * 2.0f, 0.15f, 1.0f);
    else
    {
        // Bait outside the species' depth band is found less often (a bottom feeder rarely rises to it).
        const float Mid = (S.DepthMin + S.DepthMax) * 0.5f, Half = (S.DepthMax - S.DepthMin) * 0.5f + 0.15f;
        DepthMatch = FMath::Clamp(1.0f - FMath::Max(0.0f, FMath::Abs(DepthFraction - Mid) - Half) * 2.5f, 0.15f, 1.0f);
    }
    return S.BaitLike(Bait) * DepthMatch;
}
FBiteStep LakeFish::NextBiteStep(EFishSpecies Species, FBiteState& B, float Size01, float A, bool bOnBottom, bool bWary, float Like, float Disturb, FRandomStream& R)
{
    const FFishBiteStyle& Style = Info(Species).Bite;
    // Sluggish (poor appetite, bright calm water): more testing, shorter takes, sooner let go.
    const float Lazy = FMath::Clamp(FMath::Clamp(1.0f - A, 0.0f, 0.9f) + (bWary ? 0.3f : 0.0f), 0.0f, 1.0f);
    const float Pace = FMath::Lerp(1.1f, 0.75f, Lazy);
    auto Make = [&](const FFishBiteMove& Move)
    {
        FBiteStep Step;
        Step.Motion = Move.Motion;
        Step.Duration = (Move.SecondsMax > Move.Seconds ? R.FRandRange(Move.Seconds, Move.SecondsMax) : Move.Seconds) * (Move.bPaced ? FMath::Max(Pace, Move.MinPace) : 1.0f);
        Step.Amount = (Move.AmountMax > Move.Amount ? R.FRandRange(Move.Amount, Move.AmountMax) : Move.Amount) * FMath::Lerp(Move.AmountSmall, Move.AmountBig, Size01);
        Step.bTake = Move.bTake;
        Step.Hook = Move.Hook;
        B.Felt += Move.Felt;
        return Step;
    };
    auto LetGo = [&B](FBiteStep Step) { Step.bLetGo = true; B.bDone = true; return Step; };
    auto Still = [](float Seconds) { FBiteStep Step; Step.Motion = EBiteMotion::None; Step.Duration = Seconds; Step.bTake = false; return Step; };
    // Whatever it felt: the rig moved under its nose (the bait jumps), and the float pulling back.
    B.Felt += Disturb;
    if (B.bDone) return LetGo(Still(0.1f));

    // 1. Testing the bait: a try, a pause, then the choice - take it, try again, or let it be.
    if (B.Phase == 0)
    {
        if (Disturb > 0.5f) return LetGo(Still(0.1f));
        if (B.bTried)
        {
            B.bTried = false;
            return Still(R.FRandRange(Style.PauseMin, FMath::Max(Style.PauseMin, Style.PauseMax)));
        }
        const int32 MinTries = Style.MinTries + (Size01 < 0.3f ? Style.SmallExtraTries : 0) + (Lazy > 0.4f ? 1 : 0);
        if (B.Tries >= MinTries)
        {
            const float Take = Style.TakeChance * FMath::Lerp(Style.TakeSmall, Style.TakeBig, Size01) * (0.5f + 0.5f * Like)
                * FMath::Lerp(0.6f, 1.1f, FMath::Clamp(A, 0.0f, 1.2f) / 1.2f) * (1 + 0.25f * B.Tries) * FMath::Max(0.0f, 1 - B.Felt);
            const float Leave = (0.25f * (1 - Like) + 0.1f * Lazy) * (1 + 2 * B.Felt);
            const float U = R.FRand();
            if (U < Leave || (B.Tries >= Style.MaxTries && Like < 0.4f)) return LetGo(Still(0.1f));
            if (U < Leave + Take || B.Tries >= Style.MaxTries) B.Phase = 1;
        }
        if (B.Phase == 0)
        {
            ++B.Tries;
            B.bTried = true;
            if (Style.Tries.IsEmpty()) { FBiteStep Peck = Still(0.25f); Peck.Motion = EBiteMotion::Peck; Peck.Amount = 1.5f; return Peck; }
            // One of its tries, drawn by weight.
            float Total = 0;
            for (const FFishBiteMove& Try : Style.Tries) Total += FMath::Max(0.0f, Try.Weight);
            float Pick = R.FRand() * Total;
            for (const FFishBiteMove& Try : Style.Tries)
                if ((Pick -= FMath::Max(0.0f, Try.Weight)) <= 0) return Make(Try);
            return Make(Style.Tries.Last());
        }
    }

    // 2. The take, move by move (from the bottom, some lift it first); after the last, not struck,
    // the fish lets go of the bait.
    if (B.Felt > Style.Sensitive) return LetGo(Still(0.1f));
    if (B.Phase == 1) B.Variant = bOnBottom && !Style.TakeFromBottom.IsEmpty() && R.FRand() < Style.FromBottomChance ? 1 : 0;
    const TArray<FFishBiteMove>& Moves = B.Variant == 1 ? Style.TakeFromBottom : Style.Take;
    const int32 Index = B.Phase++ - 1;
    if (!Moves.IsValidIndex(Index)) return LetGo(Still(0.1f));
    const FBiteStep Step = Make(Moves[Index]);
    return Index == Moves.Num() - 1 ? LetGo(Step) : Step;
}
TArray<FBiteStep> LakeFish::MakeBite(EFishSpecies Species, float Size01, float A, bool bOnBottom, bool bWary, FRandomStream& R)
{
    TArray<FBiteStep> Out;
    FBiteState State;
    for (int32 I = 0; I < 60; ++I)
    {
        Out.Add(NextBiteStep(Species, State, Size01, A, bOnBottom, bWary, 1.0f, 0.0f, R));
        if (Out.Last().bLetGo) break;
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
    case EBiteMotion::Sink: return Amount >= 100 ? TEXT("Поплавок резко утащило под воду и в сторону!") : TEXT("Поплавок ушёл под воду!");
    case EBiteMotion::Hold: return TEXT("Поплавок замер — рыба держит наживку.");
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
    // Warm-water fish (crucian) come up into the shallows to feed at night.
    const float NightShallows = Info(Species).Feeding.NightShallows * Night;
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
    const bool bPredator = Info(Species).Feeding.bPredator;
    return Goal == EFishGoal::Feed ? (bPredator ? TEXT("охотятся") : TEXT("кормятся")) : Goal == EFishGoal::Rest ? TEXT("отдыхают") : TEXT("гуляют");
}
namespace
{
    float ScoreHabitat(const FFishHabitat& H, EFishSpecies Species, float X, float Y, const FFishConditions* Conditions, float DeeperM = 0);
}
float LakeFish::HabitatScore(EFishSpecies Species, float X, float Y, const FFishConditions* Conditions, float DeeperM)
{
    return ScoreHabitat(Info(Species).Habitat, Species, X, Y, Conditions, DeeperM);
}
float LakeFish::DrawWeight(EFishSpecies Species, FRandomStream& Random, float Like)
{
    const FFishSpeciesInfo& S = Info(Species);
    const TArray<FFishSizeBand>& Bands = S.Stock.Sizes;
    // No bands: most fish are small, big ones are rare.
    if (Bands.IsEmpty())
        return Like >= 0 ? FMath::Clamp(Like * Random.FRandRange(0.8f, 1.2f), S.MinWeight, S.MaxWeight)
                         : FMath::Lerp(S.MinWeight, S.MaxWeight, FMath::Pow(Random.FRand(), 2.5f));
    int32 Band = Bands.Num() - 1;
    if (Like >= 0)
    {
        for (int32 I = 0; I < Bands.Num(); ++I) if (Like <= Bands[I].UpTo) { Band = I; break; }
    }
    else
    {
        float Total = 0;
        for (const FFishSizeBand& B : Bands) Total += FMath::Max(0.0f, B.Share);
        float Pick = Random.FRand() * Total;
        for (int32 I = 0; I < Bands.Num(); ++I) if ((Pick -= FMath::Max(0.0f, Bands[I].Share)) <= 0) { Band = I; break; }
    }
    const float Lo = Band > 0 ? Bands[Band - 1].UpTo : S.MinWeight;
    return FMath::Clamp(FMath::Lerp(Lo, Bands[Band].UpTo, Random.FRand()), S.MinWeight, S.MaxWeight);
}
const FFishGroupSize* LakeFish::GroupFor(EFishSpecies Species, float WeightKg)
{
    const TArray<FFishGroupSize>& Groups = Info(Species).Stock.Groups;
    for (const FFishGroupSize& G : Groups) if (WeightKg <= G.UpTo) return &G;
    return Groups.Num() ? &Groups.Last() : nullptr;
}
float LakeFish::DeeperM(EFishSpecies Species, float WeightKg)
{
    const FFishGroupSize* G = GroupFor(Species, WeightKg);
    return G ? G->DeeperM : 0.0f;
}
int32 LakeFish::UsualSchool(EFishSpecies Species, float WeightKg)
{
    const FFishGroupSize* G = GroupFor(Species, WeightKg);
    return G ? FMath::DivideAndRoundUp(G->Min + G->Max, 2) : Info(Species).Stock.SchoolSize;
}
namespace
{
float ScoreHabitat(const FFishHabitat& H, EFishSpecies Species, float X, float Y, const FFishConditions* Conditions, float DeeperM)
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
    const float Min = FMath::Max(0.25f, H.MinDepthM + Shift), Max = FMath::Max(Min + 0.4f, H.MaxDepthM + DeeperM + FMath::Max(0.0f, Shift));
    const float DepthFit = FMath::SmoothStep(Min - 0.4f, Min, D) * (1 - FMath::SmoothStep(Max, Max + 0.6f, D));
    // The wind drives food to the bank it blows onto, and the fish follow.
    float Downwind = 1;
    if (Conditions && Conditions->Wind > 0.15f)
    {
        const float SD = LakeMap::ShoreDistance(X, Y) / 100;
        const FVector2D Out = LakeMap::NearestShore(FVector2D(X, Y)).Outward;
        Downwind = 1 + 0.8f * Conditions->Wind * FMath::Max(0.0f, FVector2D::DotProduct(Out, Conditions->WindDir)) * (1 - FMath::SmoothStep(10.0f, 25.0f, SD));
    }
    // Small fish keep out of bright open shallows by day: herons, grebes and terns hunt there.
    float Exposed = 0;
    if (Conditions && Info(Species).Feeding.LightShy > 0)
    {
        const float Light = FMath::SmoothStep(5.0f, 25.0f, Conditions->SunElevation) * (1 - 0.5f * Conditions->Cloud);
        const float Shallow = 1 - FMath::SmoothStep(0.5f, 1.5f, D);
        const float Shelter = FMath::Max3(LakeMap::LilyAmount(X, Y), Z.Cover, 0.7f * Z.ReedEdge);
        Exposed = FMath::Min(0.9f, Info(Species).Feeding.LightShy * Light * Shallow * (1 - Shelter));
    }
    // Water it can live in: enough oxygen, not too hot, somewhere in the column (see LakeWater).
    const float Livable = Conditions ? 0.05f + 0.95f * LakeWater::Livable(Species, *Conditions, X, Y) : 1.0f;
    return (0.03f + Like) * DepthFit * Downwind * (1 - Exposed) * Livable;
}
}
FVector LakeFish::PickSpot(EFishSpecies Species, FRandomStream& Random, FVector2D Near, float Range, const FFishConditions* Conditions, EFishGoal Goal,
                           const TFunction<float(const FVector2D&)>& Weigh, float DeeperM)
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
        // Fish dislike the shallows: a target in nearly dry water is not worth swimming to, and the
        // shallower the water the less it is drawn there (an ambusher still takes the weed edge, and
        // a feeding one will risk a shallow patch). The fry shoals are not ALakeFish and keep to the
        // shallows by design.
        const float Depth = LakeMap::Depth(P.X, P.Y);
        if (Depth < 25.0f) continue;
        const float MinDepth = FMath::Max(20.0f, Habitat.MinDepthM * 100.0f);
        float Comfort = FMath::Clamp(0.35f + 0.65f * Depth / MinDepth, 0.35f, 1.0f);
        const float ShoreSpan = Info(Species).Feeding.bHuntsFry ? 260.0f : 150.0f;
        Comfort *= FMath::Clamp(LakeMap::ShoreDistance(P.X, P.Y) / ShoreSpan, 0.3f, 1.0f);
        if (Habitat.bAmbush) Comfort = FMath::Max(Comfort, 0.8f);
        // A feeding fish will still go for a rich patch in the shallows — reluctantly, and only
        // partly: food outweighs comfort. A fry hunter (a perch) keeps its distance even then.
        if (Goal == EFishGoal::Feed && !Info(Species).Feeding.bHuntsFry)
            Comfort = FMath::Lerp(1.0f, Comfort, 0.15f);
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
        const float Score = ScoreHabitat(Habitat, Species, P.X, P.Y, Conditions, DeeperM) * Comfort * Random.FRandRange(0.6f, 1.0f) * (Weigh ? Weigh(P) : 1.0f);
        if (Score > BestScore) { BestScore = Score; Best = P; }
    }
    if (BestScore < 0)
    {
        // No candidate passed at all: a spot in nearly dry water is no target, head for open water.
        if (!LakeMap::IsWater(Best.X, Best.Y) || LakeMap::Depth(Best.X, Best.Y) < 25.0f)
            Best = LakeMap::Centre;
    }
    const float Bottom = ALakeFish::BottomZ(Best.X, Best.Y);
    // Heat and bright sun also press the fish towards the bottom of the water column.
    const float Lower = Conditions ? FMath::Clamp(DepthShiftM(Species, *Conditions) * 0.25f, -0.2f, 0.3f) : 0.0f;
    const float Column = FMath::Clamp(Random.FRandRange(S.DepthMin, S.DepthMax) + Lower, 0.0f, 1.0f);
    // Never higher than the species keeps under the surface (a sip or a gulp is a moment, not a place).
    const float Z = FMath::Min(FMath::Lerp(SurfaceZ - 10, Bottom + 8, Column), CeilingAt(Species, Best.X, Best.Y, S.MeshLength));
    return FVector(Best, Conditions ? FMath::Min(LakeWater::ToLivable(Species, *Conditions, Best.X, Best.Y, Z), CeilingAt(Species, Best.X, Best.Y, S.MeshLength)) : Z);
}
float LakeFish::CeilingAt(EFishSpecies Species, float X, float Y, float LengthCm)
{
    // The fish's origin is at its mouth, about an eighth of its length below the back.
    const float Under = SurfaceZ - Info(Species).SurfaceClearance - 0.12f * LengthCm;
    const float Middle = 0.5f * (ALakeFish::BottomZ(X, Y) + SurfaceZ);
    return FMath::Min(SurfaceZ - 4, FMath::Max(Under, Middle));
}
FVector LakeFish::OffTheBank(FVector P, float Margin, float MinDepth)
{
    for (int32 Step = 0; Step < 30; ++Step)
    {
        if (LakeMap::IsWater(P.X, P.Y) && LakeMap::ShoreDistance(P.X, P.Y) >= Margin && LakeMap::Depth(P.X, P.Y) >= MinDepth) break;
        // Straight out from the nearest bank (the distance field's slope), 20 cm at a time.
        const FVector2D Out = -LakeMap::NearestShore(FVector2D(P)).Outward;
        P.X += Out.X * 20;
        P.Y += Out.Y * 20;
    }
    return P;
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
    DOREPLIFETIME(ALakeFish, bNetBurst);
    DOREPLIFETIME(ALakeFish, Goal);
    DOREPLIFETIME(ALakeFish, NetFeedOn);
    DOREPLIFETIME(ALakeFish, bLifted);
}

void ALakeFish::Setup(EFishSpecies InSpecies, float InWeight, FVector Home, int32 InSchool, int32 Seed)
{
    Species = InSpecies;
    WeightKg = InWeight;
    Deeper = LakeFish::DeeperM(Species, WeightKg);
    HomePoint = HomeAnchor = Home;
    School = InSchool;
    Random.Initialize(Seed >= 0 ? Seed : GetUniqueID() * 7919 + (int32)(InWeight * 1000));
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
    // Reuse an individual draw without changing the random sequence of free swimming.
    FightIndividual = FMath::Lerp(1 - S.Fight.Variation, 1 + S.Fight.Variation, Phase / (2 * PI));
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

FVector ALakeFish::MouthOffset() const
{
    const auto& S = LakeFish::Info(Species);
    const float Len = LakeFish::LengthCm(Species, WeightKg);
    // Same 180 degree component rotation, translation and girth as UpdateVisual.
    const float Girth = LakeFish::Girth(Species, WeightKg);
    return FVector((0.4f - S.MouthPoint.X) * Len, -S.MouthPoint.Y * Len * Girth, S.MouthPoint.Z * Len * Girth);
}
FVector ALakeFish::MouthLocation() const { return GetActorLocation() + GetActorQuat().RotateVector(MouthOffset()); }
float ALakeFish::ContactRadius() const
{
    return FMath::Clamp(LakeFish::LengthCm(Species, WeightKg) * LakeFish::Info(Species).MouthRadius, 0.4f, 2.0f);
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
    PickRoamTarget(bLocal);
    // Wherever it goes, it keeps to a layer it can live in (out of the stale water under the
    // thermocline, out of the hot top).
    FFishConditions Conditions;
    if (Leader() == this && GetConditions(Conditions)) RoamTarget.Z = LakeWater::ToLivable(Species, Conditions, RoamTarget.X, RoamTarget.Y, RoamTarget.Z);
    // And under its clearance, unless it is going up to sip insects off the top.
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const bool bSip = Goal == EFishGoal::Feed && (FeedOn == EFishFood::Insects || (FeedOn == EFishFood::None && S.Feeding.SipAtDusk > 0 && RoamTarget.Z > LakeFish::SurfaceZ - 15));
    if (!bSip) RoamTarget.Z = FMath::Min(RoamTarget.Z, LakeFish::CeilingAt(Species, RoamTarget.X, RoamTarget.Y, LakeFish::LengthCm(Species, WeightKg)));
}

void ALakeFish::PickRoamTarget(bool bLocal)
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
        FeedOn = Lead->FeedOn;
        return;
    }
    const bool bHomeBound = S.IsSchooling() || S.Habitat.bAmbush;
    // A much-fished place (catches, fish lost, casts) is avoided until it settles (ALakeLife::PressureAt).
    const ALakeLife* PressureLife = ALakeLife::Get(GetWorld());
    const TFunction<float(const FVector2D&)> ShyOfPressure = [this, PressureLife, &S](const FVector2D& P)
    {
        return PressureLife ? 1 - FMath::Min(0.8f, 0.3f * S.Learning.Fright * PressureLife->PressureAt(FVector(P, 0))) : 1.0f;
    };
    // A school or an ambusher whose home is much fished moves it further off (up to 20 m beyond its
    // usual range) and comes back once the place has settled.
    if (bHomeBound && PressureLife)
    {
        const float Felt = S.Learning.Fright * PressureLife->PressureAt(HomePoint);
        // (Roach after two or three catches or fish lost, crucian and pike sooner, perch keep biting longer.)
        if (Felt > 1.2f && Random.FRand() < 0.5f)
            HomePoint = LakeFish::PickSpot(Species, Random, FVector2D(HomeAnchor), HomeDrift + 2000 * FMath::Min(1.0f, Felt / 2.4f), C, EFishGoal::Cruise, ShyOfPressure, Deeper);
    }
    if (Goal == EFishGoal::Cruise)
    {
        // Schools and ambushers keep to their home area; loners wander through good water nearby.
        // Now and then a home moves to better water for the conditions (deeper in the heat, to the
        // bank the wind blows onto), so schools drift with the weather - around the place they were
        // stocked in, not step by step away from it.
        if (bHomeBound && bWeather && Random.FRand() < 0.08f) HomePoint = LakeFish::PickSpot(Species, Random, FVector2D(HomeAnchor), HomeDrift, C, EFishGoal::Cruise, ShyOfPressure, Deeper);
        const float Range = S.Habitat.bAmbush ? 350.0f : S.IsSchooling() ? 900.0f : 2500.0f;
        // A cruising fish carries on more or less the way it is going rather than doubling back.
        const FVector Ahead = GetActorForwardVector().GetSafeNormal2D();
        for (int32 Try = 0; Try < 4; ++Try)
        {
            RoamTarget = LakeFish::PickSpot(Species, Random, FVector2D(bHomeBound ? HomePoint : GetActorLocation()), Range, C, EFishGoal::Cruise, ShyOfPressure, Deeper);
            if (FVector::DotProduct((RoamTarget - GetActorLocation()).GetSafeNormal2D(), Ahead) > -0.2f) break;
        }
        // Warm still sunny water: roach and crucian bask high in the water, a pike lies up there in
        // the shallows (no higher than each keeps under the surface, see ChooseRoamTarget).
        if (bWeather && Conditions.WaterTempC > 17 && Conditions.SunElevation > 15 && Conditions.Wind < 0.3f && Conditions.Rain < 0.15f)
        {
            const float Chance = S.Feeding.Bask;
            if (Random.FRand() < Chance)
                RoamTarget.Z = FMath::Max(LakeFish::SurfaceZ - Random.FRandRange(9.0f, 20.0f), BottomZ(RoamTarget.X, RoamTarget.Y) + 10);
        }
        return;
    }
    // What it eats: the natural food of the pond as its diet and the food at hand make best (without
    // the food field or a diet - a test world, a species file without one - the old ways below).
    PreyFry = -1;
    const ALakeLife* Life = ALakeLife::Get(GetWorld());
    const FLakeFoodField* Field = Life && bWeather && Life->Food().IsReady() ? &Life->Food() : nullptr;
    const FFishDiet Diet = LakeFish::DietOf(Species, Size01);
    const bool bByFood = Goal == EFishGoal::Feed && Field && !Diet.IsEmpty();
    // Hungry perch go after the nearest fry (those living at a fishing spot only after fry close to
    // their home, so a chase does not lead them off step by step) - if fry are what they eat.
    int32 Fry = -1;
    if (Goal == EFishGoal::Feed && S.Feeding.bHuntsFry && Life && (!bByFood || Diet.Fry > 0) && !(bLocal && (LakeFood::IsNatural(FeedOn) || FeedOn == EFishFood::Groundbait)))
    {
        const bool bResident = HomeDrift < 1000;
        Fry = Life->NearestFry(bResident ? HomePoint : GetActorLocation(), bResident ? 1500.0f : 4500.0f);
    }
    auto ChaseFry = [&] { PreyFry = Fry; FeedOn = EFishFood::Fry; RoamTarget = FryChaseTarget(Life->FryLocation(Fry)); };
    if (Fry >= 0 && !bByFood) { ChaseFry(); return; }
    // A pike hunts from a lie at the edge of cover.
    if (Goal == EFishGoal::Feed && S.Habitat.bAmbush && !bLocal)
    {
        FeedOn = EFishFood::Fish;
        ChooseLie();
        return;
    }
    // Feeding and resting places: near home for schools and ambushers (closer for those that live
    // at a fishing spot, see HomeDrift), anywhere near for loners; once there, a fish browses along
    // a few metres at a time (a crucian a step at a time).
    const float Range = bLocal ? S.Feeding.BrowseStep : bHomeBound ? FMath::Min(1500.0f, HomeDrift + 300) : 1200.0f;
    const FVector2D Near = bLocal || !bHomeBound ? FVector2D(GetActorLocation()) : FVector2D(HomePoint);
    if (bByFood)
    {
        // Groundbait whose smell has reached it: how much it goes for it and how rich it is; browsing
        // on it, it keeps to it while some is left.
        float Smell = 0;
        const int32 GroundIndex = Field->SmellGroundbait(FVector2D(GetActorLocation()), Smell);
        const FGroundbaitSpot* Ground = GroundIndex >= 0 ? &Field->Groundbait()[GroundIndex] : nullptr;
        // Even a faint smell tells where rich food lies: it is worth half its value at least.
        const float GroundValue = Ground && Smell > 0.03f ? LakeFood::GroundbaitAppeal(Diet, *Ground) * LakeFood::GroundbaitRichness(*Ground) * (0.5f + 0.5f * Smell) : 0.0f;
        auto ToGroundbait = [&]
        {
            const FVector2D At = Ground->Location + FVector2D(Random.FRandRange(-90.0f, 90.0f), Random.FRandRange(-90.0f, 90.0f));
            if (FeedOn != EFishFood::Groundbait || !bLocal) FeedFound = GroundValue;
            FeedOn = EFishFood::Groundbait;
            NextFoodCheck = SimTime + 4;
            RoamTarget = FVector(At, LakeFood::FeedingZ(EFishFood::Groundbait, At.X, At.Y, Conditions, Random));
        };
        if (bLocal && FeedOn == EFishFood::Groundbait && Ground && Ground->Balls > 0.1f) { ToGroundbait(); return; }
        // Where its food is: each place weighed by the best food there for this fish (browsing on, it
        // keeps to the food it is on), and by the way there - a fish does not swim far for a little
        // better; it feeds in the layer that food is in.
        const bool bKeep = bLocal && LakeFood::IsNatural(FeedOn);
        auto Best = [&](const FVector2D& P, EFishFood& Which)
        {
            float Value = 0;
            Which = EFishFood::None;
            for (EFishFood F : { EFishFood::Plankton, EFishFood::Insects, EFishFood::Benthos, EFishFood::Plants })
            {
                if (bKeep && F != FeedOn) continue;
                const float V = Diet.Of(F) * Field->Available(F, P, Conditions) * Random.FRandRange(0.85f, 1.15f);
                if (V > Value) { Value = V; Which = F; }
            }
            return Value;
        };
        EFishFood Which;
        auto Weigh = [&](const FVector2D& P) { return ShyOfPressure(P) * (0.5f + Best(P, Which)) * (1 - 0.4f * FMath::Min(1.0f, (float)FVector2D::Distance(P, Near) / FMath::Max(1.0f, Range))); };
        const FVector Spot = LakeFish::PickSpot(Species, Random, Near, Range, C, Goal, Weigh, Deeper);
        const float Value = Best(FVector2D(Spot), Which);
        // Fry close by are a richer meal than most for a fish that eats them.
        if (Fry >= 0 && Diet.Fry * Random.FRandRange(0.7f, 1.1f) > FMath::Max(Value, GroundValue)) { ChaseFry(); return; }
        // Groundbait: concentrated food, if it beats what it would find on its own.
        if (GroundValue > 0.05f && GroundValue * Random.FRandRange(0.85f, 1.15f) > Value) { ToGroundbait(); return; }
        if (Which != EFishFood::None)
        {
            // What it found where it chose to feed (browsing on within the patch does not lower the mark,
            // so a patch eaten down is left).
            if (!bKeep) FeedFound = Value;
            FeedOn = Which;
            NextFoodCheck = SimTime + 4;
            RoamTarget = FVector(Spot.X, Spot.Y, LakeFood::FeedingZ(Which, Spot.X, Spot.Y, Conditions, Random));
            // Within the layer the species feeds in, if its passport sets one (bleak keep to the top).
            if (S.Feeding.FeedColumnMax >= 0)
            {
                const float Bottom = BottomZ(Spot.X, Spot.Y);
                const float Hi = FMath::Lerp(LakeFish::SurfaceZ - 10, Bottom + 8, FMath::Max(0.0f, S.Feeding.FeedColumnMin));
                const float Lo = FMath::Lerp(LakeFish::SurfaceZ - 10, Bottom + 8, S.Feeding.FeedColumnMax);
                RoamTarget.Z = FMath::Clamp(RoamTarget.Z, FMath::Min(Lo, Hi), FMath::Max(Lo, Hi));
            }
            return;
        }
        FeedOn = EFishFood::None;
        RoamTarget = Spot;
    }
    else
    {
        FeedOn = EFishFood::None;
        RoamTarget = LakeFish::PickSpot(Species, Random, Near, Range, C, Goal, ShyOfPressure, Deeper);
    }
    const float Bottom = BottomZ(RoamTarget.X, RoamTarget.Y);
    auto Column = [&](float Fraction) { return FMath::Lerp(LakeFish::SurfaceZ - 10, Bottom + 8, Fraction); };
    if (Goal == EFishGoal::Feed)
    {
        // Crucian root in the silt; roach sip insects off the top on calm dawns and evenings; others
        // feed in their own layer of the water.
        if (S.Feeding.bRootsInSilt) RoamTarget.Z = Bottom + 7;
        else if (S.Feeding.SipAtDusk > 0 && bWeather && IsTwilight(Conditions) && Conditions.Wind < 0.5f && Conditions.Rain < 0.6f && Random.FRand() < S.Feeding.SipAtDusk)
            RoamTarget.Z = LakeFish::SurfaceZ - 12;
        else if (S.Feeding.FeedColumnMin >= 0) RoamTarget.Z = Column(Random.FRandRange(S.Feeding.FeedColumnMin, FMath::Max(S.Feeding.FeedColumnMin, S.Feeding.FeedColumnMax)));
    }
    else
        RoamTarget.Z = Column(S.Feeding.RestColumn);
}

void ALakeFish::ChooseLie()
{
    FFishConditions Conditions;
    const FFishConditions* C = GetConditions(Conditions) ? &Conditions : nullptr;
    // A lie near home (the first one) or a short move along the weed edge (the next ones).
    const bool bMove = bAtSpot && Goal == EFishGoal::Feed;
    RoamTarget = LakeFish::PickSpot(Species, Random, FVector2D(bMove ? GetActorLocation() : HomePoint), bMove ? 800.0f : FMath::Min(1500.0f, HomeDrift + 300), C, EFishGoal::Feed, nullptr, Deeper);
    const float Bottom = BottomZ(RoamTarget.X, RoamTarget.Y);
    RoamTarget.Z = FMath::Lerp(LakeFish::SurfaceZ - 10, Bottom + 8, Random.FRandRange(0.45f, 0.8f));
    if (C) RoamTarget.Z = LakeWater::ToLivable(Species, *C, RoamTarget.X, RoamTarget.Y, RoamTarget.Z);
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
    // A predator hunts only in a lively hour: at night it rests at its lie instead of patrolling.
    if (LakeFish::Info(Species).Feeding.PreyMaxWeight > 0)
        Score[(int32)EFishGoal::Feed] *= FMath::Clamp((A - 0.2f) * 4.0f, 0.0f, 1.0f);
    // The smell of groundbait close by stirs its appetite.
    if (const ALakeLife* Life = ALakeLife::Get(GetWorld()); Life && !Life->Food().Groundbait().IsEmpty())
    {
        float Smell = 0;
        const int32 G = Life->Food().SmellGroundbait(FVector2D(GetActorLocation()), Smell);
        if (G >= 0) Score[(int32)EFishGoal::Feed] += 0.6f * Smell * LakeFood::GroundbaitAppeal(LakeFish::DietOf(Species, Size01), Life->Food().Groundbait()[G]) * FMath::Max(0.3f, Hunger);
    }
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

FVector ALakeFish::FryChaseTarget(FVector Fry) const
{
    // Under the fry shoal at its own depth, and on the open-water side of it if the shoal is close
    // in: the perch drives the fry, it does not swim into the bank after them.
    const FVector At = LakeFish::OffTheBank(Fry, 120.0f, 40.0f);
    return FVector(At.X, At.Y, LakeFish::CeilingAt(Species, At.X, At.Y, LakeFish::LengthCm(Species, WeightKg)));
}

void ALakeFish::Dash(FVector Target, float Seconds)
{
    if (State != EState::Roam) return;
    BurstTime = FMath::Max(BurstTime, Seconds);
    // A rush up into the fry: to the top, but never into the bank or onto a shoal.
    DashTarget = LakeFish::OffTheBank(Target, 70.0f, 30.0f);
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
    if (Strength < LakeFish::Info(Species).Feeding.Unflappable || (IsBusy() && Strength < 0.7f)) return;
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
    else if (Goal == EFishGoal::Feed && bAtSpot) Speed *= S.Feeding.BrowseSpeed;
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
    const float Personal = FMath::Max(18.0f, Len * G.Spacing * (1 - 0.35f * Fear));   // closer together when afraid
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
    if (Count) Out += (Centre / Count - P) * (Goal == EFishGoal::Rest ? 0.08f : 0.15f) * (1 + 3 * Fear);
    const FVector Slot = Lead->GetActorLocation() + FRotator(0, Lead->Yaw, 0).RotateVector(SlotOffset);
    const FVector ToSlot = Slot - P;
    Out += (ToSlot * (Goal == EFishGoal::Cruise ? 0.4f : 0.25f) * (1 + Fear)).GetClampedToMaxSize(Cruise * (1.3f + Fear));
    Out += Separation * Cruise * 1.8f;
    return Out;
}

FVector ALakeFish::SteerBounds() const
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FVector P = GetActorLocation();
    const float Shore = LakeMap::ShoreDistance(P.X, P.Y);
    const float Depth = LakeFish::SurfaceZ - BottomZ(P.X, P.Y);
    FVector Push = FVector::ZeroVector;
    if (State == EState::Hooked)
    {
        // A hooked fish is at the end of the line: the angler may lead it into the shallows.
        if (Shore < 60 || Depth < 20)
            Push += FVector(LakeMap::DeeperDirection(P.X, P.Y), 0) * 30 * FMath::Max(1.0f, (60 - Shore) / 60);
    }
    else
    {
        // Fish are reluctant to go into the shallows rather than barred from them: the shallower the
        // water for its kind, the stronger the pull back to deeper water. The reluctance is halved
        // while feeding (a rich patch just off the reeds still draws a fish in for a while), and an
        // ambush predator has its lie at the weed edge. The fry shoals are a separate thing and keep
        // to the shallows by design. Only the very edge is a hard turn: a fish does not dry out.
        const float MinDepth = FMath::Max(25.0f, S.Habitat.MinDepthM * 100.0f);
        const float Shallow = FMath::Clamp((MinDepth - Depth) / FMath::Max(40.0f, MinDepth), 0.0f, 1.0f);
        const float Edge = FMath::Clamp((50.0f - Shore) / 50.0f, 0.0f, 1.0f);
        float Reluctance = FMath::Max(0.8f * Shallow, Edge);
        if (Goal == EFishGoal::Feed) Reluctance *= 0.5f;
        if (S.Habitat.bAmbush) Reluctance *= 0.35f;
        // A fry hunter (a perch) keeps away from the bank altogether; only a chase after the fry
        // takes it in there, and a meal on the angler's hook is worth a detour too.
        if (S.Feeding.bHuntsFry)
        {
            const bool bChasing = PreyFry >= 0 || FeedOn == EFishFood::Fry || StrikeEnd > 0.0f;
            if (bChasing) Reluctance *= 0.2f;
            else
            {
                const float Bank = FMath::Clamp((180.0f - Shore) / 180.0f, 0.0f, 1.0f);
                Reluctance = FMath::Max(Reluctance, Bank);
            }
        }
        if (TargetAngler.IsValid() && (State == EState::Investigate || State == EState::Nibble)) Reluctance *= 0.25f;
        if (Reluctance > 0.0f)
            Push += FVector(LakeMap::DeeperDirection(P.X, P.Y), 0) * 70.0f * Reluctance;
    }
    if (P.Z > LakeFish::SurfaceZ - 8) Push.Z -= 25;
    if (P.Z < BottomZ(P.X, P.Y) + 6) Push.Z += 25;
    return Push;
}

bool ALakeFish::AtSurfaceMoment() const
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    // At a bait (it may hang just under the float), on the line, out of the water.
    if (State != EState::Roam && State != EState::Flee) return true;
    if (bLifted) return true;
    // Sipping insects off the top; gulping air; a rush up into the fry or a strike.
    if (Goal == EFishGoal::Feed && (FeedOn == EFishFood::Insects || (FeedOn == EFishFood::None && S.Feeding.SipAtDusk > 0 && RoamTarget.Z > LakeFish::SurfaceZ - 15)))
        return true;
    return SimTime < SurfaceUntil || SimTime < DashUntil + 0.6f || StrikeEnd > 0;
}

float ALakeFish::CeilingZ() const
{
    if (AtSurfaceMoment()) return LakeFish::SurfaceZ - 4;
    const FVector P = GetActorLocation();
    return LakeFish::CeilingAt(Species, P.X, P.Y, LakeFish::LengthCm(Species, WeightKg));
}

FVector ALakeFish::AvoidBank(const FVector& Desired) const
{
    // A fish sees the bank coming. Looking ahead along its wish (farther the faster it goes), it
    // takes the part of the wish that heads into the bank or onto a shoal and turns it along the
    // bank instead, so it swims round rather than nosing into the shore and stopping dead.
    const float Speed = Desired.Size2D();
    if (Speed < 1) return Desired;
    const FVector P = GetActorLocation();
    const FVector2D Dir = FVector2D(Desired) / Speed;
    const float Len = LakeFish::LengthCm(Species, WeightKg);
    const float Ahead = FMath::Max(60.0f, FMath::Max(Speed, (float)Vel.Size2D()) * 0.9f + Len);
    const FVector2D Q = FVector2D(P) + Dir * Ahead;
    const float Shore = LakeMap::IsWater(Q.X, Q.Y) ? LakeMap::ShoreDistance(Q.X, Q.Y) : 0.0f;
    const float Depth = LakeMap::IsWater(Q.X, Q.Y) ? LakeMap::Depth(Q.X, Q.Y) : 0.0f;
    // Going for a bait it lets itself closer in (the bait may lie in the shallows by the bank).
    const bool bBait = State != EState::Roam && State != EState::Flee;
    const float Edge = bBait ? 25.0f : 80.0f, Shoal = bBait ? 12.0f : 30.0f;
    const float Danger = FMath::Max(FMath::Clamp((Edge - Shore) / (0.75f * Edge), 0.0f, 1.0f), FMath::Clamp((Shoal - Depth) / (0.6f * Shoal), 0.0f, 1.0f));
    if (Danger <= 0) return Desired;
    const FVector2D Out = -LakeMap::NearestShore(FVector2D(P)).Outward;   // into the lake
    const float Into = -FVector2D::DotProduct(Dir, Out);                     // > 0: heading for the bank
    if (Into <= 0) return Desired;
    // Along the bank: the course with the part into it taken out (head-on, whichever side it leans to).
    FVector2D Along = Dir + Out * Into;
    if (Along.SizeSquared() < 0.09f) Along = FVector2D(-Out.Y, Out.X) * (FVector2D::CrossProduct(Out, Dir) >= 0 ? 1.0f : -1.0f);
    const FVector2D Safe = (Along.GetSafeNormal() + Out * 0.3f).GetSafeNormal();
    return FVector(FMath::Lerp(Dir, Safe, Danger).GetSafeNormal() * Speed, Desired.Z);
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
    Hunger = FMath::Min(1.0f, Hunger + DeltaSeconds * 0.0035f * Activity * S.Feeding.HungerRate);
    // Short of oxygen (dense weed at dawn after a warm calm night, the stale bottom layer): it leaves
    // for better water, swimming high, and the worst off gulp air at the surface.
    if (bSeen && SimTime >= NextAirCheck && State == EState::Roam)
    {
        NextAirCheck = SimTime + Random.FRandRange(2.0f, 4.0f);
        const FVector P = GetActorLocation();
        const float O2 = LakeWater::OxygenMgL(Seen, P.X, P.Y, (LakeFish::SurfaceZ - P.Z) / 100);
        if (LakeWater::IsGasping(Species, O2))
        {
            if (Leader() == this && LakeWater::LivableAt(Species, Seen, RoamTarget.X, RoamTarget.Y, (LakeFish::SurfaceZ - RoamTarget.Z) / 100) < 0.5f) ChooseRoamTarget();
            if (SimTime >= NextGulp)
            {
                NextGulp = SimTime + Random.FRandRange(6.0f, 14.0f);
                SurfaceUntil = SimTime + 3.0f;
                if (ALakeLife* AirLife = ALakeLife::Get(GetWorld())) AirLife->EmitSign(ELakeSign::Gulp, FVector(P.X, P.Y, LakeFish::SurfaceZ), 0.25f + 0.3f * WeightKg, Species, WeightKg);
            }
        }
    }
    // It forgets the hook over game hours.
    if (Caution > 0 || BaitShy[0] + BaitShy[1] + BaitShy[2] > 0)
    {
        const float GameHour = ALakeWeather::DayMinutes * 60.0f / 24.0f;
        const float Keep = FMath::Exp(-DeltaSeconds * UE_LN2 / (FMath::Max(0.1f, S.Learning.ForgetHours) * GameHour));
        Caution = Caution < 0.002f ? 0.0f : Caution * Keep;
        for (float& Shy : BaitShy) Shy = Shy < 0.002f ? 0.0f : Shy * Keep;
    }
    {
        const ALakeFish* Lead = Leader();
        if (Lead != this)
        {
            // School members share their leader's destination and arrive with it.
            RoamTarget = Lead->RoamTarget;
            bAtSpot = Lead->bAtSpot;
            FeedOn = Lead->FeedOn;
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
        FLakeFoodField* Field = Life && bSeen && Life->Food().IsReady() ? &Life->Food() : nullptr;
        if (Field && LakeFood::IsNatural(FeedOn))
        {
            // It fills as fast as the patch gives, and eats it down.
            const FVector2D Here(GetActorLocation());
            const float There = Field->Available(FeedOn, Here, Seen);
            const float Meal = DeltaSeconds * S.Feeding.FillRate * FMath::Clamp(There / 0.5f, 0.15f, 1.4f);
            Hunger -= Meal;
            Field->Eat(FeedOn, Here, Meal, WeightKg);
            // The patch is eaten down: on to the next (a school with its leader).
            if (SimTime >= NextFoodCheck && Leader() == this)
            {
                NextFoodCheck = SimTime + Random.FRandRange(2.0f, 4.0f);
                if (There < FMath::Max(0.08f, 0.45f * FeedFound))
                {
                    // Somewhere that really has more of it (not the patch it has just eaten down).
                    const float Enough = FMath::Max(0.08f, 0.45f * FeedFound);
                    for (int32 Try = 0; Try < 8; ++Try)
                    {
                        ChooseRoamTarget();
                        if (!LakeFood::IsNatural(FeedOn) || Field->Available(FeedOn, FVector2D(RoamTarget), Seen) >= Enough) break;
                    }
                    bAtSpot = false;
                    bRooting = false;
                    StateTime = 0;
                }
            }
        }
        // Grazers find food where they browse; fry hunters without fry pick at the bottom; a fish-eater
        // fills only by eating fish.
        else if (Field && FeedOn == EFishFood::Groundbait)
        {
            // On the groundbait: rich food, eaten down; when it is gone the fish moves on.
            const FVector2D Here(GetActorLocation());
            float Smell = 0;
            const int32 G = Field->SmellGroundbait(Here, Smell);
            const bool bOnIt = G >= 0 && FVector2D::Distance(Here, Field->Groundbait()[G].Location) < 200;
            if (bOnIt)
            {
                // Crumbs: picked up a little at a time, so it fills about as on a good natural patch and stays a while.
                const float Meal = DeltaSeconds * S.Feeding.FillRate * 0.8f * LakeFood::GroundbaitRichness(Field->Groundbait()[G]);
                Hunger -= Meal;
                Field->EatGroundbait(G, Meal, WeightKg);
            }
            if (SimTime >= NextFoodCheck && Leader() == this)
            {
                NextFoodCheck = SimTime + Random.FRandRange(2.0f, 4.0f);
                if (!bOnIt || Field->Groundbait()[G].Balls < 0.1f) { ChooseRoamTarget(); bAtSpot = false; bRooting = false; StateTime = 0; }
            }
        }
        else if (!(S.Feeding.bHuntsFry && PreyFry >= 0)) Hunger -= DeltaSeconds * S.Feeding.FillRate;
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
    // Rooting in the silt for the bottom fauna (on weed or plankton it does not dig).
    const bool bRoot = Goal == EFishGoal::Feed && bAtSpot && S.Feeding.bRootsInSilt && (FeedOn == EFishFood::Benthos || !LakeFood::IsNatural(FeedOn));
    if (!bRoot) bRooting = false;
    const bool bLie = Goal == EFishGoal::Feed && S.Habitat.bAmbush && bAtSpot;

    // Perch drive the fry: follow the shoal, the leader leads each rush into it.
    if (Goal == EFishGoal::Feed && S.Feeding.bHuntsFry && PreyFry >= 0 && Life)
    {
        // A school living at a fishing spot lets the shoal go once it has led them 20 m from home.
        if (!bFollower && HomeDrift < 1000 && FVector::Dist2D(Life->FryLocation(PreyFry), HomePoint) > 2000) ChooseRoamTarget();
        else if (!bFollower)
        {
            RoamTarget = FryChaseTarget(Life->FryLocation(PreyFry));
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
        // (A slow fish - a crucian, a bream - is given longer on its way before it gives the place up,
        // or it turns off a few metres short of a far patch over and over.)
        else if (Goal == EFishGoal::Cruise ? (Dist < 150 || StateTime > 45)
                                           : (bAtSpot && (Dist < 50 || StateTime > 25)) || StateTime > (S.Habitat.bAmbush ? 90.0f : 60.0f) * FMath::Max(1.0f, 24.0f / S.CruiseSpeed))
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
        // The fright of the leader runs through the school at once: all close up and keep up with it.
        Fear = FMath::Max(Fear, Lead->Fear * 0.9f);
        const FVector Flock = SteerSchool(true);
        Desired = Meander(Flock) * 0.5f + Flock * 0.5f;
        MaxSpeed = FMath::Max(MaxSpeed, S.CruiseSpeed * (1.35f + 1.2f * Fear));
    }
    else
    {
        const FVector Roam = SteerRoam();
        Desired = Meander(Roam) + SteerSchool(false);
        // The leader of a school swims a little slower so the school keeps up with it.
        if (School >= 0) Desired *= 0.85f;
        // Moving to a new place a fish may swim faster than it cruises (SteerRoam sets how fast).
        MaxSpeed = FMath::Max(MaxSpeed, (float)Roam.Size());
        // Closing in on the fry shoal it hunts: a fast swim, then the rush (see ALakeLife::AttackFry).
        if (S.Feeding.bHuntsFry && PreyFry >= 0 && Goal == EFishGoal::Feed && Life && FVector::Dist2D(GetActorLocation(), Life->FryLocation(PreyFry)) < 800)
        {
            const float Chase = S.BurstSpeed * 0.5f;
            Desired = Desired.GetSafeNormal() * FMath::Max((float)Desired.Size(), Chase);
            MaxSpeed = FMath::Max(MaxSpeed, Chase);
        }
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
    // Keeping away from a predator that could eat it: it turns off and keeps its distance; a school
    // closes up round its leader and turns away with it (a member darts off on its own only when the
    // danger is right on it); a frightened crucian stops rooting. Fear outweighs the roaming: a fish
    // with a pike a couple of metres off does not amble past it.
    const float Flight = bFollower ? FMath::Max(0.0f, Fear - 0.5f) * 1.2f : Fear * 2.4f;
    if (Fear > 0.05f)
        if (const ALakeFish* Danger = Threat.Get())
        {
            const FVector Away = (GetActorLocation() - Danger->GetActorLocation()).GetSafeNormal2D();
            Desired *= 1.0f - 0.5f * FMath::Clamp(Fear * 1.5f, 0.0f, 1.0f);
            Desired += Away * S.CruiseSpeed * Flight;
            MaxSpeed = FMath::Max(MaxSpeed, S.CruiseSpeed * (1.0f + 0.8f * Fear));
            if (bRooting && Fear > 0.4f) { bRooting = false; PhaseUntil = SimTime + 4; }
        }
    // Rushing at the fry with the school.
    if (SimTime < DashUntil)
    {
        Desired = (DashTarget - GetActorLocation()).GetSafeNormal() * S.BurstSpeed;
        MaxSpeed = S.BurstSpeed;
    }
    // Predators: a strike under way; an ambusher waits in its lie, a pursuit hunter looks about it.
    if (StrikeEnd > 0) UpdateStrike(Desired, MaxSpeed);
    else if (Goal == EFishGoal::Feed && S.Feeding.PreyMaxWeight > 0)
    {
        if (S.Habitat.bAmbush) UpdatePikeAmbush(DeltaSeconds, Desired, MaxSpeed);
        else LookForPrey();
    }
    if (!Life) return;

    const FVector P = GetActorLocation();
    const FVector Surface(P.X, P.Y, LakeFish::SurfaceZ);
    // Just under the surface a fish now and then shows itself: a dimple, a fin, a little bulge.
    // (Roach sipping at the top already show their own rings.)
    const bool bSipping = Goal == EFishGoal::Feed && (FeedOn == EFishFood::Insects || (FeedOn == EFishFood::None && S.Feeding.SipAtDusk > 0));
    if (P.Z > LakeFish::SurfaceZ - 18 && SimTime >= NextSurfaceSign && !bSipping)
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
    else if (bSipping && P.Z > LakeFish::SurfaceZ - 30)
    {
        // Sipping an insect off the top: a small ring.
        Life->EmitSign(ELakeSign::Rise, Surface, 0.3f + WeightKg * 1.5f, Species, WeightKg);
        NextSign = SimTime + Random.FRandRange(9.0f, 25.0f) / Pace;
    }
    else NextSign = SimTime + Random.FRandRange(2.0f, 5.0f);
}

void ALakeFish::UpdateStrike(FVector& Desired, float& MaxSpeed)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    ALakeLife* Life = ALakeLife::Get(GetWorld());
    if (!Life) { StrikeEnd = -1; return; }
    // The strike: a short explosive dash at the prey.
    ALakeFish* Target = Prey.Get();
    const FVector At = Target ? Target->GetActorLocation() : LakeFish::OffTheBank(Life->FryLocation(PreyFry), 70.0f, 30.0f);
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
        // A hungry predator strikes harder; prey on the alert more often slips away.
        const bool bCaught = To.Size() < 60 && Target->IsFree() && Random.FRand() < S.Feeding.StrikeChance * (1 + Hunger) * (1 - 0.5f * Target->Fear);
        if (bCaught)
        {
            Eat(FMath::Clamp(Target->WeightKg / FMath::Max(0.01f, WeightKg * S.Feeding.PreyMaxWeight), 0.3f, 0.9f));
            Life->Devour(Target, this);
        }
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
    // The dash carried it out of cover: it goes back to a new lie, not stay in the open water.
    if (S.Habitat.bAmbush) ChooseLie();
}

void ALakeFish::LookForPrey()
{
    // A pursuit hunter (a big perch) dashes at a small fish right in front of it.
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    ALakeLife* Life = ALakeLife::Get(GetWorld());
    if (!Life || SimTime < NextLook || Hunger < 0.4f) return;
    // At night a predator rests: only a lively hour makes it hunt.
    if (Activity < 0.2f) return;
    NextLook = SimTime + Random.FRandRange(0.5f, 0.9f);
    const FVector Mouth = MouthLocation(), Forward = GetActorForwardVector();
    ALakeFish* Best = nullptr;
    float BestDist = 150;
    Life->ForEachFishNear(Mouth, 150, [&](ALakeFish* Other)
    {
        if (Other == this || (School >= 0 && Other->School == School) || !Other->IsFree() || Other->WeightKg > WeightKg * S.Feeding.PreyMaxWeight) return;
        const FVector To = Other->GetActorLocation() - Mouth;
        const float Dist = To.Size();
        if (Dist < BestDist && FVector::DotProduct(Forward, To / FMath::Max(1.0f, Dist)) > 0.6f) { Best = Other; BestDist = Dist; }
    });
    if (!Best || Random.FRand() > 0.3f * Hunger * FMath::Clamp(Activity, 0.0f, 1.3f)) return;
    Prey = Best;
    PreyFry = -1;
    StrikeEnd = SimTime + 0.6f;
    BurstTime = 0.6f;
    Life->CountPikeStrike();
}

void ALakeFish::SenseThreats(float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    // Fear fades within a few seconds once the predator is out of sight.
    Fear = FMath::Max(0.0f, Fear - DeltaSeconds * 0.18f);
    if (S.Feeding.Wariness <= 0 || SimTime < NextThreatLook) return;
    NextThreatLook = SimTime + Random.FRandRange(0.4f, 0.7f);
    const FVector P = GetActorLocation();
    ALakeFish* Worst = nullptr;
    float Level = 0;
    auto Visit = [&](ALakeFish* Other)
    {
        if (Other == this || Other->bLifted || Other->State == EState::Hooked || (School >= 0 && Other->School == School)) return;
        const FFishFeeding& F = LakeFish::Info(Other->Species).Feeding;
        if (WeightKg > Other->WeightKg * F.PreyMaxWeight) return;   // too big for it to eat
        // Seen (and felt along the lateral line) from a few metres when it moves; lying still, a
        // camouflaged predator is noticed only close by. A hunting one is the more frightening.
        const bool bMoving = Other->Vel.SizeSquared() > FMath::Square(8.0f) || Other->StrikeEnd > 0;
        const float Range = 500.0f * (bMoving ? 1.0f : 1.0f - F.Camouflage);
        const float Dist = FVector::Dist(P, Other->GetActorLocation());
        // Right next to a predator even a still one is felt: the water it displaces, the shadow of its
        // body. Prey does not calmly swim past a pike's nose.
        const float Close = Dist < 220.0f ? 0.45f * S.Feeding.Wariness : 0.0f;
        if (Dist >= Range && Close <= 0.0f) return;
        const float Seen = Dist < Range
            ? S.Feeding.Wariness * (1 - Dist / Range) * (Other->Goal == EFishGoal::Feed ? 1.0f : 0.6f) * (Other->StrikeEnd > 0 ? 1.5f : 1.0f)
            : 0.0f;
        const float L = FMath::Max(Seen, Close);
        if (L > Level) { Level = L; Worst = Other; }
    };
    if (const ALakeLife* Life = ALakeLife::Get(GetWorld())) Life->ForEachFishNear(P, 500, Visit);
    else for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It) Visit(*It);
    if (Worst && Level >= Fear) Threat = Worst;
    Fear = FMath::Clamp(FMath::Max(Fear, Level), 0.0f, 1.0f);
}

void ALakeFish::Frighten(float Amount, int32 Bait)
{
    Caution = FMath::Min(1.0f, Caution + Amount * (1 - 0.5f * Caution));
    if (Bait >= 0 && Bait < 3) BaitShy[Bait] = FMath::Min(1.0f, BaitShy[Bait] + Amount * 1.2f * (1 - 0.5f * BaitShy[Bait]));
}

float ALakeFish::Willingness(EBait Bait) const
{
    return (1 - 0.6f * Caution) * (1 - 0.85f * BaitShy[FMath::Clamp((int32)Bait, 0, 2)]);
}

float ALakeFish::Clout() const
{
    return LakeFish::Info(Species).Feeding.Dominance * FMath::Pow(FMath::Max(WeightKg, 0.01f), 1.0f / 3.0f);
}

void ALakeFish::PushedOff(ALakeFish* By)
{
    // A bigger fish took the bait it was only trying: it gives way, and may come back when that one has gone.
    if (State != EState::Nibble) return;
    if (ALakeCharacter* Angler = TargetAngler.Get()) Angler->OnBiteMotion(this, FBiteStep());
    DropBait();
    if (By) Startle(By->GetActorLocation(), 0.35f);
}

void ALakeFish::UpdatePikeAmbush(float DeltaSeconds, FVector& Desired, float& MaxSpeed)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    ALakeLife* Life = ALakeLife::Get(GetWorld());
    if (!Life) return;
    if (!bAtSpot) return;
    // At its lie the pike holds its place: still, with only a slow turn of the head towards a fish.
    Desired = FVector::ZeroVector;
    MaxSpeed = S.CruiseSpeed * 0.25f;
    if (Hunger < 0.3f || Activity < 0.2f) return;   // a fed or sluggish pike does not strike
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
        if (Other == this || !Other->IsFree() || Other->WeightKg > WeightKg * S.Feeding.PreyMaxWeight) return;
        const FVector To = Other->GetActorLocation() - Mouth;
        const float Dist = To.Size();
        if (Dist < BestDist && FVector::DotProduct(Forward, To / FMath::Max(1.0f, Dist)) > 0.5f) { Best = Other; BestDist = Dist; }
        if (Dist < NearDist) { Near = Other; NearDist = Dist; }
    });
    Prey = Best ? nullptr : Near;
    const int32 Fry = Best ? -1 : Life->NearestFry(Mouth, 350);
    if (!Best && Fry < 0) return;
    if (Random.FRand() > 0.35f * Hunger * FMath::Clamp(Activity, 0.0f, 1.3f)) return;
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
        bSeen = GetConditions(Seen);
        Activity = bSeen ? LakeFish::Activity(Species, Seen) : 1.0f;
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
            // Back for another try at a bait it let go of a moment ago.
            const bool bBack = ReturnTo.Get() == Candidate && Until && SimTime < *Until + 30 && Dist < S.SenseRadius * 1.5f;
            if (!bBack && Dist > S.SenseRadius) continue;
            // Feeding, it finds a bait in the layer it feeds in, likes one that goes with its food and
            // is choosy amid plenty (a big hatch).
            // On groundbait: the hook bait it was mixed with goes down best, and the more was thrown,
            // the fuller and choosier the fish.
            const bool bOnGroundbait = Goal == EFishGoal::Feed && FeedOn == EFishFood::Groundbait;
            const bool bOnFood = Goal == EFishGoal::Feed && (LakeFood::IsNatural(FeedOn) || bOnGroundbait);
            const float FeedColumn = bOnFood ? Depth01(RoamTarget.Z, RoamTarget.X, RoamTarget.Y) : -1.0f;
            float Match = LakeFood::BaitMatch(FeedOn, Candidate->Bait), Plenty = 0;
            if (const ALakeLife* FoodLife = ALakeLife::Get(GetWorld()); bOnFood && FoodLife && FoodLife->Food().IsReady())
            {
                const FLakeFoodField& Field = FoodLife->Food();
                if (bOnGroundbait)
                {
                    float Smell = 0;
                    if (const int32 G = Field.SmellGroundbait(FVector2D(GetActorLocation()), Smell); G >= 0)
                    {
                        Match = LakeFood::GroundbaitMix(Field.Groundbait()[G]) == Candidate->Bait ? 1.15f : 0.95f;
                        Plenty = 1 - FMath::Exp(-Field.Groundbait()[G].Balls / 2);
                    }
                }
                else if (bSeen) Plenty = FMath::Clamp(Field.Available(FeedOn, FVector2D(GetActorLocation()), Seen), 0.0f, 1.0f);
            }
            float Interest = LakeFish::BaitInterest(Species, Candidate->Bait, Depth01(Bait.Z, Bait.X, Bait.Y), FeedColumn) * Match
                * Hunger * Activity * Mind * FMath::Sqrt(FMath::Max(0.0f, 1 - Dist / S.SenseRadius)) * (1 - Fear);
            if (bOnFood) Interest *= 1.15f - 0.35f * Plenty;
            // What it has learnt: the hook, the bait that pricked it, a much-fished spot.
            Interest *= Willingness(Candidate->Bait);
            if (const ALakeLife* PressureLife = ALakeLife::Get(GetWorld()))
                Interest *= 1 - FMath::Min(0.6f, 0.15f * S.Learning.Fright * PressureLife->PressureAt(Bait));
            if (bBack || Random.FRand() < Interest * 0.7f)
            {
                State = EState::Investigate;
                StateTime = 0;
                TargetAngler = Candidate;
                ReturnTo = nullptr;
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
        if (FVector::Dist(MouthLocation(), Angler->BaitLocation()) < ContactRadius() && Angler->ClaimBait(this))
        {
            State = EState::Nibble;
            StateTime = 0;
            // The bite starts; the fish decides each step as it goes (LakeFish::NextBiteStep).
            const FVector BaitAt = Angler->BaitLocation();
            BiteState = FBiteState();
            BiteNow = FBiteStep();
            BiteStepEnd = SimTime + Random.FRandRange(0.3f, 1.0f);
            // A crucian rooting at the bait sends bubbles up beside the float.
            if (LakeFish::Info(Species).Feeding.bRootsInSilt)
                if (ALakeLife* Life = ALakeLife::Get(GetWorld()))
                    Life->EmitSign(ELakeSign::Bubbles, FVector(BaitAt.X + Random.FRandRange(-40, 40), BaitAt.Y + Random.FRandRange(-40, 40), LakeFish::SurfaceZ), 0.4f + WeightKg * 0.6f, Species, WeightKg);
        }
        return;
    }
    if ((State == EState::Nibble || State == EState::Holding) && Angler && SimTime >= BiteStepEnd) NextBiteStep(Angler);
    // Jerked under its nose while it only tries the bait, or a predator close by: it backs off at once.
    else if (State == EState::Nibble && Angler && (Angler->RigDisturbance() > 0.8f || Fear > 0.6f))
    {
        BiteState.Felt += 1;
        Angler->OnBiteMotion(this, FBiteStep());
        DropBait();
    }
}

float ALakeFish::BaitLike(const ALakeCharacter* Angler) const
{
    const FVector Bait = Angler->BaitLocation();
    return FMath::Clamp(LakeFish::BaitInterest(Species, Angler->Bait, Depth01(Bait.Z, Bait.X, Bait.Y)), 0.0f, 1.0f);
}

void ALakeFish::NextBiteStep(ALakeCharacter* Angler)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    // The step it said it would let go after is over: the float settles, the fish backs off.
    if (BiteNow.bLetGo)
    {
        if (State == EState::Holding) Angler->OnFishLetGo(this);   // the angler's side; it calls DropBait
        else Angler->OnBiteMotion(this, FBiteStep());
        DropBait();   // (nothing more if the angler already made it let go)
        return;
    }
    // Wary in bright sun on a glassy lake; it feels the rig move and the float pull.
    FFishConditions Conditions;
    const bool bBright = GetConditions(Conditions) && LakeFish::IsBrightCalm(Conditions);
    const FVector BaitAt = Angler->BaitLocation();
    const bool bOnBottom = BaitAt.Z <= BottomZ(BaitAt.X, BaitAt.Y) + 8;
    // A fish that has learnt the hook tests longer, takes less readily and feels more.
    const float Shy = BaitShy[FMath::Clamp((int32)Angler->Bait, 0, 2)];
    const FBiteState Before = BiteState;
    const FBiteStep Step = LakeFish::NextBiteStep(Species, BiteState, Size01, Activity, bOnBottom, bBright || Caution > 0.4f, BaitLike(Angler) * (1 - 0.7f * Shy),
        Angler->RigDisturbance() + 0.3f * Caution, Random);
    if (Step.bTake && State == EState::Nibble && FVector::Dist(MouthLocation(), Angler->BaitLocation()) > ContactRadius())
    {
        BiteState = Before; BiteStepEnd = SimTime + 0.1f;
        return; // Finish approaching; taking cannot pull a distant hook into the mouth.
    }
    BiteNow = Step;
    BiteStepEnd = SimTime + Step.Duration;
    if (Step.bTake && State == EState::Nibble)
    {
        // It takes: the angler may strike while it has the bait in its mouth. It lets go by itself
        // (the angler's timer is only a safety net).
        Angler->OnFishTake(this, 60);
        State = EState::Holding;
        StateTime = 0;
    }
    Angler->OnBiteMotion(this, Step);
    if (State != EState::Holding) return;
    // Where it goes with the bait: up (lifting it), aside, nowhere (holding it, swallowing), or off.
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
    case EBiteMotion::Hold:
        // Still, nose a little down (a swallowing perch keeps the float under).
        RoamTarget = FVector(P.X, P.Y, FMath::Max(BottomZ(P.X, P.Y) + 10, P.Z - 15));
        HoldSpeed = 0.15f;
        break;
    case EBiteMotion::None:
        RoamTarget = P;
        HoldSpeed = 0.15f;
        break;
    default:
    {
        // Off and down; a pike runs several metres with it.
        const float Run = Random.FRandRange(S.Bite.RunMin, FMath::Max(S.Bite.RunMin, S.Bite.RunMax));
        RoamTarget = P + (Away + Side * Random.FRandRange(-0.5f, 0.5f)).GetSafeNormal() * Run;
        RoamTarget.Z = FMath::Max(BottomZ(RoamTarget.X, RoamTarget.Y) + 10, P.Z - 60);
        HoldSpeed = FMath::Clamp(Step.Amount / S.CruiseSpeed * 0.6f, 0.6f, FMath::Max(2.5f, S.BurstSpeed * 0.3f / S.CruiseSpeed));
        break;
    }
    }
}

// --- interaction ---------------------------------------------------------------------------

void ALakeFish::Spook(FVector From, float Seconds)
{
    if (State == EState::Hooked) return;
    if (ALakeCharacter* Angler = TargetAngler.Get()) Angler->ReleaseBaitClaim(this);
    TargetAngler = nullptr;
    ReturnTo = nullptr;
    BaitVisits = 0;
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
    BurstTime = 0;
    FightPhase = EFightPhase::Pause;
    FightRemaining = 0;
    FightPulses = 0;
    bFightJump = bShoreTried = bShoreRun = false;
    HookGrip = 1; HookTurnSpeed = 0; HookAngularVelocity = FVector::ZeroVector;
    bFightAwake = false;   // it feels the strike as the line pulls, not before
    NextFightJump = SimTime;
    PendingLineForce = FVector::ZeroVector;
    StrikeEnd = -1;
    if (Angler) Angler->AddTickPrerequisiteActor(this);
    RoamTarget = GetActorLocation() + GetActorForwardVector() * 80;
    // The thrashing scares the school mates nearby, and they remember it (of that bait, half as much).
    if (ALakeLife* Life = ALakeLife::Get(GetWorld()))
    {
        Life->Alarm(this, 600);
        const FFishLearning& L = LakeFish::Info(Species).Learning;
        const int32 With = Angler ? (int32)Angler->Bait : -1;
        Life->ForEachFishNear(GetActorLocation(), 600, [&](ALakeFish* Mate)
        {
            if (Mate == this || School < 0 || Mate->School != School) return;
            Mate->Frighten(L.SchoolShare * L.Fright, -1);
            if (With >= 0) Mate->BaitShy[With] = FMath::Min(1.0f, Mate->BaitShy[With] + 0.5f * L.SchoolShare * L.Fright);
        });
    }
}

void ALakeFish::ApplyLineForce(FVector Force) { PendingLineForce = Force; if (State == EState::Hooked && Force.Size() > 35) WakeOnLine(); }

void ALakeFish::DropBait()
{
    if (State != EState::Nibble && State != EState::Holding) return;
    ALakeCharacter* Angler = TargetAngler.Get();
    bool bBack = false;
    FVector BaitAt = GetActorLocation();
    if (Angler)
    {
        // Still hungry and liking the bait, it comes back for another try a few seconds later (a
        // crucian again and again, a pike seldom); one that felt the tackle, or has tried a few
        // times already, leaves it alone for a minute.
        BaitAt = Angler->BaitLocation();
        ++BaitVisits;
        const FFishBiteStyle& Style = LakeFish::Info(Species).Bite;
        const float Again = Hunger * (0.35f + 0.65f * BaitLike(Angler)) * FMath::Clamp(Activity, 0.2f, 1.0f) * Style.Comeback
            * (1 - FMath::Clamp(BiteState.Felt * 0.6f, 0.0f, 0.9f)) * (1 - Caution);
        if (BiteState.Felt > 0.5f) Frighten(0.15f * Style.Comeback * LakeFish::Info(Species).Learning.Fright, (int32)Angler->Bait);
        bBack = BaitVisits < Style.MaxVisits && Random.FRand() < Again;
        const float Delay = bBack ? Random.FRandRange(Style.ReturnMin, FMath::Max(Style.ReturnMin, Style.ReturnMax)) : 60.0f;
        IgnoreBaitUntil.Add(Angler, SimTime + Delay);
        ReturnTo = bBack ? Angler : nullptr;
        if (!bBack) BaitVisits = 0;
        Angler->ReleaseBaitClaim(this);
    }
    TargetAngler = nullptr;
    BiteNow = FBiteStep();
    BiteState = FBiteState();
    BiteStepEnd = 0;
    State = EState::Roam;
    StateTime = 0;
    ChooseRoamTarget();
    if (bBack)
    {
        // It backs off a metre or two and hangs about there.
        const FVector2D Off = FVector2D(GetActorLocation() - BaitAt).GetSafeNormal();
        const FVector2D To = FVector2D(BaitAt) + (Off.IsNearlyZero() ? FVector2D(1, 0) : Off) * Random.FRandRange(120.0f, 250.0f);
        if (LakeMap::IsWater(To.X, To.Y)) RoamTarget = FVector(To, FMath::Clamp(BaitAt.Z, BottomZ(To.X, To.Y) + 10, LakeFish::SurfaceZ - 15));
    }
}

void ALakeFish::LiftOut()
{
    bFightJump = false;
    bLifted = true;
    HangTime = 0;
    HangAngularVelocity = FVector::ZeroVector;
    HangAnchor = MouthLocation();
    HangAnchorVel = FVector::ZeroVector;
    HangForward = GetActorForwardVector();
    Vel = FVector::ZeroVector;
    PendingLineForce = FVector::ZeroVector;
}

void ALakeFish::LowerIntoWater(FVector LineVelocity)
{
    bFightJump = false;
    bLifted = false;
    State = EState::Hooked;
    Vel = LineVelocity.GetClampedToMaxSize(200);
    PendingLineForce = FVector::ZeroVector;
    ForceNetUpdate();
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
    const float Want = FMath::Sin(HangTime * 1.7f + Phase) > 0.82f ? StaminaFraction() * 0.7f : 0.04f;
    HangEffort = FMath::FInterpTo(HangEffort, Want, Dt, 3.0f);
    const FQuat Target = FRotationMatrix::MakeFromXZ(Axis, Out > 0.8f ? -Flat : FVector::UpVector).ToQuat();
    FQuat Pose = GetActorQuat();
    FQuat Error = Target * Pose.Inverse(); if (Error.W < 0) Error = Error * -1;
    FVector TurnAxis; float Angle; Error.ToAxisAndAngle(TurnAxis, Angle);
    const float Omega = FMath::Sqrt(980.0f / FMath::Max(10.0f, Len * 0.45f));
    HangAngularVelocity += (TurnAxis * Angle * Omega * Omega - HangAngularVelocity * (1.6f * Omega)) * Dt;
    const float Speed = HangAngularVelocity.Size();
    if (Speed > 0.001f) Pose = FQuat(HangAngularVelocity / Speed, Speed * Dt) * Pose;
    Pose.Normalize();
    SetActorRotation(Pose);
    SetActorLocation(Mouth - GetActorQuat().RotateVector(MouthOffset()));

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
    // It learns: off the hook - fully, pricked by a strike as it held or tried the bait - six tenths;
    // the place grows warier.
    if (State == EState::Hooked || State == EState::Holding || State == EState::Nibble)
    {
        const bool bHooked = State == EState::Hooked;
        const float Fright = LakeFish::Info(Species).Learning.Fright;
        Frighten(Fright * (bHooked ? 1.0f : 0.6f), TargetAngler.IsValid() ? (int32)TargetAngler->Bait : -1);
        if (ALakeLife* Life = ALakeLife::Get(GetWorld())) Life->AddPressure(GetActorLocation(), bHooked ? 0.6f : 0.2f);
    }
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
    if (bPrecise && (State == EState::Investigate || State == EState::Nibble) && TargetAngler.IsValid())
    {
        // Close to food, fins can brake and move backwards; a tail-only orbit cannot dock a mouth.
        const FVector Bait = TargetAngler->BaitLocation();
        const FVector P = GetActorLocation();
        const FVector Look = Bait - P - GetActorQuat().RotateVector(FVector(0, 0, MouthOffset().Z));
        FRotator Want = Look.Rotation(); Want.Pitch = FMath::Clamp(Want.Pitch, -50.0, 50.0); Want.Roll = 0;
        const FQuat From = GetActorQuat(), To = Want.Quaternion();
        const float Angle = FMath::RadiansToDegrees(From.AngularDistance(To));
        const float Step = Angle > 0.001f ? FMath::Min(1.0f, S.TurnRate * Agility * DeltaSeconds / Angle) : 1.0f;
        const FQuat Pose = FQuat::Slerp(From, To, Step);
        const FVector DockPoint = Bait - Pose.RotateVector(MouthOffset());
        const FVector WantVelocity = ((DockPoint - P) * 3).GetClampedToMaxSize(S.CruiseSpeed * 0.8f);
        const FVector Acceleration = ((WantVelocity - Vel) * 7).GetClampedToMaxSize(S.Acceleration * Agility);
        Vel += Acceleration * DeltaSeconds;
        FVector Next = P + Vel * DeltaSeconds;
        Next.Z = FMath::Clamp(Next.Z, BottomZ(Next.X, Next.Y) + 0.5f, LakeFish::SurfaceZ - 1);
        if (!LakeMap::IsWater(Next.X, Next.Y)) { Next = P; Vel = FVector::ZeroVector; }
        Effort = FMath::Clamp(float(Acceleration.Size()) / S.Acceleration, 0.0f, 1.0f);
        const FRotator Rotation = Pose.Rotator();
        YawRate = DeltaSeconds > 0 ? FMath::FindDeltaAngleDegrees(Yaw, Rotation.Yaw) / DeltaSeconds : 0;
        Yaw = Rotation.Yaw; Pitch = Rotation.Pitch; Roll = 0;
        Wish = Vel; WishSpeedNow = Vel.Size();
        SetActorLocationAndRotation(Next, Pose);
        return;
    }
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
    if (State == EState::Holding) Vel += PendingLineForce / FMath::Max(0.03f, WeightKg) * Dt;
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

    // It does not coast up past its ceiling; above it (a moment at the surface has ended, the bed
    // rises under it) it sinks back gently.
    {
        const float Room = CeilingZ() - GetActorLocation().Z;
        Vel.Z = FMath::Min((float)Vel.Z, Room >= 0 ? Room * 2.0f : FMath::Max(Room * 1.5f, -25.0f));
    }
    // Integrate and keep the fish inside the water volume.
    FVector P = GetActorLocation() + Vel * Dt;
    const float Bottom = BottomZ(P.X, P.Y);
    const float Top = LakeFish::SurfaceZ - 4;
    if (P.Z > Top) { P.Z = Top; Vel.Z = FMath::Min(0.0, Vel.Z); }
    if (P.Z < Bottom + 4) { P.Z = Bottom + 4; Vel.Z = FMath::Max(0.0, Vel.Z); }
    if (!LakeMap::IsWater(P.X, P.Y) || LakeFish::SurfaceZ - Bottom < 10)
    {
        // Hit the shore: stop the motion towards it.
        ++BankHits;
        const FVector In(LakeMap::DeeperDirection(P.X, P.Y), 0);
        P = GetActorLocation();
        Vel -= In * FMath::Min(0.0, FVector::DotProduct(Vel, In)) * 1.0;
    }
    SetActorLocationAndRotation(P, FRotator(Pitch, Yaw, Roll));
}

void ALakeFish::ChooseFightTarget(bool bFinalRun)
{
    ALakeCharacter* Angler = TargetAngler.Get();
    if (!Angler) return;
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FFishFightStyle& F = S.Fight;
    const FVector P = GetActorLocation();
    FVector Away = (P - Angler->GetActorLocation()).GetSafeNormal2D();
    if (Away.IsNearlyZero()) Away = FVector(LakeMap::DeeperDirection(P.X, P.Y), 0);
    const FVector Along = FVector::CrossProduct(Away, FVector::UpVector) * (Random.FRand() < 0.5f ? 1 : -1);
    const float Pick = Random.FRand() * (F.AlongWeight + F.AwayWeight + F.DiveWeight + F.SurfaceWeight);
    const bool bAlong = Pick < F.AlongWeight;
    const bool bDive = Pick >= F.AlongWeight + F.AwayWeight && Pick < F.AlongWeight + F.AwayWeight + F.DiveWeight;
    const bool bSurface = Pick >= F.AlongWeight + F.AwayWeight + F.DiveWeight;
    const FVector Dir = bFinalRun ? (Away + FVector(LakeMap::DeeperDirection(P.X, P.Y), 0)).GetSafeNormal()
        : bAlong ? (Along + Away * 0.25f).GetSafeNormal() : (Away + Along * Random.FRandRange(-0.5f, 0.5f)).GetSafeNormal();
    RoamTarget = P + Dir * Random.FRandRange(300, 700) * (0.4f + StaminaFraction());
    // A target on dry land would make the fish push into an invisible wall throughout a bout.
    for (int32 I = 0; I < 8 && (!LakeMap::IsWater(RoamTarget.X, RoamTarget.Y) || LakeMap::Depth(RoamTarget.X, RoamTarget.Y) < 20); ++I)
        RoamTarget = (RoamTarget + P) * 0.5f;
    const float Bottom = BottomZ(RoamTarget.X, RoamTarget.Y) + 8;
    const float Fresh = StaminaFraction();
    // A fish with strength sits deeper than its usual band and bores down; only a tired one comes up
    // and lets itself be landed. The species' own layer stays in the choice (a rudd keeps higher
    // than a bream even while fresh).
    const float Band = FMath::Clamp((S.DepthMin + S.DepthMax) * 0.5f, 0.0f, 1.0f);
    const float BandZ = FMath::Lerp(LakeFish::SurfaceZ - 12, Bottom, Band);
    RoamTarget.Z = Fresh < 0.3f || bSurface ? LakeFish::SurfaceZ - 12
        : bDive || bFinalRun ? Bottom + 6
        : FMath::Lerp(BandZ, Bottom + 6, 0.55f * FMath::SmoothStep(0.3f, 0.9f, Fresh));
    RoamTarget.Z = FMath::Clamp(RoamTarget.Z, Bottom, FMath::Max(Bottom, LakeFish::SurfaceZ - 8));
}

void ALakeFish::FightSplash()
{
    if (ALakeLife* Life = ALakeLife::Get(GetWorld()))
    {
        const FVector P = GetActorLocation();
        Life->EmitSign(ELakeSign::Splash, FVector(P.X, P.Y, LakeFish::SurfaceZ), FMath::Min(0.35f + WeightKg * 0.4f, 1.4f), Species, WeightKg, 0, true);
    }
    NextSign = SimTime + Random.FRandRange(0.9f, 2.2f);
}

void ALakeFish::StartFightBurst()
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FFishFightStyle& F = S.Fight;
    FightPhase = EFightPhase::Burst;
    const float Base = F.BurstMin > 0 ? Random.FRandRange(F.BurstMin, F.BurstMax) : S.RunBurst;
    FightRemaining = Base * FMath::Lerp(0.8f, 1.4f, Size01) * FMath::Lerp(0.55f, 1.0f, StaminaFraction()) / FightIndividual;
    BurstTime = FightRemaining;
    if (!bShoreRun && !bFightJump && F.JumpChance > 0 && SimTime >= NextFightJump && StaminaFraction() > 0.4f
        && GetActorLocation().Z > LakeFish::SurfaceZ - 35 && Random.FRand() < F.JumpChance * StaminaFraction())
    {
        bFightJump = true;
        NextFightJump = SimTime + F.JumpCooldown;
        Vel.Z = F.JumpSpeed * FMath::Lerp(0.8f, 1.0f, StaminaFraction());
        // The launch has a real cost even when the following airborne motion is passive.
        Stamina = FMath::Max(0.0f, Stamina - F.EffortCost * 0.4f);
        FightSplash();
    }
}

bool ALakeFish::ReadyToLand()
{
    if (State != EState::Hooked || bLifted) return false;
    const FFishFightStyle& F = LakeFish::Info(Species).Fight;
    const ALakeCharacter* Angler = TargetAngler.Get();
    const FVector P = GetActorLocation();
    const float Fresh = StaminaFraction();
    if (!bShoreTried && !bFightJump && F.ShoreRunChance > 0 && StateTime > 3 && Fresh > 0.15f && Fresh < 0.6f
        && Angler && FVector::Dist2D(P, Angler->GetActorLocation()) < F.ShoreDistance && LakeMap::ShoreDistance(P.X, P.Y) < 450)
    {
        bShoreTried = true;
        if (Random.FRand() < F.ShoreRunChance * FightIndividual)
        {
            bShoreRun = true;
            FightPulses = 1;
            ChooseFightTarget(true);
            StartFightBurst();
        }
    }
    return !bFightJump && !bShoreRun;
}

void ALakeFish::UpdateFightMotion(float DeltaSeconds, FVector& Desired, float& MaxSpeed)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FFishFightStyle& F = S.Fight;
    if (!bFightAwake)
    {
        // A prick causes a modest swimming response; full escape bouts require a load.
        MaxSpeed = S.CruiseSpeed * 0.65f;
        Desired = GetActorForwardVector() * MaxSpeed;
        BurstTime = 0;
        return;
    }
    const float Fresh = StaminaFraction();
    FightRemaining -= DeltaSeconds;
    ReadyToLand();
    if (FightRemaining <= 0 && !bFightJump)
    {
        if (FightPhase == EFightPhase::Burst && --FightPulses > 0)
        {
            FightPhase = EFightPhase::Gap;
            FightRemaining = F.SeriesGap / FightIndividual;
        }
        else if (FightPhase == EFightPhase::Gap) StartFightBurst();
        else if (FightPhase == EFightPhase::Pull || FightPhase == EFightPhase::Burst)
        {
            bShoreRun = false;
            FightPhase = EFightPhase::Pause;
            FightRemaining = Random.FRandRange(F.PauseMin, F.PauseMax) * FMath::Lerp(1.8f, 1.0f, Fresh) / FightIndividual;
        }
        else
        {
            ChooseFightTarget();
            if (Random.FRand() < F.BurstChance * Fresh * FightIndividual)
            {
                FightPulses = FMath::Max(1, FMath::RoundToInt(Random.RandRange(F.SeriesMin, F.SeriesMax) * FMath::Lerp(0.4f, 1.0f, Fresh)));
                StartFightBurst();
            }
            else
            {
                FightPhase = EFightPhase::Pull;
                FightRemaining = Random.FRandRange(F.PullMin, F.PullMax) * FMath::Lerp(0.8f, 1.3f, Size01) / FightIndividual;
            }
        }
    }
    BurstTime = FightPhase == EFightPhase::Burst ? FMath::Max(0.0f, FightRemaining) : 0;
    const bool bRest = FightPhase == EFightPhase::Pause || FightPhase == EFightPhase::Gap;
    const float Pace = bRest ? F.PauseSpeed : FightPhase == EFightPhase::Burst ? 1.0f : F.PullSpeed;
    MaxSpeed = S.BurstSpeed * Pace * FMath::Lerp(0.12f, 1.0f, Fresh) * FightIndividual;
    Desired = (RoamTarget - GetActorLocation()).GetSafeNormal() * MaxSpeed;
    if (Effort > 0.4f && Vel.Size() > 30 && SimTime >= NextFightDisturb)
    {
        if (auto* Life = ALakeLife::Get(GetWorld())) Life->Disturb(GetActorLocation(), 120 + WeightKg * 100, FMath::Min(1.0f, Effort * (float)Vel.Size() / 100));
        NextFightDisturb = SimTime + 1;
    }
    if (GetActorLocation().Z > LakeFish::SurfaceZ - 30 && Effort > 0.35f && SimTime >= NextSign) FightSplash();
}

void ALakeFish::MoveHooked(FVector Desired, float MaxSpeed, float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    Desired = Desired.GetClampedToMaxSize(MaxSpeed);
    FVector Heading = GetActorForwardVector();
    const bool bBurst = BurstTime > 0;
    const float TurnRate = S.TurnRate * (bBurst ? 1.2f : 0.8f) * Agility * FightIndividual;
    const float Water = FMath::Clamp(0.5f + (LakeFish::SurfaceZ - GetActorLocation().Z) / FMath::Max(8.0f, LakeFish::LengthCm(Species, WeightKg) * 0.45f), 0.0f, 1.0f);
    // On the line the fish throws itself about: the body turns straight at the wanted heading,
    // limited by the species' turn rate. Fish keep their back up: pitch is limited.
    if (Desired.SizeSquared() > 4)
    {
        FVector Want = Desired.GetSafeNormal();
        Want.Z = FMath::Clamp(Want.Z, -0.5f, 0.5f);
        Want.Normalize();
        const FQuat From = Heading.ToOrientationQuat(), To = Want.ToOrientationQuat();
        const float Angle = FMath::RadiansToDegrees(From.AngularDistance(To));
        const float WantedTurn = FMath::Min(TurnRate * Water, Angle * 2.5f);
        HookTurnSpeed = FMath::FInterpConstantTo(HookTurnSpeed, WantedTurn, DeltaSeconds, S.Gait.TurnAccel * Agility);
        const float Step = Angle > KINDA_SMALL_NUMBER ? FMath::Min(1.0f, HookTurnSpeed * DeltaSeconds / Angle) : 1.0f;
        Heading = FQuat::Slerp(From, To, Step).GetForwardVector();
    }
    // A load at the mouth creates torque about the body, independently of its swimming wish.
    const float Len = LakeFish::LengthCm(Species, WeightKg);
    const float Inertia = FMath::Max(0.2f, WeightKg * Len * Len * 0.1f);
    const FVector Lever = Heading * (Len * 0.4f);
    HookAngularVelocity += (FVector::CrossProduct(Lever, PendingLineForce) / Inertia - HookAngularVelocity * (2 + 4 * Water)) * DeltaSeconds;
    HookAngularVelocity = HookAngularVelocity.GetClampedToMaxSize(3.0f);
    const float AngularSpeed = HookAngularVelocity.Size();
    if (AngularSpeed > 0.001f) Heading = FQuat(HookAngularVelocity / AngularSpeed, AngularSpeed * DeltaSeconds).RotateVector(Heading);
    // Thrust only along the body axis, like a real tail; reverse thrust is weak.
    float Accel = S.Acceleration * (bBurst ? 2.5f : 1.0f);
    const bool bRest = FightPhase == EFightPhase::Pause || FightPhase == EFightPhase::Gap;
    Accel *= Water * S.Strength * FMath::Lerp(0.08f, 1.0f, StaminaFraction()) * FightIndividual * (bRest && bFightAwake ? 0.08f : 1.0f);
    const float ForwardSpeed = FVector::DotProduct(Vel, Heading);
    const float TargetSpeed = Desired.Size() * FMath::Max(0.0f, FVector::DotProduct(Heading, Desired.GetSafeNormal()));
    const float Thrust = bFightJump ? 0 : FMath::Clamp((TargetSpeed - ForwardSpeed) * 3.0f, -Accel * 0.3f, Accel);
    // Passive towing is not exercise. Count propulsion and opposition to the line while active.
    const float Opposition = FMath::Max(0.0f, float(-FVector::DotProduct(PendingLineForce, Heading)))
        / FMath::Max(1.0f, WeightKg * S.Acceleration * S.Strength);
    HookGrip -= DeltaSeconds * (1 - HookGrip) * 0.06f * FMath::Max(0.0f, Opposition - 0.8f);
    if (HookGrip <= 0) { Release(6); return; }
    Effort = FMath::Clamp(FMath::Abs(Thrust) / FMath::Max(1.0f, S.Acceleration * S.Strength)
        + (bRest || bFightJump ? 0 : FMath::Max(0.0f, ForwardSpeed) / S.BurstSpeed * 0.4f + Opposition * 0.35f), 0.0f, 1.0f);
    const float Recover = bRest && !bFightJump ? S.Fight.Recovery * FMath::Max(0.0f, 1 - Effort / 0.2f) : 0;
    Stamina = FMath::Clamp(Stamina + DeltaSeconds * (Recover - S.Fight.EffortCost * Effort), 0.0f, MaxStamina);

    Vel += Heading * Thrust * DeltaSeconds;
    if (!bFightJump) Vel.Z -= 980.0f * (1 - Water) * DeltaSeconds;
    // External pull from the fishing line, divided by the fish's mass.
    Vel += PendingLineForce / FMath::Max(0.03f, WeightKg) * DeltaSeconds;
    // Water drag: low along the streamlined body, high sideways and vertically.
    if (bFightJump)
    {
        Vel.Z -= 980.0f * DeltaSeconds;
        Heading = Vel.GetSafeNormal();
    }
    else
    {
        const FVector Forward = Heading * FVector::DotProduct(Vel, Heading);
        const FVector Side = Vel - Forward;
        const float ForwardDrag = 1.2f / S.BurstSpeed * Forward.Size();
        Vel = Forward / (1 + ForwardDrag * Water * DeltaSeconds) + Side / (1 + 5.0f * Water * DeltaSeconds);
    }

    FVector P = GetActorLocation() + Vel * DeltaSeconds;
    const float Bottom = BottomZ(P.X, P.Y);
    const float Top = LakeFish::SurfaceZ - 4;
    // Buoyancy fades continuously as the body emerges; no position/depth projection.
    if (!bFightJump && Water > 0 && P.Z > LakeFish::SurfaceZ - 20)
        Vel.Z -= 45.0f * StaminaFraction() * DeltaSeconds;
    if (bFightJump && P.Z <= Top && Vel.Z <= 0)
    {
        bFightJump = false;
        Vel.Z *= 0.25f;
        FightSplash();
    }

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
    if (State != EState::Hooked && !bLifted) SenseThreats(DeltaSeconds);
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
        bPrecise = BiteNow.Motion == EBiteMotion::Lift || BiteNow.Motion == EBiteMotion::Hold;
        break;
    case EState::Hooked:
    {
        if (!Angler) { State = EState::Roam; break; }
        UpdateFightMotion(DeltaSeconds, Desired, MaxSpeed);
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
    if (State == EState::Flee) MaxSpeed = FMath::Max(MaxSpeed, S.CruiseSpeed);
    if (BurstTime > 0 && State != EState::Hooked) MaxSpeed = S.BurstSpeed;
    Desired += SteerBounds();
    if (State != EState::Hooked && !bLifted && !bPrecise)
    {
        Desired = AvoidBank(Desired);
        // Under its clearance: no wish to rise past it, and above it a wish to sink back.
        const float Room = CeilingZ() - GetActorLocation().Z;
        if (Desired.Z > 0) Desired.Z = FMath::Min((float)Desired.Z, FMath::Max(0.0f, Room) * 1.5f);
        if (Room < 0) Desired.Z += FMath::Max(Room * 1.5f, -30.0f);
    }
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
    if (HasAuthority() ? BurstTime > 0 : bNetBurst) BurstSeenAt = GetWorld()->GetTimeSeconds();
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
                                    FMath::Abs(WindowTurn - ShownPrevFrameTurn) / ShownWindow, Species,
                                    GetWorld()->GetTimeSeconds() - BurstSeenAt < 0.5f);
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
    float E = bHanging ? HangEffort : HasAuthority() ? Effort : NetEffort;
    // On the hook under water it keeps working its tail even when it holds position: a fighting fish
    // never stands still, and a wooden one gives its weight away as a rag on a string.

    float Beat, Amp;
    if (bHanging)
    {
        Beat = S.TailBeat * (0.6f + 2.2f * E);
        Amp = 0.005f + 0.05f * E;
    }
    else
    {
        const float Hz = FMath::Max(FMath::Min(7.0f, (0.35f + ShownSpeed / (0.7f * Len)) * (0.75f + 0.6f * E)),
            0.0f);
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
        PendingLineForce = FVector::ZeroVector; // consumed for every substep of this frame
        NetLocation = GetActorLocation();
        NetVelocity = Vel;
        NetRotation = GetActorRotation();
        NetEffort = Effort;
        NetFeedOn = (uint8)FeedOn;
        // A burst starts: the clients hear of it at once and follow the fast start without lagging.
        if ((BurstTime > 0) != bNetBurst) { bNetBurst = BurstTime > 0; if (bNetBurst) ForceNetUpdate(); }
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
