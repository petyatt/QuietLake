#include "LakeMap.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Async/ParallelFor.h"

namespace
{
    using namespace LakeMap;
    constexpr float Cell = 100.0f;                   // 1 m grids for the shore distance and the ground
    // The layout below is written in metres (X east, Y south) and converted to cm here.
    constexpr float Mt = 100.0f;
    FVector2D At(float X, float Y) { return FVector2D(X * Mt, Y * Mt); }

    float S(float E0, float E1, float X) { return FMath::SmoothStep(E0, E1, X); }
    float Noise(const FVector2D& P, float Scale) { return FMath::PerlinNoise2D(P / Scale); }
    float Fbm(const FVector2D& P, float Scale)
    {
        return 0.6f * Noise(P, Scale) + 0.3f * Noise(P + FVector2D(1730, -910), Scale / 2.1f) + 0.1f * Noise(P + FVector2D(-4100, 2600), Scale / 4.3f);
    }
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

        /** A control point in metres: X, Y, half-width to the north (-Y side), half-width to the south. */
        struct FControl { float X, Y, Z, W; };
        FSpine(std::initializer_list<FControl> Control, float InSeed) : Seed(InSeed)
        {
            // Catmull-Rom through the points, resampled about every 8 m, widths eased between control points.
            TArray<FControl> C(Control);
            for (FControl& K : C) { K.X *= Mt; K.Y *= Mt; K.Z *= Mt; K.W *= Mt; }
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
            Hit.Width = W * (1 + 0.12f * FMath::PerlinNoise1D(Hit.Along / 4875 + (Hit.bSouth ? 13.7f : 3.1f) + Seed));
            return Hit;
        }
        float Inside(const FVector2D& Q) const { const FHit H = Query(Q); return H.Width - H.Dist; }
    };

    const FSpine& MainSpine()
    {
        // West (the outflow) to east (the brook). Widths: north, south (metres).
        static const FSpine Spine({
            { -236.25f, 13.5f, 51, 57 },          // the rounded west end: the spine's cap is the bank
            { -202.5f, 22.5f, 71.25f, 75 },
            { -112.5f, 11.25f, 60, 82.5f },
            { -22.5f, -22.5f, 45, 48.75f },       // the narrows
            { 75, -37.5f, 82.5f, 67.5f },          // the wide middle reach
            { 165, -18.75f, 63.75f, 52.5f },
            { 240, 18.75f, 41.25f, 45 },
            { 307.5f, 41.25f, 26.25f, 28.5f },
            { 363.75f, 48.75f, 6.75f, 6.75f },     // where the brook comes in
        }, 0.0f);
        return Spine;
    }
    /** The short weedy backwater running north into the forest; a spring at its head. */
    const FSpine& BackwaterSpine()
    {
        static const FSpine Spine({
            { 97.5f, -93.75f, 28.5f, 28.5f },
            { 86.25f, -142.5f, 22.5f, 22.5f },
            { 68, -176, 15, 15 },
            { 57, -190, 6, 6 },
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
        /** Elongated gaussian over the ellipse (no wave). */
        float Gauss(const FVector2D& P) const
        {
            const FVector2D L = (P - C).GetRotated(-Yaw);
            return FMath::Exp(-(FMath::Square(L.X / Axes.X) + FMath::Square(L.Y / Axes.Y)));
        }
    };
    /** An ellipse written in metres. */
    FEllipse E(float X, float Y, float AX, float AY, float Yaw = 0, float Seed = 0) { return FEllipse{ At(X, Y), At(AX, AY), Yaw, Seed }; }

    const FEllipse GSouthBay = E(-116.25f, 101.25f, 48.75f, 33.75f, 15, 2.0f);
    const FEllipse GSnagCove = E(191.25f, 63.75f, 24, 19.5f, -20, 5.0f);
    const FEllipse GRavineCove = E(-48.75f, -73.5f, 18, 13.5f, 70, 9.0f);
    const FEllipse GWestBay = E(-251.25f, -18.75f, 24, 19.5f, 35, 11.0f);   // the little bay at the west end
    const FVector2D GVillage = At(70, 160);
    constexpr float GVillageRadius = 6800.0f;

    /** Rough inside-distance of the water (cm, positive in the water); the exact distance is built from its zero line. */
    float WaterField(const FVector2D& P)
    {
        float F = MainSpine().Inside(P);
        F = SmoothMax(F, BackwaterSpine().Inside(P), 1500);
        for (const FEllipse* Bay : { &GSouthBay, &GSnagCove, &GRavineCove, &GWestBay }) F = SmoothMax(F, Bay->Inside(P), 1500);
        return F;
    }

    /** Signed distance to the waterline on a 1 m grid, a blurred copy for the hills, and the waterline itself. */
    struct FShoreField
    {
        FVector2D Min;
        int32 NX = 0, NY = 0;
        TArray<float> D, Blurred;
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
            // The exact distance creases where two banks are equally far; the hills follow a blurred copy
            // (box blur 25 m wide, three passes).
            Blurred = D;
            {
                constexpr int32 R = 12;
                TArray<float> Row;
                Row.SetNumUninitialized(FMath::Max(NX, NY));
                for (int32 Pass = 0; Pass < 3; ++Pass)
                {
                    for (int32 Y = 0; Y < NY; ++Y)
                    {
                        double Acc = 0; int32 Count = 0;
                        for (int32 X = -R; X < NX + R; ++X)
                        {
                            if (X + R < NX) { Acc += Blurred[Y * NX + X + R]; ++Count; }
                            if (X - R - 1 >= 0) { Acc -= Blurred[Y * NX + X - R - 1]; --Count; }
                            if (X >= 0 && X < NX) Row[X] = Acc / Count;
                        }
                        for (int32 X = 0; X < NX; ++X) Blurred[Y * NX + X] = Row[X];
                    }
                    for (int32 X = 0; X < NX; ++X)
                    {
                        double Acc = 0; int32 Count = 0;
                        for (int32 Y = -R; Y < NY + R; ++Y)
                        {
                            if (Y + R < NY) { Acc += Blurred[(Y + R) * NX + X]; ++Count; }
                            if (Y - R - 1 >= 0) { Acc -= Blurred[(Y - R - 1) * NX + X]; --Count; }
                            if (Y >= 0 && Y < NY) Row[Y] = Acc / Count;
                        }
                        for (int32 Y = 0; Y < NY; ++Y) Blurred[Y * NX + X] = Row[Y];
                    }
                }
            }
            // The waterline points, with the normal onto the land from the distance gradient.
            Shore.Reserve(Crossings.Num());
            for (const FVector2D& P : Crossings)
            {
                FShorePoint Point;
                Point.Location = P;
                FVector2D G(0, 0);
                float A, B, C, E;
                if (Sample(D, P.X + 100, P.Y, A) && Sample(D, P.X - 100, P.Y, B) && Sample(D, P.X, P.Y + 100, C) && Sample(D, P.X, P.Y - 100, E)) G = FVector2D(A - B, C - E);
                Point.Outward = (-G).GetSafeNormal();
                Shore.Add(Point);
                Water += P;
            }
        }
        bool Sample(const TArray<float>& Grid, float X, float Y, float& Out) const
        {
            const float FX = (X - Min.X) / Cell, FY = (Y - Min.Y) / Cell;
            if (FX < 0 || FY < 0 || FX >= NX - 1 || FY >= NY - 1) return false;
            const int32 IX = (int32)FX, IY = (int32)FY;
            const float TX = FX - IX, TY = FY - IY;
            const float* Row = &Grid[IY * NX + IX];
            Out = FMath::Lerp(FMath::Lerp(Row[0], Row[1], TX), FMath::Lerp(Row[NX], Row[NX + 1], TX), TY);
            return true;
        }
    };
    const FShoreField& Field() { static const FShoreField Instance; return Instance; }

    // --- places --------------------------------------------------------------------------------

    struct FSpotSeed { const TCHAR* Name; float X, Y, HalfWidth; bool bBoards, bLilies; };
    const FSpotSeed GSpotSeeds[] = {
        { TEXT("Лагерь"),          -180, 99.75f,   9, true, true },     // the start: camp on the meadow bank
        { TEXT("Мостки"),          30, 30,         7, false, false },   // the pier below the village
        { TEXT("Ольхи"),           -258, 68,       7, true, false },
        { TEXT("Исток"),           -291, -6,       7, true, false },    // by the outflow
        { TEXT("Омут"),            -232.5f, -45,   8, true, false },
        { TEXT("Берёзовая гряда"), -150, -49.5f,   8, true, false },
        { TEXT("Овраг"),           -48.75f, -87,   7, true, false },
        { TEXT("Под яром"),        73.5f, -121.5f, 7, true, false },
        { TEXT("Устье заводи"),    73.5f, -138.75f, 7, true, false },
        { TEXT("Заводь"),          70, -178,       7, true, false },
        { TEXT("Отмель"),          110, 34,        8, true, true },     // the crucian shallow
        { TEXT("Плёс"),            202.5f, -53.25f, 8, true, false },
        { TEXT("Сосновый яр"),     150, -96,       7, true, false },
        { TEXT("Коряжник"),        212.25f, 71.25f, 7, true, false },
        { TEXT("Верховье"),        307.5f, 69.75f, 7, true, true },
        { TEXT("Залив"),           -140, 128,      7, true, true },
        { TEXT("Луговая бровка"),  -47, 68,        7, true, true },
    };
    const TArray<FSpot>& SpotList()
    {
        static const TArray<FSpot> Instance = []
        {
            TArray<FSpot> Out;
            for (const FSpotSeed& Seed : GSpotSeeds)
            {
                const FShorePoint Shore = NearestShore(At(Seed.X, Seed.Y));
                Out.Add({ Seed.Name, Shore.Location, Shore.Outward, Seed.HalfWidth * Mt, Seed.bBoards });
            }
            return Out;
        }();
        return Instance;
    }

    /** The banks: bluffs and cliffs (centre, reach along the bank in m, height in m), low earth steps, flat banks, paths down. */
    struct FBluff { float X, Y, Reach, Height; bool bCliff; };
    const FBluff GBluffs[] = {
        { -128, -54, 60, 6, false },          // the north bluff over the trough
        { 58, -110, 30, 5, true },            // a cliff at the mouth of the backwater
        { -232, -48, 30, 2.2f, true },        // the high bank at "Омут", a path down
        { 202, -56, 34, 2.2f, true },         // the high bank at "Плёс", a path down
        { 300, 12, 30, 4, true },             // a cliff over the upper end
        { 152, -97, 34, 4, false },           // "Сосновый яр"
    };
    struct FStep { float X, Y, Reach, Height; };
    const FStep GSteps[] = {
        { -200, 96, 50, 0.5f }, { 200, 76, 30, 0.5f }, { -48, -80, 22, 0.4f }, { -258, 68, 26, 0.45f }, { -47, 68, 24, 0.5f },
    };
    struct FFlat { float X, Y, Reach; };
    const FFlat GFlats[] = { { 50, 40, 45 }, { -116, 136, 40 }, { 340, 55, 30 }, { -292, 6, 22 } };
    struct FDescent { float X, Y, Width; };
    const FDescent GDescents[] = { { -232.5f, -45, 8 }, { 202.5f, -53, 8 } };

    /** Valleys running to the lake (metres): the outflow, the brook, the spring's run (with water) and a dry ravine. */
    struct FValley { TArray<FVector2D> Points; float HalfWidth; bool bWater, bOutflow; };
    const TArray<FValley>& Valleys()
    {
        static const TArray<FValley> Instance = []
        {
            auto V = [](std::initializer_list<FVector2D> Metres, float HalfWidth, bool bWater, bool bOutflow)
            {
                FValley Out;
                for (const FVector2D& P : Metres) Out.Points.Add(P * Mt);
                Out.HalfWidth = HalfWidth * Mt;
                Out.bWater = bWater;
                Out.bOutflow = bOutflow;
                return Out;
            };
            return TArray<FValley>{
                V({ { -286, 14 }, { -322, 24 }, { -365, 16 }, { -420, 30 } }, 12, true, true),     // the outflow
                V({ { 362, 49 }, { 392, 60 }, { 420, 64 }, { 525, 71 } }, 13, true, false),         // the brook
                V({ { 57, -190 }, { 44, -222 }, { 30, -262 }, { 24, -300 } }, 10, true, false),     // the spring's run
                V({ { -48.75f, -84 }, { -56.25f, -202.5f } }, 16, false, false),                    // the dry ravine above "Овраг"
            };
        }();
        return Instance;
    }
    /** The stream's water surface (m above the lake) this far (m) along its valley: the brook and the spring's run come down to the lake, the outflow falls away from it. */
    float StreamLevel(const FValley& V, float AlongM) { return V.bOutflow ? -0.015f * FMath::Max(0.0f, AlongM - 40) : 0.04f * AlongM; }
    constexpr float StreamHalfWidth = 110.0f;     // the water; the channel is a little wider

    /** The bank at the waterline nearest to P. */
    struct FBank { float Bluff = 0, Step = 0, Flat = 0; bool bCliff = false; };
    FBank BankAt(const FVector2D& P)
    {
        FBank Out;
        const float SD = ShoreDistance(P.X, P.Y);
        const FVector2D Bank = FMath::Abs(SD) < 12000 ? NearestShore(P).Location : P;
        const float Wander = Noise(Bank, 2000);
        auto Near = [&](float X, float Y, float Reach)
        {
            const float R = Reach * Mt;
            return 1 - S(R * 0.6f, R, FVector2D::Distance(Bank, At(X, Y)) + 0.12f * R * Wander);
        };
        for (const FBluff& B : GBluffs) { const float H = B.Height * Near(B.X, B.Y, B.Reach); if (H > Out.Bluff) { Out.Bluff = H; Out.bCliff = B.bCliff; } }
        for (const FStep& B : GSteps) Out.Step = FMath::Max(Out.Step, B.Height * Near(B.X, B.Y, B.Reach));
        for (const FFlat& B : GFlats) Out.Flat = FMath::Max(Out.Flat, Near(B.X, B.Y, B.Reach));
        // A path down cuts the high bank at the water, not far into the hill.
        float Ramp = 0;
        for (const FDescent& Dsc : GDescents) Ramp = FMath::Max(Ramp, 1 - S(Dsc.Width * 50, Dsc.Width * Mt, FVector2D::Distance(Bank, At(Dsc.X, Dsc.Y))));
        Ramp *= 1 - S(1000, 2500, FMath::Max(0.0f, -SD));
        Out.Bluff *= 1 - Ramp;
        Out.Step *= 1 - Ramp;
        Out.Step *= 1 - S(0.5f, 1.5f, Out.Bluff);
        Out.Flat *= 1 - S(0.2f, 1.0f, Out.Bluff + Out.Step);
        return Out;
    }

    /** Warm weedy shallows: the south bay, the upper end, the crucian shallow and the backwater. */
    struct FShallow { FEllipse Shape; float Weight; };
    const FShallow GShallows[] = {
        { GSouthBay, 0.85f },
        { E(322, 45, 62, 30, 15), 1.0f },       // the upper end
        { E(108, 22, 34, 17, 8), 1.0f },        // "Отмель", the crucian shallow
    };

    /** The main spine's channel depth (m) along the lake: deepest in the west, shallow at the brook. */
    float ChannelDepth(float Fraction)
    {
        static const float Table[][2] = { { 0.0f, 6.5f }, { 0.15f, 6.0f }, { 0.4f, 5.0f }, { 0.62f, 4.4f }, { 0.8f, 2.8f }, { 0.92f, 1.5f }, { 1.0f, 0.8f } };
        for (int32 I = 0; I + 1 < UE_ARRAY_COUNT(Table); ++I)
            if (Fraction <= Table[I + 1][0])
                return FMath::Lerp(Table[I][1], Table[I + 1][1], FMath::SmoothStep(0.0f, 1.0f, (Fraction - Table[I][0]) / (Table[I + 1][0] - Table[I][0])));
        return Table[UE_ARRAY_COUNT(Table) - 1][1];
    }

    /** Broad swells and hollows of the open bed (+m deepens, -m shallows): wide, so the bed rolls, not steps. */
    struct FSwell { FEllipse Shape; float Amp; };
    const FSwell GSwells[] = {
        { E(-60, -30, 34, 25, 10), -0.55f },    // a broad shallow bank off the north shore
        { E(40, 25, 30, 22, -12), 0.65f },      // a hollow in the middle
        { E(150, 15, 26, 18, 8), -0.45f },
        { E(205, -55, 24, 16, 20), 0.7f },
        { E(-150, 40, 36, 22, 5), 0.6f },
        { E(-30, 95, 24, 16, 30), 0.45f },
        { E(110, -30, 20, 14, 25), -0.5f },
        { E(-200, -20, 26, 18, 0), 0.5f },
    };
    /** Holes in the bed (+m), one level patch where the island stood, weed beds. */
    struct FHole { FEllipse Shape; float Amp; bool bAnyBed; };
    const FHole GHoles[] = {
        { E(-88, -14, 115, 28, 12), 1.8f, false },     // the trough under the north bluff
        { E(142, -72, 60, 13, 25), 2.8f, false },      // the trough under "Сосновый яр"
        { E(-236, 0, 36, 28), 4.4f, false },            // the west hole
        { E(-116, 104, 17, 12, 15), 2.4f, true },      // the hole in the south bay
        { E(106, -12, 17, 12, 20), 3.6f, false },      // the hole beside the hump
        { E(-68, 50, 15, 10, -140), 2.4f, false },     // in front of "Луговая бровка"
        { E(222, -36, 34, 16, 30), 2.6f, false },      // the east hole in front of "Плёс"
    };
    struct FLevel { FEllipse Shape; float Depth, Flat; };
    const FLevel GLevels[] = { { E(70, -8, 16, 11, 25), 1.8f, 1.0f } };    // the hump where the island stood
    const FEllipse GWeeds[] = { E(108, 4, 26, 8, 8, 3), E(70, -8, 15, 10, 25, 6) };

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
        if (SD > 0)
        {
            // Lake bed: along most banks the bed drops off within a few metres of the waterline to
            // about 1.5 m (in reach of a 6 m pole), steeper in front of the fishing spots; a nearly flat
            // foot, then the old river channel, deepest in the west. The weedy shallows keep a wide shelf
            // no deeper than about 1.3 m. Under bluffs the bed falls to a broad scour pool; low undercut
            // banks are deeper right at the edge; flat banks shelve out gently. Holes, the hump where
            // the island stood and a little noise finish it.
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
            const FBank Bank = SD < 60 ? BankAt(P) : FBank();
            float Pool = 0, PoolHeight = 0;
            bool bPoolCliff = false;
            for (const FBluff& B : GBluffs)
            {
                const float W = (1 - S(B.Reach * 50, B.Reach * 110, FVector2D::Distance(P, At(B.X, B.Y)))) * FMath::Clamp((B.Height - 2) / 3, 0.0f, 1.0f);
                if (W > Pool) { Pool = W; PoolHeight = B.Height; bPoolCliff = B.bCliff; }
            }
            Pool *= S(0, bPoolCliff ? 4 : 14, SD);
            D = FMath::Lerp(D, FMath::Max(D, FMath::Min(Channel + 1, 3 + 0.65f * PoolHeight)), Pool);
            D += Bank.Step * 0.5f * S(0, 1.2f, SD) * (1 - S(3, 9, SD));
            D = FMath::Lerp(D, FMath::Min(D, 0.05f + 0.09f * SD), Bank.Flat * (1 - Lane));
            for (const FHole& H : GHoles) D += H.Amp * H.Shape.Gauss(P) * S(2, 10, SD) * (H.bAnyBed ? 1 : 1 - Back);
            // Level patches lie in open water only: the shallows and the banks keep their own bed.
            float Level = 0;
            for (const FLevel& L : GLevels)
            {
                const float W = FMath::Min(1.0f, L.Flat * L.Shape.Gauss(P)) * S(10, 26, SD) * (1 - Back);
                D = FMath::Lerp(D, L.Depth, W);
                Level = FMath::Max(Level, W);
            }
            // The open bed rolls: broad shoals and hollows, then a slow swell over the whole lake, then
            // smaller ripples. All of it is wide and soft, so the bed varies in depth without steps.
            for (const FSwell& W : GSwells) D += W.Amp * W.Shape.Gauss(P) * S(6, 18, SD) * (1 - 0.8f * Level);
            D += (0.55f * Fbm(P + FVector2D(-2100, 1300), 14000) + 0.28f * Fbm(P + FVector2D(1400, -800), 5200))
                * S(3, 12, SD) * (1 - 0.7f * Level);
            D += 0.10f * Noise(P, 1700) * S(1.5f, 5, SD) * (1 - 0.8f * Level);
            // Shoals stay well under the waterline: no bar rises into a dry hump.
            if (SD > 10) D = FMath::Max(D, 0.45f);
            return SurfaceZ - FMath::Max(D, 0.05f) * 100;
        }
        const float L = -SD;                              // metres inland
        float Blurred = SD * 100;
        Field().Sample(Field().Blurred, X, Y, Blurred);
        const float LH = FMath::Lerp(L, FMath::Max(-Blurred / 100, 0.0f), S(6, 20, L));   // for the hills, away from the water
        const float Forest = ForestAmount(P);
        const FBank Bank = L < 120 ? BankAt(P) : FBank();
        // The valley side: a low bank rising gently (flatter on flat banks), a low earth step where
        // there is one, bluffs and cliffs, then hills, higher in the forest, rolling in between.
        float H = FMath::Lerp(0.35f * S(0, 4, L), 0.3f * S(0, 16, L), Bank.Flat * (1 - S(12, 24, L))) + 0.035f * FMath::Min(LH, 120.0f);
        H += Bank.Step * S(0.1f, 0.7f, L + 0.25f * Noise(P, 300)) * (1 - S(6, 18, L));
        if (Bank.Bluff > 0)
        {
            const float Height = Bank.Bluff * (0.7f + 0.45f * Noise(P + FVector2D(123, 77), 1500));
            const float Spread = 0.5f + 0.5f * Noise(P + FVector2D(-500, 900), 1800);
            // A cliff is a steep clay wall 1.5-3 m across, a bluff a slope 3.5-6 m across.
            const float Face = Bank.bCliff ? 1.5f + 1.5f * Spread : 3.5f + 2.5f * Spread;
            const float Toe = Bank.bCliff ? 0.5f : 1.2f;
            // Gullies washed into the edge, and smaller ribs between them; a cliff is torn by deeper ones.
            const float Ragged = (Bank.bCliff ? 1.8f : 2.0f) * Noise(P + FVector2D(911, -377), 700) + (Bank.bCliff ? 1.2f : 0.8f) * Noise(P + FVector2D(37, 5), 400)
                + (Bank.bCliff ? 0.5f * Noise(P + FVector2D(-260, 140), 180) : 0.0f);
            H += Height * S(Toe, Toe + Face, L + Ragged) * (1 - S(25, 70, L));    // the top merges into the hills
        }
        H += FMath::Lerp(10.0f, 20.0f, Forest) * (0.5f + 0.5f * Fbm(P, 22000)) * S(12, 120, LH);
        H += 4.0f * Fbm(P + FVector2D(4000, -7000), 9000) * S(20, 70, LH);                 // rolling hills between the big ones
        H += 2.2f * Fbm(P + FVector2D(300, 900), 5000) * S(5, 30, LH) + 0.25f * Noise(P + FVector2D(-77, 31), 500);
        // Valleys cut down through the hills to the lake, their floors rising gently inland; the
        // streams run in a narrow channel along the floor.
        bool bChannel = false;
        for (const FValley& V : Valleys())
        {
            float Acc = 0;
            for (int32 K = 0; K + 1 < V.Points.Num(); ++K)
            {
                float T;
                const float Dist = SegmentDistance(P, V.Points[K], V.Points[K + 1], &T);
                const float Segment = FVector2D::Distance(V.Points[K], V.Points[K + 1]);
                if (Dist < V.HalfWidth * 1.8f)
                {
                    const float Along = (Acc + T * Segment) / 100;
                    const float Water = StreamLevel(V, Along);
                    const float Floor = V.bWater ? Water + 0.3f : 0.3f + 0.045f * Along;
                    H = FMath::Min(H, FMath::Lerp(Floor, H, S(0, V.HalfWidth * 1.8f, Dist + 0.3f * V.HalfWidth * Noise(P, 1200))));
                    if (V.bWater)
                    {
                        const float Channel = 1 - S(StreamHalfWidth + 20, StreamHalfWidth + 120, Dist);
                        if (Channel > 0) { H = FMath::Lerp(H, FMath::Min(H, Water - 0.3f), Channel); bChannel = true; }
                    }
                }
                Acc += Segment;
            }
        }
        // The village stands on a hill: the ground is eased, not levelled.
        const float Village = 0.45f * (1 - S(GVillageRadius * 0.75f, GVillageRadius * 1.15f, FVector2D::Distance(P, GVillage)));
        H = FMath::Lerp(H, 3.0f + 0.12f * Noise(P, 900), Village * S(4, 12, L));
        const float Camp = (1 - S(700, 1600, FVector2D::Distance(P, StartSpot()))) * S(0.5f, 4, L);
        H = FMath::Lerp(H, 0.4f * S(0, 5, L) + 0.04f, Camp);
        // Beyond the terrain (the distant ring) the hills keep rising to close the horizon.
        const FVector2D Outside = FVector2D(FMath::Max3(0.0, TerrainBounds.Min.X - X, X - TerrainBounds.Max.X), FMath::Max3(0.0, TerrainBounds.Min.Y - Y, Y - TerrainBounds.Max.Y));
        H += 0.02f * Outside.Size() / 100 + 6.0f * S(0, 60000, Outside.Size()) * (0.5f + 0.5f * Fbm(P, 60000));
        // Never below the water level away from the lake (except in a stream's channel).
        if (!bChannel) H = FMath::Max(H, 0.15f * S(0, 3, L));
        return SurfaceZ + H * 100;
    }

    /** The ground on the same 1 m grid, built once: from the baked landscape where there is one. */
    struct FGroundGrid
    {
        FVector2D Min;
        int32 NX = 0, NY = 0;
        TArray<float> H;
        bool bBaked = false;
        /** The baked landscape heights (ULakeLevelLibrary::BakeGround): a small header, then uint16 heights. */
        bool LoadBaked()
        {
            TArray<uint8> Bytes;
            if (!FFileHelper::LoadFileToArray(Bytes, *(FPaths::ProjectContentDir() / BakedGroundFile), FILEREAD_Silent)) return false;
            struct FHeader { char Magic[4]; int32 W, H; float MinX, MinY, Step, ZScale, ZOffset; };
            if (Bytes.Num() < (int32)sizeof(FHeader)) return false;
            FHeader Header;
            FMemory::Memcpy(&Header, Bytes.GetData(), sizeof(FHeader));
            if (FMemory::Memcmp(Header.Magic, "QLG1", 4) != 0 || Bytes.Num() != (int32)sizeof(FHeader) + Header.W * Header.H * 2) return false;
            const uint16* Data = (const uint16*)(Bytes.GetData() + sizeof(FHeader));
            for (int32 Y = 0; Y < NY; ++Y)
                for (int32 X = 0; X < NX; ++X)
                {
                    const float FX = (Min.X + X * Cell - Header.MinX) / Header.Step, FY = (Min.Y + Y * Cell - Header.MinY) / Header.Step;
                    const int32 IX = FMath::Clamp(FMath::RoundToInt(FX), 0, Header.W - 1), IY = FMath::Clamp(FMath::RoundToInt(FY), 0, Header.H - 1);
                    H[Y * NX + X] = Data[IY * Header.W + IX] * Header.ZScale + Header.ZOffset;
                }
            return true;
        }
        FGroundGrid()
        {
            // Everything the ground depends on is made first, so the parallel loop only reads.
            Field();
            SpotList();
            Valleys();
            MainSpine();
            BackwaterSpine();
            Min = TerrainBounds.Min;
            NX = FMath::CeilToInt((TerrainBounds.Max.X - Min.X) / Cell) + 1;
            NY = FMath::CeilToInt((TerrainBounds.Max.Y - Min.Y) / Cell) + 1;
            H.SetNumUninitialized(NX * NY);
            bBaked = LoadBaked();
            if (!bBaked)
                ParallelFor(NY, [this](int32 Y)
                {
                    for (int32 X = 0; X < NX; ++X) H[Y * NX + X] = GroundExact(Min.X + X * Cell, Min.Y + Y * Cell);
                });
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_GROUND %s"), bBaked ? TEXT("baked landscape") : TEXT("designed"));
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

    /** Weed beds under water (1 inside), between 0.6 and 3.8 m deep. */
    float WeedAt(const FVector2D& P)
    {
        float W = 0;
        for (const FEllipse& Bed : GWeeds) W = FMath::Max(W, 1 - S(0.75f, 1.05f, Bed.K(P) + 0.15f * Noise(P, 600)));
        if (W <= 0) return 0;
        const float D = Depth(P.X, P.Y) / 100;
        return W * S(0.6f, 1.0f, D) * (1 - S(3.2f, 3.8f, D));
    }
    /** An old half-sunk boat by the pier and a row of old stakes in the south bay: cover, like the snags. */
    struct FCover { FVector2D A, B; };
    const TArray<FCover>& CoverObjects()
    {
        static const TArray<FCover> Instance = []
        {
            TArray<FCover> Out;
            float Yaw = 0;
            const FVector2D Boat = OldBoat(&Yaw), Keel = FVector2D(1, 0).GetRotated(Yaw) * 220;
            Out.Add({ Boat - Keel, Boat + Keel });
            const FVector2D First = At(-122, 98), Last = At(-100, 92);
            for (int32 K = 0; K < 7; ++K) { const FVector2D Stake = First + (Last - First) * (K / 6.0); Out.Add({ Stake, Stake + FVector2D(10, 10) }); }
            return Out;
        }();
        return Instance;
    }
}

// --- the water --------------------------------------------------------------------------------

float LakeMap::ShoreDistance(float X, float Y)
{
    float D;
    if (Field().Sample(Field().D, X, Y, D)) return D;
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
float LakeMap::DesignGround(float X, float Y) { return GroundExact(X, Y); }
bool LakeMap::HasBakedGround() { return GroundGrid().bBaked; }
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
float LakeMap::BluffHeight(const FVector2D& P) { return BankAt(P).Bluff; }
float LakeMap::BackwaterAmount(float X, float Y)
{
    const FVector2D P(X, Y);
    float Amount = 0;
    for (const FShallow& Shallow : GShallows) Amount = FMath::Max(Amount, (1 - S(0.7f, 1.1f, Shallow.Shape.K(P))) * Shallow.Weight);
    const FSpine::FHit Arm = BackwaterSpine().Query(P);
    return FMath::Max(Amount, S(-600.0f, 600.0f, Arm.Width - Arm.Dist) * S(1200.0f, 4000.0f, Arm.Along));
}
float LakeMap::ForestAmount(const FVector2D& P)
{
    // Hilly meadow along the south bank from the camp past the bay to the village; forest everywhere
    // else. The edges wander.
    static const FEllipse Meadows[] = { E(60, 108, 125, 85, 0, 3), E(-82, 126, 160, 82, 4, 4) };
    float Open = 0;
    for (const FEllipse& M : Meadows) Open = FMath::Max(Open, 1 - S(0.8f, 1.05f, M.K(P) + 0.12f * Noise(P, 4500)));
    const float Edge = 1200 * Noise(P + FVector2D(5000, 0), 4000);
    Open = FMath::Max(Open, 1 - S(GVillageRadius * 1.05f, GVillageRadius * 1.5f, FVector2D::Distance(P, GVillage) + Edge));
    return 1 - Open;
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
    return Across <= Pr.Width * 0.5f + 30;
}
FShorePoint LakeMap::Landing()
{
    // The boats lie on the bank a dozen metres west of the pier.
    static const FShorePoint Instance = []
    {
        const FSpot& Spot = SpotList()[1];
        return NearestShore(Spot.Location - FVector2D(-Spot.Outward.Y, Spot.Outward.X) * 1200);
    }();
    return Instance;
}
FVector2D LakeMap::OldBoat(float* OutYaw)
{
    if (OutYaw) *OutYaw = 60;
    return At(46, 14);
}
FVector2D LakeMap::VillageCentre() { return GVillage; }

bool LakeMap::VillagePaved(const FVector2D& P)
{
    const FVillage& V = Village();
    for (const FVillagePath& Path : V.Paths)
        for (int32 I = 0; I + 1 < Path.Line.Num(); ++I)
            if (SegmentDistance(P, Path.Line[I], Path.Line[I + 1]) < Path.Width * 0.5f + 70.0f) return true;

    for (const FVillagePlot& Plot : V.Plots)
    {
        const FVector2D Offset = P - Plot.House;
        const float Along = FVector2D::DotProduct(Offset, Plot.Facing);
        const float Across = FVector2D::CrossProduct(Plot.Facing, Offset);
        if (FMath::Abs(Along) < 460.0f && FMath::Abs(Across) < 540.0f) return true;
        // Three beds behind each house, positioned as in ALakeVillage::Build.
        if (!Plot.bShop && Along > 65.0f - Plot.Back && Along < 645.0f - Plot.Back
            && FMath::Abs(Across - (Plot.HalfWidth - 290.0f)) < 173.0f) return true;
    }
    // Kitchen beds by the small cabin beyond the inlet.
    if (P.X > -13120 && P.X < -12230 && FMath::Abs(P.Y - 15830.0f) < 150.0f) return true;
    return false;
}

const FVillage& LakeMap::Village()
{
    static const FVillage Instance = []
    {
        FVillage V;
        const FPier& Pier = LakeMap::Pier();
        const FVector2D ShorePath = Pier.Root - Pier.Dir * 240.0f;

        // The lane comes out of the woods, then bends along the far side of the inlet.
        V.Paths.Add({ { At(111, 185), At(107, 168), At(98, 153), At(86, 141),
            At(78, 128), At(69, 114), At(57, 103), At(42, 93), At(25, 90),
            At(5, 94), At(-15, 101), At(-36, 114), At(-57, 131), At(-79, 145),
            At(-104, 152), At(-129, 153), At(-151, 155) }, 470.0f, true });
        V.Paths.Add({ { At(42, 93), At(38, 79), At(34, 66), At(29, 55), At(26, 42), ShorePath }, 245.0f, false });
        V.Paths.Add({ { At(34, 66), At(48, 57), At(63, 52), At(80, 48), At(99, 50) }, 180.0f, false });

        // Loosely grouped houses around the lane, with their own yards and short paths to the gate.
        auto Plot = [&V](FVector2D House, FVector2D Street, float Width, float Front, float Back, bool bShop = false)
        {
            const FVector2D Facing = (Street - House).GetSafeNormal();
            V.Plots.Add({ House, Facing, Width, Front, Back, bShop });
            const FVector2D Door = House + Facing * 300.0f;
            V.Paths.Add({ { Street, House + Facing * Front, Door }, bShop ? 200.0f : 120.0f, false });
        };
        Plot(At(121, 155), At(98, 153), 890, 800, 860);
        Plot(At(59, 150), At(86, 141), 970, 960, 910);
        Plot(At(27, 121), At(25, 90), 890, 850, 920);
        Plot(At(102, 117), At(78, 128), 810, 810, 850);
        Plot(At(78, 88), At(57, 103), 1010, 850, 990);
        Plot(At(5, 73), At(5, 94), 870, 790, 900);
        Plot(At(98, 69), At(99, 50), 910, 780, 880);
        Plot(At(64, 69), At(63, 52), 790, 780, 840);
        Plot(At(40, 45), At(26, 42), 700, 650, 550, true);
        return V;
    }();
    return Instance;
}
FVector2D LakeMap::WestBay() { return GWestBay.C - FVector2D(1, 0).GetRotated(GWestBay.Yaw) * GWestBay.Axes.X; }
FVector2D LakeMap::WestEnd() { return At(-300, 13.5f); }

const TArray<FSnag>& LakeMap::Snags()
{
    static const TArray<FSnag> Instance = []
    {
        // Fallen trees along the wooded banks: thickest in the snag zones and under the bluffs, none in
        // front of the fishing spots or the pier; and a few trees sunk in open water by the hump.
        TArray<FSnag> Out;
        FRandomStream Random(2203);
        const TArray<FShorePoint>& Shore = Shoreline();
        const FPier& Pr = Pier();
        const FEllipse Zones[] = { E(-274, 50, 16, 12, -40), GSnagCove, E(100, -110, 24, 14) };
        for (int32 Try = 0; Try < 20000 && Out.Num() < 30; ++Try)
        {
            const FShorePoint& Sh = Shore[Random.RandRange(0, Shore.Num() - 1)];
            bool bZone = false;
            for (const FEllipse& Z : Zones) bZone |= Z.K(Sh.Location) < 1.2f;
            const bool bBluff = BluffHeight(Sh.Location) > 1.5f;
            const float Chance = bZone ? 1.0f : bBluff ? 0.35f : 0.05f * ForestAmount(Sh.Location + Sh.Outward * 600);
            if (Random.FRand() > Chance) continue;
            if (SpotAmount(Sh.Location - Sh.Outward * 600) > 0.05f) continue;
            bool bCrowded = false;
            for (const FSnag& Other : Out) bCrowded |= !Other.bSunken && FVector2D::Distance(FVector2D(Other.Root), Sh.Location) < (bZone ? 900.0f : 3500.0f);
            if (bCrowded) continue;
            const FVector2D Dir = (-Sh.Outward).GetRotated(Random.FRandRange(-28, 28));
            const FVector2D Root2 = Sh.Location + Sh.Outward * 120;
            const FVector2D Tip2 = Root2 + Dir * Random.FRandRange(900, 1500);
            if (ShoreDistance(Tip2.X, Tip2.Y) < 20) continue;
            bool bInLane = SegmentDistance(Pr.Root, Root2, Tip2) < 2000;
            for (const FSpot& Spot : SpotList()) bInLane |= SegmentDistance(Spot.Location, Root2, Tip2) < 1500;
            if (bInLane) continue;
            FSnag Snag;
            Snag.Root = FVector(Root2, Ground(Root2.X, Root2.Y) + 20);
            Snag.Tip = FVector(Tip2, FMath::Min(Ground(Tip2.X, Tip2.Y) + 25, SurfaceZ - 30));
            Snag.Radius = Random.FRandRange(16, 28);
            Out.Add(Snag);
        }
        // Trees sunk in open water: by the hump and its hole, on the hump's slope, and off "Луговая бровка".
        const float Sunken[][5] = {
            { 94, -2, 100, -13, 30 }, { 116, -4, 123, -14, 25 }, { 101, -25, 111, -29, 22 }, { 57, 4, 64, -6, 25 }, { -57, 54, -60, 45, 24 },
        };
        for (const auto& T : Sunken)
        {
            const FVector2D A = At(T[0], T[1]), B = At(T[2], T[3]);
            FSnag Snag;
            Snag.Root = FVector(A, Ground(A.X, A.Y) + T[4]);
            Snag.Tip = FVector(B, Ground(B.X, B.Y) + T[4]);
            Snag.Radius = T[4];
            Snag.bSunken = true;
            Out.Add(Snag);
        }
        return Out;
    }();
    return Instance;
}

namespace
{
    /** Spots that keep only a narrow window in the lilies for the float (the rest keep a casting lane). */
    bool SpotHasLilyWindow(int32 Index) { return Index < UE_ARRAY_COUNT(GSpotSeeds) && GSpotSeeds[Index].bLilies; }
    /** True where no lily may grow: casting lanes and windows in front of the spots, along the pier. BedCentre widens the window for bed centres. */
    bool LilyBlocked(const FVector2D& P, bool bBedCentre)
    {
        const TArray<FSpot>& List = SpotList();
        for (int32 I = 0; I < List.Num(); ++I)
        {
            const FVector2D F = SpotFrame(List[I], P);
            if (F.Y >= 0) continue;
            if (SpotHasLilyWindow(I) ? FMath::Abs(F.X) < 350 + (bBedCentre ? 200 : 0) && F.Y > -1600
                                     : FMath::Abs(F.X) < List[I].HalfWidth + 300 && F.Y > -3000) return true;
        }
        const FPier& Pr = Pier();
        return SegmentDistance(P, Pr.Root, Pr.Root + Pr.Dir * Pr.Length) < 700;
    }
}

const TArray<FLilyBed>& LakeMap::LilyBeds()
{
    static const TArray<FLilyBed> Instance = []
    {
        TArray<FLilyBed> Out;
        FRandomStream Random(2203 * 7 + 4242);
        const FEllipse Yellow[] = { E(72, -165, 26, 32), E(318, 44, 80, 46, 15) };     // brandy-bottle in the backwater and the upper end
        auto Add = [&](const FVector2D& C, float Radius, float MaxDepth)
        {
            bool bYellow = false;
            for (const FEllipse& Y : Yellow) bYellow |= Y.K(C) < 1;
            Out.Add({ C, Radius, MaxDepth, bYellow });
        };
        // Tidy clumps along the reed edge at the upper end, either side of the window at "Верховье".
        const float Placed[][3] = { { 293, 54, 3.8f }, { 320, 53, 3.8f }, { 279, 54, 3.4f }, { 329, 44, 4.4f }, { 325, 33, 4 }, { 318, 23, 3.2f } };
        for (const auto& B : Placed) Add(At(B[0], B[1]), B[2] * Mt, 180.0f);
        // A belt of clumps along the meadow bank, 4-13 m out, up to 2.3 m deep; windows stay open.
        const TArray<FShorePoint>& Shore = Shoreline();
        const float Banks[][4] = { { -160, 105, 58, 7 }, { -80, 95, 48, 7 }, { -35, 52, 48, 7 }, { 8, 30, 22, 8 } };
        for (const auto& Z : Banks)
        {
            TArray<const FShorePoint*> Points;
            for (const FShorePoint& Sh : Shore) if (FVector2D::Distance(Sh.Location, At(Z[0], Z[1])) < Z[2] * Mt) Points.Add(&Sh);
            TArray<FVector2D> Used;
            for (int32 K = 0; K < Points.Num(); ++K)
            {
                const FShorePoint& Sh = *Points[Random.RandRange(0, Points.Num() - 1)];
                bool bNear = false;
                for (const FVector2D& U : Used) bNear |= FVector2D::Distance(U, Sh.Location) < Z[3] * Mt;
                if (bNear) continue;
                Used.Add(Sh.Location);
                const FVector2D C = Sh.Location - Sh.Outward * Random.FRandRange(400, 1300);
                bool bOverlap = false;
                for (const FLilyBed& B : Out) bOverlap |= FVector2D::Distance(B.Centre, C) < B.Radius + 150;
                if (bOverlap || LilyBlocked(C, true)) continue;
                Add(C, Random.FRandRange(300, 580), 230.0f);
            }
        }
        // Beds scattered through the shallows; the upper end keeps only its tidy clumps.
        const FEllipse Free = E(318, 44, 78, 44, 15);
        const FBox2D Box = LakeBounds();
        for (int32 Try = 0; Try < 40000 && Out.Num() < 200; ++Try)
        {
            const FVector2D P(Random.FRandRange(Box.Min.X, Box.Max.X), Random.FRandRange(Box.Min.Y, Box.Max.Y));
            const float SD = ShoreDistance(P.X, P.Y) / 100, D = Depth(P.X, P.Y) / 100;
            if (SD < 2.5f || SD > 26 || D < 0.45f || D > 1.6f) continue;
            const float Back = BackwaterAmount(P.X, P.Y);
            bool bNearWindow = false;
            const TArray<FSpot>& List = SpotList();
            for (int32 I = 0; I < List.Num(); ++I) bNearWindow |= SpotHasLilyWindow(I) && FVector2D::Distance(List[I].Location, P) < 2200;
            if (!bNearWindow && Back < 0.5f && Random.FRand() > 0.3f) continue;
            if (LilyBlocked(P, true) || Free.K(P) < 1) continue;
            bool bOverlap = false;
            for (const FLilyBed& B : Out) bOverlap |= FVector2D::Distance(B.Centre, P) < B.Radius + 200;
            if (bOverlap) continue;
            Add(P, Back > 0.5f || bNearWindow ? Random.FRandRange(300, 800) : Random.FRandRange(160, 420), 180.0f);
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
bool LakeMap::LilyAllowed(const FVector2D& P) { return !LilyBlocked(P, false); }

LakeMap::FZones LakeMap::ZonesAt(float X, float Y)
{
    FZones Z;
    const float SD = ShoreDistance(X, Y) / 100;
    if (SD <= 0.2f) return Z;
    const FVector2D P(X, Y);
    const float D = Depth(X, Y) / 100;
    // Warm weedy shallows: the backwater, the bay, the upper end, the crucian shallow and the lily beds.
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
    // Snags and sunken trees, the pier piles, the old boat and stakes, the weed beds.
    float Cover = 0;
    for (const FSnag& Snag : Snags())
        Cover = FMath::Max(Cover, 1 - S(150, 500, SegmentDistance(P, FVector2D(Snag.Root), FVector2D(Snag.Tip))));
    const FPier& Pr = Pier();
    Cover = FMath::Max(Cover, 1 - S(150, 450, SegmentDistance(P, Pr.Root, Pr.Root + Pr.Dir * Pr.Length)));
    for (const FCover& C : CoverObjects()) Cover = FMath::Max(Cover, 1 - S(150, 450, SegmentDistance(P, C.A, C.B)));
    Cover = FMath::Max(Cover, 0.6f * WeedAt(P));
    Z.Cover = Cover;
    Z.Deep = S(3.4f, 4.8f, D);
    Z.Open = S(12, 25, SD) * (1 - 0.5f * Z.Deep);
    return Z;
}

bool LakeMap::InPlayArea(float X, float Y)
{
    return TerrainBounds.ExpandBy(-4000).IsInside(FVector2D(X, Y));
}

// --- details of the banks ----------------------------------------------------------------------

namespace
{
    /** A point on the bank nearest to (X, Y) metres, Up cm inland, facing the water. */
    FBankThing OnBank(float X, float Y, float Up)
    {
        const FShorePoint Sh = NearestShore(At(X, Y));
        return { Sh.Location + Sh.Outward * Up, -Sh.Outward };
    }
    /** How far up the bank the footpath runs (cm), wandering a little. */
    float PathLine(const FVector2D& P) { return 700 + 120 * Noise(P + FVector2D(3000, 0), 2500); }
}

TConstArrayView<FBankThing> LakeMap::Willows()
{
    static const TArray<FBankThing> Instance = { OnBank(-72, 92, 300), OnBank(-14, 38, 300), OnBank(-206, 100, 300), OnBank(-298, 24, 300) };
    return Instance;
}
FBankThing LakeMap::OldOak()
{
    // On the meadow a little west of the pier, above the landing.
    static const FBankThing Instance = []
    {
        const FSpot& Spot = SpotList()[1];
        const FVector2D Along(-Spot.Outward.Y, Spot.Outward.X);
        return FBankThing{ Spot.Location + Spot.Outward * 1000 + Along * 900, -Spot.Outward };
    }();
    return Instance;
}
FBankThing LakeMap::OldPine() { static const FBankThing Instance = OnBank(-240, -60, 900); return Instance; }
FBankThing LakeMap::Lookout() { static const FBankThing Instance = OnBank(-226, -50, 600); return Instance; }
FBankThing LakeMap::Spring() { static const FBankThing Instance = OnBank(58, -192, 650); return Instance; }

const TArray<FStream>& LakeMap::Streams()
{
    static const TArray<FStream> Instance = []
    {
        TArray<FStream> Out;
        const FBox2D Land = TerrainBounds.ExpandBy(-500);
        for (const FValley& V : Valleys())
        {
            if (!V.bWater) continue;
            FStream Stream;
            Stream.Width = StreamHalfWidth * 2;
            Stream.bOutflow = V.bOutflow;
            float Acc = 0;
            for (int32 K = 0; K + 1 < V.Points.Num(); ++K)
            {
                const float Segment = FVector2D::Distance(V.Points[K], V.Points[K + 1]);
                for (float D = 0; D < Segment; D += 100)
                {
                    const FVector2D P = V.Points[K] + (V.Points[K + 1] - V.Points[K]) * (D / Segment);
                    if (!Land.IsInside(P)) break;
                    // From the bank outwards: the lake's own water fills the mouth of the channel.
                    if (ShoreDistance(P.X, P.Y) < -100) Stream.Surface.Add(FVector(P, SurfaceZ + StreamLevel(V, (Acc + D) / 100) * 100 - 4));
                }
                Acc += Segment;
            }
            if (Stream.Surface.Num() > 1) Out.Add(MoveTemp(Stream));
        }
        return Out;
    }();
    return Instance;
}
float LakeMap::StreamDistance(const FVector2D& P)
{
    float Best = 1e9f;
    for (const FValley& V : Valleys())
        if (V.bWater)
            for (int32 K = 0; K + 1 < V.Points.Num(); ++K) Best = FMath::Min(Best, SegmentDistance(P, V.Points[K], V.Points[K + 1]));
    return Best;
}
const TArray<FFootbridge>& LakeMap::Footbridges()
{
    static const TArray<FFootbridge> Instance = []
    {
        TArray<FFootbridge> Out;
        for (const FStream& Stream : Streams())
            for (int32 I = 1; I + 1 < Stream.Surface.Num(); ++I)
            {
                const FVector2D P(Stream.Surface[I]);
                if (-ShoreDistance(P.X, P.Y) < PathLine(P)) continue;
                const FVector2D Dir = (FVector2D(Stream.Surface[I + 1]) - FVector2D(Stream.Surface[I - 1])).GetSafeNormal();
                const FVector2D Across(-Dir.Y, Dir.X);
                Out.Add({ P + Across * 170, P - Across * 170 });
                break;
            }
        return Out;
    }();
    return Instance;
}
float LakeMap::PathAmount(const FVector2D& P)
{
    const float SD = ShoreDistance(P.X, P.Y);
    if (SD > -200) return 0;
    return 1 - S(45, 95, FMath::Abs(-SD - PathLine(P)));
}
float LakeMap::BeachAmount(const FVector2D& P)
{
    const float L = -ShoreDistance(P.X, P.Y) / 100;
    if (L < -1 || L > 11) return 0;
    return (1 - S(900, 1300, FVector2D::Distance(P, At(-104, 138)) + 300 * Noise(P + FVector2D(900, 0), 500))) * (1 - S(8.5f, 11, L + 2 * Noise(P, 400)));
}
float LakeMap::WeedAmount(const FVector2D& P) { return WeedAt(P); }
bool LakeMap::InCattails(const FVector2D& P) { static const FEllipse Stand = E(326, 44, 56, 30, 15); return Stand.K(P) < 1; }
bool LakeMap::InSedgeTussocks(const FVector2D& P)
{
    if (FVector2D::Distance(P, At(-262, 64)) > 1800) return false;
    const float SD = ShoreDistance(P.X, P.Y);
    return SD > 40 && SD < 280;
}
TConstArrayView<FVector2D> LakeMap::Stakes()
{
    static const TArray<FVector2D> Instance = []
    {
        TArray<FVector2D> Out;
        const FVector2D First = At(-122, 98), Last = At(-100, 92);
        for (int32 K = 0; K < 7; ++K) Out.Add(First + (Last - First) * (K / 6.0) + FVector2D(40 * FMath::PerlinNoise1D(K * 0.7f), 40 * FMath::PerlinNoise1D(K * 0.7f + 5)));
        return Out;
    }();
    return Instance;
}
TConstArrayView<FBankThing> LakeMap::SwallowBanks()
{
    static const TArray<FBankThing> Instance = { OnBank(-128, -54, 150), OnBank(152, -97, 150) };
    return Instance;
}
FVector2D LakeMap::BirchGrove(float* OutRadius)
{
    if (OutRadius) *OutRadius = 1600;
    return At(168, -128);
}
bool LakeMap::CliffAt(const FVector2D& P) { const FBank Bank = BankAt(P); return Bank.bCliff && Bank.Bluff > 0.9f; }
TConstArrayView<FSoundPlace> LakeMap::SoundPlaces()
{
    static const TArray<FSoundPlace> Instance = {
        { TEXT("A_Brook"), At(-292, 14), 0.5f },      // the outflow's riffle
        { TEXT("A_Brook"), At(360, 49), 0.35f },      // the brook's mouth
        { TEXT("A_Brook"), At(57, -192), 0.25f },     // the spring
        { TEXT("A_Frogs"), At(76, -168), 0.3f },      // the backwater
        { TEXT("A_Frogs"), At(334, 30), 0.3f },       // the upper end
        { TEXT("A_Cuckoo"), At(-180, -170), 0.25f },  // far in the forest
        { TEXT("A_Cuckoo"), At(250, -170), 0.2f },
    };
    return Instance;
}
