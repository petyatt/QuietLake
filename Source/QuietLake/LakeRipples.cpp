#include "LakeRipples.h"

#include "Async/ParallelFor.h"
#include "Tasks/Task.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/PrimitiveComponent.h"
#include "UObject/UObjectIterator.h"
#include "LakeFish.h"
#include "LakeMap.h"
#include "LakeRod.h"
#include "LakeWeather.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"

namespace
{
    constexpr float FixedDt = 1.0f / 60.0f;
    constexpr int32 SpongeCells = 28;        // absorbing band at the window edge
    constexpr float MeanDecay = 0.15f;       // per second: pulls the surface back to rest level

    // The window: 512 cells of 4 cm, about 20 m, centred a few metres ahead of the camera.
    constexpr int32 GridCells = 512;
    constexpr float GridCell = 4.0f;
    constexpr float AheadOfCamera = 650.0f;
    constexpr float RecentreStep = 120.0f;

    // How hard the sources push the surface (per second): tuned on the previews.
    constexpr float FishPush = 12.0f;
    constexpr float FloatPush = 12.0f;
    constexpr float LinePush = 0.5f;
    constexpr float WadePush = 6.0f;
}

// --- the simulation -------------------------------------------------------------------------------

void FLakeRippleSim::Init(int32 InN, float InCell, FVector2D Centre, FIsWater IsWater)
{
    N = InN;
    Cell = InCell;
    H.Init(0.0f, N * N);
    V.Init(0.0f, N * N);
    Corner = FIntPoint(FMath::RoundToInt(Centre.X / Cell) - N / 2, FMath::RoundToInt(Centre.Y / Cell) - N / 2);
    BuildMask(IsWater);
    BuildSponge();
}

void FLakeRippleSim::BuildMask(FIsWater IsWater)
{
    Wet.SetNumUninitialized(N * N);
    const FVector2D O = Origin();
    ParallelFor(N, [&](int32 Y)
    {
        for (int32 X = 0; X < N; ++X)
            Wet[Y * N + X] = IsWater(O.X + (X + 0.5f) * Cell, O.Y + (Y + 0.5f) * Cell) ? 1 : 0;
    });
    for (int32 I = 0; I < N * N; ++I)
        if (!Wet[I]) H[I] = V[I] = 0;
}

void FLakeRippleSim::BuildSponge()
{
    Sponge.SetNumUninitialized(N * N);
    for (int32 Y = 0; Y < N; ++Y)
        for (int32 X = 0; X < N; ++X)
        {
            const int32 Edge = FMath::Min(FMath::Min(X, Y), FMath::Min(N - 1 - X, N - 1 - Y));
            const float T = FMath::Clamp(1.0f - float(Edge) / SpongeCells, 0.0f, 1.0f);
            Sponge[Y * N + X] = 8.0f * T * T;
        }
    // Damping over one fixed step, worked out once.
    Decay.SetNumUninitialized(N * N);
    for (int32 I = 0; I < N * N; ++I) Decay[I] = FMath::Exp(-(Damping + Sponge[I]) * FixedDt);
    DecayDamping = Damping;
}

void FLakeRippleSim::Recentre(FVector2D Centre, FIsWater IsWater)
{
    const FIntPoint Want(FMath::RoundToInt(Centre.X / Cell) - N / 2, FMath::RoundToInt(Centre.Y / Cell) - N / 2);
    const FIntPoint Shift = Want - Corner;
    if (Shift == FIntPoint::ZeroValue) return;
    // Runs every second or so while the viewer walks: no allocations, and the bank is looked up
    // only for the cells that came into the window.
    ShiftH.SetNumUninitialized(N * N);
    ShiftV.SetNumUninitialized(N * N);
    ShiftWet.SetNumUninitialized(N * N);
    const FVector2D O = FVector2D(Want) * Cell;
    ParallelFor(N, [&](int32 Y)
    {
        const int32 SY = Y + Shift.Y;
        for (int32 X = 0; X < N; ++X)
        {
            const int32 SX = X + Shift.X, I = Y * N + X;
            if (SY >= 0 && SY < N && SX >= 0 && SX < N)
            {
                const int32 J = SY * N + SX;
                ShiftH[I] = H[J];
                ShiftV[I] = V[J];
                ShiftWet[I] = Wet[J];
            }
            else
            {
                ShiftH[I] = ShiftV[I] = 0;
                ShiftWet[I] = IsWater(O.X + (X + 0.5f) * Cell, O.Y + (Y + 0.5f) * Cell) ? 1 : 0;
            }
        }
    });
    Swap(H, ShiftH);
    Swap(V, ShiftV);
    Swap(Wet, ShiftWet);
    Corner = Want;
}

