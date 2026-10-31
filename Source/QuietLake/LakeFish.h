#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeFish.generated.h"

class ALakeCharacter;

/**
 * A species of fish: its number in the species files (Content/LiveFish/Species/*.json, "order").
 * The four first ones are named here for the code and the tests; any further species is added with
 * its file alone (LakeFish::NumSpecies). The hidden top value makes the network carry 6 bits.
 */
UENUM()
enum class EFishSpecies : uint8 { Perch, Roach, Crucian, Pike, MaxSpecies = 63 UMETA(Hidden) };

UENUM()
enum class EBait : uint8 { Worm, Maggot, Bread, Count UMETA(Hidden) };

/**
 * What a free fish is doing with its day (see ALakeLife, "Live Fish"): feeding (predators hunt),
 * resting in cover, or cruising its patch of water. Chosen from hunger, tiredness, the species'
 * daily rhythm and the weather; school members follow their leader.
 */
UENUM()
enum class EFishGoal : uint8 { Cruise, Feed, Rest, Count UMETA(Hidden) };

/** What a fish eats (see LakeFood): the natural food of the pond, fry, other fish; None: no food in particular. */
enum class EFishFood : uint8 { None, Plankton, Insects, Benthos, Plants, Fry, Fish, Groundbait };

/** What the float does while a fish works at the bait. */
/** Where an angler is in the fishing cycle (replicated: every machine animates the rod from it). */
UENUM()
enum class EFishingState : uint8
{
    Idle,       // no line in the water
    Waiting,    // the bait is in the water (also while the rig is being lifted out)
    Bite,       // a fish holds the bait: strike now
    Fight,      // hooked: playing the fish
    Landing,    // a tired fish lifted out of the water on the line and swung to the hand
};

/** How the cast went: let go with the rod tip loaded the rig lays out straight; too soon it falls short in a heap. */
UENUM()
enum class ECastResult : uint8 { Good, Early };

UENUM()
enum class EBiteMotion : uint8
{
    None,       // still (a pause between tries)
    Tremble,    // fine quick quivering on the spot: lips at the bait
    Peck,       // a short dip and back: a tug at the bait
    Lift,       // rises slowly (the bait and the shot lifted off the bottom); lifted high it lies down
    Drift,      // moves off to the side, slowly settling at a slant
    Sink,       // pulled under and away
    Hold,       // stays where the fish led it: the fish holds the bait still (a pike turning its prey, a perch swallowing)
};

/**
 * One step of a bite as the float shows it. Amount: centimetres of quiver, dip, rise or settling
 * (Tremble, Peck, Lift, Drift); cm/s the float is pulled under (Sink). bTake: the fish holds the
 * bait in its mouth now, a strike can hook it - with the chance Hook (a pike still carrying a bait
 * across its jaws, a crucian only just lifting it: the hook may not go home). bLetGo: at the end of
 * this step the fish lets go of the bait (it may come back for another try).
 */
struct FBiteStep
{
    EBiteMotion Motion = EBiteMotion::None;
    float Duration = 1;
    float Amount = 0;
    bool bTake = false;
    float Hook = 1;
    bool bLetGo = false;
};

/** How a bite has gone so far; the fish decides each next step from it (LakeFish::NextBiteStep). */
struct FBiteState
{
    int32 Tries = 0;          // tests of the bait this time at it (trembles, pecks)
    int32 Phase = 0;          // 0: testing; 1.. the species' take, step by step
    int32 Variant = 0;        // which way it takes it (a crucian lifting it off the bottom or leading it off)
    bool bTried = false;      // the last step was a try: a pause comes next
    float Felt = 0;           // what put it on guard: the float's pull, the rig moved (0..)
    bool bDone = false;       // it lets go after the current step
};

/**
 * Where in the lake a species lives: how strongly it is drawn to each kind of water (0..1, see
 * LakeMap::FZones) and the water depth it keeps to. Home spots and wanderings are chosen by these
 * weights.
 */
USTRUCT()
struct FFishHabitat
{
    GENERATED_BODY()
    UPROPERTY() float Shallows = 0;     // warm weedy backwater, lily beds
    UPROPERTY() float ReedEdge = 0;     // just outside the reed belt
    UPROPERTY() float DropOff = 0;      // where the bed falls away, and the water at its foot
    UPROPERTY() float Cover = 0;        // snags and pier piles
    UPROPERTY() float Deep = 0;         // the deep middle and the hole
    UPROPERTY() float Open = 0;         // open water away from the bank
    UPROPERTY() float MinDepthM = 0.5f;
    UPROPERTY() float MaxDepthM = 3;
    UPROPERTY() bool bAmbush = false;   // waits motionless at one spot and strikes from it
};

