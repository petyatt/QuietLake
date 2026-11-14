#include "LakeFishSpine.h"
#include "LakeFish.h"
#include "LakeMap.h"

namespace
{
    // Along the body from the mouth (0) to the tail tip (1): how deep the body is there (0..1 of its
    // greatest depth) and what share of the fish's weight a point carries.
    float Profile(float S) { return FMath::Max(0.12f, FMath::Sin(PI * FMath::Clamp(S * 0.9f + 0.06f, 0.0f, 1.0f))); }
}

void FFishSpine::Start(const FVector& Mouth, const FVector& Forward, float Length)
{
    Seg = FMath::Max(1.0f, Length / Segments);
    const FVector Back = -Forward.GetSafeNormal();
    for (int32 I = 0; I < Nodes; ++I)
    {
        P[I] = Prev[I] = Mouth + Back * (Seg * I);
        Wet[I] = 1;
    }
    Time = 0;
    bActive = true;
}

float FFishSpine::WaterShare() const
{
    float Sum = 0, Weight = 0;
    for (int32 I = 1; I < Nodes; ++I)
    {
        const float W = Profile(float(I) / Segments);
        Sum += Wet[I] * W;
        Weight += W;
    }
    return Weight > 0 ? Sum / Weight : 0.0f;
}

void FFishSpine::Step(const FFishSpineInput& In, float DeltaSeconds)
{
    if (!bActive) Start(In.Mouth, FVector::ForwardVector, In.Length);
    Seg = FMath::Max(1.0f, In.Length / Segments);
    const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);
    if (Dt <= 0) return;
    const int32 Steps = FMath::Clamp(FMath::CeilToInt(Dt * 120.0f), 1, 12);
    const float H = Dt / Steps;
    const FVector MouthFrom = P[0];
    for (int32 Sub = 0; Sub < Steps; ++Sub)
    {
        Time += H;
        // The mouth goes where it is put; the rest of the body is carried by its own weight, the
        // water and its muscles.
        Prev[0] = P[0];
        P[0] = FMath::Lerp(MouthFrom, In.Mouth, float(Sub + 1) / Steps);
        for (int32 I = 1; I < Nodes; ++I)
        {
            const float S = float(I) / Segments;
            const float Half = 0.5f * In.BodyHeight * In.Length * Profile(S);
            const bool bInLake = LakeMap::IsWater(P[I].X, P[I].Y);
            Wet[I] = bInLake ? FMath::Clamp(float(LakeFish::SurfaceZ - P[I].Z) / FMath::Max(1.0f, 2 * Half) + 0.5f, 0.0f, 1.0f) : 0.0f;
            FVector V = (P[I] - Prev[I]) / H;
            // The water holds the body back hard across it and little along it: drawn by its mouth it
            // trails behind, head first. In the air only a little.
            const FVector Along = (P[I] - P[I - 1]).GetSafeNormal();
            const FVector VAlong = Along * FVector::DotProduct(V, Along);
            V = VAlong * FMath::Exp(-(0.3f + 1.5f * Wet[I]) * H) + (V - VAlong) * FMath::Exp(-(0.6f + 14.0f * Wet[I]) * H);
            // A fish is as heavy as the water it displaces: under water a part floats, out of it it weighs.
            V.Z -= 980.0f * (1 - Wet[I]) * H;
            Prev[I] = P[I];
            P[I] += V * H;
        }
        // The muscles: a wave of bending runs down the body to the tail, across the back.
        const FVector Up = In.BodyUp.GetSafeNormal();
        for (int32 Pass = 0; Pass < 3; ++Pass)
        {
            // The head turns where the fish wants it, as far as it can against the line.
            if (In.Steer > 0 && !In.Want.IsNearlyZero())
            {
                const FVector Target = P[0] - In.Want.GetSafeNormal() * Seg;
                P[1] += (Target - P[1]) * FMath::Clamp(In.Steer * 0.35f, 0.0f, 1.0f);
            }
            for (int32 I = 1; I < Nodes; ++I)
            {
                // The body does not stretch: each point stays a segment behind the one before it.
                const FVector D = P[I] - P[I - 1];
                const float Size = (float)D.Size();
                if (Size > KINDA_SMALL_NUMBER) P[I] = P[I - 1] + D * (Seg / Size);
                if (I + 1 >= Nodes) continue;
                // The joint holds the next segment in line with this one, bent by the muscle wave.
                const FVector Dir = (P[I] - P[I - 1]).GetSafeNormal();
                FVector Side = FVector::CrossProduct(Up, Dir);
                if (Side.SizeSquared() < 1e-4f) Side = FVector::CrossProduct(FVector::RightVector, Dir);
                Side.Normalize();
                const float Reach = FMath::Pow(float(I) / (Segments - 1), 1.3f);
                const float Angle = In.Effort * 0.55f * Reach * FMath::Sin(Time * In.Beat - I * 1.1f);
                const FVector Want = (Dir * FMath::Cos(Angle) + Side * FMath::Sin(Angle)) * Seg;
                const float Hold = FMath::Clamp(0.12f + 0.25f * In.Stiffness, 0.0f, 1.0f);
                P[I + 1] += (P[I] + Want - P[I + 1]) * Hold;
            }
        }
        for (int32 I = 1; I < Nodes; ++I)
        {
            const FVector D = P[I] - P[I - 1];
            const float Size = (float)D.Size();
            if (Size > KINDA_SMALL_NUMBER) P[I] = P[I - 1] + D * (Seg / Size);
        }
    }
}
