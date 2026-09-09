using System.Text.Json;

namespace WiiCompiled.Setup.Windows;

internal static class ProductOwnership
{
    public static void Ensure(string directory, bool allowEmpty = false)
    {
        for (var current = new DirectoryInfo(Path.GetFullPath(directory)); current is not null; current = current.Parent)
            if (current.Exists && (current.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("Use a VR installation directory without junctions or symbolic links.");
        if (!Directory.Exists(directory))
        {
            if (allowEmpty) return;
            throw new InvalidDataException("WiiCompiled OpenXR VR is not installed in this directory.");
        }
        // A junction must not let an apparently separate VR destination overwrite another install.
        if (allowEmpty && !Directory.EnumerateFileSystemEntries(directory).Any()) return;
        var statePath = Path.Combine(directory, InstalledLayout.InstallStateFileName);
        if (File.Exists(statePath))
        {
            using var state = JsonDocument.Parse(File.ReadAllText(statePath));
            if (state.RootElement.TryGetProperty("ProductId", out var id) && id.GetString() == ProductInfo.Id)
                return;
        }
        throw new InvalidDataException("This folder belongs to a normal or unidentified WiiCompiled installation. " +
            "Choose a separate empty folder for OpenXR VR; existing files have not been changed.");
    }
}