/** What the fish feel of the time and the weather right now (ALakeWeather builds it on the server). */
struct FFishConditions
{
    float SunElevation = 30;         // degrees
    bool bMorning = true;            // before solar noon
    float WaterTempC = 19;
    float Cloud = 0.3f, Rain = 0, Wind = 0.1f, Fog = 0;
    float PressureTrend = 0;         // mm Hg per game hour; negative = falling, a front coming
    float Shock = 0;                 // 0..1 just after a sharp change of the weather
    float StableHours = 0;           // game hours of steady weather
    FVector2D WindDir = FVector2D(1, 0);
};

/**
 * How a species' appetite and boldness follow the time and the weather. Activity multiplies how
 * often the fish notices bait, how readily it takes it and how long it holds it, and how lively it
 * swims; 1 is an ordinary good day.
 */
USTRUCT()
struct FFishMood
{
    GENERATED_BODY()
    // By time of day.
    UPROPERTY() float Dawn = 1;
    UPROPERTY() float Day = 0.7f;
    UPROPERTY() float Dusk = 1;
    UPROPERTY() float Night = 0.3f;
    // Water temperature (C): sluggish towards the ends of the range.
    UPROPERTY() float TempLow = 6;
    UPROPERTY() float TempBest = 18;
    UPROPERTY() float TempHigh = 27;
    UPROPERTY() float Overcast = 0;      // -1..+1 by day: prefers sun .. prefers a grey sky
    UPROPERTY() float LightRain = 1;     // multipliers
    UPROPERTY() float HeavyRain = 0.6f;
    UPROPERTY() float Ripple = 1;        // a light breeze
    UPROPERTY() float StrongWind = 0.6f;
    UPROPERTY() float Fog = 1;           // thick mist on the water
    UPROPERTY() float Front = 0.8f;      // 0..1: feeds before a front, sulks after a sharp change
};

/**
 * How a species moves its body (see ALakeFish::Move). Turns are arcs: the turn rate is limited by
 * the speed (no tighter than TurnRadius body lengths) plus what the fins give on the spot
 * (HoverTurn), and it builds up and dies away with TurnAccel. Steering is smoothed over Response
 * seconds, so a fish never twitches from one wish to the next. Wander: how much a fish meanders
 * off its course (degrees) and how quickly (Hz). Gait: kick-and-glide (perch) - share of the
 * stroke spent kicking (1 = steady swimming). Bigger fish of a species are slower to turn and to
 * speed up (Agility); small ones are nimble.
 */
USTRUCT()
struct FFishGait
{
    GENERATED_BODY()
    UPROPERTY() float HoverTurn = 40;     // deg/s turning with fins only, at a standstill
    UPROPERTY() float TurnRadius = 0.6f;  // body lengths, tightest arc at cruise
    UPROPERTY() float TurnAccel = 300;    // deg/s^2
    UPROPERTY() float Response = 0.4f;    // s
    UPROPERTY() float WanderDeg = 20;
    UPROPERTY() float WanderHz = 0.1f;
    UPROPERTY() float Kick = 1;           // 0..1 share of the stroke spent kicking; 1 = steady
    UPROPERTY() float KickPeriod = 1;     // s, one kick and glide
    UPROPERTY() float Spacing = 1.4f;     // body lengths between school mates
};

/**
 * One move of a bite: what the float does and for how long. Seconds and Amount are drawn between
 * the value and its Max (when given); Amount grows from AmountSmall to AmountBig times with the
 * fish's size. Paced: shorter for a sluggish fish, never below MinPace of it. bTake: the fish has
 * the bait in its mouth; Hook: the chance a strike now sets the hook. Felt: how much the float's
 * pull puts it on guard. Weight: how often a try is this one.
 */
USTRUCT()
struct FFishBiteMove
{
    GENERATED_BODY()
    UPROPERTY() EBiteMotion Motion = EBiteMotion::None;
    UPROPERTY() float Seconds = 1;
    UPROPERTY() float SecondsMax = 0;
    UPROPERTY() float Amount = 0;
    UPROPERTY() float AmountMax = 0;
    UPROPERTY() float AmountSmall = 1;
    UPROPERTY() float AmountBig = 1;
    UPROPERTY() bool bTake = true;
    UPROPERTY() float Hook = 1;
    UPROPERTY() bool bPaced = false;
    UPROPERTY() float MinPace = 0;
    UPROPERTY() float Felt = 0;
    UPROPERTY() float Weight = 1;
};

/**
 * How a species takes the bait (see LakeFish::NextBiteStep). Testing: tries (Tries, one drawn by
 * weight) with pauses between them; after MinTries (SmallExtraTries more for the smallest fish)
 * each pause ends in a choice - take it (TakeChance, times TakeSmall..TakeBig with size, raised by
 * appetite, liking and tries), try again, or let it be. The take: the moves of Take, or of
 * TakeFromBottom with FromBottomChance when the bait lies on the bottom; after the last move it
 * lets go. Sensitive: how much it may feel before it spits the bait. Comeback/MaxVisits/Return:
 * coming back after letting go. RunMin..RunMax: cm it swims off with the bait as it pulls the
 * float under. DirectHold: seconds held when set on the bait directly (tests). EarlyStrikeText:
 * shown when a strike misses.
 */