void FLakeRippleSim::Advance(float Dt)
{
    Pending = FMath::Min(Pending + Dt, 4 * FixedDt);
    while (Pending >= FixedDt)
    {
        Step(FixedDt);
        Pending -= FixedDt;
    }
}

void FLakeRippleSim::Step(float Dt)
{
    if (N == 0) return;
    // Semi-implicit Euler: the rate from the curvature, then the height from the rate. Stable while
    // WaveSpeed * Dt < Cell / sqrt(2); here 32 cm/s * 1/60 s = 0.5 cm against 4 cm cells.
    const float K = WaveSpeed * WaveSpeed * Dt / (Cell * Cell);
    const float KV = Viscosity * Dt / (Cell * Cell);           // stable while below 0.25
    Scratch.SetNumUninitialized(N * N);
    const bool bFixed = Dt == FixedDt && DecayDamping == Damping && Decay.Num() == N * N;
    ParallelFor(N, [&](int32 Y)
    {
        for (int32 X = 0; X < N; ++X)
        {
            const int32 I = Y * N + X;
            if (!Wet[I]) { Scratch[I] = 0; continue; }
            const float LapH = (X > 0 ? H[I - 1] : 0.0f) + (X < N - 1 ? H[I + 1] : 0.0f) + (Y > 0 ? H[I - N] : 0.0f) + (Y < N - 1 ? H[I + N] : 0.0f) - 4 * H[I];
            const float LapV = (X > 0 ? V[I - 1] : 0.0f) + (X < N - 1 ? V[I + 1] : 0.0f) + (Y > 0 ? V[I - N] : 0.0f) + (Y < N - 1 ? V[I + N] : 0.0f) - 4 * V[I];
            const float Keep = bFixed ? Decay[I] : FMath::Exp(-(Damping + Sponge[I]) * Dt);
            Scratch[I] = (V[I] + K * LapH + KV * LapV) * Keep;
        }
    });
    Swap(V, Scratch);
    const float Rest = 1.0f - MeanDecay * Dt;
    ParallelFor(N, [&](int32 Y)
    {
        for (int32 X = 0; X < N; ++X)
        {
            const int32 I = Y * N + X;
            H[I] = Wet[I] ? (H[I] + V[I] * Dt) * Rest : 0.0f;
        }
    });
}

void FLakeRippleSim::Push(FVector2D At, float Radius, float Amount)
{
    if (N == 0) return;
    Radius = FMath::Max(Radius, Cell * 0.75f);
    const FVector2D Local = (At - Origin()) / Cell - FVector2D(0.5f);
    const float R = Radius / Cell;
    const int32 X0 = FMath::Max(0, FMath::FloorToInt(Local.X - 2 * R)), X1 = FMath::Min(N - 1, FMath::CeilToInt(Local.X + 2 * R));
    const int32 Y0 = FMath::Max(0, FMath::FloorToInt(Local.Y - 2 * R)), Y1 = FMath::Min(N - 1, FMath::CeilToInt(Local.Y + 2 * R));
    const float Inv = 1.0f / (R * R);
    for (int32 Y = Y0; Y <= Y1; ++Y)
        for (int32 X = X0; X <= X1; ++X)
        {
            const int32 I = Y * N + X;
            if (!Wet[I]) continue;
            const float D2 = FMath::Square(X - Local.X) + FMath::Square(Y - Local.Y);
            V[I] += Amount * FMath::Exp(-D2 * Inv);
        }
}

void FLakeRippleSim::Drop(FVector2D At, float Radius, float Amount)
{
    // A gaussian holds Amount * Radius^2 of volume: the ring of twice the radius at a quarter of
    // the strength gives it back.
    Radius = FMath::Max(Radius, Cell * 0.75f);
    Push(At, Radius, -Amount);
    Push(At, 2 * Radius, Amount * 0.25f);
}

