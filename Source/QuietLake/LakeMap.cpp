#include "LakeMap.h"
#include "Async/ParallelFor.h"

namespace
{
    using LakeMap::Centre;
    constexpr float AxisX = 6000.0f, AxisY = 7600.0f;   // semi-axes of the main pond
    constexpr int32 ShoreSamples = 1440;
    const FVector2D Pit = Centre + FVector2D(800, -1500); // the deepest hole

    float S(float E0, float E1, float X) { return FMath::SmoothStep(E0, E1, X); }
    float Bump(float Angle, float At, float Width)
    {
        const float D = FMath::UnwindRadians(Angle - At) / Width;
        return FMath::Exp(-D * D);
    }
    float Noise(const FVector2D& P, float Scale) { return FMath::PerlinNoise2D(P / Scale); }
    float Fbm(const FVector2D& P, float Scale)
    {
        return 0.6f * Noise(P, Scale) + 0.3f * Noise(P + FVector2D(1730, -910), Scale / 2.1f) + 0.1f * Noise(P + FVector2D(-4100, 2600), Scale / 4.3f);
    }
    float SegmentDistance(const FVector2D& P, const FVector2D& A, const FVector2D& B)
    {
        const FVector2D AB = B - A;
        const float T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(1.0f, AB.SizeSquared()), 0.0f, 1.0f);
        return FVector2D::Distance(P, A + AB * T);
    }

    // Harmonics that wobble the ellipse into a natural outline; each term is zero at the west
    // point (Angle = PI), so the start bank stays where the angler spawns.
    float Wobble(float Angle)
    {
        auto H = [Angle](int32 K, float Amp, float Phase) { return Amp * (FMath::Cos(K * (Angle - PI) + Phase) - FMath::Cos(Phase)); };
        return 1 + H(2, 0.06f, 0.9f) + H(3, 0.05f, 2.1f) + H(5, 0.03f, 0.4f) + H(9, 0.012f, 1.3f);
    }
    float EllipseRadius(float Angle)
    {
        return 1.0f / FMath::Sqrt(FMath::Square(FMath::Cos(Angle) / AxisX) + FMath::Square(FMath::Sin(Angle) / AxisY));
    }
    float MainRadius(float Angle) { return EllipseRadius(Angle) * (Wobble(Angle) + 0.12f * Bump(Angle, -2.6f, 0.13f)); }

    const LakeMap::FOpening GOpenings[] = {
        { PI,     0.07f,  TEXT("Start") },
        { -2.94f, 0.07f,  TEXT("Pier") },
        { -2.6f,  0.09f,  TEXT("Landing") },
        { -1.85f, 0.045f, TEXT("South") },
        { -0.45f, 0.05f,  TEXT("Bluff") },
        { 0.2f,   0.045f, TEXT("East") },
        { 2.45f,  0.045f, TEXT("North") },
    };

    /** Shoreline polyline and a signed distance field to it on a 1 m grid, built once. */
    struct FShoreField
    {
        TArray<LakeMap::FShorePoint> Shore;
        FVector2D Min;
        int32 NX = 0, NY = 0;
        float Cell = 100.0f;
        TArray<float> D;

        FShoreField()
        {
            Shore.SetNum(ShoreSamples);
            FBox2D Box(ForceInit);
            for (int32 I = 0; I < ShoreSamples; ++I)
            {
                const float A = -PI + 2 * PI * I / ShoreSamples;
                Shore[I].Angle = A;
                Shore[I].Location = Centre + FVector2D(FMath::Cos(A), FMath::Sin(A)) * LakeMap::ShoreRadius(A);
                Box += Shore[I].Location;
            }
            for (int32 I = 0; I < ShoreSamples; ++I)
            {
                const FVector2D T = Shore[(I + 1) % ShoreSamples].Location - Shore[(I + ShoreSamples - 1) % ShoreSamples].Location;
                FVector2D N = FVector2D(T.Y, -T.X).GetSafeNormal();
                if (FVector2D::DotProduct(N, Shore[I].Location - Centre) < 0) N = -N;
                Shore[I].Outward = N;
            }
            Box = Box.ExpandBy(7000);
            Min = Box.Min;
            NX = FMath::CeilToInt((Box.Max.X - Box.Min.X) / Cell) + 1;
            NY = FMath::CeilToInt((Box.Max.Y - Box.Min.Y) / Cell) + 1;
            D.SetNumUninitialized(NX * NY);
            ParallelFor(NY, [this](int32 Y)
            {
                for (int32 X = 0; X < NX; ++X)
                {
                    const FVector2D P = Min + FVector2D(X, Y) * Cell;
                    float Best = TNumericLimits<float>::Max();
                    for (const LakeMap::FShorePoint& Point : Shore) Best = FMath::Min(Best, FVector2D::DistSquared(P, Point.Location));
                    D[Y * NX + X] = FMath::Sqrt(Best) * (LakeMap::Radius(P.X, P.Y) < 1 ? 1 : -1);
                }
            });
        }
        bool Sample(float X, float Y, float& Out) const
        {
            const float FX = (X - Min.X) / Cell, FY = (Y - Min.Y) / Cell;
            if (FX < 0 || FY < 0 || FX >= NX - 1 || FY >= NY - 1) return false;
            const int32 IX = (int32)FX, IY = (int32)FY;
            const float TX = FX - IX, TY = FY - IY;
            const float* Row = &D[IY * NX + IX];
            Out = FMath::Lerp(FMath::Lerp(Row[0], Row[1], TX), FMath::Lerp(Row[NX], Row[NX + 1], TX), TY);
            return true;
        }
    };
    const FShoreField& Field() { static const FShoreField Instance; return Instance; }
}

