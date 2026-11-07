#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeFish.h"
#include "LakeWeather.generated.h"

/** One weather condition; every value 0..1 except the wind direction. */
USTRUCT()
struct FLakeWeatherState
{
    GENERATED_BODY()
    UPROPERTY() float Cloud = 0.3f;    // clear .. overcast
    UPROPERTY() float Fog = 0.6f;      // none .. dense
    UPROPERTY() float Rain = 0.0f;     // none .. downpour
    UPROPERTY() float Wind = 0.05f;    // calm .. strong (12 m/s)
    UPROPERTY() float WindYaw = 20.0f; // direction the wind blows towards, degrees

    static FLakeWeatherState Blend(const FLakeWeatherState& A, const FLakeWeatherState& B, float T);
};

/**
 * Game clock and weather. The server keeps the clock and picks the weather; both replicate as a
 * few numbers (a clock origin and a blend between two weather states on the server clock), so
 * every machine computes the same time and weather and drives its own sky from them:
 * sun and moon, sky light, height fog and morning mist, volumetric clouds, the plants' wind,
 * MPC_Weather for the materials (water waves and rain rings, grass sway, wet ground, rain), the
 * rain streaks around the camera, and the ambience (drizzle and downpour layers, wind, birds by day, crickets at night).
 */
UCLASS()
class QUIETLAKE_API ALakeWeather : public AActor
{
    GENERATED_BODY()
public:
    ALakeWeather();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** Real minutes per game day. */
    static constexpr float DayMinutes = 48.0f;
    static ALakeWeather* Get(const UWorld* World);

    /** Hours since midnight, 0..24. */
    float Hours() const;
    FLakeWeatherState Current() const;
    /** Fog including the mist that lies on the water on calm mornings and evenings. */
    float EffectiveFog() const;
    /** 0 at night, 1 in daylight (sun above the horizon). */
    float Daylight() const;
    float TemperatureC() const;
    /** Wind speed in m/s. */
    float WindSpeed() const { return Current().Wind * 12.0f; }
    /** The wind at the water (cm/s, blowing towards WindYaw) with slow gusts: what the line feels. */
    FVector WindVelocity() const;
    /** "Туман · штиль" and the like, for the HUD. */
    FString Describe() const;
    /** Direction towards the sun (world space, X east, -Y north) and its elevation in degrees. */
    FVector SunDirection(float* OutElevation = nullptr) const;

    /** Water temperature: follows the air slowly, a large pond keeps its warmth. */
    float WaterTempC() const { return WaterTemp; }
    /** Barometric pressure (mm Hg) and its change per game hour (falls ahead of rain, rises after). */
    float PressureMmHg() const { return Pressure; }
    float PressureTrend() const { return Trend; }
    /** 0..1 right after a sharp change of the weather, fading over three game hours. */
    float Shock() const;
    /** Game hours since the weather last settled. */
    float StableHours() const;
    /** Previews: the weather now has been steady for GameHours already (the water has had time to layer). */
    void SetSteadyFor(float GameHours) { BlendStart = ServerNow() - BlendSeconds - GameHours / GameHoursPerSecond(); ForceNetUpdate(); }
    /** The sky's clouds at a server time, the same on every machine (worked out from replicated
     *  numbers only): how far they have sailed with the wind and reshaped, and where the front of
     *  the coming weather is. Ahead of the front (downwind) the sky is the old weather, behind it
     *  the new one; it leaves the upwind horizon when the change is set and passes over the lake
     *  in the middle of the blend. */
    struct FCloudSky
    {
        FVector2D Drift = FVector2D::ZeroVector;   // km
        float Phase = 0;
        FVector2D FrontDir = FVector2D(1, 0);       // the way the front moves (downwind of the new weather)
        float FrontKm = 1e4f;                       // front line from the lake along FrontDir (negative: still coming)
        float FrontWidthKm = 10;                    // half the width of the change across it
    };
    FCloudSky CloudSkyAt(float ServerSeconds) const;
    /** How far away the front starts: beyond the horizon of the cloud layer. */
    static constexpr float FrontStartKm = 45.0f;
    /** How much of a thunderstorm the weather is now, 0..1: lightning comes only above 0. */
    float StormAmount() const;
    /** A lightning strike: when, how far from the lake and from which way, its own random seed. */
    struct FStrike { float Time = -1; float DistanceKm = 5; float Yaw = 0; int32 Seed = 0; };
    /** The latest strike at most a second before ServerSeconds, if any. Worked out from the server
     *  time and the replicated weather alone, so every machine sees the same flashes. */
    bool StrikeAt(float ServerSeconds, FStrike& Out) const;
    /** Brightness of the flash at a moment (0 dark .. ~1 a close strike at its peak). */
    float FlashAt(float ServerSeconds) const;
    /** Strikes per second at full storm. */
    static constexpr float StrikeRate = 0.08f;
    /** Everything the fish react to, for LakeFish::Activity. */
    FFishConditions FishConditions() const;

