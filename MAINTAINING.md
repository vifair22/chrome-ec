# Maintaining this fork

`vifair22/chrome-ec` on git.airies.net is the ThinkPad C14 Chromebook's
ChromeEC: Google's release branch for its board (primus, Alder Lake brya),
with commits cherry-picked from other open source trees (MrChromebox's in
particular) and our own on top. It is maintained the way the fleet's coreboot
and edk2 forks are (their MAINTAINING.md), with one difference: the C14 is the
only ChromeEC machine in the fleet, so this fork has one owner, the C14
session, and no shared branch to coordinate.

## Remotes

| Remote | URL | |
|---|---|---|
| `origin` | https://git.airies.net/vifair22/chrome-ec.git | ours, read-write |
| `google` | https://chromium.googlesource.com/chromiumos/platform/ec | upstream, read-only |
| `mrchromebox` | https://github.com/MrChromebox/chrome-ec.git | cherry-pick source, read-only |

The upstream remotes' push URLs are set to `DISABLED` on purpose. Never push
to them. Builds fetch this fork only from `origin`.

## Branches

- `base` is Google's `firmware-ec-R136-16238.2.B-main` at the snapshot we
  have adopted. Nothing of ours lives here. Google's release branches are
  append-only, so `base` fast-forwards; each adoption is also tagged
  `base/ec-YYYYMMDD`.
- `c14` is `base` plus every commit we carry, as a clean linear stack: the
  cherry-picks first, in their source's order, then our own.

`base` is Google's, not MrChromebox's, on purpose: MrChromebox rewrites his
branch (the commit the C14's 2025 EC image was built from is gone from it),
so building on it would leave our history pointing at commits that no longer
exist. His work comes in as cherry-picks instead, each one traceable.

`c14` may be force-pushed when it is rebased onto a new `base`. Do not build
a flashed image from a rolling branch; build from a tag.

## Commits

- One logical change is one commit: imperative subject, a body that says why.
- A cherry-pick keeps its author and message, drops `Change-Id`,
  `Signed-off-by` and the review trailers, and ends with a provenance line
  naming the source and the full commit:
  `MrChromebox-chrome-ec <sha>` (or `google-ec <sha>` for a commit picked
  from another Google branch).
- Board-specific behaviour stays in the board's directory or behind its
  config. A change to common code that a board needs is its own commit,
  small and with no effect on other boards.

What is carried (`git log base..c14`):

| Kind | Commits |
|---|---|
| MrChromebox | TBT5 compatibility; hibernated S4 to G3; the Vivaldi top-row command (`EC_CMD_KEYBD_TOP_ROW`, setup's "Keyboard Top Row Mode"); primus fan-table bounds; two battery static-info fixes; brya's host sleep timeout to infinite (no 10 s hang reset in s2idle); brya fan tables |
| Not taken from MrChromebox | his version string (ours replaces it); mithrax's keyboard backlight (another board) |
| Ours | the version string (`vifair22/chrome-ec`, FWID `<board>_16238.2+c14`); the C14 keyboard layout (Shift flips the top row, Search taps Caps Lock and holds Super, Search+Backspace is Delete, Lock sends screen-lock); `util/c14-build.sh`; this file |

## Tags

- `base/ec-YYYYMMDD`: an adopted Google snapshot.
- `c14-ec-build-YYYYMMDD[.N]`: the exact commit a kept EC image was built
  from. Tags are never moved or deleted once pushed.

A kept image is one flashed for day-to-day use. It is built from its pushed
tag and stored in the thinkpad-c14 repository under `firmware/ec/<date>/`
(`ec.RW.flat`, `COMMIT`, `SHA256SUMS`); the C14's coreboot config points
`CONFIG_EC_GOOGLE_CHROMEEC_FIRMWARE_FILE` at that image. Iteration builds are
not tagged; record their commit in the build directory instead.

## Building

`util/c14-build.sh` (needs Docker): the coreboot SDK container and toolchain
MrChromebox's chrome-ec-build uses for brya, with a check that fails on any
compiler warning. Output: `build/primus/RW/ec.RW.flat`. Build on
binhost-builder, not on the laptop.

## Pulling in upstream

1. `git fetch google firmware-ec-R136-16238.2.B-main` and fast-forward
   `base`; tag `base/ec-YYYYMMDD` and push both.
2. `git fetch mrchromebox` and review what is new on his branch since the
   last pick (`git log` against the provenance lines on `c14`); pick what the
   C14 needs, in his order, in the format above.
3. Rebase `c14` onto the new `base` if it moved; resolve once, there. Drop a
   pick that `base` has absorbed.
4. Build, test on the C14, then tag and keep the image.

A move to a newer Google release branch (a new `firmware-ec-R<N>-...`) is a
new `base`, adopted the same way, with the stack rebased onto it.

## Taking an image into use

The EC's RO is never written: coreboot's EC software sync writes RW from the
ROM when its hash differs and jumps to it. A bad RW is recoverable: turn off
"EC sync RW jump" in setup and the EC stays in RO; worst case, the SuzyQ.
Every new EC image is a new ROM, which moves PCR 2, so the station's TOTP
needs a reseal after it (thinkpad-c14 docs/gentoo-install.md).

Pushes, tags and flashes each need the user's go.
