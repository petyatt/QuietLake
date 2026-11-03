#include "LakeFood.h"
#include "LakeMap.h"
#include "LakeWater.h"

namespace
{
    float S(float A, float B, float X) { return FMath::SmoothStep(A, B, X); }
    float NightOf(const FFishConditions& C) { return 1 - S(-10.0f, -4.0f, C.SunElevation); }

    /** What a place holds of each natural food by its nature (plankton, insects, silt fauna, weed). */
    void RichAll(float X, float Y, float Out[4])
    {
        Out[0] = Out[1] = Out[2] = Out[3] = 0;
        if (!LakeMap::IsWater(X, Y)) return;
        const LakeMap::FZones Z = LakeMap::ZonesAt(X, Y);
        const float D = LakeMap::Depth(X, Y) / 100;
        const float SD = LakeMap::ShoreDistance(X, Y) / 100;
        const float Lily = LakeMap::LilyAmount(X, Y);
        const float Backwater = LakeMap::BackwaterAmount(X, Y);
        // Plankton: the open water and over the deep; thinner among the weed and at the very edge.
        Out[0] = FMath::Clamp((0.35f + 0.65f * FMath::Max3(Z.Open, Z.Deep, 0.6f * Z.DropOff)) * (1 - 0.4f * Lily) * S(0.4f, 1.0f, D), 0.0f, 1.0f);
        // Insects: by the reeds and the lilies, under the trees along a wooded bank; a few everywhere.
        const float Trees = LakeMap::ForestAmount(FVector2D(X, Y)) * (1 - S(4.0f, 15.0f, SD));
        Out[1] = FMath::Clamp(0.12f + 0.88f * FMath::Max3(Z.ReedEdge, Lily, 0.8f * Trees), 0.0f, 1.0f);
        // Silt fauna: the soft silt of the warm weedy shallows and of the deep flats; little on the hard slope.
        Out[2] = FMath::Clamp(0.3f + 0.55f * FMath::Max(Backwater, Lily) * (1 - S(1.8f, 2.6f, D)) + 0.35f * S(2.5f, 4.0f, D) - 0.25f * Z.DropOff, 0.08f, 1.0f)
            * S(0.25f, 0.5f, D);
        // Soft weed: the lily beds, the reed edge, the weedy backwater.
        Out[3] = FMath::Clamp(FMath::Max3(Lily, 0.7f * Z.ReedEdge, 0.5f * Backwater * (1 - S(1.5f, 2.5f, D))), 0.03f, 1.0f);
    }
}

const TCHAR* LakeFood::Name(EFishFood Food)
{
    switch (Food)
    {
    case EFishFood::Plankton: return TEXT("планктон");
    case EFishFood::Insects: return TEXT("насекомые");
    case EFishFood::Benthos: return TEXT("мотыль и личинки");
    case EFishFood::Plants: return TEXT("растения");
    case EFishFood::Fry: return TEXT("малёк");
    case EFishFood::Fish: return TEXT("рыба");
    case EFishFood::Groundbait: return TEXT("прикормка");
    default: return TEXT("что попадётся");
    }
}

float LakeFood::Richness(EFishFood Food, float X, float Y)
{
    if (!IsNatural(Food)) return 0;
    float Rich[4];
    RichAll(X, Y, Rich);
    return Rich[(int32)Food - 1];
}

float LakeFood::Season(EFishFood Food, const FFishConditions& C)
{
    const float Night = NightOf(C);
    const float Day = S(8.0f, 18.0f, C.SunElevation);
    const float Twilight = FMath::Max(0.0f, 1 - Night - Day);
    const float Warm = S(10.0f, 20.0f, C.WaterTempC);
    switch (Food)
    {
    case EFishFood::Plankton: return 0.55f + 0.45f * Warm;
    case EFishFood::Insects:
    {
        // The evening rise (the morning one is smaller): insects hatch and fall on calm warm water;
        // a breeze and rain put an end to it.
        const float Calm = 1 - 0.75f * S(0.3f, 0.65f, C.Wind);
        const float Rain = 1 - 0.7f * S(0.35f, 0.8f, C.Rain);
        const float Hatch = (0.3f + 0.7f * Warm) * (Twilight * (C.bMorning ? 0.8f : 1.15f) + Day * 0.3f + Night * 0.04f);
        return FMath::Max(0.03f, Hatch * Calm * Rain);
    }
    case EFishFood::Benthos: return 0.8f + 0.3f * Night;   // larvae leave the silt in the dark
    case EFishFood::Plants: return 0.6f + 0.4f * Warm;
    default: return 0;
    }
}

