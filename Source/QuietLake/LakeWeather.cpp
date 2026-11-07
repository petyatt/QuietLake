#include "LakeWeather.h"
#include "LakeGame.h"
#include "LakeMap.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Sound/SoundWave.h"
#include "Kismet/GameplayStatics.h"
#include "DynamicWindParameters.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "TimerManager.h"
#include "Camera/CameraActor.h"

namespace
{
    float S(float A, float B, float X) { return FMath::SmoothStep(A, B, X); }

    /** Typical weathers with ranges for each value; the server rolls one and blends towards it. */
    struct FPreset { const TCHAR* Name; float Weight; FVector2f Cloud, Fog, Rain, Wind; bool bFoggy, bRainy; };
    const FPreset GPresets[] = {
        { TEXT("Clear calm"),   1.2f, { 0.0f, 0.25f }, { 0.0f, 0.15f }, { 0, 0 },          { 0.0f, 0.12f }, false, false },
        { TEXT("Clear breezy"), 1.0f, { 0.1f, 0.35f }, { 0.0f, 0.05f }, { 0, 0 },          { 0.25f, 0.5f }, false, false },
        { TEXT("Cloudy"),       1.2f, { 0.45f, 0.7f }, { 0.0f, 0.2f },  { 0, 0 },          { 0.15f, 0.45f }, false, false },
        { TEXT("Overcast"),     0.9f, { 0.8f, 1.0f },  { 0.1f, 0.3f },  { 0, 0 },          { 0.1f, 0.4f }, false, false },
        { TEXT("Windy"),        0.5f, { 0.3f, 0.6f },  { 0.0f, 0.1f },  { 0, 0 },          { 0.6f, 0.9f }, false, false },
        { TEXT("Mist"),         0.8f, { 0.2f, 0.6f },  { 0.55f, 0.8f }, { 0, 0 },          { 0.0f, 0.1f }, true, false },
        { TEXT("Dense fog"),    0.4f, { 0.5f, 0.8f },  { 0.85f, 1.0f }, { 0, 0 },          { 0.0f, 0.06f }, true, false },
        { TEXT("Drizzle"),      0.7f, { 0.85f, 1.0f }, { 0.25f, 0.45f },{ 0.1f, 0.3f },   { 0.05f, 0.3f }, false, true },
        { TEXT("Rain"),         0.6f, { 0.9f, 1.0f },  { 0.2f, 0.35f }, { 0.35f, 0.65f }, { 0.2f, 0.5f }, false, true },
        { TEXT("Downpour"),     0.3f, { 1.0f, 1.0f },  { 0.25f, 0.4f }, { 0.75f, 1.0f },  { 0.5f, 0.9f }, false, true },
    };
}

FLakeWeatherState FLakeWeatherState::Blend(const FLakeWeatherState& A, const FLakeWeatherState& B, float T)
{
    FLakeWeatherState R;
    R.Cloud = FMath::Lerp(A.Cloud, B.Cloud, T);
    R.Fog = FMath::Lerp(A.Fog, B.Fog, T);
    R.Rain = FMath::Lerp(A.Rain, B.Rain, T);
    R.Wind = FMath::Lerp(A.Wind, B.Wind, T);
    R.WindYaw = A.WindYaw + FMath::FindDeltaAngleDegrees(A.WindYaw, B.WindYaw) * T;
    return R;
}

ALakeWeather::ALakeWeather()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;   // after the camera has moved
    bReplicates = true;
    bAlwaysRelevant = true;
    SetNetUpdateFrequency(2);
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ALakeWeather::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ALakeWeather, ClockOriginServer);
    DOREPLIFETIME(ALakeWeather, ClockOriginHours);
    DOREPLIFETIME(ALakeWeather, From);
    DOREPLIFETIME(ALakeWeather, To);
    DOREPLIFETIME(ALakeWeather, BlendStart);
    DOREPLIFETIME(ALakeWeather, BlendSeconds);
    DOREPLIFETIME(ALakeWeather, WaterTemp);
    DOREPLIFETIME(ALakeWeather, Pressure);
    DOREPLIFETIME(ALakeWeather, Trend);
    DOREPLIFETIME(ALakeWeather, ChangeAt);
    DOREPLIFETIME(ALakeWeather, SkyAtChange);
}

ALakeWeather* ALakeWeather::Get(const UWorld* World)
{
    if (!World) return nullptr;
    for (TActorIterator<ALakeWeather> It(const_cast<UWorld*>(World)); It; ++It) return *It;
    return nullptr;
}

float ALakeWeather::ServerNow() const
{
    const AGameStateBase* State = GetWorld() ? GetWorld()->GetGameState() : nullptr;
    return State ? State->GetServerWorldTimeSeconds() : (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f);
}

float ALakeWeather::Hours() const
{
    const float H = ClockOriginHours + (ServerNow() - ClockOriginServer) / (DayMinutes * 60.0f) * 24.0f;
    return FMath::Fmod(FMath::Fmod(H, 24.0f) + 24.0f, 24.0f);
}

FLakeWeatherState ALakeWeather::Current() const
{
    const float T = FMath::Clamp((ServerNow() - BlendStart) / FMath::Max(0.01f, BlendSeconds), 0.0f, 1.0f);
    return FLakeWeatherState::Blend(From, To, FMath::SmoothStep(0.0f, 1.0f, T));
}

FVector ALakeWeather::SunDirection(float* OutElevation) const
{
    // Late summer at about 56 degrees north; solar noon at 13:00 local time.
    const float Lat = FMath::DegreesToRadians(56.0f), Dec = FMath::DegreesToRadians(17.0f);
    const float HourAngle = FMath::DegreesToRadians((Hours() - 13.0f) * 15.0f);
    const float SinE = FMath::Sin(Lat) * FMath::Sin(Dec) + FMath::Cos(Lat) * FMath::Cos(Dec) * FMath::Cos(HourAngle);
    const float E = FMath::Asin(FMath::Clamp(SinE, -1.0f, 1.0f));
    // Azimuth from north, clockwise.
    const float A = FMath::Atan2(FMath::Sin(HourAngle), FMath::Cos(HourAngle) * FMath::Sin(Lat) - FMath::Tan(Dec) * FMath::Cos(Lat)) + PI;
    if (OutElevation) *OutElevation = FMath::RadiansToDegrees(E);
    // World: +X east, +Y south (Unreal's yaw turns clockwise seen from above).
    return FVector(FMath::Cos(E) * FMath::Sin(A), -FMath::Cos(E) * FMath::Cos(A), FMath::Sin(E));
}

float ALakeWeather::Daylight() const
{
    float E;
    SunDirection(&E);
    return S(-6.0f, 4.0f, E);
}

float ALakeWeather::EffectiveFog() const
{
    const FLakeWeatherState W = Current();
    const float H = Hours();
    // Mist lies on the water on calm mornings and, less, after sunset.
    const float Morning = S(3.0f, 4.5f, H) * (1 - S(7.5f, 9.5f, H));
    const float Evening = S(20.5f, 22.5f, H) * 0.5f + (H < 3.0f ? 0.5f : 0.0f);
    return FMath::Clamp(W.Fog + (0.45f * Morning + 0.25f * Evening) * (1 - S(0.2f, 0.5f, W.Wind)) * (1 - W.Rain), 0.0f, 1.0f);
}

float ALakeWeather::TemperatureC() const
{
    const FLakeWeatherState W = Current();
    const float Day = Daylight();
    // Coolest before dawn, warmest mid-afternoon; rain and wind chill, clouds keep the night warm.
    float T = 17.0f + 6.0f * FMath::Cos(2 * PI * (Hours() - 15.0f) / 24.0f);
    T += -4.0f * W.Rain - 1.5f * W.Wind - 2.5f * W.Cloud * Day + 1.5f * W.Cloud * (1 - Day);
    return T;
}