USTRUCT()
struct FFishBiteStyle
{
    GENERATED_BODY()
    UPROPERTY() int32 MinTries = 0;
    UPROPERTY() int32 SmallExtraTries = 0;
    UPROPERTY() int32 MaxTries = 3;
    UPROPERTY() float TakeChance = 0.5f;
    UPROPERTY() float TakeSmall = 1;
    UPROPERTY() float TakeBig = 1;
    UPROPERTY() float PauseMin = 0.5f;
    UPROPERTY() float PauseMax = 1.5f;
    UPROPERTY() TArray<FFishBiteMove> Tries;
    UPROPERTY() TArray<FFishBiteMove> Take;
    UPROPERTY() TArray<FFishBiteMove> TakeFromBottom;
    UPROPERTY() float FromBottomChance = 0;
    UPROPERTY() float RunMin = 400;
    UPROPERTY() float RunMax = 400;
    UPROPERTY() float Sensitive = 1;
    UPROPERTY() float Comeback = 0.8f;
    UPROPERTY() int32 MaxVisits = 3;
    UPROPERTY() float ReturnMin = 3;
    UPROPERTY() float ReturnMax = 10;
    UPROPERTY() float DirectHold = 3;
    UPROPERTY() FString EarlyStrikeText;
};

/**
 * What a species eats: 0..1 how much it goes for each food (see LakeFood). DietBig: the same for the
 * biggest fish of the species, the others in between by size (a small perch eats water fleas and
 * bloodworm, a big one fish); left out, the diet is the same at every size.
 */
USTRUCT()
struct FFishDiet
{
    GENERATED_BODY()
    UPROPERTY() float Plankton = 0;   // water fleas in the open water: deep by day, at the top at night
    UPROPERTY() float Insects = 0;    // hatching and fallen insects at the top: calm dawns and evenings, by reeds and trees
    UPROPERTY() float Benthos = 0;    // bloodworm, larvae and snails in the silt
    UPROPERTY() float Plants = 0;     // soft weed and algae
    UPROPERTY() float Fry = 0;        // fry shoals in the shallows
    UPROPERTY() float Fish = 0;       // other fish (up to Feeding.PreyMaxWeight of its own weight)
    UPROPERTY() float Groundbait = 0; // the angler's groundbait: crumbs and their smell (what is mixed in draws by the rest of the diet)
    float Of(EFishFood Food) const;
    bool IsEmpty() const { return Plankton + Insects + Benthos + Plants + Fry + Fish + Groundbait <= 0; }
};

/**
 * How a species feeds and keeps itself. bPredator: it hunts (the F3 panel says so). bHuntsFry:
 * drives fry shoals in packs. PreyMaxWeight: strikes from ambush at fish up to this share of its own
 * weight; such a fish is not alarmed by a catch nearby. bRootsInSilt: feeds nose down in the silt,
 * bubbles rising. SipAtDusk: chance a feeding fish takes insects off the top at dawn and dusk.
 * FeedColumn: where in the water it feeds (0 the top .. 1 the bottom; -1: its usual layer), RestColumn
 * where it rests. BrowseStep: cm between feeding patches; BrowseSpeed: its pace there. FillRate: how
 * fast feeding fills it (per second; 0: only by eating prey). Bask: chance to lie just under the top on
 * warm calm sunny days. NightShallows: how far into the shallows it moves at night. Unflappable:
 * disturbances weaker than this leave it be. bJumps: leaps clear of the water now and then.
 * Among the species: Wariness - how strongly it fears a fish big enough to eat it (keeps away, a
 * school closes up, it stops biting); Camouflage - a predator lying still is noticed only that much
 * closer; Dominance - with its size, how it pushes others off a bait; StrikeChance - how often a
 * predator's strike catches (more when hungry, less at alert prey). HungerRate: how fast it gets
 * hungry again (1 = in a few minutes; a fish-eater digests a meal far longer). LightShy: how much it
 * keeps out of bright open shallows by day (herons, grebes and terns hunt there), coming in at dusk.
 */
USTRUCT()
struct FFishFeeding
{
    GENERATED_BODY()
    UPROPERTY() bool bPredator = false;
    UPROPERTY() bool bHuntsFry = false;
    UPROPERTY() float PreyMaxWeight = 0;
    UPROPERTY() bool bRootsInSilt = false;
    UPROPERTY() float SipAtDusk = 0;
    UPROPERTY() float FeedColumnMin = -1;
    UPROPERTY() float FeedColumnMax = -1;
    UPROPERTY() float RestColumn = 0.8f;
    UPROPERTY() float BrowseStep = 500;
    UPROPERTY() float BrowseSpeed = 0.55f;
    UPROPERTY() float FillRate = 0.012f;
    UPROPERTY() float Bask = 0;
    UPROPERTY() float NightShallows = 0;
    UPROPERTY() float Unflappable = 0;
    UPROPERTY() bool bJumps = false;
    UPROPERTY() float Wariness = 0.5f;
    UPROPERTY() float Camouflage = 0;
    UPROPERTY() float Dominance = 1;
    UPROPERTY() float StrikeChance = 0.3f;
    UPROPERTY() float HungerRate = 1;    // how fast it gets hungry again (a pike digests a fish for a long while)
    UPROPERTY() float LightShy = 0;      // keeps out of bright open shallows by day
};

