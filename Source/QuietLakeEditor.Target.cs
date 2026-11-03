using UnrealBuildTool;
public class QuietLakeEditorTarget : TargetRules
{
    public QuietLakeEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        ExtraModuleNames.Add("QuietLake");
    }
}