FString ALakeWeather::Describe() const
{
    const FLakeWeatherState W = Current();
    const float FogNow = EffectiveFog();
    FString Sky;
    if (W.Rain > 0.75f) Sky = TEXT("Ливень");
    else if (W.Rain > 0.35f) Sky = TEXT("Дождь");
    else if (W.Rain > 0.08f) Sky = TEXT("Морось");
    else if (FogNow > 0.85f) Sky = TEXT("Густой туман");
    else if (FogNow > 0.55f) Sky = TEXT("Туман");
    else if (FogNow > 0.32f) Sky = TEXT("Дымка");
    else if (W.Cloud > 0.8f) Sky = TEXT("Пасмурно");
    else if (W.Cloud > 0.45f) Sky = TEXT("Облачно");
    else Sky = Daylight() > 0.3f ? TEXT("Ясно") : TEXT("Ясная ночь");
    const int32 Ms = FMath::RoundToInt(WindSpeed());
    const FString WindText = Ms < 1 ? TEXT("штиль") : FString::Printf(TEXT("ветер %d м/с"), Ms);
    return Sky + TEXT("  ·  ") + WindText;
}

void ALakeWeather::SetHours(float NewHours)
{
    ClockOriginServer = ServerNow();
    ClockOriginHours = NewHours;
    ForceNetUpdate();
}

float ALakeWeather::Shock() const
{
    // How big the last change was: rain and wind count most.
    const float Size = FMath::Clamp(FMath::Abs(To.Cloud - From.Cloud) * 0.6f + FMath::Abs(To.Rain - From.Rain) * 1.2f
        + FMath::Abs(To.Wind - From.Wind) * 0.8f + FMath::Abs(To.Fog - From.Fog) * 0.3f, 0.0f, 1.0f);
    const float Now = ServerNow();
    if (Now < BlendStart) return 0;
    const float During = FMath::Clamp((Now - BlendStart) / FMath::Max(0.01f, BlendSeconds), 0.0f, 1.0f);
    const float AfterHours = (Now - BlendStart - BlendSeconds) * GameHoursPerSecond();
    return Size * (AfterHours < 0 ? During : 1 - S(0.0f, 3.0f, AfterHours));
}

float ALakeWeather::StableHours() const
{
    return FMath::Max(0.0f, (ServerNow() - BlendStart - BlendSeconds) * GameHoursPerSecond());
}

FFishConditions ALakeWeather::FishConditions() const
{
    const FLakeWeatherState W = Current();
    FFishConditions C;
    SunDirection(&C.SunElevation);
    C.bMorning = Hours() < 13.0f;
    C.Hour = Hours();
    C.WaterTempC = WaterTemp;
    C.Cloud = W.Cloud;
    C.Rain = W.Rain;
    C.Wind = W.Wind;
    C.Fog = EffectiveFog();
    C.PressureTrend = Trend;
    C.Shock = Shock();
    C.StableHours = StableHours();
    const FVector Dir = FRotator(0, W.WindYaw, 0).Vector();
    C.WindDir = FVector2D(Dir.X, Dir.Y);
    return C;
}

FVector ALakeWeather::WindVelocity() const
{
    const float T = ServerNow();
    const float Gust = 1 + 0.3f * FMath::Sin(T * 0.37f) * FMath::Sin(T * 1.13f + 1.0f);
    return FRotator(0, Current().WindYaw, 0).Vector() * WindSpeed() * 100 * Gust;
}

namespace
{
    /** How fast the clouds sail (km/s, x y) and reshape (z) in a weather. Up there the wind is
     *  stronger than by the water: 3 m/s in a calm, ~18 m/s in a gale. The noise is sampled against
     *  the drift, so the clouds go downwind. */
    FVector SkyVelocity(const FLakeWeatherState& W)
    {
        const FVector Dir = FRotator(0, W.WindYaw, 0).Vector();
        const float Speed = (3.0f + 15.0f * W.Wind) / 1000.0f;
        return FVector(-Dir.X * Speed, -Dir.Y * Speed, 0.4f + 0.8f * W.Wind);
    }
}

FVector ALakeWeather::SkyAt(float Now) const
{
    // The velocity follows the weather blend (smoothstep), so its integral is exact: the same drift
    // on every machine at the same server time, from replicated numbers only.
    const FVector V0 = SkyVelocity(From), V1 = SkyVelocity(To);
    const float Ends = BlendStart + BlendSeconds;
    FVector Sky = SkyAtChange + V0 * (FMath::Min(Now, BlendStart) - ChangeAt);
    if (Now > BlendStart)
    {
        const float U = FMath::Min((Now - BlendStart) / FMath::Max(0.01f, BlendSeconds), 1.0f);
        Sky += V0 * BlendSeconds * U + (V1 - V0) * BlendSeconds * (U * U * U - 0.5f * U * U * U * U);
    }
    if (Now > Ends) Sky += V1 * (Now - Ends);
    return Sky;
}

ALakeWeather::FCloudSky ALakeWeather::CloudSkyAt(float Now) const
{
    FCloudSky Sky;
    const FVector D = SkyAt(Now);
    Sky.Drift = FVector2D(D.X, D.Y);
    Sky.Phase = D.Z;
    // The front comes with the new wind: from FrontStartKm upwind when the change is set, over the
    // lake halfway through the blend. Its width matches the blend, so the clouds overhead turn into
    // the new ones just as the rain and the light do down by the water.
    const FVector Dir = FRotator(0, To.WindYaw, 0).Vector();
    Sky.FrontDir = FVector2D(Dir.X, Dir.Y);
    const float Middle = BlendStart + 0.5f * BlendSeconds;
    const float Speed = FrontStartKm / FMath::Max(1.0f, Middle - ChangeAt);
    Sky.FrontKm = Speed * (Now - Middle);
    Sky.FrontWidthKm = FMath::Clamp(Speed * 0.5f * BlendSeconds, 0.5f, 15.0f);
    return Sky;
}

float ALakeWeather::StormAmount() const
{
    const FLakeWeatherState W = Current();
    return S(0.6f, 0.95f, W.Rain) * S(0.6f, 0.9f, W.Cloud);
}

bool ALakeWeather::StrikeAt(float Now, FStrike& Out) const
{
    if (PreviewStrikeAt > 0 && Now >= PreviewStrikeAt && Now < PreviewStrikeAt + 1.0f)
    {
        Out.Time = PreviewStrikeAt; Out.DistanceKm = 2.0f; Out.Yaw = 30; Out.Seed = 7;
        return true;
    }
    if (PreviewStrikeAt > 0) return false;
    const float Storm = StormAmount();
    if (Storm <= 0) return false;
    // Half-second slots, each with its own seeded roll: a strike or none, and when in the slot.
    const int32 Slot = FMath::FloorToInt(Now / 0.5f);
    for (int32 J = Slot; J >= Slot - 2; --J)
    {
        FRandomStream R((int32)((uint32)J * 2654435761u ^ 0x5eed1234u));
        if (R.FRand() >= StrikeRate * 0.5f * Storm) continue;
        const float Time = J * 0.5f + R.FRand() * 0.5f;
        if (Time > Now || Now - Time >= 1.0f) continue;
        Out.Time = Time;
        Out.DistanceKm = FMath::Lerp(1.5f, 14.0f, R.FRand() * R.FRand() < 0.5f ? 0.6f + 0.4f * R.FRand() : R.FRand());
        Out.Yaw = R.FRandRange(0, 360);
        Out.Seed = R.RandHelper(1 << 20);
        return true;
    }
    return false;
}

float ALakeWeather::FlashAt(float Now) const
{
    FStrike Strike;
    if (!StrikeAt(Now, Strike)) return 0;
    // A stroke flickers: the first return, then one to three more along the same channel.
    FRandomStream R(Strike.Seed);
    const float T = Now - Strike.Time;
    float Light = 0, Delay = 0;
    const int32 Strokes = 2 + R.RandHelper(3);
    for (int32 I = 0; I < Strokes; ++I)
    {
        const float Amp = I == 0 ? 1.0f : R.FRandRange(0.35f, 0.9f);
        if (T >= Delay) Light += Amp * FMath::Exp(-(T - Delay) / 0.09f);
        Delay += R.FRandRange(0.05f, 0.16f);
    }
    return FMath::Min(1.5f, Light) * FMath::Clamp(3.0f / (Strike.DistanceKm + 1.0f), 0.15f, 1.0f);
}

void ALakeWeather::MarkChange()
{
    const float Now = ServerNow();
    SkyAtChange = SkyAt(Now);
    ChangeAt = Now;
}