float LakeMap::ShoreRadius(float Angle)
{
    // The main pond plus a long, narrow backwater reaching out to the north-east.
    return MainRadius(Angle) + EllipseRadius(Angle) * 0.55f * Bump(Angle, 1.05f, 0.2f);
}
float LakeMap::AngleOf(float X, float Y) { return FMath::Atan2(Y - Centre.Y, X - Centre.X); }
float LakeMap::Radius(float X, float Y)
{
    return FVector2D::Distance(FVector2D(X, Y), Centre) / ShoreRadius(AngleOf(X, Y));
}
float LakeMap::ShoreDistance(float X, float Y)
{
    float D;
    if (Field().Sample(X, Y, D)) return D;
    const float Angle = AngleOf(X, Y);
    return ShoreRadius(Angle) - FVector2D::Distance(FVector2D(X, Y), Centre);
}
FVector2D LakeMap::PointAt(float Angle, float R)
{
    return Centre + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * ShoreRadius(Angle) * R;
}
const TArray<LakeMap::FShorePoint>& LakeMap::Shoreline() { return Field().Shore; }
LakeMap::FShorePoint LakeMap::ShoreAt(float Angle)
{
    const int32 I = FMath::RoundToInt((FMath::UnwindRadians(Angle) + PI) / (2 * PI) * ShoreSamples) % ShoreSamples;
    return Field().Shore[I];
}
TConstArrayView<LakeMap::FOpening> LakeMap::Openings() { return GOpenings; }
float LakeMap::OpeningAmount(float Angle)
{
    float Amount = 0;
    for (const FOpening& O : GOpenings)
        Amount = FMath::Max(Amount, 1 - S(O.HalfWidth * 0.7f, O.HalfWidth * 1.3f, FMath::Abs(FMath::UnwindRadians(Angle - O.Angle))));
    return Amount;
}
float LakeMap::BluffHeight(float Angle)
{
    return 3.4f * Bump(Angle, -0.55f, 0.22f) + 2.4f * Bump(Angle, 1.8f, 0.12f);
}
float LakeMap::BackwaterAmount(float X, float Y)
{
    const float Angle = AngleOf(X, Y);
    return Bump(Angle, 1.05f, 0.28f) * S(0.8f, 1.02f, FVector2D::Distance(FVector2D(X, Y), Centre) / MainRadius(Angle));
}

FVector2D LakeMap::StartSpot(FVector2D* OutFacing)
{
    const FShorePoint Shore = ShoreAt(PI);
    if (OutFacing) *OutFacing = -Shore.Outward;
    return Shore.Location + Shore.Outward * 250;
}
const LakeMap::FPier& LakeMap::Pier()
{
    static const FPier Instance = []
    {
        const FShorePoint Shore = ShoreAt(-2.94f);
        return FPier{ Shore.Location + Shore.Outward * 350, -Shore.Outward, 1700.0f, 170.0f };
    }();
    return Instance;
}
bool LakeMap::OnPier(const FVector& P)
{
    const FPier& Pr = Pier();
    const FVector2D Local(P.X - Pr.Root.X, P.Y - Pr.Root.Y);
    const float Along = FVector2D::DotProduct(Local, Pr.Dir);
    const float Across = FMath::Abs(FVector2D::DotProduct(Local, FVector2D(-Pr.Dir.Y, Pr.Dir.X)));
    if (Along < -30 || Along > Pr.Length + 30) return false;
    // A wider landing stage at the far end.
    return Across <= (Along > Pr.Length - 300 ? 230.0f : Pr.Width * 0.5f + 30);
}
FVector2D LakeMap::VillageCentre()
{
    static const FVector2D Instance = [] { const FShorePoint Shore = ShoreAt(-2.72f); return Shore.Location + Shore.Outward * 2600; }();
    return Instance;
}

