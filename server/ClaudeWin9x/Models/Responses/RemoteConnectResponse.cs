using System.Text.Json.Serialization;

namespace ClaudeWin9xServer.Models.Responses;

public record RemoteConnectResponse
{
    [JsonPropertyName("session_id")]
    public required string SessionId { get; init; }
}