void ALakeWeather::SetWeatherNow(const FLakeWeatherState& State)
{
    MarkChange();
    From = To = State;
    BlendStart = ServerNow() - 1;
    BlendSeconds = 0.01f;
    NextChange = ServerNow() + Random.FRandRange(240, 480);
    ForceNetUpdate();
}

void ALakeWeather::SetWeather(const FLakeWeatherState& State, float Seconds, float Delay)
{
    MarkChange();
    From = Current();
    To = State;
    BlendStart = ServerNow() + FMath::Max(0.0f, Delay);
    BlendSeconds = FMath::Max(0.01f, Seconds);
    NextChange = BlendStart + BlendSeconds + Random.FRandRange(240, 480);
    ForceNetUpdate();
}

void ALakeWeather::PickNextWeather()
{
    // Weighted roll: fog is likelier around dawn and at night, rain after a grey sky, and the
    // weather rarely jumps from clear straight into a downpour.
    const float H = Hours();
    const bool bFogHours = H < 8.5f || H > 21.0f;
    const float CloudNow = Current().Cloud;
    float Total = 0;
    float Weights[UE_ARRAY_COUNT(GPresets)];
    for (int32 I = 0; I < UE_ARRAY_COUNT(GPresets); ++I)
    {
        const FPreset& P = GPresets[I];
        float W = P.Weight;
        if (P.bFoggy) W *= bFogHours ? 2.5f : 0.4f;
        if (P.bRainy) W *= CloudNow > 0.7f ? 2.0f : CloudNow < 0.4f ? 0.25f : 1.0f;
        Weights[I] = W;
        Total += W;
    }
    float Roll = Random.FRandRange(0, Total);
    int32 Pick = 0;
    for (; Pick < UE_ARRAY_COUNT(GPresets) - 1 && Roll > Weights[Pick]; ++Pick) Roll -= Weights[Pick];
    const FPreset& P = GPresets[Pick];
    FLakeWeatherState Next;
    Next.Cloud = Random.FRandRange(P.Cloud.X, P.Cloud.Y);
    Next.Fog = Random.FRandRange(P.Fog.X, P.Fog.Y);
    Next.Rain = Random.FRandRange(P.Rain.X, P.Rain.Y);
    Next.Wind = Random.FRandRange(P.Wind.X, P.Wind.Y);
    Next.WindYaw = To.WindYaw + Random.FRandRange(-60, 60);
    // Rain and big changes come as a front: the pressure starts falling one to two game hours ahead.
    const FLakeWeatherState Now = Current();
    const bool bFront = P.bRainy || FMath::Abs(Next.Cloud - Now.Cloud) > 0.5f || FMath::Abs(Next.Wind - Now.Wind) > 0.4f;
    const float Delay = bFront ? Random.FRandRange(150, 300) : Random.FRandRange(10, 40);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_WEATHER next %s cloud=%.2f fog=%.2f rain=%.2f wind=%.2f in %.0f s"), P.Name, Next.Cloud, Next.Fog, Next.Rain, Next.Wind, Delay);
    SetWeather(Next, Random.FRandRange(60, 150), Delay);
}