float LakeMap::Ground(float X, float Y)
{
    const FVector2D P(X, Y);
    const float SD = ShoreDistance(X, Y) / 100;      // metres, positive in the water
    const float Bluff = BluffHeight(AngleOf(X, Y));
    if (SD > 0)
    {
        // Lake bed: a shallow shelf with reeds and lilies, a drop-off at 7-16 m from the bank,
        // a gently deepening middle and one deep hole. Steep under the bluffs, shallow in the backwater.
        float D = 0.12f * S(0, 0.6f, SD) + 0.06f * FMath::Min(SD, 7.0f) + 1.9f * S(7, 16, SD) + 1.3f * S(18, 45, SD);
        D *= 1 - 0.35f * BackwaterAmount(X, Y);
        D += Bluff * 0.45f * S(0, 5, SD);
        D += 1.6f * FMath::Exp(-FVector2D::DistSquared(P, Pit) / FMath::Square(1400.0f));
        D += 0.18f * Noise(P, 1500) * S(1, 4, SD);
        return SurfaceZ - FMath::Max(D, 0.0f) * 100;
    }
    const float L = -SD;                              // metres inland
    // A low bank rising gently, clay bluffs where BluffHeight says, rolling hills further back.
    float H = 0.4f * S(0, 5, L) + 0.022f * FMath::Min(L, 90.0f);
    // The bluff rises over about 3 m with a ragged, gullied edge.
    H += Bluff * S(0.3f, 3.0f, L + 0.6f * Noise(P + FVector2D(911, -377), 700));
    H += (4.0f * Fbm(P, 9000) + 1.3f * Fbm(P + FVector2D(300, 900), 2600) + 0.25f * Noise(P + FVector2D(-77, 31), 500)) * S(5, 30, L);
    // In places the ground steps down in terraces with low scarps.
    const float Terraces = S(0.0f, 0.25f, Noise(P + FVector2D(50000, -30000), 9000)) * S(16, 36, L);
    if (Terraces > 0)
    {
        constexpr float Step = 1.6f;
        const float K = H / Step;
        const float Stepped = (FMath::FloorToFloat(K) + S(0.72f, 1.0f, K - FMath::FloorToFloat(K))) * Step;
        H = FMath::Lerp(H, Stepped, Terraces);
    }
    // Level ground for the village and the angler's start spot.
    const float Village = (1 - S(2200, 3800, FVector2D::Distance(P, VillageCentre()))) * S(1, 6, L);
    H = FMath::Lerp(H, 0.65f + 0.08f * Noise(P, 700), Village);
    const float Start = (1 - S(700, 1600, FVector2D::Distance(P, StartSpot()))) * S(0.5f, 4, L);
    H = FMath::Lerp(H, 0.4f * S(0, 5, L) + 0.04f, Start);
    // Never below the water level away from the lake.
    H = FMath::Max(H, 0.15f * S(0, 3, L));
    return SurfaceZ + H * 100;
}
bool LakeMap::IsWater(float X, float Y) { return ShoreDistance(X, Y) > 20; }
float LakeMap::Depth(float X, float Y) { return FMath::Max(0.0f, SurfaceZ - Ground(X, Y)); }
FVector2D LakeMap::DeeperDirection(float X, float Y)
{
    const FVector2D G(Depth(X + 150, Y) - Depth(X - 150, Y), Depth(X, Y + 150) - Depth(X, Y - 150));
    return G.SizeSquared() > 1 ? G.GetSafeNormal() : (Centre - FVector2D(X, Y)).GetSafeNormal();
}