float LakeFood::PlanktonDepthM(const FFishConditions& C)
{
    // Water fleas sink away from the light by day and rise to the top at dusk; a grey sky or fog lets them up.
    const float Light = S(-6.0f, 20.0f, C.SunElevation) * (1 - 0.35f * C.Cloud) * (1 - 0.3f * C.Fog);
    return FMath::Lerp(0.25f, 2.3f, Light);
}

float LakeFood::FeedingZ(EFishFood Food, float X, float Y, const FFishConditions& C, FRandomStream& Random)
{
    const float Bottom = ALakeFish::BottomZ(X, Y);
    const float Top = LakeFish::SurfaceZ - 10, Low = Bottom + 8;
    if (Low >= Top) return 0.5f * (Top + Low);
    switch (Food)
    {
    case EFishFood::Plankton:
    {
        // It does not go down into the stale water under the thermocline.
        const float Deep = LakeWater::Stratification(C) > 0.3f ? FMath::Min(PlanktonDepthM(C), LakeWater::ThermoclineM(C)) : PlanktonDepthM(C);
        return FMath::Clamp(LakeFish::SurfaceZ - Deep * 100 + Random.FRandRange(-25.0f, 25.0f), Low + 5, Top);
    }
    case EFishFood::Insects: return FMath::Max(Low, LakeFish::SurfaceZ - Random.FRandRange(10.0f, 28.0f));
    case EFishFood::Benthos: return FMath::Min(Top, Bottom + Random.FRandRange(6.0f, 14.0f));
    case EFishFood::Plants: return FMath::Lerp(Top, Low, Random.FRandRange(0.25f, 0.7f));
    case EFishFood::Groundbait: return FMath::Min(Top, Bottom + Random.FRandRange(4.0f, 18.0f));   // it lies on the bottom
    default: return FMath::Lerp(Top, Low, 0.5f);
    }
}

float LakeFood::BaitMatch(EFishFood Food, EBait Bait)
{
    //                         worm   maggot bread
    static const float Table[8][3] = {
        { 1.0f,  1.0f,  1.0f },    // none
        { 0.95f, 1.05f, 1.0f },    // plankton
        { 0.9f,  1.15f, 0.85f },   // insects
        { 1.15f, 1.05f, 0.9f },    // silt fauna
        { 0.85f, 0.95f, 1.15f },   // weed
        { 1.1f,  0.9f,  0.85f },   // fry
        { 1.1f,  0.9f,  0.85f },   // fish
        { 1.0f,  1.0f,  1.0f },    // groundbait (see ALakeFish::UpdateBait: what was mixed in)
    };
    return Table[FMath::Clamp((int32)Food, 0, 7)][FMath::Clamp((int32)Bait, 0, 2)];
}

// --- the field ---------------------------------------------------------------------------------

void FLakeFoodField::Init()
{
    const FBox2D Lake = LakeMap::LakeBounds();
    Origin = Lake.Min;
    NX = FMath::Max(1, FMath::CeilToInt((Lake.Max.X - Lake.Min.X) / Cell));
    NY = FMath::Max(1, FMath::CeilToInt((Lake.Max.Y - Lake.Min.Y) / Cell));
    Places.SetNum(NX * NY);
    for (TArray<float>& E : Eaten) E.Init(0, NX * NY);
    for (int32 Y = 0; Y < NY; ++Y)
        for (int32 X = 0; X < NX; ++X)
        {
            FPlace& P = Places[Y * NX + X];
            const FVector2D At = Origin + FVector2D((X + 0.5f) * Cell, (Y + 0.5f) * Cell);
            P.bWater = LakeMap::IsWater(At.X, At.Y) && LakeMap::Depth(At.X, At.Y) > 20;
            if (!P.bWater) continue;
            RichAll(At.X, At.Y, P.Rich);
            P.Shallow = 1 - S(0.8f, 1.4f, LakeMap::Depth(At.X, At.Y) / 100);
            P.NearBank = 1 - S(4.0f, 12.0f, LakeMap::ShoreDistance(At.X, At.Y) / 100);
            if (P.NearBank > 0) P.Outward = LakeMap::NearestShore(At).Outward;
        }
}