void ALakeWeather::BeginPlay()
{
    Super::BeginPlay();
    Random.Initialize(FDateTime::Now().GetTicks() & 0x7fffffff);
    if (HasAuthority())
    {
        // The game opens on the misty dawn the lake was designed around.
        ClockOriginServer = ServerNow();
        ClockOriginHours = 5.5f;
        From = To = FLakeWeatherState();
        BlendStart = ChangeAt = ClockOriginServer;
        NextChange = ClockOriginServer + Random.FRandRange(300, 480);
    }
#if !UE_BUILD_SHIPPING
    // -WeatherPreview: a fixed set of times and weathers seen from the start spot, HUD on.
    // -NightPreview: only the night, at once — no waiting for the day to run through.
    const bool bNightOnly = FParse::Param(FCommandLine::Get(), TEXT("NightPreview"));
    const bool bCloudsOnly = FParse::Param(FCommandLine::Get(), TEXT("CloudPreview"));
    // -FrontPreview: a fair noon, then a downpour comes in over the lake from the far shore; frames
    // looking upwind as the front nears, passes overhead, and after.
    if (HasAuthority() && FParse::Param(FCommandLine::Get(), TEXT("FrontPreview")))
    {
        auto Look = [this](float Pitch, float Turn)
        {
            if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
                if (APawn* Pawn = PC->GetPawn())
                {
                    FVector2D Facing;
                    const FVector2D Spot = LakeMap::StartSpot(&Facing) - Facing * 250;
                    Pawn->SetActorLocation(FVector(Spot, LakeMap::Ground(Spot.X, Spot.Y) + 100));
                    PC->SetControlRotation(FRotator(Pitch, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)) + Turn, 0));
                }
        };
        FTimerHandle Start, Front;
        GetWorldTimerManager().SetTimer(Start, [this, Look]
        {
            FVector2D Facing;
            LakeMap::StartSpot(&Facing);
            FLakeWeatherState Fair;
            Fair.Cloud = 0.35f; Fair.Fog = 0.0f; Fair.Rain = 0.0f; Fair.Wind = 0.3f;
            // The wind blows from across the lake towards the angler.
            Fair.WindYaw = FMath::RadiansToDegrees(FMath::Atan2(-Facing.Y, -Facing.X));
            SetHours(12.0f);
            SetWeatherNow(Fair);
            NextChange = ServerNow() + 1e6f;
            Wetness = 0;
            Look(12, 0);
        }, 12.0f, false);
        GetWorldTimerManager().SetTimer(Front, [this]
        {
            FLakeWeatherState Storm = To;
            Storm.Cloud = 1.0f; Storm.Fog = 0.3f; Storm.Rain = 0.9f; Storm.Wind = 0.75f;
            SetWeather(Storm, 60.0f, 50.0f);
            NextChange = ServerNow() + 1e6f;
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FRONT set: blend from %.0f s for 60 s"), ServerNow() + 50.0f);
        }, 20.0f, false);
        // Seconds after the front was set: at the horizon, nearing, over the lake, past.
        const float Shots[] = { 4, 25, 45, 60, 72, 82, 95, 120 };
        for (int32 I = 0; I < UE_ARRAY_COUNT(Shots); ++I)
        {
            FTimerHandle Measure, Snap;
            const float At = 20.0f + Shots[I];
            GetWorldTimerManager().SetTimer(Measure, [this] { PreviewFrameSeconds = 0; PreviewFrames = 0; bMeasureFrames = true; }, At - 3.0f, false);
            GetWorldTimerManager().SetTimer(Snap, [this, I, Look]
            {
                const FString File = FString::Printf(TEXT("Front_%d.png"), I + 1);
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / File, true, false);
                const FCloudSky Sky = CloudSkyAt(ServerNow());
                const FLakeWeatherState W = Current();
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FRONT %s front %.1f km (width %.1f) | %s cloud %.2f rain %.2f | frame %.1f ms"), *File,
                    Sky.FrontKm, Sky.FrontWidthKm, *Describe(), W.Cloud, W.Rain, PreviewFrames > 0 ? PreviewFrameSeconds / PreviewFrames * 1000.0f : 0.0f);
                // The last frames look up at the storm overhead.
                if (I == UE_ARRAY_COUNT(Shots) - 3) Look(35, 0);
            }, At, false);
        }
        FTimerHandle Exit;
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 20.0f + Shots[UE_ARRAY_COUNT(Shots) - 1] + 2.0f, false);
    }
    if (HasAuthority() && (bNightOnly || bCloudsOnly || FParse::Param(FCommandLine::Get(), TEXT("WeatherPreview"))))
    {
        struct FShot { float Hours, Cloud, Fog, Rain, Wind; const TCHAR* File; float Turn = -25, Pitch = -4, Back = 250; bool bMoon = false; bool bStrike = false; bool bAir = false; };
        TArray<FShot> Shots = {
            { 5.8f,  0.3f, 0.7f, 0.0f, 0.05f, TEXT("Weather_Dawn.png") },
            { 12.5f, 0.15f, 0.0f, 0.0f, 0.3f, TEXT("Weather_Noon.png") },
            { 15.0f, 0.95f, 0.15f, 0.0f, 0.75f, TEXT("Weather_OvercastWind.png") },
            { 14.0f, 0.9f, 0.25f, 0.25f, 0.15f, TEXT("Weather_Drizzle.png") },
            { 16.0f, 1.0f, 0.3f, 1.0f, 0.85f, TEXT("Weather_Downpour.png") },
            { 20.6f, 0.25f, 0.05f, 0.0f, 0.1f, TEXT("Weather_Sunset.png") },
            { 23.5f, 0.1f, 0.1f, 0.0f, 0.05f, TEXT("Weather_Night.png") },
            { 1.5f,  0.5f, 0.9f, 0.0f, 0.0f, TEXT("Weather_NightFog.png") },
        };
        if (bNightOnly)
            Shots = {
                { 23.5f, 0.08f, 0.05f, 0.0f, 0.05f, TEXT("Night_Clear.png") },
                // Straight at the moon: the camera turns to the lunar light, stars and the disk around it.
                { 23.5f, 0.02f, 0.00f, 0.0f, 0.02f, TEXT("Night_Moon.png"), 0, 22, 250, true },
                { 1.5f,  0.5f,  0.9f,  0.0f, 0.0f,  TEXT("Night_Fog.png") },
            };
        // -CloudPreview: the sky, the camera tilted up over the lake: every kind of cloud the weather
        // brings, and the low sun lighting them.
        if (bCloudsOnly)
            Shots = {
                { 11.0f, 0.25f, 0.0f, 0.0f, 0.2f,  TEXT("Clouds_FairCumulus.png"), -25, 30 },
                { 13.0f, 0.5f,  0.0f, 0.0f, 0.35f, TEXT("Clouds_Cumulus.png"), 60, 30 },
                { 14.0f, 0.8f,  0.1f, 0.0f, 0.4f,  TEXT("Clouds_Broken.png"), -25, 30 },
                { 15.0f, 0.95f, 0.15f, 0.0f, 0.6f, TEXT("Clouds_Overcast.png"), -25, 30 },
                { 14.0f, 0.9f,  0.25f, 0.25f, 0.2f, TEXT("Clouds_Drizzle.png"), -25, 30 },
                { 16.0f, 1.0f,  0.3f, 1.0f, 0.85f, TEXT("Clouds_Storm.png"), -25, 30 },
                { 20.4f, 0.4f,  0.05f, 0.0f, 0.15f, TEXT("Clouds_Sunset.png"), 0, 15 },
                { 6.2f,  0.35f, 0.3f, 0.0f, 0.1f,  TEXT("Clouds_Dawn.png"), 0, 15 },
                // From 400 m up: the shadows of fair-weather cumulus on the water and the woods.
                { 12.5f, 0.35f, 0.0f, 0.0f, 0.3f,  TEXT("Clouds_Shadows.png"), 150, -38, 300, false, false, true },
                { 16.0f, 1.0f,  0.3f, 1.0f, 0.85f, TEXT("Clouds_Lightning.png"), -25, 20, 250, false, true },
                { 23.0f, 1.0f,  0.2f, 0.9f, 0.7f,  TEXT("Clouds_NightLightning.png"), -25, 15, 250, false, true },
                { 23.5f, 0.45f, 0.05f, 0.0f, 0.1f, TEXT("Clouds_Night.png"), 0, 25 },
            };
        // -CloudPreview -CloudQuick: only the four skies that show the shapes best, to iterate fast.
        if (bCloudsOnly && FParse::Param(FCommandLine::Get(), TEXT("CloudQuick")))
            Shots = {
                { 11.0f, 0.25f, 0.0f, 0.0f, 0.2f,  TEXT("Clouds_FairCumulus.png"), -25, 30 },
                { 13.0f, 0.5f,  0.0f, 0.0f, 0.35f, TEXT("Clouds_Cumulus.png"), 60, 30 },
                { 14.0f, 0.8f,  0.1f, 0.0f, 0.4f,  TEXT("Clouds_Broken.png"), -25, 30 },
                { 15.0f, 0.95f, 0.15f, 0.0f, 0.6f, TEXT("Clouds_Overcast.png"), -25, 30 },
                { 16.0f, 1.0f,  0.3f, 1.0f, 0.85f, TEXT("Clouds_Storm.png"), -25, 30 },
            };
        if (FParse::Param(FCommandLine::Get(), TEXT("GrassLightPreview")))
            Shots = {
                { 12.5f, 0.15f, 0, 0, 0.1f, TEXT("Grass_NoonAcross.png"), -60, -25, 1000 },
                { 12.5f, 0.15f, 0, 0, 0.1f, TEXT("Grass_NoonAway.png"), 120, -25, 1000 },
                { 18.0f, 0.15f, 0, 0, 0.1f, TEXT("Grass_LowSun.png"), 0, -25, 1000 },
                { 15.0f, 0.95f, 0.15f, 0, 0.1f, TEXT("Grass_Overcast.png"), -60, -25, 1000 },
            };
        // The night run does not wait for the day: the first pose is set as soon as the world exists.
        // Ten seconds per night frame, and the camera is already in place ten seconds before the
        // shot: the auto exposure needs that long to settle, so each frame shows what the player
        // actually sees rather than a moment mid-adaptation.
        const float First = bNightOnly ? 12.0f : 18.0f;
        const float Gap = bNightOnly ? 12.0f : 9.0f;
        const float PoseLead = bNightOnly ? 10.0f : 7.0f;
        for (int32 I = 0; I < Shots.Num(); ++I)
        {
            const FShot Shot = Shots[I];
            const float At = First + I * Gap;
            FTimerHandle Pose, Snap;
            GetWorldTimerManager().SetTimer(Pose, [this, Shot, PoseLead]
            {
                FLakeWeatherState W;
                W.Cloud = Shot.Cloud; W.Fog = Shot.Fog; W.Rain = Shot.Rain; W.Wind = Shot.Wind; W.WindYaw = 60;
                MarkChange();
                From = To = W;
                BlendStart = ServerNow();
                NextChange = BlendStart + 1e6f;
                SetHours(Shot.Hours);
                Wetness = Shot.Rain > 0 ? 0.8f : 0.0f;
                // A close strike right as the frame is taken; no other strikes meanwhile.
                PreviewStrikeAt = Shot.bStrike ? ServerNow() + PoseLead - 0.12f : -1.0f;
                if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
                    if (APawn* Pawn = PC->GetPawn())
                    {
                        if (Shot.bAir)
                        {
                            // A camera high over the shore behind the angler, looking out over the lake.
                            FVector2D Facing;
                            const FVector2D Spot = LakeMap::StartSpot(&Facing) - Facing * Shot.Back;
                            FActorSpawnParameters Params;
                            Params.ObjectFlags |= RF_Transient;
                            ACameraActor* Camera = GetWorld()->SpawnActor<ACameraActor>(FVector(Spot, 40000),
                                FRotator(Shot.Pitch, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)) + Shot.Turn, 0), Params);
                            PC->SetViewTarget(Camera);
                            return;
                        }
                        if (PC->GetViewTarget() != Pawn) PC->SetViewTarget(Pawn);
                        FVector2D Facing;
                        const FVector2D Spot = LakeMap::StartSpot(&Facing) - Facing * Shot.Back;
                        Pawn->SetActorLocation(FVector(Spot, LakeMap::Ground(Spot.X, Spot.Y) + 100));
                        FRotator View(Shot.Pitch, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)) + Shot.Turn, 0);
                        if (Shot.bMoon && Moon)
                        {
                            // The light shines from the moon, so the moon itself is the opposite way.
                            const FVector ToMoon = -Moon->GetForwardVector();
                            View = FRotator(FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(ToMoon.Z, -1.0f, 1.0f))),
                                           FMath::RadiansToDegrees(FMath::Atan2(ToMoon.Y, ToMoon.X)), 0);
                        }
                        PC->SetControlRotation(View);
                    }
            }, At - PoseLead, false);
            // Frame time over the last 4 s before the shot (logged with it).
            FTimerHandle Measure;
            GetWorldTimerManager().SetTimer(Measure, [this] { PreviewFrameSeconds = 0; PreviewFrames = 0; bMeasureFrames = true; }, At - 4.0f, false);
            GetWorldTimerManager().SetTimer(Snap, [this, Shot]
            {
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Shot.File, true, false);
                const FFishConditions C = FishConditions();
                FString Bite;
                for (int32 Sp = 0; Sp < LakeFish::NumSpecies(); ++Sp) Bite += FString::Printf(TEXT(" %s %.2f"), *LakeFish::Info((EFishSpecies)Sp).Id, LakeFish::Activity((EFishSpecies)Sp, C));
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_WEATHER_SHOT %s %s %.0f C | frame %.1f ms | bite:%s"), Shot.File, *Describe(), TemperatureC(),
                    PreviewFrames > 0 ? PreviewFrameSeconds / PreviewFrames * 1000.0f : 0.0f, *Bite);
            }, At, false);
        }
        FTimerHandle Exit;
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, First + Shots.Num() * Gap + 1.5f, false);
    }
