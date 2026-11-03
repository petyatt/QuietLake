using UnrealBuildTool;
public class QuietLakeTarget : TargetRules
{
    public QuietLakeTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        ExtraModuleNames.Add("QuietLake");
    }
}
