using System.Text.Json;
using System.Text.Json.Serialization;

namespace Digi21.CrsKit.Interop;

/// <summary>A known source of grid files. GridFileNameSubstring is a case-insensitive substring looked up in a grid file name as
/// the EPSG catalogue names it; SourcePageUrl is the page of the source; Attribution is the text that the source licence requires
/// the application to display. DownloadUrlPrefix is the URL prefix that GetDirectDownloadUrl uses to construct direct download
/// URLs. DownloadUrlsByGridFileName maps a grid file name to its direct download URL and takes precedence over
/// DownloadUrlPrefix. The JSON keys of grid-sources.json are match, name, url, attribution, fileBase and files.</summary>
public sealed record GridDownloadSource(
    [property: JsonPropertyName("match")] string GridFileNameSubstring,
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("url")] string SourcePageUrl,
    [property: JsonPropertyName("attribution")] string Attribution = "",
    [property: JsonPropertyName("fileBase")] string DownloadUrlPrefix = "",
    [property: JsonPropertyName("files")] IReadOnlyDictionary<string, string>? DownloadUrlsByGridFileName = null);

/// <summary>Catalogue of the sources of grid files, read from data/grid-sources.json of the crskit repository. Each application
/// passes the path of the file where the catalogue keeps its last downloaded copy.</summary>
public sealed class GridDownloadCatalog(string cacheFilePath)
{
    public const string RemoteUrl = "https://raw.githubusercontent.com/digi21/crskit/main/data/grid-sources.json";

    private static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };

    // InitializeAsync replaces the whole array, so Find and GetDirectDownloadUrl always read a complete list.
    private volatile GridDownloadSource[] sources = [];

    /// <summary>Loads the copy at cacheFilePath and then the remote catalogue, with a timeout of 10 s. The remote catalogue
    /// replaces the copy at cacheFilePath when it has at least one valid entry. A failed cache read, catalogue download or JSON
    /// parse does not clear the sources already loaded.</summary>
    public async Task InitializeAsync()
    {
        TryLoad(await ReadCacheAsync());
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            var json = await http.GetStringAsync(RemoteUrl);
            if (TryLoad(json))
                await WriteCacheAsync(json);
        }
        catch (Exception exception) when (exception is HttpRequestException or TaskCanceledException or IOException)
        {
        }
    }

    /// <summary>Returns the first source whose Match is contained in gridFileName, or null if no source matches.</summary>
    public GridDownloadSource? Find(string gridFileName) =>
        string.IsNullOrWhiteSpace(gridFileName) ? null
            : sources.FirstOrDefault(source => TrimGridFileNameAndRemoveGzipSuffix(gridFileName).Contains(source.GridFileNameSubstring, StringComparison.OrdinalIgnoreCase));

    /// <summary>Returns the direct download URL of gridFileName, or null if no source matches or the source only has a page.</summary>
    public string? GetDirectDownloadUrl(string gridFileName)
    {
        if (Find(gridFileName) is not { } source)
            return null;
        var name = TrimGridFileNameAndRemoveGzipSuffix(gridFileName);
        if (source.DownloadUrlsByGridFileName?.FirstOrDefault(file => string.Equals(file.Key, name, StringComparison.OrdinalIgnoreCase)) is { Value: { } url })
            return url;
        return string.IsNullOrEmpty(source.DownloadUrlPrefix) ? null : source.DownloadUrlPrefix.TrimEnd('/') + "/" + name;
    }

    /// <summary>Removes a ".gz" suffix: the EPSG catalogue names some grids with the suffix of their compressed download, and
    /// crskit reads the uncompressed file.</summary>
    public static string TrimGridFileNameAndRemoveGzipSuffix(string gridFileName)
    {
        var name = gridFileName.Trim();
        return name.EndsWith(".gz", StringComparison.OrdinalIgnoreCase) ? name[..^3] : name;
    }

    /// <summary>Returns the response Content-Length in bytes for a successful HEAD request to url; returns null if the header is
    /// absent, the response has a non-success status, an HttpRequestException occurs or the request times out.</summary>
    public static async Task<long?> GetDownloadSizeBytesAsync(string url)
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
            using var response = await http.SendAsync(new HttpRequestMessage(HttpMethod.Head, url));
            return response.IsSuccessStatusCode ? response.Content.Headers.ContentLength : null;
        }
        catch (Exception exception) when (exception is HttpRequestException or TaskCanceledException)
        {
            return null;
        }
    }

    /// <summary>Downloads url to destinationFilePath through a ".part" file that replaces destinationFilePath only when the
    /// download completes, and reports the downloaded fraction, from 0 to 1, when the response Content-Length is greater than
    /// zero. Throws on failure.</summary>
    public static async Task DownloadFileAsync(string url, string destinationFilePath, IProgress<double>? progress = null,
        CancellationToken cancellation = default)
    {
        if (Path.GetDirectoryName(destinationFilePath) is { Length: > 0 } directoryPath)
            Directory.CreateDirectory(directoryPath);
        var partialFilePath = destinationFilePath + ".part";

        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        using var response = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellation);
        response.EnsureSuccessStatusCode();
        var totalBytes = response.Content.Headers.ContentLength;

        await using (var source = await response.Content.ReadAsStreamAsync(cancellation))
        await using (var destination = File.Create(partialFilePath))
        {
            var buffer = new byte[81920];
            long readBytes = 0;
            int count;
            while ((count = await source.ReadAsync(buffer, cancellation)) > 0)
            {
                await destination.WriteAsync(buffer.AsMemory(0, count), cancellation);
                readBytes += count;
                if (totalBytes is > 0)
                    progress?.Report((double)readBytes / totalBytes.Value);
            }
        }

        File.Move(partialFilePath, destinationFilePath, overwrite: true);
    }

    // Replaces the active list with the valid entries of json; returns false, without changing the list, if there are none.
    private bool TryLoad(string? json)
    {
        if (string.IsNullOrWhiteSpace(json))
            return false;
        try
        {
            var validSources = (JsonSerializer.Deserialize<GridDownloadSource[]>(json, JsonOptions) ?? []).Where(IsValid).ToArray();
            if (validSources.Length == 0)
                return false;
            sources = validSources;
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    // Requires a nonempty GridFileNameSubstring and Name, an HTTPS SourcePageUrl, an empty or HTTPS DownloadUrlPrefix, and HTTPS URLs in
    // DownloadUrlsByGridFileName when DownloadUrlsByGridFileName is present.
    private static bool IsValid(GridDownloadSource source) =>
        !string.IsNullOrWhiteSpace(source.GridFileNameSubstring) && !string.IsNullOrWhiteSpace(source.Name) && IsHttps(source.SourcePageUrl) &&
        (string.IsNullOrEmpty(source.DownloadUrlPrefix) || IsHttps(source.DownloadUrlPrefix)) &&
        (source.DownloadUrlsByGridFileName is null || source.DownloadUrlsByGridFileName.Values.All(IsHttps));

    private static bool IsHttps(string url) => Uri.TryCreate(url, UriKind.Absolute, out var uri) && uri.Scheme == Uri.UriSchemeHttps;

    private async Task<string?> ReadCacheAsync()
    {
        try
        {
            return File.Exists(cacheFilePath) ? await File.ReadAllTextAsync(cacheFilePath) : null;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private async Task WriteCacheAsync(string json)
    {
        try
        {
            if (Path.GetDirectoryName(cacheFilePath) is { Length: > 0 } directoryPath)
                Directory.CreateDirectory(directoryPath);
            await File.WriteAllTextAsync(cacheFilePath, json);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
        }
    }
}