float FLakeRippleSim::HeightAt(FVector2D P) const
{
    if (N == 0) return 0;
    const FVector2D Local = (P - Origin()) / Cell - FVector2D(0.5f);
    const int32 X = FMath::FloorToInt(Local.X), Y = FMath::FloorToInt(Local.Y);
    if (X < 0 || Y < 0 || X >= N - 1 || Y >= N - 1) return 0;
    const float TX = Local.X - X, TY = Local.Y - Y;
    const float* Row = &H[Y * N + X];
    return FMath::Lerp(FMath::Lerp(Row[0], Row[1], TX), FMath::Lerp(Row[N], Row[N + 1], TX), TY);
}

bool FLakeRippleSim::Contains(FVector2D P, float Margin) const
{
    const FVector2D Local = P - Origin();
    return Local.X >= Margin && Local.Y >= Margin && Local.X <= Size() - Margin && Local.Y <= Size() - Margin;
}

float FLakeRippleSim::Energy() const
{
    double E = 0;
    for (int32 I = 0; I < H.Num(); ++I) E += double(H[I]) * H[I];
    return float(E);
}

// --- shown on the lake ----------------------------------------------------------------------------

ALakeRipples::ALakeRipples()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;   // after the fish and the rod have moved
    bReplicates = false;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

ALakeRipples* ALakeRipples::Get(const UWorld* World)
{
    if (World)
        for (TActorIterator<ALakeRipples> It(const_cast<UWorld*>(World)); It; ++It)
            return *It;
    return nullptr;
}

static bool RippleWater(float X, float Y) { return LakeMap::ShoreDistance(X, Y) > 0; }

void ALakeRipples::BeginPlay()
{
    Super::BeginPlay();
    Random.Initialize(4242);
    FVector Eye, Forward;
    const FVector2D Centre = ViewPoint(Eye, Forward) ? FVector2D(Eye + Forward * AheadOfCamera) : LakeMap::StartSpot();
    // The grid (a quarter of a million cells and the bank under them) is laid out on a worker; the
    // waves start once it is ready.
    Stepping = UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Centre] { Simulation.Init(GridCells, GridCell, Centre, RippleWater); });
    Texture = UTexture2D::CreateTransient(GridCells, GridCells, PF_R16F);
    Texture->SRGB = false;
    Texture->Filter = TF_Bilinear;
    Texture->AddressX = Texture->AddressY = TA_Clamp;
    Texture->UpdateResource();
    FindWater();
}

bool ALakeRipples::ViewPoint(FVector& OutLocation, FVector& OutForward) const
{
    const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (!PC || !PC->PlayerCameraManager) return false;
    OutLocation = PC->PlayerCameraManager->GetCameraLocation();
    OutForward = PC->PlayerCameraManager->GetCameraRotation().Vector();
    OutForward.Z = 0;
    OutForward = OutForward.GetSafeNormal();
    return true;
}

void ALakeRipples::FindWater()
{
    Materials.Reset();
    for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
    {
        if (It->GetWorld() != GetWorld() || It->GetNumMaterials() == 0) continue;
        UMaterialInterface* Material = It->GetMaterial(0);
        // The water itself, and the water lilies (M_Plant), which ride the waves in their material.
        const FString Base = Material && Material->GetBaseMaterial() ? Material->GetBaseMaterial()->GetName() : FString();
        if (Base != TEXT("M_LakeSurface") && Base != TEXT("M_Plant")) continue;
        UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Material);
        if (!Dynamic) Dynamic = It->CreateDynamicMaterialInstance(0, Material);
        Dynamic->SetTextureParameterValue(TEXT("Ripples"), Texture);
        Materials.Add(Dynamic);
    }
}

void ALakeRipples::FinishStep() const
{
    if (Stepping.IsValid())
    {
        Stepping.Wait();
        Stepping = UE::Tasks::FTask();
    }
}

void ALakeRipples::EndPlay(const EEndPlayReason::Type Reason)
{
    FinishStep();
    delete[] Ready;
    Ready = nullptr;
    Super::EndPlay(Reason);
}

void ALakeRipples::Splash(FVector At, float Radius, float Amount, float Delay)
{
    Queue.Add({ FVector2D(At), Radius, Amount, float(GetWorld()->GetTimeSeconds() + Delay) });
}

bool ALakeRipples::Covers(FVector P, float Margin) const
{
    const FVector2D Local = FVector2D(P) - ShownOrigin;
    return ShownSize > 0 && Local.X >= Margin && Local.Y >= Margin && Local.X <= ShownSize - Margin && Local.Y <= ShownSize - Margin;
}

