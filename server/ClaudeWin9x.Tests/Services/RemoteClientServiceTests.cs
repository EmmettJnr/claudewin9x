using Microsoft.Extensions.Logging;
using NSubstitute;
using Shouldly;
using ClaudeWin9xServer.Services;

namespace ClaudeWin9xServer.Tests.Services;

public class RemoteClientServiceTests
{
    private readonly ILogger<RemoteClientService> _logger = Substitute.For<ILogger<RemoteClientService>>();

    [Fact]
    public void Connect_ReplacesPreviousClient()
    {
        var service = new RemoteClientService(_logger);
        var first = service.Connect("Windows 98", "OLD", "10.0.0.1", false);
        var second = service.Connect("Windows 98", "NEW", "10.0.0.2", false);

        service.Current!.SessionId.ShouldBe(second.SessionId);
        service.Heartbeat(first.SessionId, false).ShouldBeFalse();
        service.Heartbeat(second.SessionId, false).ShouldBeTrue();
    }

    [Fact]
    public void Heartbeat_UpdatesAllowAll()
    {
        var service = new RemoteClientService(_logger);
        var client = service.Connect("Windows 98", "PC", null, false);

        service.Heartbeat(client.SessionId, true).ShouldBeTrue();

        service.Current!.AllowAll.ShouldBeTrue();
    }

    [Fact]
    public void Current_WhenStale_ReturnsNullButKeepsLastKnown()
    {
        var service = new RemoteClientService(_logger, TimeSpan.Zero);
        service.Connect("Windows 98", "PC", null, false);

        service.Current.ShouldBeNull();
        service.LastKnown.ShouldNotBeNull();
    }

    [Fact]
    public void Current_WhenStaleButOperationInFlight_ReturnsClient()
    {
        var service = new RemoteClientService(_logger, TimeSpan.Zero);
        service.Connect("Windows 98", "PC", null, false);

        using (service.BeginOperation())
        {
            service.Current.ShouldNotBeNull();
        }

        service.Current.ShouldBeNull();
    }

    [Fact]
    public void Disconnect_OnlyForCurrentSession()
    {
        var service = new RemoteClientService(_logger);
        var client = service.Connect("Windows 98", "PC", null, false);

        service.Disconnect("other").ShouldBeFalse();
        service.Disconnect(client.SessionId).ShouldBeTrue();
        service.LastKnown.ShouldBeNull();
    }
}