/** How many of the species the pond holds at the start (see ALakeLife::StockPond), in schools of SchoolSize (1: alone). */
USTRUCT()
struct FFishStock
{
    GENERATED_BODY()
    UPROPERTY() int32 Count = 0;
    UPROPERTY() int32 SchoolSize = 1;
};

/** What the F3 panel says of the species' signs on the water (empty: a general note). */
USTRUCT()
struct FFishSigns
{
    GENERATED_BODY()
    UPROPERTY() FString Rise;
    UPROPERTY() FString Bubbles;
    UPROPERTY() FString Swirl;
    UPROPERTY() FString Splash;
    UPROPERTY() FString Scatter;
    UPROPERTY() FString Jump;
};

/**
 * The passport of a species: everything Live Fish knows of it, read from its file in
 * Content/LiveFish/Species (see the README.md there). Speeds in cm/s, distances in cm, weights in
 * kg, depths in m.
 */
USTRUCT()
struct FFishSpeciesInfo
{
    GENERATED_BODY()
    UPROPERTY() int32 Order = -1;         // its number (EFishSpecies); the files are sorted by it
    UPROPERTY() FString Id;               // the file's name
    UPROPERTY() FString Name;             // shown to the player
    UPROPERTY() FString Noun;             // in a sentence: lowercase
    UPROPERTY() FString Description;
    UPROPERTY() FString Mesh;             // asset path
    UPROPERTY() float MeshLength = 20;    // cm, length of the imported mesh
    UPROPERTY() float Condition = 1.2f;   // Fulton K: weight[g] = K * length[cm]^3 / 100
    UPROPERTY() float MinWeight = 0.1f;
    UPROPERTY() float MaxWeight = 1;
    UPROPERTY() float GirthGrowth = 0.14f;   // how much deeper-bodied the biggest fish are
    UPROPERTY() FFishHabitat Habitat;     // where it lives
    UPROPERTY() FFishHabitat FeedHabitat; // where it feeds (predators: where they hunt or lie in ambush)
    UPROPERTY() FFishHabitat RestHabitat; // where it rests
    UPROPERTY() float DepthMin = 0.3f;    // preferred band in the water column, 0 = surface, 1 = bottom
    UPROPERTY() float DepthMax = 0.7f;
    UPROPERTY() float CruiseSpeed = 20;
    UPROPERTY() float BurstSpeed = 120;
    UPROPERTY() float TurnRate = 200;     // deg/s at cruise
    UPROPERTY() float Acceleration = 300; // cm/s^2
    UPROPERTY() float TailBeat = 9;       // base tail beat rate (rad/s)
    UPROPERTY() FFishGait Gait;
    UPROPERTY() float SenseRadius = 500;  // how far it notices bait
    UPROPERTY() TMap<FString, float> Baits;  // worm, maggot, bread: 0..1 how it likes each
    UPROPERTY() FFishBiteStyle Bite;
    UPROPERTY() FFishFeeding Feeding;
    UPROPERTY() FFishDiet Diet;           // what it eats (see LakeFood)
    UPROPERTY() FFishDiet DietBig;        // what the biggest fish eat (empty: as Diet)
    UPROPERTY() FFishMood Mood;
    UPROPERTY() float Strength = 1;       // thrust per kg relative to a perch, used when hooked
    UPROPERTY() float Stamina = 10;       // seconds of hard pulling at full strength
    UPROPERTY() float RunBurst = 0.5f;    // s of a hooked fish's burst (plus more for a big one)
    UPROPERTY() FFishStock Stock;
    UPROPERTY() FFishSigns Signs;
    bool IsSchooling() const { return Stock.SchoolSize > 1; }
    float BaitLike(EBait Bait) const;
};

/** Smooths a fish's position and heading between network updates on clients (testable on its own). */
struct QUIETLAKE_API FFishNetSmoothing
{
    /**
     * A replicated snapshot arrived (Now: local game time; RealNow: local unclamped time, the
     * world's real time, or Now when not given).
     */
    void Receive(const FVector& Location, const FVector& Velocity, const FRotator& Rotation, double Now, double RealNow = -1);
    /** Advance the shown motion by Dt (Now: local time after the step). */
    void Advance(float Dt, double Now);
    FVector Location = FVector::ZeroVector, Velocity = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    float YawRate = 0;              // deg/s, as shown
    bool bReady = false;
    /** Set when the shown motion started afresh after this machine stood still (the owner clears it). */
    bool bRestarted = false;
private:
    FVector NetLocation = FVector::ZeroVector, NetVelocity = FVector::ZeroVector;
    FRotator NetRotation = FRotator::ZeroRotator;
    float NetYawRate = 0;
    double LastReceive = -1, LastReceiveReal = -1;
    FVector Offset = FVector::ZeroVector, OffsetVelocity = FVector::ZeroVector;   // shown minus predicted
    float YawOffset = 0, YawOffsetRate = 0;
};

