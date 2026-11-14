#include "LakeFishTools.h"
#if WITH_EDITOR
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#endif

#if WITH_EDITOR
namespace
{
    float Percentile(TArray<float>& Values, float Share)
    {
        if (Values.Num() == 0) return -1;
        Values.Sort();
        return Values[FMath::Clamp(FMath::RoundToInt(Share * (Values.Num() - 1)), 0, Values.Num() - 1)];
    }

    // A profile along the body with holes (slices with too few vertices) filled from their
    // neighbours and smoothed over three slices.
    void FillAndSmooth(TArray<float>& Profile, float Fallback)
    {
        const int32 N = Profile.Num();
        for (int32 I = 0; I < N; ++I)
            if (Profile[I] <= 0)
            {
                float Near = -1;
                for (int32 D = 1; D < N && Near <= 0; ++D)
                {
                    if (I - D >= 0 && Profile[I - D] > 0) Near = Profile[I - D];
                    else if (I + D < N && Profile[I + D] > 0) Near = Profile[I + D];
                }
                Profile[I] = Near > 0 ? Near : Fallback;
            }
        TArray<float> Copy = Profile;
        for (int32 I = 0; I < N; ++I)
            Profile[I] = (Copy[FMath::Max(0, I - 1)] + 2 * Copy[I] + Copy[FMath::Min(N - 1, I + 1)]) / 4;
    }
}
#endif

FString ULakeFishTools::MarkFishFins(UStaticMesh* Mesh)
{
#if WITH_EDITOR
    if (!Mesh) return TEXT("no mesh");
    FString Report = Mesh->GetName();
    constexpr int32 Slices = 32;
    for (int32 Lod = 0; Lod < Mesh->GetNumSourceModels(); ++Lod)
    {
        FMeshDescription* Description = Mesh->GetMeshDescription(Lod);
        if (!Description) continue;
        FStaticMeshAttributes Attributes(*Description);
        TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
        TVertexInstanceAttributesRef<FVector4f> Colours = Attributes.GetVertexInstanceColors();
        float MinX = TNumericLimits<float>::Max(), MaxX = -TNumericLimits<float>::Max(), MaxZ = 0;
        for (const FVertexID V : Description->Vertices().GetElementIDs())
        {
            const FVector3f P = Positions[V];
            MinX = FMath::Min(MinX, P.X);
            MaxX = FMath::Max(MaxX, P.X);
            MaxZ = FMath::Max(MaxZ, FMath::Abs(P.Z));
        }
        const float Length = FMath::Max(1.0f, MaxX - MinX);
        auto SliceOf = [&](float X) { return FMath::Clamp(int32((X - MinX) / Length * Slices), 0, Slices - 1); };
        // The body's width near its middle height, then its depth up and down on its flanks (fins are
        // thin sheets on the midline or stand out low on the sides, so they hardly count in either).
        TArray<TArray<float>> Mid, Up, Down;
        Mid.SetNum(Slices); Up.SetNum(Slices); Down.SetNum(Slices);
        for (const FVertexID V : Description->Vertices().GetElementIDs())
        {
            const FVector3f P = Positions[V];
            if (FMath::Abs(P.Z) < 0.3f * MaxZ) Mid[SliceOf(P.X)].Add(FMath::Abs(P.Y));
        }
        TArray<float> Width; Width.SetNum(Slices);
        for (int32 S = 0; S < Slices; ++S) Width[S] = Mid[S].Num() >= 6 ? Percentile(Mid[S], 0.85f) : -1;
        FillAndSmooth(Width, 0.1f * Length);
        for (const FVertexID V : Description->Vertices().GetElementIDs())
        {
            const FVector3f P = Positions[V];
            const int32 S = SliceOf(P.X);
            if (FMath::Abs(P.Y) < 0.45f * Width[S]) continue;
            (P.Z >= 0 ? Up[S] : Down[S]).Add(FMath::Abs(P.Z));
        }
        TArray<float> Top, Bottom; Top.SetNum(Slices); Bottom.SetNum(Slices);
        for (int32 S = 0; S < Slices; ++S)
        {
            Top[S] = Up[S].Num() >= 4 ? Percentile(Up[S], 0.9f) : -1;
            Bottom[S] = Down[S].Num() >= 4 ? Percentile(Down[S], 0.9f) : -1;
        }
        FillAndSmooth(Top, 0.5f * MaxZ);
        FillAndSmooth(Bottom, 0.5f * MaxZ);

        int32 Counts[5] = {};
        for (const FVertexInstanceID I : Description->VertexInstances().GetElementIDs())
        {
            const FVector3f P = Positions[Description->GetVertexInstanceVertex(I)];
            const float Along = (P.X - MinX) / Length;
            const int32 S = SliceOf(P.X);
            const float W = FMath::Max(0.01f * Length, Width[S]);
            const float H = FMath::Max(0.01f * Length, P.Z >= 0 ? Top[S] : Bottom[S]);
            const float Outside = FMath::Square(P.Y / W) + FMath::Square(P.Z / H);
            float Fin = FMath::SmoothStep(1.35f, 2.3f, Outside);
            // (No fins on the head: the gill covers and the mouth stay body.)
            Fin *= FMath::SmoothStep(0.12f, 0.18f, Along);
            float Kind = 1.0f;
            if (Along > 0.84f)
            {
                // The tail fin: all of it behind the narrow of the tail.
                Fin = FMath::Max(Fin, FMath::SmoothStep(0.84f, 0.92f, Along));
                Kind = 0.0f;
            }
            else if (Fin > 0.05f)
            {
                const bool bLow = P.Z < 0.4f * Top[S];
                if (FMath::Abs(P.Y) > 0.55f * W && bLow) Kind = Along < 0.45f ? 0.5f : 0.25f;   // pectoral / pelvic
                else Kind = P.Z >= 0 ? 0.75f : 0.25f;                                           // dorsal / anal
            }
            if (Fin <= 0.05f) { Fin = 0; Kind = 1.0f; }
            FVector4f C = Colours[I];
            C.Y = 1 - Fin;
            C.Z = Kind;
            Colours[I] = C;
            ++Counts[Kind >= 1 ? 0 : Kind >= 0.7f ? 1 : Kind >= 0.45f ? 2 : Kind >= 0.2f ? 3 : 4];
        }
        Mesh->CommitMeshDescription(Lod);
        Report += FString::Printf(TEXT(" | LOD%d body %d dorsal %d pectoral %d pelvic/anal %d tail %d"), Lod, Counts[0], Counts[1], Counts[2], Counts[3], Counts[4]);
    }
    Mesh->Build(false);
    Mesh->PostEditChange();
    Mesh->MarkPackageDirty();
    return Report;
#else
    return FString();
#endif
}