void ALakeRipples::ApplyQueue()
{
    const float Now = GetWorld()->GetTimeSeconds();
    for (int32 I = Queue.Num() - 1; I >= 0; --I)
        if (Queue[I].Time <= Now)
        {
            Simulation.Drop(Queue[I].At, Queue[I].Radius, Queue[I].Amount);
            Queue.RemoveAtSwap(I);
            ++Applied;
        }
}

void ALakeRipples::AddFishSources(float DeltaSeconds)
{
    // A fish displaces the water above it: a bulge ahead of its head, a trough along its back.
    // Close under the surface it shows; from about half a metre down it leaves nothing.
    for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
    {
        ALakeFish* Fish = *It;
        const FVector P = Fish->GetActorLocation();
        if (!Simulation.Contains(FVector2D(P), 2 * GridCell)) { LastSeen.Remove(Fish); continue; }
        FVector& Last = LastSeen.FindOrAdd(Fish, P);
        const FVector Prev = Last;
        const FVector Move = P - Prev;
        Last = P;
        const float Length = LakeFish::LengthCm(Fish->Species, Fish->WeightKg);
        const float Size = FMath::Sqrt(Length / 20.0f);
        if (Fish->IsLifted())
        {
            // Drawn out on the line: a splash where it leaves the water, then it drips while it hangs
            // over it (each drop lands a moment later, as long as it takes to fall).
            const float Above = P.Z - LakeFish::SurfaceZ;
            if (Prev.Z < LakeFish::SurfaceZ + 3 && P.Z >= LakeFish::SurfaceZ + 3)
                Splash(P, FMath::Max(0.2f * Length, 3.0f), 90.0f * Size);
            if (Above > 0 && Above < 250 && Random.FRand() < DeltaSeconds * 6.0f * Size)
                Splash(P + FVector(Random.FRandRange(-0.2f, 0.2f) * Length, Random.FRandRange(-3, 3), 0), 1.0f,
                    Random.FRandRange(10.0f, 22.0f), FMath::Sqrt(2 * Above / 980.0f));
            continue;
        }
        const float Top = LakeFish::SurfaceZ - (P.Z + 0.12f * Length);        // water above its back
        if (Top > 50) continue;
        const float Near = FMath::Exp(-FMath::Max(Top, 0.0f) / 12.0f);
        const float Speed = FMath::Min(Move.Size2D() / FMath::Max(DeltaSeconds, 1e-3f), 400.0f);
        // On the hook near the top it rolls and beats its tail: boils and slaps.
        const bool bHooked = Fish->IsHooked();
        if (bHooked && Top < 15 && Random.FRand() < DeltaSeconds * 3.0f)
            Splash(P + FVector(Random.FRandRange(-0.3f, 0.3f) * Length, Random.FRandRange(-0.15f, 0.15f) * Length, 0),
                FMath::Max(0.15f * Length, 2.5f), Random.FRand() < 0.5f ? 70.0f * Size * Near : -60.0f * Size * Near);
        if (Speed < 1) continue;
        const FVector2D Dir = FVector2D(Move).GetSafeNormal();
        const float Width = FMath::Max(0.18f * Length, 2.5f);
        // Push as an acceleration of the surface (cm/s^2) over this frame.
        const float Strength = Near * Speed * Size * FishPush * (bHooked ? 2.0f : 1.0f) * DeltaSeconds;
        Simulation.Push(FVector2D(P) + Dir * 0.35f * Length, Width, Strength);
        Simulation.Push(FVector2D(P) - Dir * 0.1f * Length, Width * 1.3f, -Strength / 1.69f);   // same volume
    }
}

