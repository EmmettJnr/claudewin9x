using System.Text.Json.Serialization;

namespace ClaudeWin9xServer.Models.Requests;

public record RemoteConnectRequest
{
    [JsonPropertyName("windows_version")]
    public string? WindowsVersion { get; init; }

    [JsonPropertyName("computer_name")]
    public string? ComputerName { get; init; }

    [JsonPropertyName("allow_all")]
    public bool AllowAll { get; init; }
}
