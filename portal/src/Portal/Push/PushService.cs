// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using Microsoft.Extensions.Options;
using Portal.Channels;

namespace Portal.Push;

/// <summary>
/// The push protocol agreed with the Pkg client (board thread 24):
/// plan, then files (large ones in parts), then commit. Nothing a push sends
/// is visible until the commit, and the commit publishes only what Pkg's own
/// check accepts. Published files never change and a push never deletes.
/// </summary>
public sealed class PushService(IOptions<PortalOptions> options, PkgRunner pkg, Catalogue catalogue,
                                ArchiveChecker archives, Publishers publishers, ILogger<PushService> log)
{
    readonly PortalOptions o = options.Value;
    static readonly ConcurrentDictionary<string, SemaphoreSlim> Locks = new();
    static readonly ConcurrentDictionary<string, SemaphoreSlim> KeyLocks = new();

    /// One writer per channel at a time: pushes and admin changes alike.
    public static SemaphoreSlim LockFor(string channel) => Locks.GetOrAdd(channel, _ => new SemaphoreSlim(1, 1));

    sealed record PlanLine(string Path, string Sha, long Size);

    string Live(string channel) => Path.Combine(o.ChannelsDir, channel);
    string Staging(Publisher who, string channel) => Path.Combine(o.StagingDir, who.Name, channel);
    static string PlanFile(string staging) => Path.Combine(staging, ".plan");
    static string StatsFile(string staging) => Path.Combine(staging, ".stats");

    // ---- plan ---------------------------------------------------------------

    public async Task<Record> Plan(Publisher who, string channel, string body)
    {
        RemoveExpiredStaging();
        var keyGate = KeyLocks.GetOrAdd(who.Name, _ => new SemaphoreSlim(1, 1));
        await keyGate.WaitAsync();
        try { return await PlanLocked(who, channel, body); }
        finally { keyGate.Release(); }
    }

    async Task<Record> PlanLocked(Publisher who, string channel, string body)
    {
        var staging = Staging(who, channel);
        Directory.CreateDirectory(staging);
        var plan = ReadPlan(staging);
        var r = new Record().Add("result", "planned");
        int need = 0, have = 0, refused = 0, line = 0;
        long needBytes = 0, haveBytes = 0;
        foreach (var raw in body.Split('\n'))
        {
            line++;
            var text = raw.Trim();
            if (text.Length == 0) continue;
            // <relative path> <sha256> <size>; the path is everything before the last two fields.
            int b = text.LastIndexOf(' '), a = b > 0 ? text.LastIndexOf(' ', b - 1) : -1;
            if (a <= 0 || !long.TryParse(text[(b + 1)..], NumberStyles.None, CultureInfo.InvariantCulture, out var size)
                || !IsHex64(text[(a + 1)..b]))
            {
                r.Add("refused", $"line {line} 20 not '<path> <sha256> <size>'");
                refused++;
                continue;
            }
            var p = new PlanLine(text[..a], text[(a + 1)..b].ToLowerInvariant(), size);
            var why = Refusal(p.Path) ?? LinkOnly(who, p.Path, o.Policy) ?? ChannelFilesRefusal(who, channel, p.Path);
            if (why is not null) { r.Add("refused", $"{p.Path} 20 {why}"); refused++; continue; }
            if (ChannelPaths.PromisedDigest(p.Path) is { } promised && promised != p.Sha)
            {
                r.Add("refused", $"{p.Path} 12 its name promises {promised} but the plan gives {p.Sha}");
                refused++;
                continue;
            }
            var live = Path.Combine(Live(channel), p.Path);
            if (Published(live))
            {
                var liveSha = await LiveSha(channel, p.Path);
                if (liveSha == p.Sha) { have++; haveBytes += p.Size; continue; }
                if (ChannelPaths.Immutable(p.Path))
                {
                    r.Add("refused", $"{p.Path} 15 already published with other content, and a published file never changes");
                    refused++;
                    continue;
                }
            }
            var staged = Path.Combine(staging, p.Path);
            if (File.Exists(staged) && plan.TryGetValue(p.Path, out var old) && old.Sha == p.Sha)
            {
                have++; haveBytes += p.Size;
                plan[p.Path] = p;
                continue;
            }
            if (File.Exists(staged)) File.Delete(staged);
            plan[p.Path] = p;
            r.Add("need", p.Path);
            need++; needBytes += p.Size;
        }
        WritePlan(staging, plan);
        File.WriteAllText(StatsFile(staging), $"started {DateTime.UtcNow:O}\nskipped {have} {haveBytes}\n");
        r.Add("files-needed", need).Add("bytes-needed", needBytes)
         .Add("files-skipped", have).Add("bytes-skipped", haveBytes);
        if (refused > 0) r.Add("files-refused", refused);
        r.Add("summary", (need == 0 ? "the portal already has every file" : $"{need} file{S(need)} to send ({Record.Size(needBytes)})")
            + (have > 0 && need > 0 ? $"; {have} already on the portal, {Record.Size(haveBytes)} not sent again" : "")
            + (refused > 0 ? $"; {refused} refused, each with its reason above" : ""));
        return r;
    }

    // ---- files --------------------------------------------------------------

    public async Task<(Record Answer, int Status)> PutFile(Publisher who, string channel, string path,
                                                           string? contentRange, long? contentLength, Stream body,
                                                           CancellationToken ct)
    {
        // Planning, uploading and committing share the publisher's staging and quota.
        var keyGate = KeyLocks.GetOrAdd(who.Name, _ => new SemaphoreSlim(1, 1));
        await keyGate.WaitAsync(ct);
        try { return await PutFileLocked(who, channel, path, contentRange, contentLength, body, ct); }
        finally { keyGate.Release(); }
    }

    async Task<(Record Answer, int Status)> PutFileLocked(Publisher who, string channel, string path,
                                                        string? contentRange, long? contentLength, Stream body,
                                                        CancellationToken ct)
    {
        if (Refusal(path) is { } why) return (Record.Refused(20, $"{path}: {why}", "check the path"), 400);
        if (ChannelFilesRefusal(who, channel, path) is { } notTheirs)
            return (Record.Refused(14, $"{path}: {notTheirs}", "ask the portal's maintainers"), 403);
        if (LinkOnly(who, path, o.Policy) is { } linkOnly)
            return (Record.Refused(20, $"{path}: {linkOnly}", "publish the files as an archive on an https server and name it with an Archive: line"), 403);
        var staging = Staging(who, channel);
        var plan = ReadPlan(staging);
        if (!plan.TryGetValue(path, out var want))
            return (Record.Refused(20, $"{path} is not in this push's plan", "send the plan first, then the files it lists"), 400);
        var live = Path.Combine(Live(channel), path);
        if (Published(live) && ChannelPaths.Immutable(path))
        {
            if (await LiveSha(channel, path) == want.Sha)
                return (new Record().Add("result", "unchanged").Add("path", path)
                    .Add("summary", $"{path} is already published with these bytes"), 200);
            return (Record.Refused(15, $"{path} is already published with other content, and a published file never changes",
                "publish a new version instead"), 409);
        }

        long start = 0, end = want.Size - 1;
        if (contentRange is not null)
        {
            if (!TryRange(contentRange, out start, out end, out var total) || total != want.Size)
                return (Record.Refused(20, $"Content-Range '{contentRange}' does not fit a file of {want.Size} bytes",
                    "send 'bytes <first>-<last>/<size>'"), 400);
        }
        else if (want.Size > o.MaxPartBytes)
        {
            return (Record.Refused(20, $"{path} is {Record.Size(want.Size)}, more than one request carries",
                $"send it in parts of at most {o.MaxPartBytes} bytes with Content-Range"), 400);
        }

        var final = Path.Combine(staging, path);
        var part = final + ".part";
        var cap = who.Files ? o.MaxStagingBytes : o.MaxLinkOnlyStagingBytes;
        var keyRoot = Path.Combine(o.StagingDir, who.Name);
        var held = Directory.Exists(keyRoot)
            ? new DirectoryInfo(keyRoot).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length) : 0;
        if (held + (end - start + 1) > cap)
            return (Record.Refused(20, $"this push would hold {Record.Size(held + end - start + 1)} in staging, more than the {Record.Size(cap)} a key may hold",
                "commit or let the staged files expire, then push again"), 413);
        Directory.CreateDirectory(Path.GetDirectoryName(final)!);
        long received = File.Exists(part) ? new FileInfo(part).Length : 0;
        if (File.Exists(final))
            return (new Record().Add("result", "received").Add("path", path).Add("received", want.Size)
                .Add("summary", $"{path} was already received"), 200);
        if (start != received)
            // A resumed upload: the client continues from what the portal holds.
            return (new Record().Add("result", "partial").Add("path", path).Add("received", received)
                .Add("summary", $"the portal holds {received} of {want.Size} bytes; continue from byte {received}"), 200);

        long wrote;
        await using (var f = new FileStream(part, FileMode.Append, FileAccess.Write, FileShare.None, 1 << 16, true))
        {
            var before = f.Length;
            await body.CopyToAsync(f, ct);
            wrote = f.Length - before;
            if (wrote != end - start + 1)
            {
                f.SetLength(before);
                return (Record.Refused(20, $"the part carried {wrote} bytes where its range says {end - start + 1}",
                    $"send the part from byte {before} again"), 400);
            }
        }
        AddStats(staging, 1, wrote, partOnly: end + 1 < want.Size);
        received = end + 1;
        if (received < want.Size)
            return (new Record().Add("result", "partial").Add("path", path).Add("received", received)
                .Add("summary", $"{Record.Size(received)} of {Record.Size(want.Size)} received"), 200);

        var sha = await Sha(part, ct);
        if (sha != want.Sha)
        {
            File.Delete(part);
            return (Record.Refused(12, $"{path} arrived with SHA-256 {sha}, not the {want.Sha} its plan gave",
                "send the file again from byte 0"), 200);
        }
        File.Move(part, final, overwrite: true);
        return (new Record().Add("result", "received").Add("path", path).Add("received", received)
            .Add("summary", $"{path} received and checked, {Record.Size(want.Size)}"), 200);
    }

    // ---- commit -------------------------------------------------------------

    public async Task<Record> Commit(Publisher who, string channel, string localIndex, CancellationToken ct)
    {
        var clock = Stopwatch.StartNew();
        var gate = Locks.GetOrAdd(channel, _ => new SemaphoreSlim(1, 1));
        var keyGate = KeyLocks.GetOrAdd(who.Name, _ => new SemaphoreSlim(1, 1));
        await keyGate.WaitAsync(ct);
        try
        {
            await gate.WaitAsync(ct);
            try { return await CommitLocked(who, channel, localIndex, clock, ct); }
            finally { gate.Release(); }
        }
        finally { keyGate.Release(); }
    }

    async Task<Record> CommitLocked(Publisher who, string channel, string localIndex, Stopwatch clock, CancellationToken ct)
    {
        var live = Live(channel);
        var staging = Staging(who, channel);
        Directory.CreateDirectory(Path.Combine(live, "objects"));
        Directory.CreateDirectory(staging);
        var liveLines = File.Exists(Path.Combine(live, "index"))
            ? IndexLine.ParseAll(await File.ReadAllTextAsync(Path.Combine(live, "index"), ct)) : [];
        var liveSet = liveLines.Select(l => l.ToString()).ToHashSet(StringComparer.Ordinal);

        var refusedItems = new List<string>();
        var candidates = new List<IndexLine>();
        int unchanged = 0, line = 0;
        foreach (var raw in localIndex.Split('\n'))
        {
            line++;
            if (raw.Trim().Length == 0) continue;
            var l = IndexLine.Parse(raw);
            if (l is null) { refusedItems.Add($"line {line} 20 not an index line 'name version arch digest'"); continue; }
            if (liveSet.Contains(l.ToString())) { unchanged++; continue; }
            if (candidates.Any(c => c.ToString() == l.ToString())) continue;
            if (liveLines.FirstOrDefault(x => x.Name == l.Name && x.Arch == l.Arch
                    && PkgVersion.Order.Compare(x.Version, l.Version) == 0) is { } clash)
            {
                refusedItems.Add($"{l.Name} {l.Version} {l.Arch} 15 this version is already published with other content "
                    + $"({clash.Digest[..16]}), and a published version never changes: publish a new version");
                continue;
            }
            candidates.Add(l);
        }

        // Each candidate's files, from this push or already published.
        string? Find(string rel) =>
            File.Exists(Path.Combine(staging, rel)) ? Path.Combine(staging, rel)
            : Published(Path.Combine(live, rel)) ? Path.Combine(live, rel) : null;

        var ready = new List<(IndexLine Line, Manifest M, string Signer)>();
        foreach (var c in candidates)
        {
            var mp = Find($"objects/{c.Digest}.manifest");
            var sp = Find($"objects/{c.Digest}.sig");
            if (mp is null || sp is null)
            {
                refusedItems.Add($"{c.Name} {c.Version} {c.Arch} 11 its {(mp is null ? "manifest" : "signature")} was not sent: "
                    + "push the objects the index names");
                continue;
            }
            if (await Sha(mp, ct) != c.Digest)
            {
                refusedItems.Add($"{c.Name} {c.Version} {c.Arch} 12 its manifest does not match the digest the index gives");
                continue;
            }
            var m = Manifest.Load(mp);
            // The archive a manifest names becomes a file path here: a plain name only.
            if (m.SourceArchive is { } named && ChannelPaths.Classify("archives/" + named) != ChannelPaths.Kind.Archive)
            {
                refusedItems.Add($"{c.Name} {c.Version} {c.Arch} 12 its Source names the archive '{named}', which is not a plain file name");
                continue;
            }
            if (LinkCheck(who, m, o.Policy) is { } notLink)
            {
                refusedItems.Add($"{c.Name} {c.Version} {c.Arch} 20 {notLink}");
                continue;
            }
            if (m.Payload is { } pay && Find($"objects/{pay}.pkg") is null)
            {
                refusedItems.Add($"{c.Name} {c.Version} {c.Arch} 11 its payload {pay[..16]} was not sent");
                continue;
            }
            // An archive published upstream (Archive: line) is fetched from there by the client.
            if (m.SourceArchive is { } arc && m.Upstream is null && Find($"archives/{arc}") is null)
            {
                refusedItems.Add($"{c.Name} {c.Version} {c.Arch} 11 the archive {arc} it names is neither on the portal nor in this push");
                continue;
            }
            var signer = Signer(sp) ?? "";
            ready.Add((c, m, signer));
        }

        // A package keeps the key of its first version, as Pkg pins it.
        var owners = new Dictionary<string, (string Signer, string Version)>(StringComparer.OrdinalIgnoreCase);
        foreach (var l in liveLines)
            if (!owners.ContainsKey(l.Name) && Signer(Path.Combine(live, "objects", l.Digest + ".sig")) is { } s)
                owners[l.Name] = (s, l.Version);
        var owned = new List<(IndexLine Line, Manifest M, string Signer)>();
        var spelled = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var l in liveLines) spelled.TryAdd(l.Name, l.Name);
        foreach (var x in ready)
        {
            // Names differing only in case would be one package on the pages and two in the channel.
            if (spelled.TryGetValue(x.Line.Name, out var existing) && existing != x.Line.Name)
            {
                refusedItems.Add($"{x.Line.Name} {x.Line.Version} {x.Line.Arch} 15 the channel already has a package named {existing}; a name may not differ from another only in case");
                continue;
            }
            spelled.TryAdd(x.Line.Name, x.Line.Name);
            if (!owners.TryGetValue(x.Line.Name, out var owner)) owners[x.Line.Name] = owner = (x.Signer, x.Line.Version);
            // A maintainers' transfer (Portal:Owners) names the key from now on.
            if (publishers.OwnerOf(channel, x.Line.Name, null) is { } moved && !moved.Equals(owner.Signer, StringComparison.OrdinalIgnoreCase))
                owners[x.Line.Name] = owner = (moved, "the maintainers' transfer");
            if (!owner.Signer.Equals(x.Signer, StringComparison.OrdinalIgnoreCase))
            {
                var why = owner.Version == "the maintainers' transfer"
                    ? "to which the portal's maintainers moved it"
                    : $"which signed {owner.Version}";
                refusedItems.Add($"{x.Line.Name} {x.Line.Version} {x.Line.Arch} 14 {x.Line.Name} belongs to key {Short(owner.Signer)}, "
                    + $"{why}; this version is signed by {Short(x.Signer)}. A package keeps its key unless the maintainers move it");
                continue;
            }
            owned.Add(x);
        }

        // New withdrawals of versions already published, or of this push's.
        var withdrawals = liveLines.Concat(owned.Select(x => x.Line))
            .Where(l => File.Exists(Path.Combine(staging, "objects", l.Digest + ".withdrawn"))).ToList();

        // Pkg's own check, on a channel holding exactly what would be added.
        var accepted = new List<(IndexLine Line, Manifest M, string Signer)>();
        var acceptedWithdrawals = new List<IndexLine>();
        if (owned.Count > 0 || withdrawals.Count > 0)
        {
            var check = await CheckWithPkg(owned.Select(x => x.Line).Concat(withdrawals).Distinct().ToList(), Find, ct);
            foreach (var x in owned)
            {
                if (check.TryGetValue(x.Line.ToString(), out var v) && v.Ok) accepted.Add(x);
                else refusedItems.Add($"{x.Line.Name} {x.Line.Version} {x.Line.Arch} {v?.Class ?? 12} {v?.Reason ?? "Pkg did not report on it"}");
            }
            foreach (var w in withdrawals)
            {
                if (check.TryGetValue(w.ToString(), out var v) && v.Withdrawn) acceptedWithdrawals.Add(w);
                else refusedItems.Add($"{w.Name} {w.Version} {w.Arch} 13 its withdrawal is not signed by the key that signed the version");
            }
        }

        // Publish: files first, the index last, so a reader never sees an
        // index line whose files are missing.
        var newArchives = new List<string>();
        foreach (var x in accepted)
        {
            Place(staging, live, $"objects/{x.Line.Digest}.manifest");
            Place(staging, live, $"objects/{x.Line.Digest}.sig");
            if (x.M.Payload is { } pay) Place(staging, live, $"objects/{pay}.pkg");
            if (x.M.SourceArchive is { } arc && Place(staging, live, $"archives/{arc}"))
            {
                await File.WriteAllTextAsync(Path.Combine(live, "archives", arc + ".sha256"),
                    $"{await Sha(Path.Combine(live, "archives", arc), ct)}  {arc}\n", ct);
                newArchives.Add(arc);
            }
        }
        foreach (var w in acceptedWithdrawals)
        {
            Place(staging, live, $"objects/{w.Digest}.withdrawn");
            Place(staging, live, $"objects/{w.Digest}.withdrawn.sig");
        }
        // The list beside the index, so a reader asks for a signed withdrawal
        // only where one exists. Written after the files it names, and on every
        // commit, so a channel published today holds one from its first push.
        Withdrawals.Write(live);
        // Channel files (Bootstrap, Install-Pkg, ReadMe) change only with a
        // commit that refuses nothing: a bootstrap never runs ahead of its package.
        int mutable = 0;
        foreach (var rel in refusedItems.Count == 0 ? StagedMutable(staging) : [])
        {
            if (ChannelFilesRefusal(who, channel, rel) is not null) continue;
            var ownerFile = ChannelFilesOwnerFile(channel);
            if (!File.Exists(ownerFile))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(ownerFile)!);
                await File.WriteAllTextAsync(ownerFile, who.Name + "\n", ct);
            }
            var dst = Path.Combine(live, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
            File.Move(Path.Combine(staging, rel), dst, overwrite: true);
            mutable++;
        }
        if (accepted.Count > 0)
        {
            var text = string.Concat(liveLines.Concat(accepted.Select(a => a.Line)).Select(l => l + "\n"));
            var tmp = Path.Combine(live, $".index.{Guid.NewGuid():N}");
            await File.WriteAllTextAsync(tmp, text, ct);
            File.Move(tmp, Path.Combine(live, "index"), overwrite: true);
        }
        // Recorded before the catalogue is rebuilt, so no page shows the version without it.
        if (accepted.Count > 0)
        {
            var seen = catalogue.FirstSeenPath(channel);
            Directory.CreateDirectory(Path.GetDirectoryName(seen)!);
            await File.AppendAllLinesAsync(seen, accepted.Select(a => $"{a.Line.Digest}\t{DateTime.UtcNow:O}"), ct);
        }
        catalogue.Invalidate(channel);
        foreach (var arc in newArchives.Distinct()) archives.Enqueue(channel, arc);
        foreach (var signer in accepted.Select(a => a.Signer).Distinct()) publishers.Learn(signer, who.Name);

        var stats = ReadStats(staging);
        // Another push from this publisher may still need staged or planned files.
        ForgetPlanned(staging, live);

        var published = accepted.Count;
        var r = new Record().Add("result", published > 0 || acceptedWithdrawals.Count > 0 || mutable > 0 ? "published"
            : refusedItems.Count > 0 ? "refused" : "unchanged");
        if (refusedItems.Count > 0 && StagedMutable(staging).Any())
            r.Add("held", "the channel files of this push (Bootstrap, Install-Pkg, ReadMe) wait for a commit that refuses nothing");
        foreach (var x in accepted) r.Add("published", $"{x.Line.Name} {x.Line.Version} {x.Line.Arch}");
        foreach (var w in acceptedWithdrawals) r.Add("withdrawn", $"{w.Name} {w.Version} {w.Arch}");
        foreach (var x in refusedItems) r.Add("refused", x);
        foreach (var arc in newArchives.Distinct()) r.Add("archive-check", $"{arc} pending");
        r.Add("published-count", published).Add("unchanged-count", unchanged).Add("refused-count", refusedItems.Count);
        if (mutable > 0) r.Add("files-replaced", mutable);
        r.Add("files-sent", stats.Files).Add("bytes-sent", stats.Bytes)
         .Add("files-skipped", stats.SkippedFiles).Add("bytes-skipped", stats.SkippedBytes)
         .Add("time", string.Create(CultureInfo.InvariantCulture, $"{clock.Elapsed.TotalSeconds:0.0} s for the commit")
             + (stats.Started is { } st ? $", {(DateTime.UtcNow - st).TotalSeconds:0} s since the plan" : ""));
        var parts = new List<string>();
        if (published > 0) parts.Add($"{published} version{S(published)} published");
        if (acceptedWithdrawals.Count > 0) parts.Add($"{acceptedWithdrawals.Count} withdrawn");
        if (unchanged > 0) parts.Add($"{unchanged} already there");
        if (mutable > 0) parts.Add($"{mutable} channel file{S(mutable)} replaced");
        if (refusedItems.Count > 0) parts.Add($"{refusedItems.Count} refused, each with its reason above");
        if (stats.SkippedBytes > 0) parts.Add($"{Record.Size(stats.SkippedBytes)} not sent again");
        r.Add("summary", parts.Count == 0 ? "nothing to publish: the channel already has all of it" : string.Join("; ", parts));
        if (published == 0 && refusedItems.Count > 0)
        {
            // Nothing went in: the class of the first refusal is the answer's,
            // so that the client exits with it.
            var first = refusedItems[0].Split(' ');
            var code = first.Select(w => int.TryParse(w, out var c) && c is >= 10 and <= 20 ? c : 0).FirstOrDefault(c => c > 0);
            if (code == 0) code = 12;
            r.Add("class", Record.ClassName(code)).Add("code", code);
            r.Add("next", "read each refusal, fix it, and push again: accepted files stay staged for 24 hours");
        }
        log.LogInformation("commit {Channel} by {Who}: {Summary}", channel, who.Name, r.Get("summary"));
        return r;
    }

    sealed record Verdict(bool Ok, bool Withdrawn, int Class, string Reason);

    async Task<Dictionary<string, Verdict>> CheckWithPkg(List<IndexLine> lines, Func<string, string?> find, CancellationToken ct)
    {
        var dir = Path.Combine(Path.GetTempPath(), "pkg-portal-check-" + Guid.NewGuid().ToString("N"));
        var result = new Dictionary<string, Verdict>(StringComparer.Ordinal);
        try
        {
            Directory.CreateDirectory(Path.Combine(dir, "objects"));
            foreach (var l in lines)
            {
                foreach (var ext in new[] { "manifest", "sig", "withdrawn", "withdrawn.sig" })
                    if (find($"objects/{l.Digest}.{ext}") is { } src)
                        File.Copy(src, Path.Combine(dir, "objects", $"{l.Digest}.{ext}"), true);
                var m = Manifest.Load(Path.Combine(dir, "objects", l.Digest + ".manifest"));
                if (m.Payload is { } p && find($"objects/{p}.pkg") is { } pk)
                    File.Copy(pk, Path.Combine(dir, "objects", p + ".pkg"), true);
            }
            await File.WriteAllTextAsync(Path.Combine(dir, "index"), string.Concat(lines.Select(l => l + "\n")), ct);
            var answer = await pkg.Run(["SHOW", "CHANNEL", dir, "METADATA", "MACHINE"], o.CheckTimeout, ct);
            // A Pkg from before METADATA takes the word for a package name and
            // checks nothing (count: 0): that is no answer, never a pass.
            if (!answer.All("entry").Any() && (answer.One("class") == "usage" || answer.One("count") == "0"))
            {
                // A Pkg from before METADATA: a full SHOW is complete for packages
                // with a payload; one that reads source archives would be refused
                // for the archive missing here, so those wait for METADATA.
                var sourced = lines.Where(l => Manifest.Load(Path.Combine(dir, "objects", l.Digest + ".manifest")).Source is not null).ToList();
                foreach (var l in sourced)
                    result[l.ToString()] = new Verdict(false, false, 17,
                        "the portal's Pkg cannot check a package from a source archive yet: it needs SHOW METADATA");
                var rest = lines.Except(sourced).ToList();
                if (rest.Count == 0) return result;
                await File.WriteAllTextAsync(Path.Combine(dir, "index"), string.Concat(rest.Select(l => l + "\n")), ct);
                answer = await pkg.Run(["SHOW", "CHANNEL", dir, "MACHINE"], o.CheckTimeout, ct);
            }
            if (answer.TimedOut || (!answer.All("entry").Any() && answer.One("class") is not null))
            {
                var reason = answer.TimedOut ? "the check took too long" : $"the portal's Pkg refused the check: {answer.One("reason")}";
                foreach (var l in lines) result[l.ToString()] = new Verdict(false, false, 17, reason);
                return result;
            }
            // entry: name version kind arch status signer; problem: name version reason
            var problems = answer.All("problem").ToList();
            foreach (var e in answer.All("entry"))
            {
                var f = e.Split(' ');
                if (f.Length < 5) continue;
                var l = lines.FirstOrDefault(x => x.Name == f[0] && x.Version == f[1] && (f[3] == "-" || x.Arch == f[3]));
                if (l is null) continue;
                var status = f[4];
                var reason = problems.FirstOrDefault(p => p.StartsWith($"{f[0]} {f[1]} ", StringComparison.Ordinal))?[(f[0].Length + f[1].Length + 2)..]
                             ?? status;
                result[l.ToString()] = status switch
                {
                    "ok" => new Verdict(true, false, 0, ""),
                    "withdrawn" => new Verdict(true, true, 0, ""),
                    _ => new Verdict(false, false, ClassCode(status), reason),
                };
            }
            foreach (var w in answer.All("warning"))
                log.LogWarning("pkg check warning: {Warning}", w);
            return result;
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch (IOException) { }
        }
    }

    // ---- helpers --------------------------------------------------------------

    /// Channel files (Bootstrap programs, Install-Pkg, ReadMe) belong to the key
    /// that first published them in the channel: another key may not replace them.
    string ChannelFilesOwnerFile(string channel) => Path.Combine(o.StateDir, channel, "channel-files-owner");

    string? ChannelFilesRefusal(Publisher who, string channel, string path)
    {
        if (ChannelPaths.Classify(path) != ChannelPaths.Kind.Mutable) return null;
        var f = ChannelFilesOwnerFile(channel);
        var owner = File.Exists(f) ? File.ReadAllText(f).Trim() : null;
        return owner is null || owner == who.Name ? null
            : $"the channel files of {channel} (Bootstrap, Install-Pkg, ReadMe) belong to the key of {owner}, which published them first";
    }

    /// What a key may send without the right to upload binaries: signed
    /// manifests, signatures and withdrawals. The reason names which rule applies.
    static string? LinkOnly(Publisher who, string path, PortalPolicy policy)
    {
        if (path.EndsWith(".manifest", StringComparison.Ordinal) || path.EndsWith(".sig", StringComparison.Ordinal)
            || path.EndsWith(".withdrawn", StringComparison.Ordinal))
            return null;
        if (!policy.BinariesAllowed)
            return policy.Why("Binaries", "off", "this portal accepts no binaries: publishers host their files on an https server and name them with an Archive: line");
        return who.Files ? null
            : "this key publishes by link only (it has no files right; the portal's operators grant it): it sends signed manifests and signatures, and the files stay on an https server";
    }

    /// A version's files: for a key without binaries, no payload of its own and an
    /// archive named by an https address; for every key, an archive host this
    /// portal links to.
    static string? LinkCheck(Publisher who, Manifest m, PortalPolicy policy)
    {
        if (m.Upstream is { } up && Uri.TryCreate(up.Url, UriKind.Absolute, out var host) && !policy.HostAllowed(host.Host))
            return policy.Why("LinkHosts", policy.LinkHosts, $"its archive is on {host.Host}, which this portal does not link to");
        if (who.Files && policy.BinariesAllowed) return null;
        var why = LinkOnlyManifest(m);
        return why is null ? null : policy.BinariesAllowed ? why : policy.Why("Binaries", "off", why);
    }

    /// A version a link-only key may publish: no payload of its own, its files in
    /// an archive the signed manifest names by an https address, size and SHA-256.
    static string? LinkOnlyManifest(Manifest m)
    {
        if (m.Payload is not null)
            return "its manifest names a payload to upload, and this key publishes by link only: put the files in an archive on an https server and name it with an Archive: line";
        if (m.Source is null || m.Upstream is null)
            return "its manifest names no Archive: line, and this key publishes by link only";
        if (!Uri.TryCreate(m.Upstream.Url, UriKind.Absolute, out var u) || u.Scheme != "https" || u.Host.Length == 0)
            return $"its archive address '{m.Upstream.Url}' is not an https address";
        return null;
    }

    static string? Refusal(string path) => ChannelPaths.Classify(path) switch
    {
        ChannelPaths.Kind.Index => "the index is sent with the commit, not as a file",
        ChannelPaths.Kind.ArchiveDigest => "the portal writes an archive's .sha256 itself",
        ChannelPaths.Kind.None => "not a file a channel holds (objects/, archives/, Bootstrap/<cpu>/Pkg, Install-Pkg, ReadMe)",
        _ => null,
    };

    /// Move a staged file into the live channel. True when it was moved now.
    static bool Place(string staging, string live, string rel)
    {
        var src = Path.Combine(staging, rel);
        var dst = Path.Combine(live, rel);
        if (!File.Exists(src)) return false;
        if (Published(dst)) { File.Delete(src); return false; }
        Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
        File.Move(src, dst);
        return true;
    }

    /// On the disk, or moved to R2 with its .url left behind.
    static bool Published(string path) => File.Exists(path) || File.Exists(path + ".url");

    static IEnumerable<string> StagedMutable(string staging) =>
        Directory.Exists(staging)
            ? Directory.EnumerateFiles(staging, "*", SearchOption.AllDirectories)
                .Select(f => Path.GetRelativePath(staging, f).Replace('\\', '/'))
                .Where(r => ChannelPaths.Classify(r) == ChannelPaths.Kind.Mutable).ToList()
            : [];

    static void ForgetPlanned(string staging, string live)
    {
        // Drop only files that went live; retain pending uploads, including unstarted ones.
        var plan = ReadPlan(staging);
        foreach (var k in plan.Keys.ToList())
            if (Published(Path.Combine(live, k)) && !File.Exists(Path.Combine(staging, k))
                && !File.Exists(Path.Combine(staging, k + ".part"))) plan.Remove(k);
        WritePlan(staging, plan);
    }

    async Task<string> LiveSha(string channel, string rel)
    {
        if (ChannelPaths.PromisedDigest(rel) is { } promised) return promised;
        var path = Path.Combine(Live(channel), rel);
        if (ChannelPaths.Classify(rel) == ChannelPaths.Kind.Archive && File.Exists(path + ".sha256"))
            return (await File.ReadAllTextAsync(path + ".sha256")).Split(' ')[0].Trim();
        return await Sha(path, default);
    }

    static async Task<string> Sha(string path, CancellationToken ct)
    {
        await using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, true);
        return Convert.ToHexString(await SHA256.HashDataAsync(f, ct)).ToLowerInvariant();
    }

    static string? Signer(string sigPath) =>
        File.Exists(sigPath)
            ? File.ReadLines(sigPath).FirstOrDefault(s => s.StartsWith("Signer: ", StringComparison.Ordinal))?["Signer: ".Length..].Trim()
            : null;

    static Dictionary<string, PlanLine> ReadPlan(string staging)
    {
        var d = new Dictionary<string, PlanLine>(StringComparer.Ordinal);
        var p = PlanFile(staging);
        if (!File.Exists(p)) return d;
        foreach (var l in File.ReadLines(p))
        {
            var f = l.Split('\t');
            if (f.Length == 3) d[f[0]] = new PlanLine(f[0], f[1], long.Parse(f[2], CultureInfo.InvariantCulture));
        }
        return d;
    }

    static void WritePlan(string staging, Dictionary<string, PlanLine> plan)
    {
        Directory.CreateDirectory(staging);
        File.WriteAllLines(PlanFile(staging), plan.Values.Select(p => $"{p.Path}\t{p.Sha}\t{p.Size}"));
    }

    sealed record Stats(DateTime? Started, int Files, long Bytes, int SkippedFiles, long SkippedBytes);

    static Stats ReadStats(string staging)
    {
        DateTime? started = null; int files = 0, sf = 0; long bytes = 0, sb = 0;
        if (File.Exists(StatsFile(staging)))
            foreach (var l in File.ReadLines(StatsFile(staging)))
            {
                var f = l.Split(' ');
                if (f[0] == "started" && DateTime.TryParse(f[1], null, DateTimeStyles.RoundtripKind, out var t)) started = t;
                else if (f[0] == "skipped") { sf = int.Parse(f[1]); sb = long.Parse(f[2]); }
                else if (f[0] == "sent") { files += int.Parse(f[1]); bytes += long.Parse(f[2]); }
            }
        return new Stats(started, files, bytes, sf, sb);
    }

    static void AddStats(string staging, int files, long bytes, bool partOnly) =>
        File.AppendAllText(StatsFile(staging), $"sent {(partOnly ? 0 : files)} {bytes}\n");

    void RemoveExpiredStaging()
    {
        if (!Directory.Exists(o.StagingDir)) return;
        foreach (var who in Directory.EnumerateDirectories(o.StagingDir))
        {
            var keyGate = KeyLocks.GetOrAdd(Path.GetFileName(who), _ => new SemaphoreSlim(1, 1));
            if (!keyGate.Wait(0)) continue;
            try
            {
                foreach (var ch in Directory.EnumerateDirectories(who))
                {
                    // The last file written, so an upload in progress is never taken away.
                    var when = new DirectoryInfo(ch).EnumerateFiles("*", SearchOption.AllDirectories)
                        .Select(f => f.LastWriteTimeUtc).DefaultIfEmpty(Directory.GetLastWriteTimeUtc(ch)).Max();
                    if (DateTime.UtcNow - when > o.StagingLifetime)
                    {
                        try { Directory.Delete(ch, true); log.LogInformation("removed expired staging {Dir}", ch); }
                        catch (IOException) { }
                    }
                }
            }
            finally { keyGate.Release(); }
        }
    }

    static bool TryRange(string header, out long start, out long end, out long total)
    {
        start = end = total = -1;
        // bytes <first>-<last>/<size>
        if (!header.StartsWith("bytes ", StringComparison.Ordinal)) return false;
        var s = header[6..].Split('/', 2);
        if (s.Length != 2) return false;
        var r = s[0].Split('-', 2);
        return r.Length == 2
            && long.TryParse(r[0], NumberStyles.None, CultureInfo.InvariantCulture, out start)
            && long.TryParse(r[1], NumberStyles.None, CultureInfo.InvariantCulture, out end)
            && long.TryParse(s[1], NumberStyles.None, CultureInfo.InvariantCulture, out total)
            && start <= end && end < total;
    }

    static bool IsHex64(string s) => s.Length == 64 && s.All(Uri.IsHexDigit);
    static string S(int n) => n == 1 ? "" : "s";
    static string Short(string key) => key.Length > 16 ? key[..16] : key;

    static int ClassCode(string name) => name switch
    {
        "not-found" => 11, "integrity" => 12, "signature" => 13, "key" => 14, "conflict" => 15, "usage" => 20, _ => 12,
    };
}