const TArray<LakeMap::FSnag>& LakeMap::Snags()
{
    static const TArray<FSnag> Instance = []
    {
        TArray<FSnag> Out;
        FRandomStream Random(9113);
        for (float Angle : { -2.25f, -1.45f, -1.0f, 0.82f, 1.25f, 2.05f, 2.75f })
        {
            const FShorePoint Shore = ShoreAt(Angle);
            const FVector2D Dir = (-Shore.Outward).GetRotated(Random.FRandRange(-25, 25));
            const FVector2D Root2 = Shore.Location + Shore.Outward * 120;
            const FVector2D Tip2 = Root2 + Dir * Random.FRandRange(900, 1400);
            FSnag Snag;
            Snag.Root = FVector(Root2, Ground(Root2.X, Root2.Y) + 20);
            Snag.Tip = FVector(Tip2, FMath::Min(Ground(Tip2.X, Tip2.Y) + 25, SurfaceZ - 30));
            Snag.Radius = Random.FRandRange(16, 26);
            Out.Add(Snag);
        }
        return Out;
    }();
    return Instance;
}
const TArray<LakeMap::FLilyBed>& LakeMap::LilyBeds()
{
    static const TArray<FLilyBed> Instance = []
    {
        TArray<FLilyBed> Out;
        FRandomStream Random(4242);
        const FShoreField& F = Field();
        const FVector2D Max = F.Min + FVector2D(F.NX - 1, F.NY - 1) * F.Cell;
        const FPier& Pr = Pier();
        for (int32 Try = 0; Try < 6000 && Out.Num() < 130; ++Try)
        {
            const FVector2D P(Random.FRandRange(F.Min.X, Max.X), Random.FRandRange(F.Min.Y, Max.Y));
            const float SD = ShoreDistance(P.X, P.Y) / 100, D = Depth(P.X, P.Y) / 100;
            if (SD < 1.5f || SD > 22 || D < 0.35f || D > 1.7f) continue;
            const float Back = BackwaterAmount(P.X, P.Y);
            if (Back < 0.5f && Random.FRand() > 0.55f) continue;
            // Keep casting lanes clear in front of every opening and along the pier.
            const float Angle = AngleOf(P.X, P.Y);
            bool bLane = false;
            for (const FOpening& O : GOpenings)
                bLane |= FMath::Abs(FMath::UnwindRadians(Angle - O.Angle)) < O.HalfWidth + 0.04f && SD < 35;
            if (bLane || SegmentDistance(P, Pr.Root, Pr.Root + Pr.Dir * Pr.Length) < 700) continue;
            Out.Add({ P, (float)(Back > 0.5f ? Random.FRandRange(350.0f, 750.0f) : Random.FRandRange(180.0f, 480.0f)) });
        }
        return Out;
    }();
    return Instance;
}
float LakeMap::LilyAmount(float X, float Y)
{
    float Amount = 0;
    const FVector2D P(X, Y);
    for (const FLilyBed& Bed : LilyBeds())
    {
        const float DistSq = FVector2D::DistSquared(P, Bed.Centre);
        if (DistSq < FMath::Square(Bed.Radius * 1.2f)) Amount = FMath::Max(Amount, 1 - S(0.6f, 1.2f, FMath::Sqrt(DistSq) / Bed.Radius));
    }
    return Amount;
}

LakeMap::FZones LakeMap::ZonesAt(float X, float Y)
{
    FZones Z;
    const float SD = ShoreDistance(X, Y) / 100;
    if (SD <= 0.2f) return Z;
    const FVector2D P(X, Y);
    const float D = Depth(X, Y) / 100;
    const float Angle = AngleOf(X, Y);
    // Warm weedy shallows: the backwater and the lily beds.
    Z.Shallows = FMath::Max(BackwaterAmount(X, Y), LilyAmount(X, Y)) * S(0.25f, 0.45f, D) * (1 - S(1.8f, 2.4f, D));
    // Just outside the reed belt (none in the openings or under the bluffs).
    Z.ReedEdge = (1 - OpeningAmount(Angle)) * (1 - S(0.5f, 1.2f, BluffHeight(Angle))) * S(1.0f, 2.5f, SD) * (1 - S(7, 11, SD));
    // Where the bed falls away steeply.
    const float Slope = FVector2D(Depth(X + 150, Y) - Depth(X - 150, Y), Depth(X, Y + 150) - Depth(X, Y - 150)).Size() / 300;
    Z.DropOff = S(0.08f, 0.2f, Slope) * S(0.8f, 1.5f, D);
    // Snags and the pier piles.
    float Cover = 0;
    for (const FSnag& Snag : Snags())
        Cover = FMath::Max(Cover, 1 - S(150, 500, SegmentDistance(P, FVector2D(Snag.Root), FVector2D(Snag.Tip))));
    const FPier& Pr = Pier();
    Cover = FMath::Max(Cover, 1 - S(150, 450, SegmentDistance(P, Pr.Root, Pr.Root + Pr.Dir * Pr.Length)));
    Z.Cover = Cover;
    Z.Deep = S(2.8f, 3.8f, D);
    Z.Open = S(10, 22, SD) * (1 - 0.5f * Z.Deep);
    return Z;
}