int32 FLakeFoodField::Index(const FVector2D& P) const
{
    if (NX <= 0) return -1;
    const int32 X = FMath::FloorToInt((P.X - Origin.X) / Cell), Y = FMath::FloorToInt((P.Y - Origin.Y) / Cell);
    return X >= 0 && Y >= 0 && X < NX && Y < NY ? Y * NX + X : -1;
}

float FLakeFoodField::Available(EFishFood Food, const FVector2D& P, const FFishConditions& C) const
{
    const int32 I = Index(P);
    if (I < 0 || !LakeFood::IsNatural(Food) || !Places[I].bWater) return 0;
    const FPlace& Pl = Places[I];
    const int32 F = (int32)Food - 1;
    float A = Pl.Rich[F] * LakeFood::Season(Food, C) * (1 - Eaten[F][I]);
    // At night the silt fauna of the shallows is there for the taking (and the dark keeps the fish safe there).
    if (Food == EFishFood::Benthos) A *= 1 + 0.35f * NightOf(C) * Pl.Shallow;
    // The wind blows insects onto the bank it blows towards.
    if (Food == EFishFood::Insects && Pl.NearBank > 0 && C.Wind > 0.15f)
        A *= 1 + 0.8f * C.Wind * FMath::Max(0.0f, FVector2D::DotProduct(Pl.Outward, C.WindDir)) * Pl.NearBank;
    return A;
}

float FLakeFoodField::Left(EFishFood Food, const FVector2D& P) const
{
    const int32 I = Index(P);
    return I >= 0 && LakeFood::IsNatural(Food) ? 1 - Eaten[(int32)Food - 1][I] : 0.0f;
}

void FLakeFoodField::Eat(EFishFood Food, const FVector2D& P, float Meal, float Kg)
{
    const int32 I = Index(P);
    if (I < 0 || !LakeFood::IsNatural(Food) || Meal <= 0) return;
    const int32 F = (int32)Food - 1;
    // A crucian of 0.4 kg filling up eats about a third of a rich patch; a school of roach clears one.
    Eaten[F][I] = FMath::Min(1.0f, Eaten[F][I] + Meal * Kg * 0.7f / FMath::Max(0.1f, Places[I].Rich[F]));
}

float FLakeFoodField::RegrowSeconds(EFishFood Food)
{
    switch (Food)
    {
    case EFishFood::Plankton: return 150;
    case EFishFood::Insects: return 60;
    case EFishFood::Benthos: return 900;
    case EFishFood::Plants: return 1200;
    default: return 600;
    }
}

void FLakeFoodField::Step(float DeltaSeconds)
{
    Clock += DeltaSeconds;
    Pending += DeltaSeconds;
    if (Pending < 1 || NX <= 0) return;
    // Groundbait dissolves and loses its smell.
    const float Dissolve = FMath::Exp(-Pending * UE_LN2 / DissolveSeconds);
    for (FGroundbaitSpot& G : GroundbaitSpots) G.Balls *= Dissolve;
    GroundbaitSpots.RemoveAll([](const FGroundbaitSpot& G) { return G.Balls < 0.05f; });
    for (int32 F = 0; F < 4; ++F)
    {
        const float Keep = FMath::Exp(-Pending * UE_LN2 / RegrowSeconds((EFishFood)(F + 1)));
        for (float& E : Eaten[F]) if (E > 0) E = E < 0.002f ? 0.0f : E * Keep;
    }
    Pending = 0;
}

