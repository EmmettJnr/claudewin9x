using ClaudeWin9xServer.Models.Responses;

namespace ClaudeWin9xServer.Services.Interfaces;

public interface IFileSystemService
{
    Task<FileOpResult?> ListDirectoryAsync(string path, CancellationToken cancellationToken = default);
    Task<(string? Content, bool Truncated, int TotalSize)?> ReadFileAsync(string path, int? maxSize = null, CancellationToken cancellationToken = default);
    Task<bool> WriteFileAsync(string path, string content, string? sessionId = null, CancellationToken cancellationToken = default);
    /// <summary>Queues any client file operation (list, read, write, mkdir, fetch, send) without approval.</summary>
    Task<FileOpResult?> QueueOperationAsync(string operation, string path, string? content, TimeSpan timeout, CancellationToken cancellationToken = default);
    FileOperation? PollPendingOperation();
    void SubmitResult(FileOpResult result);
    (string ZipPath, long Size)? CreateBundle(string sourcePath, string? outputName, string? allowedBasePath = null);
}
