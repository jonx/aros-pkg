// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.Mvc.Testing;
using Portal.Admin;
using Portal.Push;

namespace Portal.Tests;

/// <summary>
/// A push the size of a real Pkg release: 46 MB in files larger than one part,
/// published by the portal running Pkg itself, as a publisher's push does. The
/// first such push answered HTTP 500 with an empty body, which told nobody
/// anything; the size is what this holds to, and the empty body is what the
/// second test here forbids for every machine-facing address.
/// </summary>
public class BigPushTests
{
    sealed class Site : WebApplicationFactory<Program>
    {
        public readonly string Data = Directory.CreateTempSubdirectory("portal-big-").FullName;
        public readonly string Key, AdminKey;
        readonly string keys, adminConfig;

        public Site()
        {
            (Key, keys) = PublisherKeys.Create("publisher", "*", files: true);
            (AdminKey, adminConfig) = AdminKeys.Create("owner");
        }

        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder b)
        {
            b.UseSetting("Portal:DataDir", Data);
            b.UseSetting("Portal:Keys", keys);
            b.UseSetting("Portal:AdminKeys", adminConfig);
            b.UseSetting("Portal:PkgPath", Pkg);          // the real Pkg checks the real push
        }
        public HttpClient Https() => CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost") });
    }

    /// The Pkg this tree builds, which the portal runs to check what it is given.
    static readonly string Pkg = Find();
    static string Find()
    {
        var dir = AppContext.BaseDirectory;
        for (var d = new DirectoryInfo(dir); d is not null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "build", "pkg"))) return Path.Combine(d.FullName, "build", "pkg");
        return "";
    }

    static async Task<(HttpStatusCode Status, string Body)> Send(HttpClient c, string key, HttpMethod m, string url,
                                                                 HttpContent content, string? range = null)
    {
        var req = new HttpRequestMessage(m, url) { Content = content };
        req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", key);
        if (range is not null) req.Content.Headers.TryAddWithoutValidation("Content-Range", range);
        var r = await c.SendAsync(req);
        return (r.StatusCode, await r.Content.ReadAsStringAsync());
    }

    [Fact]
    public async Task A_release_sized_push_goes_through_and_the_portal_checks_it()
    {
        Skip.If(Pkg.Length == 0, "build/pkg is not built");
        using var f = new Site();
        var c = f.Https();
        var work = Directory.CreateTempSubdirectory("portal-big-src-").FullName;
        try
        {
            // Two programs of 23 MB: a push of 46 MB, each file larger than one part.
            var drawer = Path.Combine(work, "Big", "C");
            Directory.CreateDirectory(drawer);
            var bytes = new byte[23 * 1024 * 1024];
            RandomNumberGenerator.Fill(bytes.AsSpan(0, 1 << 20));       // not compressible away
            for (int i = 1 << 20; i < bytes.Length; i += 1 << 20) Array.Copy(bytes, 0, bytes, i, Math.Min(1 << 20, bytes.Length - i));
            Encoding.ASCII.GetBytes("\0$VER: Big 1.0 (20.9.2026)\0").CopyTo(bytes, 64);
            await File.WriteAllBytesAsync(Path.Combine(drawer, "Big"), bytes);
            await File.WriteAllBytesAsync(Path.Combine(drawer, "Big2"), bytes);

            var key = Path.Combine(work, "k");
            Assert.Equal(0, await Run(Pkg, ["KEYGEN", "FILE", key]));
            var channel = Path.Combine(work, "ch");
            Assert.Equal(0, await Run(Pkg, ["PUBLISH", Path.Combine(work, "Big"), "CHANNEL", channel,
                                            "NAME", "big", "VERSION", "1.0", "ARCH", "generic", "KIND", "data", "SIGN", key]));

            // plan, as PUSH sends it: every file with its digest and size
            var files = Directory.EnumerateFiles(channel, "*", SearchOption.AllDirectories)
                .Select(p => (Rel: Path.GetRelativePath(channel, p).Replace('\\', '/'), Path: p)).ToList();
            var plan = new StringBuilder();
            foreach (var (rel, path) in files.Where(x => x.Rel != "index"))
                plan.Append($"{rel} {Digest(path)} {new FileInfo(path).Length}\n");
            var (status, answer) = await Send(c, f.Key, HttpMethod.Post, "/big/_push/plan", new StringContent(plan.ToString()));
            Assert.Equal(HttpStatusCode.OK, status);
            var needed = answer.Split('\n').Where(l => l.StartsWith("need: ", StringComparison.Ordinal)).Select(l => l[6..].Trim()).ToList();
            Assert.Contains(needed, n => n.EndsWith(".pkg", StringComparison.Ordinal));

            // the files, the large one in parts, as PUSH does over a slow line
            const int part = 40 * 1024 * 1024;
            long sent = 0;
            foreach (var rel in needed)
            {
                var path = files.First(x => x.Rel == rel).Path;
                var all = await File.ReadAllBytesAsync(path);
                sent += all.Length;
                for (int at = 0; at < all.Length || all.Length == 0; at += part)
                {
                    var n = Math.Min(part, all.Length - at);
                    var (s, body) = all.Length <= part
                        ? await Send(c, f.Key, HttpMethod.Put, $"/big/_push/files/{rel}", new ByteArrayContent(all))
                        : await Send(c, f.Key, HttpMethod.Put, $"/big/_push/files/{rel}",
                                     new ByteArrayContent(all, at, n), $"bytes {at}-{at + n - 1}/{all.Length}");
                    Assert.Equal(HttpStatusCode.OK, s);
                    Assert.Contains("result: ", body);
                    if (all.Length == 0) break;
                }
            }
            Assert.True(sent > 46_000_000, $"the push carried {sent} bytes, not a release's worth");

            // commit: the portal runs Pkg over what it was given, and answers a record
            var index = await File.ReadAllTextAsync(Path.Combine(channel, "index"));
            var (cs, commit) = await Send(c, f.Key, HttpMethod.Post, "/big/_push/commit", new StringContent(index));
            Assert.Equal(HttpStatusCode.OK, cs);
            Assert.False(string.IsNullOrWhiteSpace(commit), "a commit never answers an empty body");
            Assert.Contains("published: big 1.0 generic", commit);
            Assert.True(File.Exists(Path.Combine(f.Data, "channels", "big", "index")));
        }
        finally { Directory.Delete(work, true); }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task A_commit_preserves_another_push_from_the_same_publisher(bool stillUploading)
    {
        Skip.If(Pkg.Length == 0, "build/pkg is not built");
        using var f = new Site();
        using var c = f.Https();
        var work = Directory.CreateTempSubdirectory("portal-overlap-").FullName;
        try
        {
            var key = Path.Combine(work, "key");
            Assert.Equal(0, await Run(Pkg, ["KEYGEN", "FILE", key]));
            var channels = new List<string>();
            foreach (var arch in new[] { "i386", "x86_64" })
            {
                var source = Path.Combine(work, arch, "source");
                Directory.CreateDirectory(source);
                await File.WriteAllTextAsync(Path.Combine(source, "data"), arch);
                var channel = Path.Combine(work, arch, "channel");
                Assert.Equal(0, await Run(Pkg, ["PUBLISH", source, "CHANNEL", channel,
                    "NAME", "overlap", "VERSION", "1.0", "ARCH", arch, "KIND", "data", "SIGN", key]));
                channels.Add(channel);
            }

            var deferred = new List<(string Rel, byte[] Bytes, int Offset)>();
            for (int i = 0; i < channels.Count; i++)
            {
                var files = Directory.EnumerateFiles(channels[i], "*", SearchOption.AllDirectories)
                    .Where(p => Path.GetFileName(p) != "index")
                    .Select(p => (Rel: Path.GetRelativePath(channels[i], p).Replace('\\', '/'), Path: p)).ToList();
                var plan = string.Concat(files.Select(p => $"{p.Rel} {Digest(p.Path)} {new FileInfo(p.Path).Length}\n"));
                var (status, answer) = await Send(c, f.Key, HttpMethod.Post, "/overlap/_push/plan", new StringContent(plan));
                Assert.Equal(HttpStatusCode.OK, status);
                Assert.DoesNotContain("refused:", answer);
                foreach (var (rel, path) in files)
                {
                    var bytes = await File.ReadAllBytesAsync(path);
                    if (i == 1 && stillUploading && rel.EndsWith(".sig", StringComparison.Ordinal))
                    {
                        deferred.Add((rel, bytes, 0)); // Planned, but not uploaded yet.
                        continue;
                    }
                    int length = i == 1 && stillUploading && rel.EndsWith(".pkg", StringComparison.Ordinal)
                        ? bytes.Length / 2 : bytes.Length;
                    var (putStatus, put) = await Send(c, f.Key, HttpMethod.Put, $"/overlap/_push/files/{rel}",
                        new ByteArrayContent(bytes, 0, length), length == bytes.Length ? null : $"bytes 0-{length - 1}/{bytes.Length}");
                    Assert.Equal(HttpStatusCode.OK, putStatus);
                    Assert.Contains(length == bytes.Length ? "result: received" : "result: partial", put);
                    if (length != bytes.Length) deferred.Add((rel, bytes, length));
                }
            }

            async Task Commit(int i, string arch)
            {
                var index = await File.ReadAllTextAsync(Path.Combine(channels[i], "index"));
                var (status, answer) = await Send(c, f.Key, HttpMethod.Post, "/overlap/_push/commit", new StringContent(index));
                Assert.Equal(HttpStatusCode.OK, status);
                Assert.Contains($"published: overlap 1.0 {arch}", answer);
                Assert.DoesNotContain("refused:", answer);
            }
            await Commit(0, "i386");
            foreach (var (rel, bytes, offset) in deferred)
            {
                var (status, answer) = await Send(c, f.Key, HttpMethod.Put, $"/overlap/_push/files/{rel}",
                    new ByteArrayContent(bytes, offset, bytes.Length - offset),
                    offset == 0 ? null : $"bytes {offset}-{bytes.Length - 1}/{bytes.Length}");
                Assert.Equal(HttpStatusCode.OK, status);
                Assert.Contains("result: received", answer);
            }
            await Commit(1, "x86_64");
            var live = await File.ReadAllTextAsync(Path.Combine(f.Data, "channels", "overlap", "index"));
            Assert.Contains("overlap 1.0 i386", live);
            Assert.Contains("overlap 1.0 x86_64", live);
        }
        finally { Directory.Delete(work, true); }
    }

    [Fact]
    public async Task A_machine_facing_address_never_answers_an_empty_body()
    {
        using var f = new Site();
        var c = f.Https();
        // Content-Range is read by the portal: one it cannot read must still answer a record.
        var (status, body) = await Send(c, f.Key, HttpMethod.Put, $"/big/_push/files/objects/{new string('a', 64)}.pkg",
                                        new StringContent("x"), "bytes not-a-range");
        Assert.NotEqual(HttpStatusCode.OK, status);
        Assert.False(string.IsNullOrWhiteSpace(body));
        Assert.Contains("result: refused", body);
        Assert.Contains("next: ", body);
    }

    static string Digest(string path)
    {
        using var s = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(s)).ToLowerInvariant();
    }

    static async Task<int> Run(string exe, string[] args)
    {
        var psi = new System.Diagnostics.ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = System.Diagnostics.Process.Start(psi)!;
        await p.WaitForExitAsync();
        return p.ExitCode;
    }
}

/// xunit has no Skip.If of its own in this version: a tiny one.
static class Skip
{
    public static void If(bool condition, string why) { if (condition) throw new SkipException(why); }
    public sealed class SkipException(string why) : Exception(why);
}