namespace LakeFish
{
    /** The species' passport (Content/LiveFish/Species/*.json, read once). */
    QUIETLAKE_API const FFishSpeciesInfo& Info(EFishSpecies Species);
    /** How many species there are (the files). */
    QUIETLAKE_API int32 NumSpecies();
    /** Problems found reading the species files (unknown fields, bad numbers), for the log and the tests. */
    QUIETLAKE_API const TArray<FString>& SpeciesProblems();
    /** Read one species file's text (File: its name, for the messages); false if it is no passport at all. */
    QUIETLAKE_API bool ReadSpecies(const FString& Text, const FString& File, FFishSpeciesInfo& Out, TArray<FString>& Problems);
    QUIETLAKE_API const FFishGait& Gait(EFishSpecies Species);
    QUIETLAKE_API const TCHAR* BaitName(EBait Bait);
    /** 0..1: where a weight falls between the species' smallest and biggest fish. */
    QUIETLAKE_API float Size01(EFishSpecies Species, float WeightKg);
    /** Body depth and width relative to the model: big fish of a species are deeper-bodied. */
    QUIETLAKE_API float Girth(EFishSpecies Species, float WeightKg);
    /** Length in cm of a fish of the given species and weight (Fulton's condition factor). */
    QUIETLAKE_API float LengthCm(EFishSpecies Species, float WeightKg);
    /**
     * 0..1 how attractive this bait at this depth is to the species, before mood and fear. FeedColumn
     * (0 top .. 1 bottom): where the fish is feeding now - bait in that layer is found, off it seldom;
     * < 0: the species' usual layer.
     */
    QUIETLAKE_API float BaitInterest(EFishSpecies Species, EBait Bait, float DepthFraction, float FeedColumn = -1);
    /** What a fish of this size eats: the diet of the small ones blending into that of the biggest. */
    QUIETLAKE_API FFishDiet DietOf(EFishSpecies Species, float Size01);
    /** 0..1 how good this point of the lake is for the species to live at (now, if conditions are given). */
    QUIETLAKE_API float HabitatScore(EFishSpecies Species, float X, float Y, const FFishConditions* Conditions = nullptr);
    /** A good place for the species, near Near within Range cm (Range <= 0: anywhere in the lake), for what it is doing. */
    /** Weigh: an extra weight for each candidate place (the food there), or none. */
    QUIETLAKE_API FVector PickSpot(EFishSpecies Species, FRandomStream& Random, FVector2D Near, float Range, const FFishConditions* Conditions = nullptr,
                                   EFishGoal Goal = EFishGoal::Cruise, const TFunction<float(const FVector2D&)>& Weigh = nullptr);
    QUIETLAKE_API const FFishHabitat& HabitatFor(EFishSpecies Species, EFishGoal Goal);
    QUIETLAKE_API const TCHAR* GoalName(EFishSpecies Species, EFishGoal Goal);
    /**
     * A fish at the bait decides its next move from how the bite has gone so far, as anglers know it:
     *  crucian - tests the bait at length (the float trembles, pauses, trembles; small ones keep at
     *            it), then lifts it off the bottom - the float rises and, lifted high, lies down -
     *            and leads it off to the side, slowly, holding it for 4-8 s; bait off the bottom: a
     *            dip and a slow lead away;
     *  roach   - quick pecks, then the float goes under at a slant or is led aside (from the bottom
     *            it sometimes lifts); holds it 2-4 s and spits it as soon as it feels the float pull;
     *  perch   - a tap or two, then the float is pulled under sharply and led away; then it stands
     *            swallowing the worm, the float held under - a late strike still hooks it;
     *  pike    - grabs the bait and runs off with it across its jaws (a strike now often misses),
     *            stops to turn it (the float still), then a second run swallowing it: strike then.
     * After each try it takes the bait, tries again, or lets it be; a liked bait, appetite and a
     * long try make it take, a poor bait, a sluggish mood and anything it felt (the rig moved under
     * its nose - Disturb 0..1, the float pulling against it) make it let go. Like 0..1: how much it
     * likes this bait at this depth (LakeFish::BaitInterest). Size01 0..1 within the species,
     * Activity as LakeFish::Activity, bWary: bright calm water.
     */
    QUIETLAKE_API FBiteStep NextBiteStep(EFishSpecies Species, FBiteState& State, float Size01, float Activity, bool bBaitOnBottom, bool bWary,
                                         float Like, float Disturb, FRandomStream& Random);
    /** A whole bite left undisturbed on a favourite bait, until the fish lets go (for tests and previews). */
    QUIETLAKE_API TArray<FBiteStep> MakeBite(EFishSpecies Species, float Size01, float Activity, bool bBaitOnBottom, bool bWary, FRandomStream& Random);
    /** What the angler sees, for the status line. */
    QUIETLAKE_API const TCHAR* BiteText(EBiteMotion Motion, float Amount);
    /** Appetite and boldness now, about 0.05..1.6 (1 = an ordinary good day). */
    QUIETLAKE_API float Activity(EFishSpecies Species, const FFishConditions& Conditions);
    /** Metres the species moves its depth band: deeper in warm water and bright sun, up under a grey sky. */
    QUIETLAKE_API float DepthShiftM(EFishSpecies Species, const FFishConditions& Conditions);
    /** Bright sun on a glassy surface: fish see the angler and the line and are wary. */
    QUIETLAKE_API bool IsBrightCalm(const FFishConditions& Conditions);
    /** Water surface height (cm). */
    constexpr float SurfaceZ = -25.0f;
}

