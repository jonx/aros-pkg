<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# What pkg shows while it works

A person watching a command needs two things: to be told what is about to
take time, before it takes it, and to see that it is still going. This is
what pkg shows, and what it never shows.

The whole behaviour lives in one file, `src/pkg_activity.c`, behind
`include/pkg_activity.h`. Nothing else decides when a line appears, how
often it is redrawn, or what it says. What it is told comes from the
[progress events](events.md), the same a program receives through
`pkg_sink.event`: the line shows the steps that count something (a
download, an archive read, the files checked, written and placed, a
channel's entries, an upload) and nothing the events do not say. `examples/activity.c` is a sample
that goes through every case; it is built as `build/example-activity` and
takes no network and no large file.

## The two shapes

**A step that knows its size** says so and shows how far it has got, in one
line rewritten in place:

```
  reading the archive contrib.tar.bz2, 212/637 MB  14 MB/s  0:30 left
  checking contrib-nightly, 57/208
```

It appears at once, from the first byte, because a step that knows its size
knew it would be long before it started. The figure is what shows that pkg
is alive, so the line carries no mark.

**A step that does not know** carries the mark instead, the project's logo
pulsing in place:

```
 (O) waiting for aros-pkg.azurewebsites.net
```

It waits half a second before appearing, so that what turns out to be quick
says nothing at all, and the mark then advances up to eight times a second.

The mark never stands beside a figure: one or the other, never both.

## The rules

| | |
|---|---|
| Before the work | the line names the step and its size, and is drawn before the first byte is read |
| While it runs | rewritten in place, at most eight times a second; the figure moves a point at a time from 0 |
| A step inside a step | draws over the line; when it ends, the step around it takes its line back, so a long step made of small ones keeps saying how far it has got |
| When it ends | the line is erased, the cursor comes back, and the result prints where the line stood |
| Quicker than half a second | nothing is printed, unless the size was known |
| Only the mark changed | only the mark is rewritten, so the words do not flicker |

## Where it is never seen

- `MACHINE` output: records only, never a frame, a carriage return or an escape.
- A `LOG` file: the result and the refusals, never the line.
- Output that is not a terminal, unless `PKG_PROGRESS=1` says otherwise.
- `PKG_COLOR=never`: the line stays, in plain text, with no escape sequence.
- `PKG_PROGRESS=0` switches it off everywhere.

## The mark

Six frames, a drop that becomes a ring and fades:

```
 ·    o    O   (O)  ( )   ·
```

Purple where the terminal has colours, the highlight pen on the AROS
console. The middle dot is written as the one byte 0xB7 on AROS, as UTF-8
under a UTF-8 locale, and as a full stop anywhere else.

## The cursor

Hidden while the line is alive, shown again by every path that erases it:
a result, a refusal, a table, or the end of the command.

## Connecting work to the display

`src/pkg_lib_events.c` turns each step into these calls: a step's begin
into `pkg_activity_step`, its counters into `pkg_activity_bytes` or
`pkg_activity_count`, a wait into `pkg_activity_waiting`, its end into
`pkg_activity_done`. `pkg_activity_step` draws a measured step at zero
before returning to its caller. The same line names the work and carries its counter. A byte total
learned from an HTTP response is reported at zero before reading its body.
The archive reader reports its file size before reading the archive.

`pkg_activity_tick` advances an unknown-duration pulse after the display
delay. Socket readiness waits call it every 100 milliseconds. The AROS TLS
transport uses these waits for its socket reads and writes, including the
handshake. AROS resolves host names in a worker with its own socket-library
base; the caller services ticks until that worker finishes. The POSIX curl child is polled while it runs. A known total keeps
its counter during a wait. The eye represents activity; byte and item
counts represent completed work.

A nested step saves the enclosing counter, rate and display timing. Ending
the nested step restores the nearest visible enclosing step. Disconnecting
the display erases its line and clears the nesting state.

Run `make test` for the activity state tests and the CLI checks. The network
checks use local HTTP and HTTPS servers that delay their headers and body,
and exercise redirects with measured and unknown-length responses.