void ALakeRipples::AddLineSources(float DeltaSeconds)
{
    // The line, the shot and the hook: a tiny plop wherever they come down onto the water, and a
    // faint trail where the line lying on the water is drawn across it.
    for (TActorIterator<ALakeRod> It(GetWorld()); It; ++It)
    {
        ALakeRod* Rod = *It;
        TArray<FVector> Now = Rod->MainLinePoints();
        const int32 LinePoints = Now.Num();
        Now.Add(Rod->ShotLocation());
        Now.Add(Rod->HookLocation());
        TArray<FVector>& Last = LastLine.FindOrAdd(Rod);
        if (Last.Num() != Now.Num()) { Last = Now; continue; }
        for (int32 I = 0; I < Now.Num(); ++I)
        {
            const FVector P = Now[I], L = Last[I];
            if (!Simulation.Contains(FVector2D(P), 2 * GridCell)) continue;
            const FVector Vel = (P - L) / FMath::Max(DeltaSeconds, 1e-3f);
            const bool bRig = I >= LinePoints;
            if (L.Z > LakeFish::SurfaceZ + 0.5f && P.Z <= LakeFish::SurfaceZ + 0.5f)
            {
                const float Down = FMath::Max(-Vel.Z, 0.0f);
                Simulation.Drop(FVector2D(P), bRig ? 1.3f : 1.0f, FMath::Clamp(Down * (bRig ? 0.25f : 0.12f), bRig ? 6.0f : 2.0f, bRig ? 50.0f : 20.0f));
            }
            else if (!bRig && FMath::Abs(P.Z - LakeFish::SurfaceZ) < 1.5f)
            {
                const float Slide = FMath::Min(FVector2D(Vel).Size(), 300.0f);
                if (Slide > 10) Simulation.Push(FVector2D(P), 1.2f, (Slide - 10) * LinePush * DeltaSeconds);
            }
        }
        Last = MoveTemp(Now);
    }
}

void ALakeRipples::AddWaders(float DeltaSeconds)
{
    // An angler standing in the shallows: each leg pushes a bow wave as he walks, and every step
    // lifts a foot out and puts it back in.
    const float Now = GetWorld()->GetTimeSeconds();
    for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
    {
        ACharacter* Angler = *It;
        const UCapsuleComponent* Capsule = Angler->GetCapsuleComponent();
        if (!Capsule) continue;
        const FVector Feet = Angler->GetActorLocation() - FVector(0, 0, Capsule->GetScaledCapsuleHalfHeight());
        if (Feet.Z > LakeFish::SurfaceZ - 2 || !Simulation.Contains(FVector2D(Feet), 2 * GridCell)) continue;
        if (LakeMap::ShoreDistance(Feet.X, Feet.Y) <= 0) continue;
        const FVector2D Vel(Angler->GetVelocity());
        const float Speed = FMath::Min(Vel.Size(), 400.0f);
        const FVector2D Facing = FVector2D(Angler->GetActorForwardVector()).GetSafeNormal();
        const FVector2D Side(-Facing.Y, Facing.X);
        const float Deep = FMath::Clamp((LakeFish::SurfaceZ - Feet.Z) / 40.0f, 0.2f, 1.0f);
        if (Speed > 5)
        {
            const FVector2D Dir = Vel / Speed;
            const float Strength = Speed * Deep * WadePush * DeltaSeconds;
            for (int32 Leg = -1; Leg <= 1; Leg += 2)
            {
                const FVector2D At = FVector2D(Feet) + Side * 11.0f * Leg;
                Simulation.Push(At + Dir * 6.0f, 5.0f, Strength);
                Simulation.Push(At - Dir * 4.0f, 6.5f, -Strength / 1.69f);
            }
        }
        float& Next = NextStep.FindOrAdd(Angler, Now);
        if (Speed > 20 && Now >= Next)
        {
            int32& Foot = StepFoot.FindOrAdd(Angler, 1);
            Foot = -Foot;
            Splash(FVector(FVector2D(Feet) + Side * 11.0f * Foot + Vel / Speed * 20.0f, 0), 5.0f, 45.0f * Deep);
            Next = Now + FMath::Clamp(55.0f / Speed, 0.3f, 0.8f);
        }
    }
}

void ALakeRipples::AddFloatSources(float DeltaSeconds)
{
    // The float's body moves water as it rises and sinks: every bob sends out a ring.
    for (TActorIterator<ALakeRod> It(GetWorld()); It; ++It)
    {
        ALakeRod* Rod = *It;
        const FVector K = Rod->FloatKeel(), T = Rod->FloatTip();
        if (!Simulation.Contains(FVector2D(K), 2 * GridCell)) { LastSeen.Remove(Rod); continue; }
        FVector& Last = LastSeen.FindOrAdd(Rod, K);
        const FVector Move = K - Last;
        Last = K;
        const bool bAtSurface = FMath::Min(K.Z, T.Z) < LakeFish::SurfaceZ + 2 && FMath::Max(K.Z, T.Z) > LakeFish::SurfaceZ - 15;
        if (!bAtSurface) continue;
        const FVector Vel = Move / FMath::Max(DeltaSeconds, 1e-3f);
        const FVector2D At = FVector2D(FMath::Lerp(K, T, 0.3f));
        // Only its motion against the water makes waves: riding a wave, it moves with the water.
        const float Water = ShownHeightAt(At);
        FloatWave = FMath::Max(FloatWave, FMath::Abs(Water));
        float& LastW = LastWater.FindOrAdd(Rod, Water);
        const float WaterRise = (Water - LastW) / FMath::Max(DeltaSeconds, 1e-3f);
        LastW = Water;
        const float Rise = FMath::Clamp(Vel.Z - WaterRise, -300.0f, 300.0f);
        if (FMath::Abs(Rise) > 0.3f) Simulation.Push(At, 1.6f, Rise * FloatPush * DeltaSeconds);
        // Dragged along (a fish leading the bait away, the wind): a small bow wave.
        const FVector2D Slide(Vel);
        if (Slide.Size() > 6) Simulation.Push(At + Slide.GetSafeNormal() * 1.5f, 1.4f, FMath::Min(Slide.Size() - 6, 200.0f) * 0.15f * FloatPush * DeltaSeconds);
    }
}