#endif
}

void ALakeWeather::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bMeasureFrames) { PreviewFrameSeconds += DeltaSeconds; ++PreviewFrames; }
    if (HasAuthority() && ServerNow() >= NextChange) PickNextWeather();
    // Every half minute of server time each machine logs the sky at that exact moment: the lines
    // of the server and the clients must agree (Tools/TestNetwork.ps1 logs).
    const int32 Half = FMath::FloorToInt(ServerNow() / 30.0f);
    if (Half != LastSyncLog && Half > 0)
    {
        LastSyncLog = Half;
        const FCloudSky Sky = CloudSkyAt(Half * 30.0f);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CLOUD_SYNC t=%d drift=%.4f,%.4f phase=%.3f front=%.3f km width=%.2f"),
            Half * 30, Sky.Drift.X, Sky.Drift.Y, Sky.Phase, Sky.FrontKm, Sky.FrontWidthKm);
    }
    if (HasAuthority())
    {
        // Pressure heads for the level of the weather that is coming (so it falls before the rain),
        // at most 2.5 mm per game hour; the water warms and cools a few hours behind the air.
        const float Hours = DeltaSeconds * GameHoursPerSecond();
        const float Target = 762.0f - 9.0f * To.Cloud - 13.0f * To.Rain - 5.0f * To.Wind + 3.0f * To.Fog * (1 - To.Rain);
        const float Rate = FMath::Clamp((Target - Pressure) / 2.5f, -2.5f, 2.5f);
        Pressure += Rate * Hours;
        Trend = FMath::FInterpTo(Trend, Rate, DeltaSeconds, 0.5f);
        WaterTemp += (0.35f * TemperatureC() + 0.65f * 23.0f - WaterTemp) * Hours / 5.0f;   // a warm, shallow summer pond: about +20..+22
    }
    // Diagnostic for the network test: every machine reports the same clock and weather.
    if (!bLoggedClock && GetWorld()->GetTimeSeconds() > 7)
    {
        bLoggedClock = true;
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CLOCK %.2f h, %s, %.0f C, water %.1f C, %.0f mm (netmode %d)"), Hours(), *Describe(), TemperatureC(), WaterTemp, Pressure, (int32)GetNetMode());
    }
    if (GetNetMode() != NM_DedicatedServer) ApplyToWorld(DeltaSeconds);
}

