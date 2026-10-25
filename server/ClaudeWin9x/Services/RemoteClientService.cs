using ClaudeWin9xServer.Infrastructure;

namespace ClaudeWin9xServer.Services;

public record RemoteClient
{
    public required string SessionId { get; init; }
    public string? WindowsVersion { get; init; }
    public string? ComputerName { get; init; }
    public string? Address { get; init; }
    public DateTime ConnectedAt { get; init; }
    public DateTime LastSeen { get; init; }
    public bool AllowAll { get; init; }
}

/// <summary>
/// Tracks the remote control client (ClaudeWin9x-Remote) on the retro machine. Only one is
/// connected at a time; a new connection replaces the old one.
/// </summary>
public class RemoteClientService(ILogger<RemoteClientService> logger, TimeSpan? staleAfter = null)
{
    private readonly TimeSpan _staleAfter = staleAfter ?? TimeSpan.FromSeconds(30);
    private readonly Lock _lock = new();
    private RemoteClient? _client;
    private int _inFlight;

    public RemoteClient Connect(string? windowsVersion, string? computerName, string? address, bool allowAll)
    {
        var now = DateTime.UtcNow;
        var client = new RemoteClient
        {
            SessionId = IdGenerator.NewId(),
            WindowsVersion = windowsVersion,
            ComputerName = computerName,
            Address = address,
            ConnectedAt = now,
            LastSeen = now,
            AllowAll = allowAll
        };

        lock (_lock)
        {
            if (_client != null)
            {
                logger.LogInformation("Remote client {SessionId} replaced by a new connection", _client.SessionId);
            }
            _client = client;
        }

        logger.LogInformation("Remote client {SessionId} connected: {ComputerName} ({WindowsVersion}) from {Address}",
            client.SessionId, computerName, windowsVersion, address);
        return client;
    }

    /// <summary>Returns false if the session is not the current one, so the client reconnects.</summary>
    public bool Heartbeat(string sessionId, bool allowAll)
    {
        lock (_lock)
        {
            if (_client?.SessionId != sessionId)
            {
                return false;
            }

            if (_client.AllowAll != allowAll)
            {
                logger.LogInformation("Remote client {SessionId}: allow anything {State}",
                    sessionId, allowAll ? "on" : "off");
            }
            _client = _client with { LastSeen = DateTime.UtcNow, AllowAll = allowAll };
            return true;
        }
    }

    public bool Disconnect(string sessionId)
    {
        lock (_lock)
        {
            if (_client?.SessionId != sessionId)
            {
                return false;
            }
            _client = null;
        }

        logger.LogInformation("Remote client {SessionId} disconnected", sessionId);
        return true;
    }

    /// <summary>The last client to connect, whether or not it is still responding.</summary>
    public RemoteClient? LastKnown
    {
        get
        {
            lock (_lock)
            {
                return _client;
            }
        }
    }

    /// <summary>
    /// The client if it is responding. It counts as connected while an operation is in flight,
    /// because it does not send heartbeats while running a long command.
    /// </summary>
    public RemoteClient? Current
    {
        get
        {
            lock (_lock)
            {
                if (_client == null)
                {
                    return null;
                }
                return _inFlight > 0 || DateTime.UtcNow - _client.LastSeen < _staleAfter ? _client : null;
            }
        }
    }

    public IDisposable BeginOperation()
    {
        Interlocked.Increment(ref _inFlight);
        return new OperationScope(this);
    }

    private sealed class OperationScope(RemoteClientService owner) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
            {
                Interlocked.Decrement(ref owner._inFlight);
            }
        }
    }
}