/**
 * A single fish. The server simulates it: steering behaviours pick a desired velocity
 * (meander through its habitat, school with its group, root at the bottom, lie in ambush,
 * investigate bait, shy away, flee, fight on the line), the wish is smoothed, and a swimming
 * model turns it into motion: thrust only along the body, turns as arcs with a turn rate that
 * builds up and dies away, water drag that keeps the motion along the body, mass from the
 * fish's weight, and the water volume as hard bounds. Clients extrapolate the replicated
 * position with the replicated velocity and blend out the error (FFishNetSmoothing). Every
 * machine bends the body from its own shown motion: tail beat from speed and effort, a C-curve
 * into turns.
 */
UCLASS()
class QUIETLAKE_API ALakeFish : public AActor
{
    GENERATED_BODY()
public:
    ALakeFish();
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;

    void Setup(EFishSpecies InSpecies, float InWeight, FVector Home, int32 InSchool);

    UPROPERTY(Replicated) EFishSpecies Species = EFishSpecies::Perch;
    UPROPERTY(Replicated) float WeightKg = 0.2f;

    // --- interaction with anglers (server) ---
    /** Scare the fish away from a point, e.g. the splash of a cast or a failed strike. */
    void Spook(FVector From, float Seconds);
    /** The angler struck while this fish held the bait. */
    void Hook(ALakeCharacter* Angler);
    /** Line tension pulls the hooked fish towards the rod tip; force in kg*cm/s^2. */
    void ApplyLineForce(FVector Force);
    /** The fish got off: lost bait, broken line or released. */
    void Release(float SpookSeconds);
    /**
     * It lets go of the bait (the angler did not strike, or it felt something): it backs off and,
     * still hungry and liking the bait, comes back for another try a few seconds later; otherwise it
     * leaves it alone for a minute.
     */
    void DropBait();
    /** Only trying the bait (not holding it yet). */
    bool IsNibbling() const { return State == EState::Nibble; }
    /** How it pushes others off a bait: its species' dominance and its size. */
    float Clout() const;
    /** A bigger fish took over the bait it was trying: it gives way (and may come back). */
    void PushedOff(ALakeFish* By);
    /** 0..1 how afraid it is now of a predator close by (see SenseThreats). */
    float FearNow() const { return Fear; }
    /** Holding the bait: the chance a strike now sets the hook (see FBiteStep::Hook). */
    float StrikeHookChance() const { return State == EState::Holding ? BiteNow.Hook : 1.0f; }
    /** Drawn out of the water on the line (server): from now on the angler's rod carries it. */
    void LiftOut();
    void LowerIntoWater(FVector LineVelocity);
    bool IsLifted() const { return bLifted; }
    /**
     * Out of the water on the line: placed on every machine from the rod's simulated hook. It hangs
     * from its mouth along the line, swings with it and thrashes; still in the water it is drawn
     * mouth first.
     */
    void HangOnLine(FVector Mouth, FVector LineUp, float DeltaSeconds);
    bool IsHooked() const { return State == EState::Hooked; }
    float StaminaFraction() const { return Stamina / FMath::Max(0.01f, MaxStamina); }
    FVector MouthLocation() const;
    FVector Velocity() const { return Vel; }
    /** Appetite from the time and weather, refreshed as the fish swims (1 without a weather actor). */
    float Appetite() const { return Activity; }

    /** Bounds of the swimmable water at a point (cm). */
    static float BottomZ(float X, float Y);