    // --- server ---
    void SetHours(float NewHours);
    /** Blend to State over BlendSeconds, starting after Delay (a front: the pressure falls first). */
    void SetWeather(const FLakeWeatherState& State, float BlendSeconds, float Delay = 0);
    /** This weather at once, with no blend (previews and tests). */
    void SetWeatherNow(const FLakeWeatherState& State);

private:
    float ServerNow() const;
    void PickNextWeather();
    void ApplyToWorld(float DeltaSeconds);
    /** Before the weather is changed: remember where the clouds are now, so their drift goes on
     *  smoothly from here under the new wind. */
    void MarkChange();
    /** Cloud drift (km x, y) and reshaping phase (z) at a server time. */
    FVector SkyAt(float ServerSeconds) const;

    UPROPERTY(Replicated) float ClockOriginServer = 0;
    UPROPERTY(Replicated) float ClockOriginHours = 5.5f;
    UPROPERTY(Replicated) FLakeWeatherState From;
    UPROPERTY(Replicated) FLakeWeatherState To;
    UPROPERTY(Replicated) float BlendStart = -100;
    UPROPERTY(Replicated) float BlendSeconds = 1;
    UPROPERTY(Replicated) float WaterTemp = 20.5f;
    UPROPERTY(Replicated) float Pressure = 758.0f;
    UPROPERTY(Replicated) float Trend = 0.0f;
    /** When the last change was set (the front leaves the horizon) and the sky drift then. */
    UPROPERTY(Replicated) float ChangeAt = 0;
    UPROPERTY(Replicated) FVector SkyAtChange = FVector::ZeroVector;
    float GameHoursPerSecond() const { return 24.0f / (DayMinutes * 60.0f); }
    float NextChange = 0;
    FRandomStream Random;

    // Per machine (not on a dedicated server).
    bool bBound = false;
    bool bLoggedClock = false;
    // Last values given to the sky, so it is only touched when something visibly changes.
    FVector LastSunDir = FVector::ZeroVector;
    /** Which directional light is the forward shading one: 1 the sun, 0 the moon (-1: not set yet). */
    int32 ForwardMain = -1;
    float LastSun = -1, LastSunTemp = -1, LastSunAngle = -1, LastMoon = -1, LastSky = -1, LastMie = -1, LastRayleigh = -1, LastFog = -1, LastCloud = -1, LastSunInFog = -1, LastDayTint = -1;
    float Wetness = 0;
    UPROPERTY() TObjectPtr<class UDirectionalLightComponent> Sun;
    UPROPERTY() TObjectPtr<class UDirectionalLightComponent> Moon;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> NightSky;
    UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> NightSkyMaterial;
    UPROPERTY() TObjectPtr<class USkyAtmosphereComponent> Atmosphere;
    UPROPERTY() TObjectPtr<class USkyLightComponent> SkyLight;
    UPROPERTY() TObjectPtr<class UExponentialHeightFogComponent> Fog;
    UPROPERTY() TObjectPtr<class UVolumetricCloudComponent> Clouds;
    UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> CloudMaterial;
    float PreviewFrameSeconds = 0;           // previews: frame time before each shot
    int32 PreviewFrames = 0;
    bool bMeasureFrames = false;
    bool bOwnClouds = false;                 // M_LakeClouds (else the engine's sample clouds)
    int32 LastSyncLog = -1;                  // QUIET_LAKE_CLOUD_SYNC: last logged half minute
    float LastCloudShadow = -1;
    // Lightning: a directional light that only shows during a flash (the clouds are lit by
    // atmosphere lights only), and the thunder each strike sends after it.
    UPROPERTY() TObjectPtr<class UDirectionalLightComponent> Lightning;
    UPROPERTY() TArray<TObjectPtr<class USoundWave>> ThunderSounds;
    struct FThunder { float At; float Volume; float Pitch; int32 Kind; };
    TArray<FThunder> PendingThunder;
    float LastStrikeTime = -1;
    float PreviewStrikeAt = -1;              // previews: a close strike at this server time
    UPROPERTY() TObjectPtr<class UMaterialParameterCollection> WeatherParameters;
    UPROPERTY() TObjectPtr<class UHierarchicalInstancedStaticMeshComponent> RainTiles;
    UPROPERTY() TObjectPtr<class UAudioComponent> RainLightSound;
    UPROPERTY() TObjectPtr<class UAudioComponent> RainHeavySound;
    UPROPERTY() TObjectPtr<class UAudioComponent> CricketSound;
    UPROPERTY() TObjectPtr<class UAudioComponent> WindSound;
    UPROPERTY() TObjectPtr<class APostProcessVolume> Post;
    /** The weather's own unbound volume for exposure: the world already has one for the look and
     *  one for the underwater murk (with weight 0 on land), and writing the exposure into those
     *  either did nothing or was overridden. Higher priority than the look, lower than the murk. */
    UPROPERTY() TObjectPtr<class APostProcessVolume> ExposureVolume;
};
