<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Progress events

A program that links libpkg can show what an operation is doing (which
package it resolves, how many bytes of a download have arrived, which
files it places, whether it is taking a failed change back out) without
reading a sentence. It sets `event` in its `pkg_sink`, and receives every
step of every operation as a `struct pkg_event`. The contract is in
`include/pkg.h`, under "Progress events". This page explains it with
examples.

Pkg uses this interface itself: the activity line the command line draws at
a terminal is drawn from these events (`src/pkg_lib_events.c`), and
`PKG_EVENTS=<file>` writes them out one per line.

- [Connecting](#connecting)
- [What an install looks like](#what-an-install-looks-like)
- [The rules](#the-rules)
- [Which verb says what](#which-verb-says-what)
- [Cancelling](#cancelling)
- [Threads, lifetime and cost](#threads-lifetime-and-cost)
- [From the command line](#from-the-command-line)
- [What the events cannot say yet](#what-the-events-cannot-say-yet)
- [Tests](#tests)

## Connecting

```c
static void on_event(void *user, const struct pkg_event *e)
{
    /* e and its strings are lent for this call: copy what you keep */
}

struct pkg_sink sink;
memset(&sink, 0, sizeof sink);
sink.structured = 1;          /* or 0: events come in both forms */
sink.record = on_record;      /* the answer, as before */
sink.event = on_event;
rc = pkg_install(&sink, &options);
```

Events are sent whether `structured` is set or not, and whatever `progress`
says. `progress` is about the activity line drawn as text; `event` is for a
program. Without `event`, nothing is built, and the results and the text are
the same with it as without.

`examples/events.c` is a complete program: the operation runs on a worker
thread, the callback copies each event into a queue, and the main thread,
standing for a UI's own loop, takes them out and shows them. It needs no
toolkit and is run by `make test`.

## What an install looks like

`hello 1.0`, two files, from a local channel into a root that exists. Each
line is one event; nesting is the `parent` field.

```text
op-begin     op 7  install
step-begin   1     resolve  hello 1.0
step-begin   2       verify   hello 1.0  manifest 7af1eac1921d.manifest
step-end     2       ok
step-begin   3       fetch    hello 1.0  payload 6c5c2627d3d6.pkg   bytes-read, total unknown
step-progress 3                                                     0 of 36
step-end     3       ok                                             36 of 36
step-begin   4       verify   hello 1.0  payload 6c5c2627d3d6.pkg   bytes-verified 0 of 36
step-end     4       ok                                             36 of 36
step-end     1     ok
step-begin   5     verify   hello 1.0   files-verified   0 of 2
step-end     5     ok                                    2 of 2
step-begin   6     inspect  hello 1.0   files-checked    0 of 2
step-end     6     ok                                    2 of 2
step-begin   7     prepare  hello 1.0   files-written    0 of 2
step-end     7     ok                                    2 of 2
step-begin   8     place    hello 1.0   files-placed     0 of 2
step-end     8     ok                                    2 of 2
step-begin   9     record   hello 1.0
step-end     9     ok
step-begin  10     flush    root /Work/root
step-end    10     ok
op-end       op 7  code 0, ok, restore none
```

From a channel on the network, each manifest, signature and payload is
first a `fetch` step counted in `bytes-transferred`, with a wait while the
server is connected to and while it has not answered, and a `fetch` of the
channel's `index` comes first. An upgrade adds a `remove` step for the files
the old version had and the new one drops.

The `place` step ending is not the end of the install: the records, the
attributes and the write-back to the medium follow, and only `op-end` with
code 0 says the operation is done.

## The rules

**Order.** `op-begin` comes before any work, even for a request refused at
once (code 20), and exactly one `op-end` comes last, before the call
returns. Steps nest: a step that begins while another is open has it as
`parent` and ends before it. Every step that begins ends.

**Identity.** `op` numbers the operations of the process from 1; `step`
numbers the steps of one operation from 1. A step is the pair (op, step),
and a UI keys its rows by that pair. `attempt` is 1, or more when the same
work starts over: the earlier attempt ends `retried` and a new step begins.
The numbers mean nothing beyond the process, and nothing is kept from one
call to the next.

**Phases.** `phase` says what a step does. A phase that does not run is not
announced: a dry run has no `prepare`, `place`, `record` or `flush`.

| Phase | What it does | Counted in |
|---|---|---|
| `resolve` | channels read, a version picked, dependencies planned; each package's fetch and verify nest in it | nothing |
| `fetch` | an object downloaded, or a payload, archive or drawer read | bytes transferred, bytes read |
| `verify` | bytes or files checked against what was signed; SHOW: the channel's entries | bytes verified, files verified, entries checked |
| `inspect` | the root's files compared with the records, or with what is coming | files checked |
| `prepare` | files written to staging; PUBLISH and PUSH: a file hashed | files written, bytes hashed |
| `place` | files put in their place in the root; REPAIR: written back | files placed, files written |
| `remove` | files taken out of the root | files processed |
| `record` | the database entry, the pinned key, the previous version, attributes | nothing |
| `restore` | what this operation placed, taken back out after a refusal or a cancel | packages restored |
| `flush` | the root's changes written back to its medium | nothing |
| `upload` | PUSH: files sent to the portal | files sent, bytes sent |

**Counters.** `done` and `total` are 64-bit and absolute, in `unit`,
counting exactly what `measure` names: an archive's compressed bytes are
never added to uncompressed ones, nor bytes read to bytes verified. `done`
never goes back within a step, and never passes a known total. `total` means
something only when `total_known` is set, and a known total of 0 is an empty
file, not an unknown one. A total can become known during a step, which is
then said at once. A total that turns out wrong (a second, different one, or
one that the count passes) becomes unknown for the rest of the step, so a UI
never shows a share above 100%.

The library gives these facts per step. It gives no percentage of the whole
operation: a UI that wants one decides its own weighting, and should say
which step it is on.

**Waits.** A step that waits for something it cannot count (a host being
connected to, a server that has not answered) says so once, with `wait`
`network` and the host in `object`, and once again with `wait` `none` when
work comes back. Nothing is sent in between: the UI animates its own
indeterminate indicator.

**Ends.** A step ends `ok`, `failed` (with `code`, the class of the
operation's first refusal), `cancelled`, `retried`, or `absent`: a file
fetched that is not published there, such as a withdrawal that was never
made, which is not an error by itself. `abandoned` would mean the library
ended an operation with a step still open; the tests check that it never
happens. `op-end` carries `code`, which is what the call returns; `outcome`
`ok` exactly when the code is 0, `cancelled` when the caller's cancel
stopped it, else `failed`; and `restore`:

| `restore` | Meaning |
|---|---|
| `none` | nothing was left to take back: success, a dry run, or a refusal before anything was placed |
| `done` | every package this operation placed was taken back out |
| `failed` | taking back was tried and left something in place; a warning names it |
| `incomplete` | a package was left partly placed and was not taken back; the same operation repeated finishes it |

A restore is never reported as done because it was attempted: each package
taken back counts in the `restore` step only once its files, its records and
the key the operation pinned are gone.

**Pace.** `step-begin`, `step-wait`, `step-end` and the operation events are
never held back. `step-progress` is sent at most once per `PKG_EVENT_GAP_MS`
(100 ms) per step, measured on `pkg_sink.clock` when set, else on the host's
monotonic clock, except when a total becomes known or is found wrong.
`step-end` carries the last counters. A UI that draws less often than that
keeps the last event of each step.

**Privacy.** `object` names a file, an archive, a channel object or a host,
never a user name, a password, a query string or a fragment of a URL.

## Which verb says what

Every verb sends `op-begin` and `op-end`, and a `fetch` step for every file
of a channel it downloads. Beyond that:

| Verb | Steps |
|---|---|
| INSTALL | `resolve` (`fetch` and `verify` of each package inside), then for each package `verify`, `inspect`, `prepare`, `place`, `record`; `restore` after a refusal or cancel once something was placed; `flush` |
| UPGRADE, ROLLBACK | as INSTALL, with `remove` for the files the old version had and the new one drops |
| UPGRADE ALL | `resolve` of the whole root, then for each package its own `resolve` and the steps of UPGRADE |
| VERIFY | `inspect` per package; a second `inspect` of the whole root when files are missing, to find any moved by hand |
| REPAIR | `inspect`, the `fetch` and `verify` of the package, `place` (files written back), `flush` |
| REMOVE | `remove`, `record`, `flush`; with ORPHANS, `remove` and `record` per package |
| SHOW | `verify` of the channel's entries, each entry's own steps inside; `fetch` of each archive read once for all its entries |
| STATUS | `inspect` per package, not counted |
| PUBLISH, MANIFEST | `fetch` of the drawer or the archive; `prepare` when an archive is hashed |
| PUSH | `prepare` per file hashed for the plan; `upload` of the files the portal needs, one `upload` step per file inside it |
| KEYGEN, KEYINFO, SIGN, CHECKSIG, WITHDRAW, LIST, RESOLVE, IMAGE, MOUNTLIST, CHANNEL, SEARCH | nothing but the operation events and any download |

`flush` comes only for a root that existed when the operation began, since
only such a root is locked and written back (see below). The integrity
checks are `verify` steps: a manifest that is not the one indexed, a
signature that does not verify and a payload that is not the one signed
each end that step `failed`, code 12 or 13, and nothing is placed.

## Cancelling

`pkg_sink.cancel` is still how an operation is stopped; an event callback
cannot stop anything. libpkg asks `cancel` once per package while resolving,
once before each package is placed, once per entry while SHOW checks a
channel, and once per package while STATUS compares. The files of one
package are never left half-placed because of a cancel: the question is not
asked while they are placed.

When `cancel` answers yes, the step open at that moment, if there is one,
ends `cancelled`, what the operation had placed is taken back in a
`restore` step, and
`op-end` has `outcome` `cancelled`, `cancel` set and code 10. A cancel asked
for after the last boundary changes nothing: the operation finishes, and
`op-end` says `ok`. The `restore` step can itself fail; `op-end` then says
`cancelled` and `restore` `failed`.

## Threads, lifetime and cost

- The callback runs on the thread that called the operation, before the
  operation goes on. libpkg starts no thread and calls no UI. A UI copies
  the event into its own queue and returns; `examples/events.c` shows a
  bounded queue the worker waits on when it is full, so no `step-begin`
  loses its `step-end`.
- The event and all its strings are lent for the length of the call.
- One operation runs at a time in a process. A libpkg operation called from
  inside a callback returns 20 at once, without touching the running one; if
  its own sink has `event`, it receives its own `op-begin` and `op-end`.
- The counts come from the reads and transfers the operation makes anyway:
  nothing is read twice, hashed twice or walked to draw a bar.

## From the command line

`PKG_EVENTS=<file>` (or `-` for stderr) writes every event of a command, one
tab-separated line each, never into stdout:

```text
step-end	op=1	verb=install	step=3	parent=1	attempt=1	phase=fetch	package=hello	version=1.0	payload=6c5c2627d3d6.pkg	bytes-read=36	total=36	outcome=ok	code=0
```

The C structure is the contract; these lines follow it field for field, for
watching what a front end would be shown, or for a script.

## What the events cannot say yet

These are limits of the engine found while connecting the events to it.
The events report them as they are rather than paper over them.

- **A first INSTALL into a root that does not exist yet is not locked and
  not written back.** `call()` locks and flushes only a root that is a
  directory when the operation starts, so that install has no `flush` step,
  and `flushed` is absent from its records.
- **A package refused while its files are placed is not taken back.** The
  engine finishes such a change when the same operation is repeated; the
  events say `restore` `incomplete`. The dependencies this operation placed
  before it are still taken back, even when the refused package's files are
  already in place, as in a failure to write its database entry.
- **The payload is read and hashed whole.** Its `fetch` and `verify` steps
  have a known total and no step in between: begin at 0, end at the size.
- **A download does not start over within a step.** The transport has no
  retry of its own, so `retried` and a second `attempt` come today from one
  place: an archive whose block map no longer serves is read again whole.
- **SEARCH through a portal and the self-update** (`pkg U`, a command-line
  function) do not go through these steps; the self-update draws the
  activity line itself. `pkg_update_check` takes no sink and sends no event.
- **STATUS counts nothing**: each package it compares is an `inspect` step
  that begins and ends, though comparing reads and hashes its files.
- **IMAGE reads its drawer and builds the volume without a step**, and so
  does PUBLISH of KIND image for the volume itself.

## Tests

`tests/test_events.c` drives libpkg through `pkg.h`, keeps every event,
and checks each operation's whole stream against the rules above before it
checks what the case is about: install, upgrade, rollback, verify, repair,
remove, upgrade all and show; an upgrade with nothing to do and a dry run;
a payload and a manifest that are not the ones signed, a missing payload, a
downgrade not asked for, a write the file system refuses, a request refused
at once; a restore that succeeds and one that fails; a cancel while
resolving and between two packages; the same install with no callback, in
both forms, with and without `progress`; successive calls and a call from
inside a callback; downloads of known and unknown length, an empty file, an
absent channel, the cache and a slow server; a test clock frozen and
running; an archive read again; and, through the transfer hooks, more than
4 GiB, a restart, a wrong total and a URL with credentials.
`tests/events.sh` runs it with a local HTTP server. Both run in `make test`,
and again under the sanitizers in `make test-ubsan`.