    // --- daily life (see ALakeLife) ---
    EFishGoal GetGoal() const { return Goal; }
    /** What it feeds on now (while feeding). */
    EFishFood GetFeedOn() const { return Goal != EFishGoal::Feed ? EFishFood::None : HasAuthority() ? FeedOn : (EFishFood)NetFeedOn; }
    int32 GetSchool() const { return School; }
    /**
     * Where the pond stocked this fish (its school's or its own place) and how far its home may move
     * from there with the weather. Homes move around the anchor, so a school stays in its water.
     */
    FVector GetHomeAnchor() const { return HomeAnchor; }
    float GetHomeDrift() const { return HomeDrift; }
    void SetHomeDrift(float Drift) { HomeDrift = Drift; }
    float GetHunger() const { return Hunger; }
    float GetEnergy() const { return Energy; }
    /** Free to live its life: not testing, holding or hooked on anyone's bait, not fleeing. */
    bool IsFree() const { return State == EState::Roam; }
    /** On someone's bait or line. */
    bool IsBusy() const { return State == EState::Investigate || State == EState::Nibble || State == EState::Holding || State == EState::Hooked; }
    /** Full simulation near players; far away the fish is stepped coarsely and replicated rarely. */
    void SetNearPlayers(bool bNear);
    /** A predator struck at the fry next to it, or a hunting school mate dashes: a short burst towards Target. */
    void Dash(FVector Target, float Seconds);
    /** Eat: a mouthful of fry or a whole fish (0..1 of a full meal). */
    void Eat(float Meal);
    /**
     * Something disturbed the water at From (a rig landing, steps on the bank, a fish thrashing):
     * Strength 0..1 at the fish. Weak: it turns and swims calmly away for a few seconds; strong
     * (close): it bolts.
     */
    void Startle(FVector From, float Strength);
    /** Shown turn rate (deg/s, positive to the right) and speed, on every machine. */
    float ShownYawRate() const { return ShownTurn; }
    float SpeedNow() const { return HasAuthority() ? (float)Vel.Size() : (float)Smooth.Velocity.Size(); }
    /** 0..1 within the species: big fish are calmer and slower to turn. */
    float SizeFraction() const { return Size01; }
    float TurnRateNow() const { return YawRate; }

    // Exposed for tests: advance the simulation by a fixed step.
    void Simulate(float DeltaSeconds);

private:
    enum class EState : uint8 { Roam, Investigate, Nibble, Holding, Hooked, Flee };
    void ChooseRoamTarget(bool bLocal = false);
    /** Pick what to do next from hunger, tiredness, the daily rhythm and the weather. */
    void Decide();
    void SetGoal(EFishGoal NewGoal, float Seconds);
    /** Goal behaviour while roaming: feeding signs, hunting, the speed it swims at. */
    void Live(float DeltaSeconds, FVector& Desired, float& MaxSpeed);
    void UpdatePikeAmbush(float DeltaSeconds, FVector& Desired, float& MaxSpeed);
    /** A predator's strike under way (at a fish or at fry): the dash, then caught or missed. */
    void UpdateStrike(FVector& Desired, float& MaxSpeed);
    /** A pursuit hunter (a big perch) looks for a small fish right in front of it to dash at. */
    void LookForPrey();
    /** Notices a fish close by that could eat it: fear, and which one (Threat). */
    void SenseThreats(float DeltaSeconds);
    /** The fish this one follows: the school leader, or itself. */
    const ALakeFish* Leader() const;
    FVector SteerRoam() const;
    /** School mates: keep apart, line up, stay together, follow the leader. bFollow: steering of a follower. */
    FVector SteerSchool(bool bFollow) const;
    FVector SteerBounds() const;
    /** Slow off-course meandering, as a rotation of the wished direction. */
    FVector Meander(const FVector& Desired) const;
    /** Pick the spot a pike lies at and which way it faces (out into open water from its cover). */
    void ChooseLie();
    /** Crucian at its feeding spot: nose down in the silt, then a short shuffle to the next patch. */
    void UpdateRooting(FVector& Desired, float& MaxSpeed);
    void UpdateBait(float DeltaSeconds);
    void Move(FVector DesiredVelocity, float MaxSpeed, float DeltaSeconds);
    /** The old direct-turning model, kept for a fish fighting on the line (its fight is tuned to it). */
    void MoveHooked(FVector DesiredVelocity, float MaxSpeed, float DeltaSeconds);
    void UpdateVisual(float DeltaSeconds);
    UFUNCTION() void OnRep_NetLocation();

    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Body;

    UPROPERTY(ReplicatedUsing = OnRep_NetLocation) FVector_NetQuantize10 NetLocation;
    UPROPERTY(Replicated) FVector_NetQuantize10 NetVelocity;
    UPROPERTY(Replicated) FRotator NetRotation;
    UPROPERTY(Replicated) float NetEffort = 0;   // 0..1, drives the tail beat on clients
    UPROPERTY(Replicated) bool bNetBurst = false; // in a burst (a strike, a rush at the fry): a fast start of its own
    UPROPERTY(Replicated) EFishGoal Goal = EFishGoal::Cruise;
    UPROPERTY(Replicated) uint8 NetFeedOn = 0;   // EFishFood, for the F3 panel on clients
    UPROPERTY(Replicated) bool bLifted = false;  // out of the water on an angler's line
    // Hanging on the line (cosmetic, every machine).
    float HangTime = 0, HangEffort = 0;
    FVector HangForward = FVector::ForwardVector;
    uint64 LastHangFrame = 0;

