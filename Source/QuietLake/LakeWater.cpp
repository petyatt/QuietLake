#include "LakeWater.h"
#include "LakeMap.h"

namespace
{
    float S(float A, float B, float X) { return FMath::SmoothStep(A, B, X); }
    /** Oxygen the water holds at saturation (mg/l) at a temperature. */
    float Saturation(float T) { return 14.6f - 0.394f * T + 0.0077f * T * T; }
    float Calm(const FFishConditions& C) { return 1 - S(0.2f, 0.45f, C.Wind); }
}

float LakeWater::Stratification(const FFishConditions& C)
{
    // Hours of steady calm warm weather build it; a breeze mixes the water; the cool of the night
    // stirs the top a little.
    const float Warm = S(17.0f, 22.0f, C.WaterTempC);
    const float Night = 1 - S(-10.0f, -4.0f, C.SunElevation);
    return Calm(C) * Warm * S(0.0f, 12.0f, C.StableHours) * (1 - 0.2f * Night);
}

float LakeWater::ThermoclineM(const FFishConditions& C)
{
    return 2.2f + 0.8f * (1 - Stratification(C));
}

float LakeWater::TempC(const FFishConditions& C, float DepthM)
{
    const float Strat = Stratification(C);
    const float Thermo = ThermoclineM(C);
    // The sun warms the top layer through the afternoon; under the drop the water stays cold.
    const float Afternoon = S(9.0f, 15.0f, C.Hour) * (1 - S(18.0f, 23.0f, C.Hour)) * (1 - 0.6f * C.Cloud);
    const float Top = C.WaterTempC + 3.0f * Strat * Afternoon;
    return Top - 0.3f * FMath::Min(DepthM, Thermo) - 7.0f * Strat * S(Thermo, Thermo + 1.5f, DepthM);
}

float LakeWater::WeedDeficit(const FFishConditions& C)
{
    // Dark hours since about half past nine in the evening; after sunrise the plants make it good by mid-morning.
    const float H = C.Hour;
    float Dark = H >= 21.5f ? H - 21.5f : H < 5.5f ? H + 2.5f : 8.0f;
    float Recover = H >= 5.5f && H < 21.5f ? 1 - S(5.5f, 8.5f, H) : 1.0f;
    if (H >= 8.5f && H < 21.5f) Recover = 0;
    const float Warm = S(14.0f, 24.0f, C.WaterTempC);
    const float Still = 1 - S(0.15f, 0.4f, C.Wind);
    return FMath::Clamp(0.13f * Dark * Warm * Still * Recover, 0.0f, 1.0f);
}

float LakeWater::WeedAt(float X, float Y)
{
    return FMath::Max(LakeMap::LilyAmount(X, Y), 0.6f * LakeMap::BackwaterAmount(X, Y));
}

float LakeWater::OxygenMgL(const FFishConditions& C, float X, float Y, float DepthM)
{
    const float T = TempC(C, DepthM);
    float O2 = Saturation(T);
    // Under the thermocline the water is cut off from the air and runs short of oxygen.
    const float Thermo = ThermoclineM(C);
    O2 *= 1 - 0.85f * Stratification(C) * S(Thermo, Thermo + 1.0f, DepthM);
    // Weed: oxygen bubbling off the leaves by day, taken by them at night (most near the bottom).
    const float Weed = WeedAt(X, Y);
    if (Weed > 0)
    {
        const float DaySun = S(5.0f, 25.0f, C.SunElevation) * (1 - 0.6f * C.Cloud);
        const float Water = FMath::Max(0.3f, LakeMap::Depth(X, Y) / 100);
        const float Low = 0.9f + 0.1f * FMath::Clamp(DepthM / Water, 0.0f, 1.0f);
        O2 = O2 * (1 - 0.9f * Weed * WeedDeficit(C) * Low) + 2.0f * Weed * DaySun;
    }
    return FMath::Max(0.3f, O2);
}

float LakeWater::LivableAt(EFishSpecies Species, const FFishConditions& C, float X, float Y, float DepthM)
{
    const FFishSpeciesInfo& Info = LakeFish::Info(Species);
    const float Min = FMath::Max(0.1f, Info.OxygenMin);
    const float Air = S(0.6f * Min, Min, OxygenMgL(C, X, Y, DepthM));
    const float Heat = 1 - S(Info.Mood.TempHigh, Info.Mood.TempHigh + 3, TempC(C, DepthM));
    return Air * (0.3f + 0.7f * Heat);
}

float LakeWater::Livable(EFishSpecies Species, const FFishConditions& C, float X, float Y)
{
    const float Water = LakeMap::Depth(X, Y) / 100;
    if (Water <= 0) return 0;
    float Best = 0;
    for (int32 I = 0; I < 5; ++I) Best = FMath::Max(Best, LivableAt(Species, C, X, Y, Water * (0.1f + 0.2f * I)));
    return Best;
}

float LakeWater::ToLivable(EFishSpecies Species, const FFishConditions& C, float X, float Y, float Z)
{
    const float Bottom = ALakeFish::BottomZ(X, Y);
    const float Top = LakeFish::SurfaceZ - 10, Low = Bottom + 8;
    if (Low >= Top) return Z;
    auto At = [&](float H) { return LivableAt(Species, C, X, Y, (LakeFish::SurfaceZ - H) / 100); };
    const float Here = At(Z);
    if (Here > 0.9f) return Z;
    // The nearest height that suits it clearly better, looking up first (the better water is
    // usually above the cold, stale bottom layer).
    for (float Step = 15; Step < Top - Low + 15; Step += 15)
    {
        const float Up = FMath::Min(Top, Z + Step), Down = FMath::Max(Low, Z - Step);
        if (At(Up) > FMath::Max(0.95f, Here + 0.2f)) return Up;
        if (At(Down) > FMath::Max(0.95f, Here + 0.2f)) return Down;
    }
    return Z;
}

bool LakeWater::IsGasping(EFishSpecies Species, float OxygenMgL)
{
    return OxygenMgL < 0.6f * LakeFish::Info(Species).OxygenMin;
}
