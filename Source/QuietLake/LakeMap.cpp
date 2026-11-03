#include "LakeMap.h"
#include "Async/ParallelFor.h"

namespace
{
    using namespace LakeMap;
    constexpr float Cell = 100.0f;                   // 1 m grids for the shore distance and the ground
    // The lake's layout below is written at 4/3 of its size and scaled down by Lay(): positions and
    // widths of the water, the island, the bays, bluffs, ravines and holes. Depths, heights and the
    // widths of banks and reed belts stay in real units.
    constexpr float LakeScale = 0.75f;
    constexpr float Lay(float V) { return V * LakeScale; }
    const FVector2D GIsland(Lay(11000.0f), Lay(-2500.0f));
    const FVector2D GIslandAxes(Lay(2800.0f), Lay(1700.0f));
    constexpr float GIslandYaw = 25.0f;
    const FVector2D GVillage(Lay(-17000.0f), Lay(31000.0f));

    float S(float E0, float E1, float X) { return FMath::SmoothStep(E0, E1, X); }
    float Noise(const FVector2D& P, float Scale) { return FMath::PerlinNoise2D(P / Scale); }
    float Fbm(const FVector2D& P, float Scale)
    {
        return 0.6f * Noise(P, Scale) + 0.3f * Noise(P + FVector2D(1730, -910), Scale / 2.1f) + 0.1f * Noise(P + FVector2D(-4100, 2600), Scale / 4.3f);
    }
    float Gauss(const FVector2D& P, const FVector2D& C, float R) { return FMath::Exp(-FVector2D::DistSquared(P, C) / (R * R)); }
    float SegmentDistance(const FVector2D& P, const FVector2D& A, const FVector2D& B, float* OutT = nullptr)
    {
        const FVector2D AB = B - A;
        const float T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(1.0f, AB.SizeSquared()), 0.0f, 1.0f);
        if (OutT) *OutT = T;
        return FVector2D::Distance(P, A + AB * T);
    }
    float SmoothMax(float A, float B, float K)
    {
        const float H = FMath::Max(K - FMath::Abs(A - B), 0.0f);
        return FMath::Max(A, B) + H * H / (4 * K);
    }

    // --- the outline ---------------------------------------------------------------------------

    /** A winding axis with a half-width on each side: the main lake and the backwater are drawn this way. */
    struct FSpine
    {
        TArray<FVector2D> P;
        TArray<float> Along, North, South;
        float Length = 0;
        float Seed = 0;

        /** A control point: X, Y, half-width to the north (-Y side), half-width to the south. */
        struct FControl { float X, Y, Z, W; };
        FSpine(std::initializer_list<FControl> Control, float InSeed) : Seed(InSeed)
        {
            // Catmull-Rom through the points, resampled about every 8 m, widths eased between control points.
            TArray<FControl> C(Control);
            for (int32 I = 0; I + 1 < C.Num(); ++I)
            {
                const FVector2D P0(C[FMath::Max(I - 1, 0)].X, C[FMath::Max(I - 1, 0)].Y), P1(C[I].X, C[I].Y), P2(C[I + 1].X, C[I + 1].Y);
                const FVector2D P3(C[FMath::Min(I + 2, C.Num() - 1)].X, C[FMath::Min(I + 2, C.Num() - 1)].Y);
                const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector2D::Distance(P1, P2) / 800));
                for (int32 K = 0; K < Steps; ++K)
                {
                    const float T = float(K) / Steps, T2 = T * T, T3 = T2 * T;
                    P.Add(0.5f * ((2 * P1) + (-P0 + P2) * T + (2 * P0 - 5 * P1 + 4 * P2 - P3) * T2 + (-P0 + 3 * P1 - 3 * P2 + P3) * T3));
                    const float E = FMath::InterpEaseInOut(0.0f, 1.0f, T, 2.0f);
                    North.Add(FMath::Lerp(C[I].Z, C[I + 1].Z, E));
                    South.Add(FMath::Lerp(C[I].W, C[I + 1].W, E));
                }
            }
            P.Add(FVector2D(C.Last().X, C.Last().Y));
            North.Add(C.Last().Z);
            South.Add(C.Last().W);
            Along.SetNum(P.Num());
            for (int32 I = 1; I < P.Num(); ++I) Along[I] = Along[I - 1] + FVector2D::Distance(P[I], P[I - 1]);
            Length = Along.Last();
        }

        struct FHit { float Dist = 1e9f, Along = 0, Width = 0; bool bSouth = false; };
        FHit Query(const FVector2D& Q) const
        {
            FHit Hit;
            int32 Best = 0;
            float BestT = 0, BestD2 = TNumericLimits<float>::Max();
            for (int32 I = 0; I + 1 < P.Num(); ++I)
            {
                const FVector2D AB = P[I + 1] - P[I];
                const float T = FMath::Clamp(FVector2D::DotProduct(Q - P[I], AB) / FMath::Max(1.0f, AB.SizeSquared()), 0.0f, 1.0f);
                const float D2 = FVector2D::DistSquared(Q, P[I] + AB * T);
                if (D2 < BestD2) { BestD2 = D2; Best = I; BestT = T; }
            }
            const FVector2D AB = P[Best + 1] - P[Best];
            const FVector2D Off = Q - (P[Best] + AB * BestT);
            Hit.Dist = FMath::Sqrt(BestD2);
            Hit.Along = FMath::Lerp(Along[Best], Along[Best + 1], BestT);
            Hit.bSouth = AB.X * Off.Y - AB.Y * Off.X > 0;
            const float W = Hit.bSouth ? FMath::Lerp(South[Best], South[Best + 1], BestT) : FMath::Lerp(North[Best], North[Best + 1], BestT);
            // The banks wander in and out a little.
            Hit.Width = W * (1 + 0.12f * FMath::PerlinNoise1D(Hit.Along / Lay(6500) + (Hit.bSouth ? 13.7f : 3.1f) + Seed));
            return Hit;
        }
        float Inside(const FVector2D& Q) const { const FHit H = Query(Q); return H.Width - H.Dist; }
    };

    const FSpine& MainSpine()
    {
        // West (a rounded end with a little bay) to east (the brook). Widths: north, south.
        static const FSpine Spine({
            { Lay(-31500), Lay(1800), Lay(6800), Lay(7600) },     // the west end: the spine's round cap is the bank
            { Lay(-27000), Lay(3000), Lay(9500), Lay(10000) },
            { Lay(-15000), Lay(1500), Lay(8000), Lay(11000) },
            { Lay(-3000), Lay(-3000), Lay(6000), Lay(6500) },     // the narrows
            { Lay(10000), Lay(-5000), Lay(11000), Lay(9000) },    // the wide middle reach with the island
            { Lay(22000), Lay(-2500), Lay(8500), Lay(7000) },
            { Lay(32000), Lay(2500), Lay(5500), Lay(6000) },
            { Lay(41000), Lay(5500), Lay(3500), Lay(3800) },
            { Lay(48500), Lay(6500), Lay(900), Lay(900) },        // where the brook comes in
        }, 0.0f);
        return Spine;
    }
    const FSpine& BackwaterSpine()
    {
        static const FSpine Spine({
            { Lay(13000), Lay(-12500), Lay(3800), Lay(3800) },
            { Lay(11500), Lay(-19000), Lay(3000), Lay(3000) },
            { Lay(8000), Lay(-25000), Lay(2600), Lay(2600) },
            { Lay(3500), Lay(-29500), Lay(2000), Lay(2000) },
            { Lay(-500), Lay(-31500), Lay(800), Lay(800) },
        }, 7.0f);
        return Spine;
    }

    struct FEllipse
    {
        FVector2D C, Axes;
        float Yaw, Seed;
        /** Normalised radius: below 1 inside, with a wavy outline. */
        float K(const FVector2D& P) const
        {
            const FVector2D L = (P - C).GetRotated(-Yaw);
            const float K0 = FVector2D(L.X / Axes.X, L.Y / Axes.Y).Size();
            return K0 * (1 + 0.09f * FMath::PerlinNoise1D(FMath::Atan2(L.Y, L.X) * 1.6f + Seed));
        }
        float Inside(const FVector2D& P) const { return (1 - K(P)) * FMath::Min(Axes.X, Axes.Y); }
    };
    const FEllipse GSouthBay{ { Lay(-15500), Lay(13500) }, { Lay(6500), Lay(4500) }, 15, 2.0f };
    const FEllipse GSnagCove{ { Lay(25500), Lay(8500) }, { Lay(3200), Lay(2600) }, -20, 5.0f };
    const FEllipse GRavineCove{ { Lay(-6500), Lay(-9800) }, { Lay(2400), Lay(1800) }, 70, 9.0f };
    const FEllipse GWestBay{ { Lay(-33500), Lay(-2500) }, { Lay(3200), Lay(2600) }, 35, 11.0f };   // the little bay at the west end
    const FEllipse GIslandShape{ GIsland, GIslandAxes, GIslandYaw, 4.0f };

    /** Rough inside-distance of the water (cm, positive in the water); the exact distance is built from its zero line. */
    float WaterField(const FVector2D& P)
    {
        float F = MainSpine().Inside(P);
        F = SmoothMax(F, BackwaterSpine().Inside(P), 1500);
        for (const FEllipse* E : { &GSouthBay, &GSnagCove, &GRavineCove, &GWestBay }) F = SmoothMax(F, E->Inside(P), 1500);
        F = FMath::Min(F, -GIslandShape.Inside(P));        // the island
        return F;
    }

    /** Signed distance to the waterline on a 1 m grid, and the waterline itself. */
    struct FShoreField
    {
        FVector2D Min;
        int32 NX = 0, NY = 0;
        TArray<float> D;
        TArray<FShorePoint> Shore;
        FBox2D Water = FBox2D(ForceInit);

        FShoreField()
        {
            Min = TerrainBounds.Min;
            NX = FMath::CeilToInt((TerrainBounds.Max.X - Min.X) / Cell) + 1;
            NY = FMath::CeilToInt((TerrainBounds.Max.Y - Min.Y) / Cell) + 1;
            TArray<float> F;
            F.SetNumUninitialized(NX * NY);
            ParallelFor(NY, [&](int32 Y)
            {
                for (int32 X = 0; X < NX; ++X) F[Y * NX + X] = WaterField(Min + FVector2D(X, Y) * Cell);
            });
            // Seeds: where the rough field crosses zero between neighbouring samples.
            TArray<FVector2f> Nearest;
            Nearest.Init(FVector2f(1e9f, 1e9f), NX * NY);
            TArray<float> Dist2;
            Dist2.Init(1e18f, NX * NY);
            auto Offer = [&](int32 Index, const FVector2f& Point)
            {
                const FVector2f Here = FVector2f(Min) + FVector2f(Index % NX, Index / NX) * Cell;
                const float D2 = FVector2f::DistSquared(Here, Point);
                if (D2 < Dist2[Index]) { Dist2[Index] = D2; Nearest[Index] = Point; }
            };
            TArray<FVector2D> Crossings;
            for (int32 Y = 0; Y < NY; ++Y)
                for (int32 X = 0; X < NX; ++X)
                {
                    const int32 I = Y * NX + X;
                    const float F0 = F[I];
                    for (int32 Dir = 0; Dir < 2; ++Dir)
                    {
                        const int32 J = Dir == 0 ? (X + 1 < NX ? I + 1 : -1) : (Y + 1 < NY ? I + NX : -1);
                        if (J < 0 || (F0 > 0) == (F[J] > 0)) continue;
                        const float T = F0 / (F0 - F[J]);
                        const FVector2D Point = Min + FVector2D(X + (Dir == 0 ? T : 0), Y + (Dir == 1 ? T : 0)) * Cell;
                        Offer(I, FVector2f(Point));
                        Offer(J, FVector2f(Point));
                        Crossings.Add(Point);
                    }
                }
            // Spread the nearest waterline point over the grid (two-pass distance transform, twice).
            auto Pull = [&](int32 I, int32 X, int32 Y)
            {
                if (X < 0 || Y < 0 || X >= NX || Y >= NY) return;
                const int32 J = Y * NX + X;
                if (Dist2[J] < 1e17f) Offer(I, Nearest[J]);
            };
            for (int32 Round = 0; Round < 2; ++Round)
            {
                for (int32 Y = 0; Y < NY; ++Y)
                    for (int32 X = 0; X < NX; ++X)
                    {
                        const int32 I = Y * NX + X;
                        Pull(I, X - 1, Y - 1); Pull(I, X, Y - 1); Pull(I, X + 1, Y - 1); Pull(I, X - 1, Y);
                    }
                for (int32 Y = NY - 1; Y >= 0; --Y)
                    for (int32 X = NX - 1; X >= 0; --X)
                    {
                        const int32 I = Y * NX + X;
                        Pull(I, X + 1, Y); Pull(I, X - 1, Y + 1); Pull(I, X, Y + 1); Pull(I, X + 1, Y + 1);
                    }
            }
            D.SetNumUninitialized(NX * NY);
            for (int32 I = 0; I < NX * NY; ++I) D[I] = FMath::Sqrt(Dist2[I]) * (F[I] > 0 ? 1 : -1);
            // The waterline points, with the normal onto the land from the distance gradient.
            Shore.Reserve(Crossings.Num());
            for (const FVector2D& P : Crossings)
            {
                FShorePoint Point;
                Point.Location = P;
                FVector2D G(0, 0);
                float A, B, C, E;
                if (Sample(P.X + 100, P.Y, A) && Sample(P.X - 100, P.Y, B) && Sample(P.X, P.Y + 100, C) && Sample(P.X, P.Y - 100, E)) G = FVector2D(A - B, C - E);
                Point.Outward = (-G).GetSafeNormal();
                Point.bIsland = FVector2D::Distance(P, GIsland) < Lay(4500);
                Shore.Add(Point);
                Water += P;
            }
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

    // --- places --------------------------------------------------------------------------------

    struct FSpotSeed { const TCHAR* Name; FVector2D Near; float HalfWidth; bool bBoards; };
    const FSpotSeed GSpotSeeds[] = {
        { TEXT("Лагерь"),            { Lay(-24000), Lay(13300) }, 900, true },     // the start: camp on the meadow bank
        { TEXT("Мостки"),            { Lay(-11000), Lay(16500) }, 700, false },    // the pier in the south bay
        { TEXT("Омут"),              { Lay(-31000), Lay(-6000) }, 800, true },     // by the little bay at the west end
        { TEXT("Берёзовая гряда"),   { Lay(-20000), Lay(-6600) }, 800, true },
        { TEXT("Овраг"),             { Lay(-6500), Lay(-11600) }, 700, true },
        { TEXT("Горло"),             { Lay(-1000), Lay(3700) },   800, true },
        { TEXT("Под яром"),          { Lay(9800), Lay(-16200) },  700, true },
        { TEXT("Устье заводи"),      { Lay(9800), Lay(-18500) },  700, true },
        { TEXT("Заводь"),            { Lay(10200), Lay(-26000) }, 700, true },
        { TEXT("Остров"),            { Lay(11500), Lay(4300) },   800, true },
        { TEXT("Плёс"),              { Lay(27000), Lay(-7100) },  800, true },
        { TEXT("Коряжник"),          { Lay(28300), Lay(9500) },   700, true },
        { TEXT("Верховье"),          { Lay(41000), Lay(9300) },   700, true },
    };
    const TArray<FSpot>& SpotList()
    {
        static const TArray<FSpot> Instance = []
        {
            TArray<FSpot> Out;
            for (const FSpotSeed& Seed : GSpotSeeds)
            {
                const FShorePoint Shore = NearestShore(Seed.Near);
                Out.Add({ Seed.Name, Shore.Location, Shore.Outward, Seed.HalfWidth, Seed.bBoards });
            }
            return Out;
        }();
        return Instance;
    }

    /** Clay bluffs: a point on the bank, how far along the bank (cm) and how high (m). */
    struct FBluff { FVector2D C; float Reach, Height; };
    const FBluff GBluffs[] = {
        { { Lay(3800), Lay(-12500) }, Lay(5500), 6.0f },      // the high bluff of the north bank, pines on top
        { { Lay(31000), Lay(8200) }, Lay(3500), 3.5f },       // past the snag cove
        { { Lay(-27000), Lay(-6500) }, Lay(3000), 2.2f },     // a low one near the west end
    };

    /** Ravines running down to the lake: mouth (at the water), head, half-width (cm). */
    struct FRavine { FVector2D Mouth, Head; float HalfWidth; };
    const FRavine GRavines[] = {
        { { Lay(-6500), Lay(-11200) }, { Lay(-7500), Lay(-27000) }, 1600 },
        { { Lay(19500), Lay(-10500) }, { Lay(21500), Lay(-29000) }, 1500 },
        { { Lay(38500), Lay(1000) },   { Lay(40500), Lay(-16000) }, 1300 },
        { { Lay(48500), Lay(6500) },   { Lay(70000), Lay(9500) },   1400 },     // the brook valley above the lake
    };

    /** The main spine's channel depth (m) along the lake: deepest at the west end, shallow at the brook. */
    float ChannelDepth(float Fraction)
    {
        static const float Table[][2] = { { 0.0f, 6.8f }, { 0.15f, 5.6f }, { 0.4f, 4.3f }, { 0.62f, 3.9f }, { 0.8f, 2.6f }, { 0.92f, 1.6f }, { 1.0f, 0.9f } };
        for (int32 I = 0; I + 1 < UE_ARRAY_COUNT(Table); ++I)
            if (Fraction <= Table[I + 1][0])
                return FMath::Lerp(Table[I][1], Table[I + 1][1], FMath::SmoothStep(0.0f, 1.0f, (Fraction - Table[I][0]) / (Table[I + 1][0] - Table[I][0])));
        return Table[UE_ARRAY_COUNT(Table) - 1][1];
    }

    /** Along the bank and across it (positive inland) relative to a spot. */
    FVector2D SpotFrame(const FSpot& Spot, const FVector2D& P)
    {
        const FVector2D Local = P - Spot.Location;
        return FVector2D(FVector2D::DotProduct(Local, FVector2D(-Spot.Outward.Y, Spot.Outward.X)), FVector2D::DotProduct(Local, Spot.Outward));
    }
    /** 1 in the water in front of a fishing spot with boards (its casting lane), fading out 8-12 m off the bank. */
    float SpotLane(const FVector2D& P)
    {
        float Lane = 0;
        for (const FSpot& Spot : SpotList())
        {
            if (!Spot.bBoards) continue;
            const FVector2D F = SpotFrame(Spot, P);
            if (FMath::Abs(F.X) > Spot.HalfWidth * 1.3f || F.Y < -1200 || F.Y > 300) continue;
            Lane = FMath::Max(Lane, (1 - S(Spot.HalfWidth * 0.6f, Spot.HalfWidth * 1.3f, FMath::Abs(F.X))) * S(-1200, -800, F.Y));
        }
        return Lane;
    }

    /** The old gentle bed (m): a shallow shelf about 8 m wide, then the edge of the old river channel 8-18 m out. */
    float ShelfBed(float SD) { return 0.1f * S(0, 0.6f, SD) + 0.065f * FMath::Min(SD, 8.0f) + 1.5f * S(8, 18, SD) + 0.045f * FMath::Max(SD - 18, 0.0f); }
    /**
     * The bed along the banks (m): a narrow shallow lip at the waterline, the drop-off down to Foot
     * (about 1.5 m) within 2-4 m of the bank, steepest at 0.7 Width out, then a nearly flat foot
     * until the edge of the old channel takes over.
     */
    float BankBed(float SD, float Width, float Foot)
    {
        const float Near = 0.06f * S(0, 0.3f, SD) + Foot * (1 - FMath::Exp(-FMath::Square(SD / Width))) + 0.035f * FMath::Clamp(SD - Width * 1.3f, 0.0f, 8.0f);
        return SmoothMax(Near, ShelfBed(SD), 0.4f);
    }

    float GroundExact(float X, float Y)
    {
        const FVector2D P(X, Y);
        const float SD = ShoreDistance(X, Y) / 100;     // metres, positive in the water
        const float Bluff = BluffHeight(P);
        if (SD > 0)
        {
            // Lake bed: along most banks the bed drops off within a few metres of the waterline to
            // about 1.5 m (in reach of a 6 m pole), steeper in front of the fishing spots, more gently
            // here and there; a nearly flat foot, then the old river channel, deepest towards the
            // west end. The backwater, the bay and the upper end keep a wide shallow shelf (reeds,
            // lilies) and are no deeper than about 1.3 m, with a pocket in front of their spots.
            const FSpine::FHit Hit = MainSpine().Query(P);
            const float Channel = ChannelDepth(Hit.Along / MainSpine().Length);
            const float Lane = SD < 15 ? SpotLane(P) : 0.0f;
            const float Wander = Noise(P + FVector2D(-2300, 1700), 3200);
            const float Width = FMath::Lerp(FMath::Clamp(2.1f + 0.8f * Wander, 1.5f, 2.9f), 1.6f, Lane);
            const float Foot = FMath::Lerp(1.5f + 0.15f * Noise(P + FVector2D(800, -3100), 4500), 1.45f, Lane);
            const float Back = BackwaterAmount(X, Y);
            float D = FMath::Lerp(BankBed(SD, Width, Foot), FMath::Min(ShelfBed(SD), 1.25f), Back * (1 - Lane));
            D = FMath::Lerp(D, FMath::Min(D, 1.35f), Back);
            D = FMath::Min(D, Channel);
            D += Bluff * 0.45f * S(0, 5, SD);                                   // steep under the bluffs
            D += 1.6f * Gauss(P, { Lay(-29500), Lay(1800) }, Lay(2600));          // the deep pool at the west end
            D += 1.8f * Gauss(P, { Lay(16500), Lay(-7000) }, Lay(1600));          // the hole beyond the island
            D += 1.1f * Gauss(P, { Lay(28500), Lay(300) }, Lay(1300));
            D = FMath::Max(D - 2.4f * Gauss(P, { Lay(1500), Lay(-2500) }, Lay(1700)), FMath::Min(D, 1.0f));   // an underwater hump
            D += 0.16f * Noise(P, 1500) * S(1, 4, SD);
            return SurfaceZ - FMath::Max(D, 0.05f) * 100;
        }
        const float L = -SD;                              // metres inland
        const float Forest = ForestAmount(P);
        // The valley side: a low bank rising gently, bluffs where they are, then hills, higher and
        // rounder in the forest, lower and terraced in the meadow.
        float H = 0.35f * S(0, 4, L) + 0.035f * FMath::Min(L, 120.0f);
        if (Bluff > 0)
        {
            const float Height = Bluff * (0.7f + 0.45f * Noise(P + FVector2D(123, 77), 1500));
            const float Face = 3.5f + 2.5f * (0.5f + 0.5f * Noise(P + FVector2D(-500, 900), 1800));
            // Gullies washed into the edge, and smaller ribs between them.
            const float Ragged = 2.0f * Noise(P + FVector2D(911, -377), 700) + 0.8f * Noise(P + FVector2D(37, 5), 400) + 0.3f * Noise(P, 150);
            H += Height * S(1.2f, 1.2f + Face, L + Ragged);
        }
        H += FMath::Lerp(7.0f, 16.0f, Forest) * (0.5f + 0.5f * Fbm(P, 22000)) * S(8, 90, L);
        H += 2.2f * Fbm(P + FVector2D(300, 900), 5000) * S(5, 30, L) + 0.25f * Noise(P + FVector2D(-77, 31), 500);
        // The island: a low wooded hump.
        H += 1.8f * (1 - S(0, Lay(2600), FVector2D::Distance(P, GIsland))) * S(1.5f, 6, L);
        // Ravines cut down through the hills to the lake, their floors rising gently inland.
        for (const FRavine& R : GRavines)
        {
            float T;
            const float Dist = SegmentDistance(P, R.Mouth, R.Head, &T);
            if (Dist > R.HalfWidth * 1.8f) continue;
            const float Floor = 0.3f + 0.045f * T * FVector2D::Distance(R.Mouth, R.Head) / 100;
            H = FMath::Min(H, FMath::Lerp(Floor, H, S(0, R.HalfWidth * 1.8f, Dist + 0.3f * R.HalfWidth * Noise(P, 1200))));
        }
        // The village plateau and the camp: level ground.
        const float Village = 1 - S(6000, 9500, FVector2D::Distance(P, GVillage));
        H = FMath::Lerp(H, 3.0f + 0.12f * Noise(P, 900), Village * S(4, 12, L));
        const float Camp = (1 - S(700, 1600, FVector2D::Distance(P, StartSpot()))) * S(0.5f, 4, L);
        H = FMath::Lerp(H, 0.4f * S(0, 5, L) + 0.04f, Camp);
        // Beyond the terrain (the distant ring) the hills keep rising to close the horizon.
        const FVector2D Outside = FVector2D(FMath::Max3(0.0, TerrainBounds.Min.X - X, X - TerrainBounds.Max.X), FMath::Max3(0.0, TerrainBounds.Min.Y - Y, Y - TerrainBounds.Max.Y));
        H += 0.02f * Outside.Size() / 100 + 6.0f * S(0, 60000, Outside.Size()) * (0.5f + 0.5f * Fbm(P, 60000));
        // Never below the water level away from the lake.
        H = FMath::Max(H, 0.15f * S(0, 3, L));
        return SurfaceZ + H * 100;
    }

    /** The ground on the same 1 m grid, built once. */
    struct FGroundGrid
    {
        FVector2D Min;
        int32 NX = 0, NY = 0;
        TArray<float> H;
        FGroundGrid()
        {
            // Everything the ground depends on is made first, so the parallel loop only reads.
            Field();
            SpotList();
            Min = TerrainBounds.Min;
            NX = FMath::CeilToInt((TerrainBounds.Max.X - Min.X) / Cell) + 1;
            NY = FMath::CeilToInt((TerrainBounds.Max.Y - Min.Y) / Cell) + 1;
            H.SetNumUninitialized(NX * NY);
            ParallelFor(NY, [this](int32 Y)
            {
                for (int32 X = 0; X < NX; ++X) H[Y * NX + X] = GroundExact(Min.X + X * Cell, Min.Y + Y * Cell);
            });
        }
        bool Sample(float X, float Y, float& Out) const
        {
            const float FX = (X - Min.X) / Cell, FY = (Y - Min.Y) / Cell;
            if (FX < 0 || FY < 0 || FX >= NX - 1 || FY >= NY - 1) return false;
            const int32 IX = (int32)FX, IY = (int32)FY;
            const float TX = FX - IX, TY = FY - IY;
            const float* Row = &H[IY * NX + IX];
            Out = FMath::Lerp(FMath::Lerp(Row[0], Row[1], TX), FMath::Lerp(Row[NX], Row[NX + 1], TX), TY);
            return true;
        }
    };
    const FGroundGrid& GroundGrid() { static const FGroundGrid Instance; return Instance; }
}

// --- the water --------------------------------------------------------------------------------

float LakeMap::ShoreDistance(float X, float Y)
{
    float D;
    if (Field().Sample(X, Y, D)) return D;
    return -50000.0f;    // far outside the terrain: deep inland
}
FShorePoint LakeMap::NearestShore(const FVector2D& P)
{
    const float D = ShoreDistance(P.X, P.Y);
    const FVector2D G(ShoreDistance(P.X + 100, P.Y) - ShoreDistance(P.X - 100, P.Y), ShoreDistance(P.X, P.Y + 100) - ShoreDistance(P.X, P.Y - 100));
    FShorePoint Out;
    Out.Outward = (-G).GetSafeNormal();
    if (Out.Outward.IsNearlyZero()) Out.Outward = FVector2D(0, 1);
    Out.Location = P + Out.Outward * D;
    Out.bIsland = FVector2D::Distance(Out.Location, GIsland) < Lay(4500);
    return Out;
}
const TArray<FShorePoint>& LakeMap::Shoreline() { return Field().Shore; }
FBox2D LakeMap::LakeBounds() { return Field().Water; }
float LakeMap::Ground(float X, float Y)
{
    float H;
    if (GroundGrid().Sample(X, Y, H)) return H;
    return GroundExact(X, Y);
}
bool LakeMap::IsWater(float X, float Y) { return ShoreDistance(X, Y) > 20; }
float LakeMap::Depth(float X, float Y) { return FMath::Max(0.0f, SurfaceZ - Ground(X, Y)); }
FVector2D LakeMap::DeeperDirection(float X, float Y)
{
    const FVector2D G(Depth(X + 150, Y) - Depth(X - 150, Y), Depth(X, Y + 150) - Depth(X, Y - 150));
    if (G.SizeSquared() > 1) return G.GetSafeNormal();
    // Flat bed (or dry land): away from the nearest bank.
    return -NearestShore(FVector2D(X, Y)).Outward;
}

// --- places ------------------------------------------------------------------------------------

TConstArrayView<FSpot> LakeMap::Spots() { return SpotList(); }
float LakeMap::SpotAmount(const FVector2D& P)
{
    float Amount = 0;
    for (const FSpot& Spot : SpotList())
    {
        const FVector2D F = SpotFrame(Spot, P);
        if (FMath::Abs(F.X) > Spot.HalfWidth * 1.4f || F.Y > 1500 || F.Y < -900) continue;
        Amount = FMath::Max(Amount, (1 - S(Spot.HalfWidth * 0.7f, Spot.HalfWidth * 1.3f, FMath::Abs(F.X))) * (1 - S(900, 1400, F.Y)) * S(-850, -600, F.Y));
    }
    return Amount;
}
const FSpot* LakeMap::SpotAt(const FVector2D& P)
{
    for (const FSpot& Spot : SpotList())
    {
        const FVector2D F = SpotFrame(Spot, P);
        if (FMath::Abs(F.X) < Spot.HalfWidth + 500 && F.Y > -1500 && F.Y < 1800) return &Spot;
    }
    return nullptr;
}
float LakeMap::BluffHeight(const FVector2D& P)
{
    // Find the bank first, so the bluff's height carries inland and out over the water.
    const FVector2D Bank = FMath::Abs(ShoreDistance(P.X, P.Y)) < 3000 ? NearestShore(P).Location : P;
    float H = 0;
    for (const FBluff& B : GBluffs) H = FMath::Max(H, B.Height * (1 - S(B.Reach * 0.6f, B.Reach, FVector2D::Distance(Bank, B.C))));
    return H;
}
float LakeMap::BackwaterAmount(float X, float Y)
{
    const FVector2D P(X, Y);
    const FSpine::FHit Arm = BackwaterSpine().Query(P);
    const float Backwater = S(-600.0f, 600.0f, Arm.Width - Arm.Dist) * S(Lay(2000), Lay(5000), Arm.Along);
    const float Bay = 1 - S(0.7f, 1.1f, GSouthBay.K(P));
    const FSpine::FHit Main = MainSpine().Query(P);
    const float Upper = S(0.8f, 0.9f, Main.Along / MainSpine().Length);
    return FMath::Max3(Backwater, Bay * 0.85f, Upper);
}
float LakeMap::ForestAmount(const FVector2D& P)
{
    // Open meadow on the south bank west of the narrows, round the west end and on the village plateau;
    // forest everywhere else. The edges wander.
    const FSpine::FHit Main = MainSpine().Query(P);
    const float Edge = 2500.0f * Noise(P, 6000);
    const float SouthMeadow = (Main.bSouth && P.Y > Lay(-14000)) ? 1 - S(Lay(1500), Lay(9000), P.X + Edge) : 0.0f;
    const float WestMeadow = 1 - S(Lay(-36000), Lay(-29000), P.X + Edge);
    const float Village = 1 - S(8000, 13000, FVector2D::Distance(P, GVillage));
    // Far away on the meadow side the forest closes in again on the horizon.
    const float Far = S(Lay(25000), Lay(34000), P.Y + Edge);
    return 1 - FMath::Max(FMath::Max(SouthMeadow, WestMeadow) * (1 - Far), Village);
}

FVector2D LakeMap::StartSpot(FVector2D* OutFacing)
{
    // Just behind the boards at the water's edge (they lie 0.5-1.1 m up the bank): a step forward
    // stands on them, and a pendulum swing of 2-4 m then lays the float on the drop-off.
    const FSpot& Camp = SpotList()[0];
    if (OutFacing) *OutFacing = -Camp.Outward;
    return Camp.Location + Camp.Outward * 140;
}
const FPier& LakeMap::Pier()
{
    static const FPier Instance = []
    {
        const FSpot& Spot = SpotList()[1];
        return FPier{ Spot.Location + Spot.Outward * 350, -Spot.Outward, 1700.0f, 170.0f };
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
FShorePoint LakeMap::Landing() { static const FShorePoint Instance = NearestShore({ Lay(-14500), Lay(18200) }); return Instance; }
FVector2D LakeMap::VillageCentre() { return GVillage; }
FVector2D LakeMap::IslandCentre() { return GIsland; }
FVector2D LakeMap::WestBay() { return GWestBay.C - FVector2D(1, 0).GetRotated(GWestBay.Yaw) * GWestBay.Axes.X; }
FVector2D LakeMap::WestEnd() { return FVector2D(Lay(-40000), Lay(1800)); }

const TArray<FSnag>& LakeMap::Snags()
{
    static const TArray<FSnag> Instance = []
    {
        // Fallen trees along the wooded banks: thickest in the snag cove and under the bluff, a
        // couple off the island, none in front of the fishing spots or the pier.
        TArray<FSnag> Out;
        FRandomStream Random(9113);
        const TArray<FShorePoint>& Shore = Shoreline();
        auto TryAdd = [&](const FShorePoint& S, float MinSpacing)
        {
            if (SpotAmount(S.Location - S.Outward * 600) > 0.05f) return false;
            for (const FSnag& Other : Out) if (FVector2D::Distance(FVector2D(Other.Root), S.Location) < MinSpacing) return false;
            const FVector2D Dir = (-S.Outward).GetRotated(Random.FRandRange(-28, 28));
            const FVector2D Root2 = S.Location + S.Outward * 120;
            const FVector2D Tip2 = Root2 + Dir * Random.FRandRange(900, 1500);
            if (!IsWater(Tip2.X, Tip2.Y)) return false;
            for (const FSpot& Spot : SpotList()) if (SegmentDistance(Spot.Location, Root2, Tip2) < 1500) return false;
            FSnag Snag;
            Snag.Root = FVector(Root2, Ground(Root2.X, Root2.Y) + 20);
            Snag.Tip = FVector(Tip2, FMath::Min(Ground(Tip2.X, Tip2.Y) + 25, SurfaceZ - 30));
            Snag.Radius = Random.FRandRange(16, 28);
            Out.Add(Snag);
            return true;
        };
        for (int32 Try = 0; Try < 30000 && Out.Num() < 40; ++Try)
        {
            const FShorePoint& S = Shore[Random.RandRange(0, Shore.Num() - 1)];
            const bool bCove = GSnagCove.K(S.Location) < 1.3f;
            const bool bBluff = BluffHeight(S.Location) > 1.5f;
            const float Chance = bCove ? 1.0f : bBluff ? 0.4f : S.bIsland ? 0.15f : 0.05f * ForestAmount(S.Location);
            if (Random.FRand() > Chance) continue;
            TryAdd(S, bCove ? 900.0f : 3500.0f);
        }
        // The fishing spot in the snag cove has a fallen tree of its own, 4-5 m along the bank and
        // leaning away from its casting lane: the perch and pike holding by it are in reach of a pole.
        for (const FSpot& Spot : SpotList())
        {
            if (!Spot.bBoards || GSnagCove.K(Spot.Location) > 1.3f) continue;
            const FVector2D Along(-Spot.Outward.Y, Spot.Outward.X);
            const FVector2D Root2 = Spot.Location + Along * 450 + Spot.Outward * 120;
            const FVector2D Tip2 = Root2 + (-Spot.Outward).GetRotated(-18) * 1150;
            if (!IsWater(Tip2.X, Tip2.Y)) continue;
            FSnag Snag;
            Snag.Root = FVector(Root2, Ground(Root2.X, Root2.Y) + 20);
            Snag.Tip = FVector(Tip2, FMath::Min(Ground(Tip2.X, Tip2.Y) + 25, SurfaceZ - 30));
            Snag.Radius = 24;
            Out.Add(Snag);
        }
        return Out;
    }();
    return Instance;
}
const TArray<FLilyBed>& LakeMap::LilyBeds()
{
    static const TArray<FLilyBed> Instance = []
    {
        TArray<FLilyBed> Out;
        FRandomStream Random(4242);
        const FBox2D Box = LakeBounds();
        const FPier& Pr = Pier();
        for (int32 Try = 0; Try < 40000 && Out.Num() < 380; ++Try)
        {
            const FVector2D P(Random.FRandRange(Box.Min.X, Box.Max.X), Random.FRandRange(Box.Min.Y, Box.Max.Y));
            const float SD = ShoreDistance(P.X, P.Y) / 100, D = Depth(P.X, P.Y) / 100;
            if (SD < 1.5f || SD > 24 || D < 0.35f || D > 1.7f) continue;
            const float Back = BackwaterAmount(P.X, P.Y);
            if (Back < 0.5f && Random.FRand() > 0.45f) continue;
            // Keep casting lanes clear in front of every spot and along the pier.
            bool bLane = false;
            for (const FSpot& Spot : SpotList())
            {
                const FVector2D F = SpotFrame(Spot, P);
                bLane |= FMath::Abs(F.X) < Spot.HalfWidth + 500 && F.Y < 0 && F.Y > -3500;
            }
            if (bLane || SegmentDistance(P, Pr.Root, Pr.Root + Pr.Dir * Pr.Length) < 700) continue;
            Out.Add({ P, (float)(Back > 0.5f ? Random.FRandRange(350.0f, 800.0f) : Random.FRandRange(180.0f, 500.0f)) });
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
    // Warm weedy shallows: the backwater, the bay, the upper end and the lily beds.
    Z.Shallows = FMath::Max(BackwaterAmount(X, Y), LilyAmount(X, Y)) * S(0.25f, 0.45f, D) * (1 - S(1.8f, 2.4f, D));
    // Just outside the reed belt (none under the bluffs; at a spot the clearing is only a gap that
    // fish swimming along the weed edge pass through).
    Z.ReedEdge = (1 - 0.5f * SpotAmount(P)) * (1 - S(0.5f, 1.2f, BluffHeight(P))) * S(1.0f, 2.5f, SD) * (1 - S(7, 11, SD));
    // Where the bed falls away steeply, and at its foot: the water just beyond a slope that rose
    // steeply within the last 3 m towards the bank (the drop-off along the banks is only 2-3 m
    // wide, hard by the shore, and the fish hold under it).
    const float Slope = FVector2D(Depth(X + 150, Y) - Depth(X - 150, Y), Depth(X, Y + 150) - Depth(X, Y - 150)).Size() / 300;
    const FVector2D Bankward = P + NearestShore(P).Outward * 300;
    const float Rise = (D - Depth(Bankward.X, Bankward.Y) / 100) / 3;
    Z.DropOff = FMath::Max(S(0.08f, 0.2f, Slope), 0.8f * S(0.12f, 0.25f, Rise)) * S(0.8f, 1.5f, D);
    // Snags, the pier piles and the island's fringe.
    float Cover = 0;
    for (const FSnag& Snag : Snags())
        Cover = FMath::Max(Cover, 1 - S(150, 500, SegmentDistance(P, FVector2D(Snag.Root), FVector2D(Snag.Tip))));
    const FPier& Pr = Pier();
    Cover = FMath::Max(Cover, 1 - S(150, 450, SegmentDistance(P, Pr.Root, Pr.Root + Pr.Dir * Pr.Length)));
    Cover = FMath::Max(Cover, 0.5f * (1 - S(900, 1500, FVector2D::Distance(P, GIsland) - Lay(2500))));
    Z.Cover = Cover;
    Z.Deep = S(3.4f, 4.8f, D);
    Z.Open = S(12, 25, SD) * (1 - 0.5f * Z.Deep);
    return Z;
}

bool LakeMap::InPlayArea(float X, float Y)
{
    return TerrainBounds.ExpandBy(-4000).IsInside(FVector2D(X, Y));
}