    EState State = EState::Roam;
    int32 School = -1;
    FVector HomePoint, RoamTarget, Vel = FVector::ZeroVector;
    FVector HomeAnchor = FVector::ZeroVector;
    float HomeDrift = 2500;
    FVector FleeFrom;
    float StateTime = 0, FleeTime = 0, NextThink = 0, Hunger = 1, Effort = 0;
    float Stamina = 1, MaxStamina = 1, BurstTime = 0, Phase = 0;
    int32 NibblesLeft = 0;
    TWeakObjectPtr<ALakeCharacter> TargetAngler;
    TMap<TWeakObjectPtr<ALakeCharacter>, float> IgnoreBaitUntil;
    FVector PendingLineForce = FVector::ZeroVector;
    float SimTime = 0;
    float NextRun = 0;                // hooked: seconds until the next run
    float Activity = 1;               // see LakeFish::Activity
    float NextMoodCheck = 0;
    FFishConditions Seen;             // the time and the weather, looked at with the mood
    bool bSeen = false;
    // Daily life.
    float Energy = 1;                 // 1 rested .. 0 tired
    float GoalUntil = 0;              // SimTime the current goal is reconsidered
    float NextSign = 0;               // next feeding sign (bubbles, a rise)
    float NextLook = 0;               // predators: next look for prey
    float NextFryAttack = 0;
    float StrikeEnd = -1;             // pike: SimTime the strike ends
    TWeakObjectPtr<ALakeFish> Prey;   // pike: the fish it strikes at
    int32 PreyFry = -1;               // pike/perch: the fry shoal it strikes at
    EFishFood FeedOn = EFishFood::None; // what it went to feed on
    float FeedFound = 0;              // how much of it there was where it chose to feed
    float NextFoodCheck = 0;          // when it next sees whether the patch is eaten down
    bool bAtSpot = false;             // reached its feeding or resting spot
    bool bNear = true;                // full detail
    float LastBeat = -1, LastAmp = -1;// what the material was last given
    // The bite in progress (see LakeFish::NextBiteStep).
    FBiteStep BiteNow;
    FBiteState BiteState;
    float BiteStepEnd = 0;
    int32 BaitVisits = 0;             // times it came to this angler's bait in a row
    TWeakObjectPtr<ALakeCharacter> ReturnTo;   // let go of this bait meaning to come back to it
    // Fear of predators.
    float Fear = 0, NextThreatLook = 0;
    TWeakObjectPtr<ALakeFish> Threat;
    float HoldSpeed = 0.6f;           // swimming with the bait, fraction of the cruise speed
    // Body motion (server).
    float Yaw = 0, Pitch = 0, Roll = 0;   // degrees: the body's heading
    float YawRate = 0;                // deg/s, builds up and dies away
    FVector Wish = FVector::ZeroVector;   // smoothed desired velocity
    float WishSpeedNow = 0;           // its smoothed size
    FVector FaceDir = FVector::ZeroVector;// hovering: which way to turn the body (zero: keep)
    float PitchBias = 0;              // nose down while rooting (degrees)
    bool bPrecise = false;            // at the bait: may stop dead to aim the mouth
    float Size01 = 0, Agility = 1;    // see LakeFish::Size01; nimbleness from size
    float WanderSeed = 0;
    // Shying away from a disturbance (still free: not a flight).
    FVector AvoidFrom = FVector::ZeroVector;
    float AvoidUntil = -1, AvoidStrength = 0;
    // Dash at fry (perch rushing in with the school).
    FVector DashTarget = FVector::ZeroVector;
    float DashUntil = -1;
    // Crucian rooting in the silt; pike lying in ambush.
    bool bRooting = false;
    float PhaseUntil = 0;             // rooting / shuffling switches at this SimTime
    FVector LieFacing = FVector::ForwardVector;
    float LieUntil = 0;               // pike: SimTime it moves to a new lie
    float NextSurfaceSign = 0;
    FVector SlotOffset = FVector::ZeroVector; // school follower: its place beside the leader (leader's frame)
    // Shown motion (every machine).
    FFishNetSmoothing Smooth;
    FVector ShownPrevLocation = FVector::ZeroVector, ShownPrevVelocity = FVector::ZeroVector;
    float ShownTurn = 0, ShownBend = 0, ShownPrevYaw = 0, ShownSpeed = 0, ShownPrevFrameTurn = 0, ShownWindow = 0;
    float LastBend = 1000, LastGirth = -1;
    bool bShownValid = false;
    float BurstSeenAt = -100;   // when this machine last saw it bursting
    void NextBiteStep(class ALakeCharacter* Angler);
    /** How much it likes the angler's bait where it hangs, 0..1. */
    float BaitLike(const class ALakeCharacter* Angler) const;
    friend class FLakePondLifeTest;
    friend class FLakeFishMotionTest;
    friend class FLakeBitePatternsTest;
    friend class FLakeInteractionsTest;
    friend class FLakeFoodTest;
    friend class FLakeGroundbaitTest;
    friend class ALakeLife;   // the -FishPreview scenes set fish up directly
    /** Current conditions from the weather; false (and neutral values) without a weather actor. */
    bool GetConditions(FFishConditions& Out) const;
    FRandomStream Random;
};