void ALakeWeather::ApplyToWorld(float DeltaSeconds)
{
    UWorld* World = GetWorld();
    if (!bBound)
    {
        // The environment spawns the sky on every machine; take it over once it exists.
        for (TActorIterator<ADirectionalLight> It(World); It && !Sun; ++It) Sun = Cast<UDirectionalLightComponent>(It->GetLightComponent());
        if (!Sun) return;
        for (TActorIterator<ASkyAtmosphere> It(World); It; ++It) Atmosphere = It->GetComponent();
        for (TActorIterator<ASkyLight> It(World); It; ++It) SkyLight = It->GetLightComponent();
        for (TActorIterator<AExponentialHeightFog> It(World); It; ++It) Fog = It->GetComponent();
        for (TActorIterator<AVolumetricCloud> It(World); It; ++It) Clouds = It->FindComponentByClass<UVolumetricCloudComponent>();
        // The sun casts the clouds' shadows on the ground and the water: the default 512 map over
        // 25 km, ~50 m a texel, fine enough for the shadow of a single cumulus.
        Sun->bCastCloudShadows = true;
        Sun->CloudShadowExtent = 25.0f;
        Sun->MarkRenderStateDirty();
        // Lightning: the clouds glow from within (the material's Flash), and this light gives the
        // ground its blink: shown only while it flashes; no shadows. Not an atmosphere light: as
        // one it lit the whole atmosphere and the sky flashed clear blue.
        Lightning = NewObject<UDirectionalLightComponent>(this);
        Lightning->SetupAttachment(RootComponent);
        Lightning->SetMobility(EComponentMobility::Movable);
        Lightning->SetAtmosphereSunLight(false);
        Lightning->SetForwardShadingPriority(-1);
        Lightning->SetCastShadows(false);
        Lightning->SetLightColor(FLinearColor(0.78f, 0.84f, 1.0f));
        Lightning->SetLightSourceAngle(10.0f);
        Lightning->SetIntensity(0);
        Lightning->SetVisibility(false);
        Lightning->RegisterComponent();
        for (const TCHAR* Name : { TEXT("A_ThunderNear"), TEXT("A_ThunderMid"), TEXT("A_ThunderFar") })
            ThunderSounds.Add(LoadObject<USoundWave>(nullptr, *FString::Printf(TEXT("/Game/Audio/%s.%s"), Name, Name)));
        for (TActorIterator<APostProcessVolume> It(World); It; ++It) Post = *It;
        for (TActorIterator<ALakeEnvironment> It(World); It; ++It)
        {
            TInlineComponentArray<UAudioComponent*> Audio(*It);
            for (UAudioComponent* A : Audio)
            {
                const FString Name = A->Sound ? A->Sound->GetName() : FString();
                if (Name == TEXT("A_Wind")) WindSound = A;
                    }
        }
        // Moonlight: a second, cool directional light. It is deliberately not an atmosphere sun
        // light: as one, it painted the night sky with a daylight-blue gradient (and the sky then
        // banded); the night sky is left to the faint atmosphere and the moon.
        auto* MoonActor = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 3000), FRotator(-30, 0, 0));
        MoonActor->SetMobility(EComponentMobility::Movable);
        Moon = Cast<UDirectionalLightComponent>(MoonActor->GetLightComponent());
        Moon->SetAtmosphereSunLight(false);
        // Below the sun's priority, so the two directional lights never compete for forward shading.
        Moon->SetForwardShadingPriority(0);
        Moon->SetLightColor(FLinearColor(0.62f, 0.72f, 1.0f));
        Moon->SetLightSourceAngle(0.5f);
        Moon->SetCastShadows(true);
        if (UMaterialInterface* SkyBase = FParse::Param(FCommandLine::Get(), TEXT("NoNightSky")) ? nullptr : LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Weather/M_NightSky.M_NightSky")))
        {
            NightSky = NewObject<UStaticMeshComponent>(this);
            NightSky->SetupAttachment(RootComponent);
            NightSky->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
            NightSky->SetWorldScale3D(FVector(9000));
            NightSky->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            NightSky->SetCastShadow(false);
            NightSky->SetAffectDynamicIndirectLighting(false);
            NightSky->SetAffectDistanceFieldLighting(false);
            NightSky->bVisibleInRayTracing = false;
            NightSkyMaterial = UMaterialInstanceDynamic::Create(SkyBase, this);
            NightSky->SetMaterial(0, NightSkyMaterial);
            NightSky->RegisterComponent();
        }
        if (Clouds)
        {
            // Our own cloud material (Tools/CreateCloudMaterial.py): cumulus, stratus and storm
            // clouds mixed by the weather, drifting with the wind. The layer starts at 0.8 km and is
            // 9 km thick, room for storm towers; light reaching every sample is reddened by the
            // atmosphere, so the clouds glow at sunset.
            // The engine's authored cloud material is the default look: its noise layers and
            // height profiles already give the rounded cumulus of the reference skies, and the
            // weather only has to move its coverage, density and storminess. Our own material is
            // kept under -OwnClouds for experiments (it has fronts, in-cloud lightning and a
            // moonlit glow, but its shaped heaps never read as well as the authored ones).
            const bool bWantOwnClouds = FParse::Param(FCommandLine::Get(), TEXT("OwnClouds"));
            UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, bWantOwnClouds
                ? TEXT("/Game/Weather/M_LakeClouds.M_LakeClouds")
                : TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst"));
            if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst"));
            if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Weather/M_LakeClouds.M_LakeClouds"));
            if (Base)
            {
                CloudMaterial = UMaterialInstanceDynamic::Create(Base, this);
                Clouds->SetMaterial(CloudMaterial);
                // The authored material's noise tiling (its own default is 256): a smaller value makes
                // the heaps bigger, which is what the reference cumulus need.
                CloudMaterial->SetScalarParameterValue(TEXT("Layout_CloudGlobalScale"), 150.0f);
                bOwnClouds = Base->GetName() == TEXT("M_LakeClouds");
            }
            if (bOwnClouds)
            {
                Clouds->SetLayerBottomAltitude(1.0f);
                Clouds->SetLayerHeight(9.0f);
                Clouds->SetbUsePerSampleAtmosphericLightTransmittance(true);
                // More steps through the cloud than the default: smooth billows instead of grain.
                Clouds->SetViewSampleCountScale(3.5f);
                Clouds->SetShadowViewSampleCountScale(3.0f);
            }
            else
            {
                // The engine material: a cumulus layer from about 1.4 km up, five kilometres thick.
                Clouds->SetLayerBottomAltitude(1.8f);
                Clouds->SetLayerHeight(4.0f);
                Clouds->SetbUsePerSampleAtmosphericLightTransmittance(true);
                Clouds->SetViewSampleCountScale(2.5f);
                Clouds->SetShadowViewSampleCountScale(2.0f);
            }
        }
        WeatherParameters = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Weather/MPC_Weather.MPC_Weather"));
        RainTiles = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        RainTiles->SetupAttachment(RootComponent);
        RainTiles->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Weather/SM_RainTile.SM_RainTile")));
        RainTiles->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        RainTiles->SetCastShadow(false);
        RainTiles->SetUsingAbsoluteLocation(true);
        RainTiles->RegisterComponent();
        for (int32 I = 0; I < 9; ++I) RainTiles->AddInstance(FTransform::Identity, true);
        auto Sound = [this](const TCHAR* Name)
        {
            auto* A = NewObject<UAudioComponent>(this);
            A->SetupAttachment(RootComponent);
            A->SetSound(LoadObject<USoundWave>(nullptr, *FString::Printf(TEXT("/Game/Audio/%s.%s"), Name, Name)));
            A->SetVolumeMultiplier(0.001f);
            A->bAutoActivate = false;
            A->RegisterComponent();
            A->Play();
            return A;
        };
        RainLightSound = Sound(TEXT("A_RainLight"));
        RainHeavySound = Sound(TEXT("A_RainHeavy"));
        CricketSound = Sound(TEXT("A_Crickets"));
        // The exposure needs its own unbound volume: the world already has the look volume and the
        // underwater murk volume (weight 0 on land), and writing the exposure into those either did
        // nothing or was overridden — the night lighting changes had no visible effect at all.
        FActorSpawnParameters PostParams;
        PostParams.ObjectFlags |= RF_Transient;
        ExposureVolume = World->SpawnActor<APostProcessVolume>(PostParams);
        if (ExposureVolume)
        {
            ExposureVolume->bUnbound = true;
            ExposureVolume->BlendWeight = 1.0f;
            ExposureVolume->Priority = 5.0f;   // above the look volume (0), below the murk (10)
            ExposureVolume->Settings.bOverride_AutoExposureBias = true;
            ExposureVolume->Settings.bOverride_AutoExposureMethod = true;
            ExposureVolume->Settings.bOverride_AutoExposureSpeedUp = true;
            ExposureVolume->Settings.bOverride_AutoExposureSpeedDown = true;
        }
        bBound = true;
    }

    const FLakeWeatherState W = Current();
    const float FogNow = EffectiveFog();
    float Elevation;
    const FVector ToSun = SunDirection(&Elevation);
    const float Day = S(-6.0f, 4.0f, Elevation);
    const float Night = 1 - S(-12.0f, -2.0f, Elevation);
    const float Overcast = FMath::Max(W.Cloud, W.Rain);
    if (NightSkyMaterial && NightSky)
    {
        const FVector ToMoon = FVector(-ToSun.X, -ToSun.Y, FMath::Max(0.3f, -ToSun.Z * 0.8f + 0.2f)).GetSafeNormal();
        NightSkyMaterial->SetVectorParameterValue(TEXT("MoonDirection"), FLinearColor(ToMoon.X, ToMoon.Y, ToMoon.Z));
        NightSkyMaterial->SetScalarParameterValue(TEXT("Visibility"), Night * (1 - S(0.35f, 0.95f, Overcast)) * (1 - S(0.2f, 0.85f, FogNow)));
        NightSky->SetVisibility(Night > 0.001f);
        if (APlayerController* PC = World->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
            NightSky->SetWorldLocation(PC->PlayerCameraManager->GetCameraLocation());
    }

    // Changes are applied in small steps only: every update of a light, the sky or the fog throws
    // away cached shadow pages and sky captures, which is expensive. The sun moves 0.125 degrees a
    // second, so a step of 0.15 degrees is invisible.
    auto Changed = [](float& Last, float Now, float Step) { if (FMath::Abs(Now - Last) < Step) return false; Last = Now; return true; };
    // Two directional lights (the sun and the moon): the engine wants exactly one of them as the
    // forward shading light for translucency, the water and the volumetric fog, or it warns that
    // they compete. Whoever lights the scene wins — the sun by day, the moon at night.
    const int32 SunMain = Day > 0.35f ? 1 : 0;
    if (SunMain != ForwardMain)
    {
        ForwardMain = SunMain;
        if (Sun) Sun->SetForwardShadingPriority(SunMain);
        if (Moon) Moon->SetForwardShadingPriority(1 - SunMain);
    }
    // Sun: rises in the north-east, low and warm at the ends of the day, softened by cloud.
    if (FVector::DotProduct(ToSun, LastSunDir) < FMath::Cos(FMath::DegreesToRadians(0.15f)))
    {
        LastSunDir = ToSun;
        Sun->SetWorldRotation((-ToSun).Rotation());
        // Moon: roughly opposite the sun, always a little above the horizon at night.
        const FVector ToMoon(-ToSun.X, -ToSun.Y, FMath::Max(0.3f, -ToSun.Z * 0.8f + 0.2f));
        Moon->SetWorldRotation((-ToMoon.GetSafeNormal()).Rotation());
    }
    // 6.5 lux: the sky is lit up to match in LakeEnvironment (SkyLuminanceFactor), so only the direct light is softer.
    if (Changed(LastSun, 6.5f * S(-3.0f, 6.0f, Elevation) * FMath::Lerp(1.0f, 0.12f, Overcast * Overcast), 0.05f)) Sun->SetIntensity(LastSun);
    // The atmosphere already reddens a low sun (the light crosses more air); the colour
    // temperature only adds a little warmth on top, or a misty dawn turns blood red.
    if (Changed(LastSunTemp, FMath::Lerp(4800.0f, 5800.0f, S(0.0f, 20.0f, Elevation)), 25.0f)) Sun->SetTemperature(LastSunTemp);
    if (Changed(LastSunAngle, FMath::Lerp(0.8f, 6.0f, Overcast), 0.1f)) Sun->SetLightSourceAngle(LastSunAngle);
    if (Changed(LastMoon, 0.4f * Night * FMath::Lerp(1.0f, 0.15f, Overcast), 0.005f))
    {
        Moon->SetIntensity(LastMoon);
        Moon->SetVisibility(LastMoon > 0.003f);
        if (CloudMaterial && bOwnClouds) CloudMaterial->SetScalarParameterValue(TEXT("NightGlow"), LastMoon * 0.12f);
    }
    // Under a thick cloud the whole sky is dimmer, more so in rain. At night the sky is only the
    // moon: left at daylight strength it lit the fog and the whole valley like day
    // (and the fog showed banding in the sky), so its intensity follows the sun.
    if (SkyLight && Changed(LastSky, FMath::Lerp(0.07f, 1.0f, Day) * FMath::Lerp(1.0f, 0.8f, W.Cloud * W.Cloud) * FMath::Lerp(1.0f, 0.6f, W.Rain), 0.01f))
        SkyLight->SetIntensity(LastSky);
    if (Atmosphere)
    {
        // A grey sky under cloud and rain: less blue scattering, more milky haze.
        if (Changed(LastMie, 0.004f + 0.003f * FogNow + 0.02f * W.Rain + 0.006f * W.Cloud, 0.0002f)) Atmosphere->SetMieScatteringScale(LastMie);
        if (Changed(LastRayleigh, FMath::Lerp(0.0331f, 0.012f, Overcast * 0.85f), 0.0003f)) Atmosphere->SetRayleighScatteringScale(LastRayleigh);
    }
    if (Post)
    {
        // By day the eye adapts only part of the way: a grey or rainy day must look darker than a
        // sunny one, not be brightened back to it. Measured with -ExposureProbe, free adaptation
        // settles at EV100 -0.1 at a clear noon, -3.3 against a low sun, and drops with the
        // sunlight the cloud lets through (-2.5 overcast, -3.35 in a downpour) while fog raises it.
        // The exposure is held in a narrow band that follows only half of the loss of sunlight,
        // and a little darker overall than before. At night adaptation stops early, which keeps
        // the night dark but readable.
        const float SunShare = FMath::Lerp(1.0f, 0.12f, Overcast * Overcast);
        const float ClearEV = FMath::Lerp(-3.3f, -0.1f, S(0.0f, 35.0f, Elevation));
        const float DayEV = ClearEV + 0.3f + 0.5f * FMath::Log2(SunShare) + 1.3f * FogNow;
        // Night: the adaptation is clamped to a dark target, never allowed to lift a near-black sky
        // to daytime (that showed as a bright blue night with banding and flicker in the sky).
        // Larger brightness values mean a darker exposure; the night sits several stops below the
        // old dusk-like target, and the adaptation is fast so the switch to night does not creep up
        // as a brightening ramp over many seconds.
        if (ExposureVolume)
        {
            FPostProcessSettings& PP = ExposureVolume->Settings;
            // The engine's own luminance range stays as it is; the night is darkened by exposure
            // compensation, and adaptation is fast so the change to night does not show as a slow
            // brightening ramp (the histogram settles within a few frames).
            PP.AutoExposureMethod = AEM_Histogram;
            PP.AutoExposureBias = FMath::Lerp(-3.2f, 0.0f, Day);
            PP.AutoExposureSpeedUp = FMath::Lerp(8.0f, 2.0f, Day);
            PP.AutoExposureSpeedDown = FMath::Lerp(8.0f, 1.5f, Day);
        }
#if !UE_BUILD_SHIPPING
        // -ExposureProbe: free adaptation and the HDR debug view, to read what the eye would choose.
        if (FParse::Param(FCommandLine::Get(), TEXT("ExposureProbe")))
        {
            if (ExposureVolume) ExposureVolume->Settings.AutoExposureMinBrightness = -6;
            if (ExposureVolume) ExposureVolume->Settings.AutoExposureMaxBrightness = 16;
            static bool bShown = false;
            if (!bShown && GetWorld()->GetFirstPlayerController()) { bShown = true; GetWorld()->GetFirstPlayerController()->ConsoleCommand(TEXT("ShowFlag.VisualizeHDR 1")); }
        }
#endif
    }

    // Fog: a thin haze over the valley and a dense layer on the water; mist on calm mornings.
    if (Fog && Changed(LastFog, FogNow + 3.0f * W.Rain, 0.004f))
    {
        Fog->SetFogDensity(0.01f + 0.032f * FogNow + 0.02f * W.Rain);
        FExponentialHeightFogData Water = Fog->SecondFogData;
        Water.FogDensity = 0.02f + 0.26f * FogNow;
        Water.FogHeightFalloff = 2.0f;
        Water.FogHeightOffset = -100.0f;
        Fog->SetSecondFogData(Water);
        Fog->SetVolumetricFogExtinctionScale(0.5f + 0.8f * FogNow + 0.4f * W.Rain);
    }
    // A misty dawn is milky white, not orange: a low sun lights the fog only weakly (its light is
    // reddened by the long way through the air), the cool light of the sky lights most of it.
    // Only around dawn and dusk: at night the sky's light stays as it was, or the fog glows.
    const float Twilight = S(-6.0f, 2.0f, Elevation) * (1 - S(2.0f, 20.0f, Elevation));
    if (Changed(LastSunInFog, FMath::Lerp(0.3f, 1.0f, S(2.0f, 20.0f, Elevation)) + 2 * Twilight, 0.02f))
    {
        Sun->SetVolumetricScatteringIntensity(FMath::Lerp(0.3f, 1.0f, S(2.0f, 20.0f, Elevation)));
        if (SkyLight) SkyLight->SetVolumetricScatteringIntensity(1.0f + 0.6f * Twilight);
    }
    // The haze colour and the lift of the sky (LakeEnvironment dims the sun and raises the sky to
    // match) are daytime light: the haze's own glow fades out at night, the night sky is not raised.
    if (Changed(LastDayTint, Day, 0.01f))
    {
        if (Fog) Fog->SetFogInscatteringColor(FLinearColor(0.42f, 0.47f, 0.55f) * FMath::Lerp(0.015f, 1.0f, Day * Day));
        if (Atmosphere)
        {
            const float Lift = FMath::Lerp(1.0f, 10.0f / 6.5f, Day);
            Atmosphere->SetSkyLuminanceFactor(FLinearColor(Lift, Lift, Lift));
        }
    }
    if (CloudMaterial && bOwnClouds)
    {
        // Which clouds a weather brings: fair-weather cumulus in a clear to cloudy sky, growing by
        // day and settling at night; a grey stratus deck when it is overcast or raining; dark storm
        // towers in heavy rain. Ahead of the front the sky keeps the old weather's clouds, behind
        // it the new one's.
        auto Kinds = [Day](const FLakeWeatherState& K)
        {
            return FVector4f(
                S(0.05f, 0.45f, K.Cloud) * (1 - S(0.75f, 0.95f, K.Cloud)) * FMath::Lerp(0.6f, 1.0f, Day) * (1 - 0.6f * S(0.3f, 0.8f, K.Rain)),
                FMath::Min(1.0f, S(0.55f, 0.9f, K.Cloud) + 0.6f * S(0.1f, 0.6f, K.Rain)),
                S(0.45f, 0.9f, K.Rain) * S(0.5f, 0.9f, K.Cloud),
                0.02f * (1 + 0.5f * K.Rain));
        };
        const FVector4f Old = Kinds(From), New = Kinds(To);
        if (Changed(LastCloud, Old.X + 3 * Old.Y + 9 * Old.Z + 27 * New.X + 81 * New.Y + 243 * New.Z, 0.003f))
        {
            CloudMaterial->SetScalarParameterValue(TEXT("Cumulus"), Old.X);
            CloudMaterial->SetScalarParameterValue(TEXT("Stratus"), Old.Y);
            CloudMaterial->SetScalarParameterValue(TEXT("Storm"), Old.Z);
            CloudMaterial->SetScalarParameterValue(TEXT("Density"), Old.W);
            CloudMaterial->SetScalarParameterValue(TEXT("Cumulus2"), New.X);
            CloudMaterial->SetScalarParameterValue(TEXT("Stratus2"), New.Y);
            CloudMaterial->SetScalarParameterValue(TEXT("Storm2"), New.Z);
            CloudMaterial->SetScalarParameterValue(TEXT("Density2"), New.W);
        }
        const FCloudSky Sky = CloudSkyAt(ServerNow());
        CloudMaterial->SetVectorParameterValue(TEXT("WindOffset"), FLinearColor(Sky.Drift.X, Sky.Drift.Y, Sky.Phase, 0));
        CloudMaterial->SetVectorParameterValue(TEXT("Front"), FLinearColor(Sky.FrontDir.X, Sky.FrontDir.Y, Sky.FrontKm, Sky.FrontWidthKm));
    }
    else if (CloudMaterial && Changed(LastCloud, W.Cloud + 2.0f * W.Rain, 0.004f))
    {
        // The engine material's own scale: coverage -0.25 gives a clear sky, more than 1 an overcast
        // one; the density is the thickness of the heaps.
        // A gentle curve: clear skies stay clear, "partly cloudy" gives separate heaps with blue
        // between them, and only an almost fully cloudy day closes the deck.
        float CloudAmount = FMath::Pow(FMath::Clamp(W.Cloud, 0.0f, 1.0f), 1.6f);
        CloudMaterial->SetScalarParameterValue(TEXT("Cloud_GlobalCoverage"), FMath::Lerp(-0.42f, 0.32f, W.Cloud) + 0.30f * S(0.5f, 1.0f, W.Rain));
        CloudMaterial->SetScalarParameterValue(TEXT("Cloud_GlobalDensity"), FMath::Lerp(0.014f, 0.045f, S(0.05f, 0.95f, W.Cloud)));
        CloudMaterial->SetScalarParameterValue(TEXT("StormClouds"), S(0.3f, 1.0f, W.Rain));
    }

    // Cloud shadows: full under a broken sky, fainter as it closes over (the sun is dimmed then
    // anyway, and a fully shadowed overcast would turn the day to dusk).
    if (Changed(LastCloudShadow, 1.0f - 0.7f * S(0.6f, 0.95f, Overcast), 0.05f))
    {
        Sun->CloudShadowOnSurfaceStrength = LastCloudShadow;
        Sun->MarkRenderStateDirty();
    }
    // Lightning: the flash lights the storm from high above the strike, the thunder follows after
    // the time the sound takes to come (343 m/s), louder and sharper the closer the strike.
    if (Lightning)
    {
        const float Now = ServerNow();
        FStrike Strike;
        const bool bStrike = StrikeAt(Now, Strike);
        if (bStrike && Strike.Time != LastStrikeTime)
        {
            LastStrikeTime = Strike.Time;
            const int32 Kind = Strike.DistanceKm < 3.5f ? 0 : Strike.DistanceKm < 8.0f ? 1 : 2;
            PendingThunder.Add({ Strike.Time + Strike.DistanceKm / 0.343f, FMath::Clamp(2.5f / (Strike.DistanceKm + 0.5f), 0.2f, 1.0f),
                FMath::FRandRange(0.9f, 1.1f), Kind });
            Lightning->SetWorldRotation(FRotator(-55.0f, Strike.Yaw + 180.0f, 0));
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LIGHTNING at %.2f s, %.1f km"), Strike.Time, Strike.DistanceKm);
        }
        const float Flash = FlashAt(Now);
        const bool bFlash = Flash > 0.003f;
        if (bFlash || Lightning->IsVisible())
        {
            Lightning->SetVisibility(bFlash);
            Lightning->SetIntensity(Flash * 0.8f);
            if (SkyLight) SkyLight->SetIntensity(LastSky * (1 + Flash));
            if (CloudMaterial && bOwnClouds)
            {
                const FVector2D At = FVector2D(FRotator(0, Strike.Yaw, 0).Vector()) * Strike.DistanceKm;
                // The eye is used to the dark at night: the same flash is far brighter to it then.
                CloudMaterial->SetVectorParameterValue(TEXT("Flash"), FLinearColor(At.X, At.Y, Flash * FMath::Lerp(0.8f, 0.5f, Day), 0));
            }
        }
        for (int32 I = PendingThunder.Num() - 1; I >= 0; --I)
            if (Now >= PendingThunder[I].At)
            {
                const FThunder T = PendingThunder[I];
                PendingThunder.RemoveAtSwap(I);
                if (ThunderSounds.IsValidIndex(T.Kind) && ThunderSounds[T.Kind])
                    UGameplayStatics::PlaySound2D(this, ThunderSounds[T.Kind], T.Volume, T.Pitch);
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_THUNDER kind %d volume %.2f"), T.Kind, T.Volume);
            }
    }

    // Wind for the Dynamic Wind plants, the materials and the rain.
    const FVector WindDir = FRotator(0, W.WindYaw, 0).Vector();
    // The plugin exports neither the subsystem class nor a usable header, so its Blueprint
    // function UpdateWindParameters is called through reflection.
    static UClass* DynamicWindClass = FindObject<UClass>(nullptr, TEXT("/Script/DynamicWind.DynamicWindSubsystem"));
    if (UObject* DynamicWind = DynamicWindClass ? World->GetSubsystemBase(DynamicWindClass) : nullptr)
        if (UFunction* Update = DynamicWind->FindFunction(TEXT("UpdateWindParameters")))
        {
            FDynamicWindParameters P;
            P.WindDirection = WindDir;
            P.WindSpeed = 6.0f + 40.0f * W.Wind;
            P.WindAmplitude = 0.1f + 2.4f * W.Wind;
            DynamicWind->ProcessEvent(Update, &P);
        }
    // The ground gets wet in the rain and dries slowly afterwards.
    Wetness = FMath::Clamp(Wetness + DeltaSeconds * (W.Rain > 0.05f ? 0.02f * W.Rain : -0.004f * (1 - 0.6f * W.Cloud)), 0.0f, 1.0f);
    if (WeatherParameters)
        if (UMaterialParameterCollectionInstance* MPC = World->GetParameterCollectionInstance(WeatherParameters))
        {
            MPC->SetScalarParameterValue(TEXT("Rain"), W.Rain);
            MPC->SetScalarParameterValue(TEXT("Wind"), W.Wind);
            MPC->SetScalarParameterValue(TEXT("WindDirX"), WindDir.X);
            MPC->SetScalarParameterValue(TEXT("WindDirY"), WindDir.Y);
            MPC->SetScalarParameterValue(TEXT("Wetness"), Wetness);
            MPC->SetScalarParameterValue(TEXT("Daylight"), FMath::Max(Day, 0.05f));
            // Direct sunshine: light patterns (caustics) on the bed only show in sun, not under cloud or in fog.
            MPC->SetScalarParameterValue(TEXT("Sunshine"), Day * (1 - S(0.35f, 0.9f, W.Cloud)) * (1 - S(0.3f, 0.8f, EffectiveFog())) * (1 - W.Rain));
        }

    // Rain: nine identical 12 m tiles on a world grid around the camera, so the streaks stay put
    // in the world while the angler walks.
    FVector Camera = FVector::ZeroVector;
    if (APlayerController* PC = World->GetFirstPlayerController())
        if (PC->PlayerCameraManager) Camera = PC->PlayerCameraManager->GetCameraLocation();
    RainTiles->SetVisibility(W.Rain > 0.01f);
    if (W.Rain > 0.01f)
    {
        constexpr float Tile = 1200.0f;
        const float BaseX = FMath::FloorToFloat(Camera.X / Tile) * Tile, BaseY = FMath::FloorToFloat(Camera.Y / Tile) * Tile;
        for (int32 I = 0; I < 9; ++I)
            RainTiles->UpdateInstanceTransform(I, FTransform(FVector(BaseX + (I % 3 - 1) * Tile, BaseY + (I / 3 - 1) * Tile, Camera.Z - 400)), true, I == 8, true);
    }

    // Ambience: rain, wind and crickets on warm dry nights (no birds: they are not wanted here).
    // Rain: single drops in a drizzle, giving way to the dense hiss of a downpour.
    if (RainLightSound) RainLightSound->SetVolumeMultiplier(FMath::Max(0.001f, 0.7f * S(0.0f, 0.25f, W.Rain) * (1 - 0.6f * S(0.4f, 1.0f, W.Rain))));
    if (RainHeavySound) RainHeavySound->SetVolumeMultiplier(FMath::Max(0.001f, 0.85f * S(0.2f, 1.0f, W.Rain)));
    if (WindSound) WindSound->SetVolumeMultiplier(0.12f + 0.75f * W.Wind);
    if (CricketSound) CricketSound->SetVolumeMultiplier(FMath::Max(0.001f, 0.45f * Night * (1 - W.Rain) * S(10.0f, 15.0f, TemperatureC())));
}