void ALakeRipples::AddRain(float DeltaSeconds)
{
    const ALakeWeather* Weather = ALakeWeather::Get(GetWorld());
    const float Rain = Weather ? Weather->Current().Rain : 0.0f;
    if (Rain < 0.02f) return;
    // Drizzle: a drop now and then on every square metre; a downpour: a few per second.
    const float Area = FMath::Square(Simulation.Size() / 100.0f);
    const float Expected = Area * DeltaSeconds * 2.5f * FMath::Pow(Rain, 1.5f);
    int32 Count = FMath::FloorToInt(Expected) + (Random.FRand() < FMath::Frac(Expected) ? 1 : 0);
    const FVector2D O = Simulation.Origin();
    while (Count-- > 0)
    {
        const FVector2D P = O + FVector2D(Random.FRand(), Random.FRand()) * Simulation.Size();
        Simulation.Drop(P, Random.FRandRange(0.9f, 1.6f), Random.FRandRange(15.0f, 35.0f));
    }
}

void ALakeRipples::Upload()
{
    // The heights were packed to half floats by the last background step (PackHeights).
    if (!Ready) return;
    FFloat16* Data = Ready;
    Ready = nullptr;
    if (!Texture || !Texture->GetResource())
    {
        delete[] Data;
        return;
    }
    auto* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Simulation.N, Simulation.N);
    Texture->UpdateTextureRegions(0, 1, Region, Simulation.N * sizeof(FFloat16), sizeof(FFloat16), (uint8*)Data,
        [](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
        {
            delete[] reinterpret_cast<FFloat16*>(SrcData);
            delete Regions;
        });
    // Where the window lay for those heights (cm) and one texel in UV, for the water material.
    const FLinearColor Window(ReadyOrigin.X, ReadyOrigin.Y, Simulation.Size(), 1.0f / Simulation.N);
    for (UMaterialInstanceDynamic* Material : Materials)
        if (Material) Material->SetVectorParameterValue(TEXT("RippleWindow"), Window);
}

void ALakeRipples::PackHeights()
{
    const int32 Count = Simulation.N * Simulation.N;
    FFloat16* Data = new FFloat16[Count];
    ParallelFor(Simulation.N, [&](int32 Y)
    {
        const int32 Row = Y * Simulation.N;
        for (int32 X = 0; X < Simulation.N; ++X) Data[Row + X] = FFloat16(Simulation.H[Row + X]);
    });
    delete[] Ready;
    Ready = Data;
    ReadyOrigin = Simulation.Origin();
    BackH = Simulation.H;
    BackOrigin = Simulation.Origin();
    bBackReady = true;
}

float ALakeRipples::ShownHeightAt(FVector2D P) const
{
    const int32 N = Simulation.N;
    if (N == 0 || FrontH.Num() != N * N) return 0;
    const float Cell = Simulation.Cell;
    const FVector2D Local = (P - FrontOrigin) / Cell - FVector2D(0.5f);
    const int32 X = FMath::FloorToInt(Local.X), Y = FMath::FloorToInt(Local.Y);
    if (X < 0 || Y < 0 || X >= N - 1 || Y >= N - 1) return 0;
    const float TX = Local.X - X, TY = Local.Y - Y;
    const float* Row = &FrontH[Y * N + X];
    // Fades out towards the window edge, as in the water material.
    const float Edge = FMath::Clamp(FMath::Min(FMath::Min(X, Y), FMath::Min(N - 1 - X, N - 1 - Y)) / (N / 14.0f), 0.0f, 1.0f);
    return Edge * FMath::Lerp(FMath::Lerp(Row[0], Row[1], TX), FMath::Lerp(Row[N], Row[N + 1], TX), TY);
}