float FLakeFoodField::MeanLeft(EFishFood Food) const
{
    if (!LakeFood::IsNatural(Food)) return 0;
    double Sum = 0;
    int32 N = 0;
    for (int32 I = 0; I < Places.Num(); ++I)
        if (Places[I].bWater) { Sum += 1 - Eaten[(int32)Food - 1][I]; ++N; }
    return N ? float(Sum / N) : 1.0f;
}

int32 FLakeFoodField::EatenPatches(EFishFood Food, float Below) const
{
    if (!LakeFood::IsNatural(Food)) return 0;
    int32 N = 0;
    for (const float E : Eaten[(int32)Food - 1]) N += 1 - E < Below;
    return N;
}

// --- groundbait --------------------------------------------------------------------------------

float LakeFood::GroundbaitAppeal(const FFishDiet& D, const FGroundbaitSpot& G)
{
    const float Total = FMath::Max(0.01f, G.Added[0] + G.Added[1] + G.Added[2]);
    const float Worm = G.Added[0] / Total, Maggot = G.Added[1] / Total, Bread = G.Added[2] / Total;
    const float Mixed = Worm * 0.6f * D.Benthos + Maggot * 0.35f * (D.Insects + D.Benthos) + Bread * 0.3f * D.Plants;
    return FMath::Clamp(FMath::Max(D.Groundbait, Mixed) + 0.3f * FMath::Min(D.Groundbait, Mixed), 0.0f, 1.3f);
}

EBait LakeFood::GroundbaitMix(const FGroundbaitSpot& G)
{
    int32 Most = 2;
    for (int32 I = 0; I < 3; ++I) if (G.Added[I] > G.Added[Most]) Most = I;
    return (EBait)Most;
}

float LakeFood::GroundbaitRichness(const FGroundbaitSpot& G)
{
    return 0.9f + 0.5f * (1 - FMath::Exp(-G.Balls / 2));
}

void FLakeFoodField::AddGroundbait(const FVector2D& P, EBait With)
{
    const int32 Mix = FMath::Clamp((int32)With, 0, 2);
    for (FGroundbaitSpot& G : GroundbaitSpots)
        if (FVector2D::Distance(G.Location, P) < 150)
        {
            // Onto the same patch: it grows and shifts a little towards the new ball.
            G.Location = (G.Location * G.Balls + P) / (G.Balls + 1);
            G.Balls += 1;
            G.Added[Mix] += 1;
            return;
        }
    FGroundbaitSpot G;
    G.Location = P;
    G.Balls = 1;
    G.Added[Mix] = 1;
    G.Since = Clock;
    GroundbaitSpots.Add(G);
}

float FLakeFoodField::Scent(const FGroundbaitSpot& G, const FVector2D& P) const
{
    // In still water the cloud and the smell spread slowly: a few metres at first, about 15 m after three minutes.
    const float Age = Clock - G.Since;
    const float Strength = 1 - FMath::Exp(-G.Balls / 2);
    const float Build = S(0.0f, 60.0f, Age);
    const float Reach = 600 + 900 * S(0.0f, 180.0f, Age);
    return Strength * Build * (1 - S(0.0f, Reach, (float)FVector2D::Distance(P, G.Location)));
}

int32 FLakeFoodField::SmellGroundbait(const FVector2D& P, float& OutScent) const
{
    int32 Best = -1;
    OutScent = 0;
    for (int32 I = 0; I < GroundbaitSpots.Num(); ++I)
    {
        const float Here = Scent(GroundbaitSpots[I], P);
        if (Here > OutScent) { OutScent = Here; Best = I; }
    }
    return Best;
}

void FLakeFoodField::EatGroundbait(int32 Spot, float Meal, float Kg)
{
    // A crucian of 0.4 kg filling up eats about a quarter of a ball; a school of roach half a ball.
    if (GroundbaitSpots.IsValidIndex(Spot)) GroundbaitSpots[Spot].Balls = FMath::Max(0.0f, GroundbaitSpots[Spot].Balls - Meal * Kg * 0.7f);
}