namespace
{
    // A Catmull-Rom spline through the control points, resampled about every metre.
    TArray<FVector2D> Smooth(const TArray<FVector2D>& Control)
    {
        TArray<FVector2D> Out;
        for (int32 I = 0; I + 1 < Control.Num(); ++I)
        {
            const FVector2D P0 = Control[FMath::Max(I - 1, 0)], P1 = Control[I], P2 = Control[I + 1], P3 = Control[FMath::Min(I + 2, Control.Num() - 1)];
            const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector2D::Distance(P1, P2) / 100));
            for (int32 K = 0; K < Steps; ++K)
            {
                const float T = float(K) / Steps, T2 = T * T, T3 = T2 * T;
                Out.Add(0.5f * ((2 * P1) + (-P0 + P2) * T + (2 * P0 - 5 * P1 + 4 * P2 - P3) * T2 + (-P0 + 3 * P1 - 3 * P2 + P3) * T3));
            }
        }
        Out.Add(Control.Last());
        return Out;
    }

    /** A footpath between two points that wanders a little, as trodden paths do, and keeps off the water. */
    TArray<FVector2D> Footpath(FVector2D A, FVector2D B, float Seed)
    {
        TArray<FVector2D> Control;
        const float Length = FVector2D::Distance(A, B);
        const FVector2D Dir = (B - A).GetSafeNormal(), Side(-Dir.Y, Dir.X);
        const int32 N = FMath::Max(2, FMath::CeilToInt(Length / 900));
        for (int32 I = 0; I <= N; ++I)
        {
            const float T = float(I) / N;
            FVector2D P = FMath::Lerp(A, B, T) + Side * 450 * FMath::PerlinNoise1D(Seed + I * 0.43f) * FMath::Sin(PI * T);
            // Never through the water: step back onto the bank.
            for (int32 K = 0; K < 40 && LakeMap::ShoreDistance(P.X, P.Y) > -150; ++K) P -= LakeMap::DeeperDirection(P.X, P.Y) * 100;
            Control.Add(P);
        }
        return Smooth(Control);
    }

    struct FTrackField
    {
        TArray<LakeMap::FTrack> Tracks;
        FVector2D Min;
        int32 NX = 0, NY = 0;
        static constexpr float Cell = 100.0f, Far = 1200.0f;
        TArray<float> Clearance;
        TArray<uint8> Road;

        FTrackField()
        {
            using namespace LakeMap;
            // Roads and paths are switched off until they are redone; the field stays empty.
            constexpr bool bEnabled = false;
            if (!bEnabled)
            {
                Min = TerrainBounds.Min;
                return;
            }
            // Roads: one north-south through the western forest, one round the south and east, and
            // the village lane joining the first. All 60-150 m from the water.
            Tracks.Add({ Smooth({ { -9500, -24000 }, { -8800, -14000 }, { -10200, -5000 }, { -9000, 3000 }, { -10500, 12000 }, { -9500, 26000 } }), 380, true });
            Tracks.Add({ Smooth({ { -9200, -12500 }, { 0, -17500 }, { 9000, -19500 }, { 18000, -16500 }, { 24500, -8000 }, { 26500, 4000 }, { 24000, 15000 }, { 19000, 26000 } }), 360, true });
            const FVector2D Village = VillageCentre();
            Tracks.Add({ Smooth({ Village, Village + FVector2D(-2200, -300), { -6500, -3400 }, { -9900, -4200 } }), 300, true });
            auto NearestRoadPoint = [this](const FVector2D& P)
            {
                FVector2D Best = P;
                float BestD = TNumericLimits<float>::Max();
                for (const FTrack& T : Tracks)
                    if (T.bRoad)
                        for (const FVector2D& Q : T.Points)
                            if (FVector2D::DistSquared(P, Q) < BestD) { BestD = FVector2D::DistSquared(P, Q); Best = Q; }
                return Best;
            };
            // Footpaths: from the start spot and every fishing spot to the nearest road, plus the
            // paths round the village: start to pier, start to village, village to the landing.
            FVector2D Facing;
            const FVector2D Start = StartSpot(&Facing);
            float Seed = 3.1f;
            Tracks.Add({ Footpath(Start - Facing * 300, NearestRoadPoint(Start), Seed), 85, false });
            for (const FOpening& O : Openings())
            {
                const FString Name = O.Name;
                if (Name == TEXT("Start") || Name == TEXT("Pier") || Name == TEXT("Landing")) continue;
                const FShorePoint S = ShoreAt(O.Angle);
                const FVector2D From = S.Location + S.Outward * 250;
                Seed += 7.7f;
                Tracks.Add({ Footpath(From, NearestRoadPoint(From + S.Outward * 3000), Seed), 75, false });
            }
            const FPier& P = Pier();
            const FShorePoint Landing = ShoreAt(-2.6f);
            Tracks.Add({ Footpath(Start, P.Root - P.Dir * 50, 41.0f), 85, false });
            Tracks.Add({ Footpath(Start, Village, 53.0f), 95, false });
            Tracks.Add({ Footpath(Village, Landing.Location + Landing.Outward * 120, 67.0f), 95, false });

            // Clearance field on a 1 m grid: distance to the nearest track edge.
            const FBox2D Bounds = TerrainBounds;
            Min = Bounds.Min;
            NX = FMath::CeilToInt((Bounds.Max.X - Bounds.Min.X) / Cell) + 1;
            NY = FMath::CeilToInt((Bounds.Max.Y - Bounds.Min.Y) / Cell) + 1;
            Clearance.Init(Far, NX * NY);
            Road.Init(0, NX * NY);
            for (const FTrack& T : Tracks)
                for (int32 I = 0; I + 1 < T.Points.Num(); ++I)
                {
                    const FVector2D A = T.Points[I], B = T.Points[I + 1];
                    const float Reach = T.Width * 0.5f + Far;
                    const int32 X0 = FMath::Max(0, FMath::FloorToInt((FMath::Min(A.X, B.X) - Reach - Min.X) / Cell));
                    const int32 X1 = FMath::Min(NX - 1, FMath::CeilToInt((FMath::Max(A.X, B.X) + Reach - Min.X) / Cell));
                    const int32 Y0 = FMath::Max(0, FMath::FloorToInt((FMath::Min(A.Y, B.Y) - Reach - Min.Y) / Cell));
                    const int32 Y1 = FMath::Min(NY - 1, FMath::CeilToInt((FMath::Max(A.Y, B.Y) + Reach - Min.Y) / Cell));
                    const FVector2D AB = B - A;
                    for (int32 Y = Y0; Y <= Y1; ++Y)
                        for (int32 X = X0; X <= X1; ++X)
                        {
                            const FVector2D Q = Min + FVector2D(X, Y) * Cell;
                            const float U = FMath::Clamp(FVector2D::DotProduct(Q - A, AB) / FMath::Max(1.0f, AB.SizeSquared()), 0.0f, 1.0f);
                            const float D = FVector2D::Distance(Q, A + AB * U) - T.Width * 0.5f;
                            const int32 Index = Y * NX + X;
                            if (D < Clearance[Index]) { Clearance[Index] = D; Road[Index] = T.bRoad; }
                        }
                }
        }
        float Sample(float X, float Y, bool* bRoad) const
        {
            const float FX = (X - Min.X) / Cell, FY = (Y - Min.Y) / Cell;
            if (FX < 0 || FY < 0 || FX >= NX - 1 || FY >= NY - 1) { if (bRoad) *bRoad = false; return Far; }
            const int32 IX = (int32)FX, IY = (int32)FY;
            const float TX = FX - IX, TY = FY - IY;
            const float* Row = &Clearance[IY * NX + IX];
            if (bRoad) *bRoad = Road[IY * NX + IX] != 0;
            return FMath::Lerp(FMath::Lerp(Row[0], Row[1], TX), FMath::Lerp(Row[NX], Row[NX + 1], TX), TY);
        }
    };
    const FTrackField& TrackField() { static const FTrackField Instance; return Instance; }
}

const TArray<LakeMap::FTrack>& LakeMap::Tracks() { return TrackField().Tracks; }
float LakeMap::TrackClearance(float X, float Y, bool* bRoad) { return TrackField().Sample(X, Y, bRoad); }

bool LakeMap::InPlayArea(float X, float Y)
{
    return X > -13000 && X < 27000 && Y > -21000 && Y < 23000;
}