FVector2D ALakeRipples::ShownSlopeAt(FVector2D P) const
{
    const float D = Simulation.Cell;
    return FVector2D(ShownHeightAt(P + FVector2D(D, 0)) - ShownHeightAt(P - FVector2D(D, 0)),
                     ShownHeightAt(P + FVector2D(0, D)) - ShownHeightAt(P - FVector2D(0, D))) / (2 * D);
}

void ALakeRipples::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    // Never hold the frame for the waves: if the worker has not finished (the machine is busy,
    // e.g. while loading), skip this frame and step the time over on the next one.
    if (Stepping.IsValid() && !Stepping.IsCompleted())
    {
        Skipped = FMath::Min(Skipped + DeltaSeconds, 0.1f);
        ++SkippedFrames;
        return;
    }
    DeltaSeconds += Skipped;
    Skipped = 0;
    if (Simulation.N == 0) return;
    const double Start = FPlatformTime::Seconds();
    FinishStep();
    if (bBackReady)
    {
        Swap(FrontH, BackH);
        FrontOrigin = BackOrigin;
        bBackReady = false;
    }
    const double Waited = FPlatformTime::Seconds();
    WaitSeconds += Waited - Start;
    // Where the window should be; it moves on the worker, after this frame's pushes.
    FVector Eye, Forward;
    bool bMove = false;
    FVector2D Want = FVector2D::ZeroVector;
    if (ViewPoint(Eye, Forward))
    {
        Want = FVector2D(Eye + Forward * AheadOfCamera);
        const FVector2D Mid = Simulation.Origin() + FVector2D(Simulation.Size() * 0.5f);
        bMove = FVector2D::Distance(Want, Mid) > RecentreStep;
    }
    // The water is built with the world; until it is there, look for it once a second (looking
    // goes through every component in the world).
    if (Materials.Num() == 0 && GetWorld()->GetTimeSeconds() >= NextWaterSearch)
    {
        NextWaterSearch = GetWorld()->GetTimeSeconds() + 1.0f;
        FindWater();
    }
    ShownOrigin = Simulation.Origin();
    ShownSize = Simulation.Size();
    ApplyQueue();
    AddFishSources(DeltaSeconds);
    AddFloatSources(DeltaSeconds);
    AddLineSources(DeltaSeconds);
    AddWaders(DeltaSeconds);
    AddRain(DeltaSeconds);
    // Show the last finished step, then compute the next one in the background.
    const double BeforeUpload = FPlatformTime::Seconds();
    SourceSeconds += BeforeUpload - Waited;
    Upload();
    UploadSeconds += FPlatformTime::Seconds() - BeforeUpload;
    Stepping = UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, DeltaSeconds, bMove, Want]
    {
        if (bMove) Simulation.Recentre(Want, RippleWater);
        Simulation.Advance(DeltaSeconds);
        PackHeights();
    });
    const double Spent = FPlatformTime::Seconds() - Start;
    StepSeconds += Spent;
    StepWorst = FMath::Max(StepWorst, Spent);
    ++StepFrames;
    const float Now = GetWorld()->GetTimeSeconds();
    if (Now >= NextLog)
    {
        if (StepFrames > 0)
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_RIPPLES game thread %.2f ms per frame (worst %.2f; waiting %.2f, sources %.2f, upload %.2f), %d splashes, %d frames skipped, water under a float up to %.1f mm, window %.0f m at (%.0f, %.0f), %d water and lily materials"),
                StepSeconds / StepFrames * 1000.0, StepWorst * 1000.0, WaitSeconds / StepFrames * 1000.0, SourceSeconds / StepFrames * 1000.0, UploadSeconds / StepFrames * 1000.0, Applied, SkippedFrames, FloatWave * 10,
                Simulation.Size() / 100, Simulation.Origin().X, Simulation.Origin().Y, Materials.Num());
        WaitSeconds = SourceSeconds = UploadSeconds = 0;
        Applied = 0;
        SkippedFrames = 0;
        FloatWave = 0;
        StepSeconds = 0;
        StepWorst = 0;
        StepFrames = 0;
        NextLog = Now + 10;
    }
}
