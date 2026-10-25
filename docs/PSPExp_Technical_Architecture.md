# PSP Expansion Port (PSPExp)
## Technical Goals and Architecture

**Status:** Architecture specification, revision 3 — v0.1 scope frozen  
**Target client:** Sony PSP, initially PSP-1000  
**Host platforms:** macOS and Linux, including Raspberry Pi, ClockworkPi GameShell, Onion Omega2+, and conventional computers

---

## 1. Executive Summary

### Origins

PSPExp originated from an experiment initially called **PSPOmega**, which explored using an Onion Omega2+ as an external coprocessor for the PSP. The concept quickly evolved beyond any specific companion board: rather than merely providing services to PSP applications, an external host could execute the application while using the PSP itself as a hardware-accelerated graphics, audio, display, and input processor. That inversion led to the host-independent **PSP Expansion Port (PSPExp)** architecture described here.

### Architecture summary

PSPExp is a host-independent heterogeneous computing platform that exposes a Sony PSP as a hardware-accelerated graphics, audio, display, and input processor to an application running on another computer.

The central design principle is:

> **Do not stream completed frames. Stream rendering intent.**

Instead of rendering a game on a host and transferring a raw framebuffer to the PSP, the host executes the application and emits compact rendering commands such as `DRAW_SPRITE_BATCH`, `DRAW_TILE_BATCH`, and `PRESENT`. The PSP interprets those commands and uses its own Graphics Engine (GE) to perform the actual rendering.

A second principle follows from the asymmetry between the two processors:

> **The wire format is optimised for the PSP consumer, not the host producer.**

The host has CPU and memory to spare; the PSP does not, and the transport between them is the scarcest resource of all. The wire format is therefore **compact on the wire and trivial to expand on the PSP**: the host precomputes everything that requires knowledge or arithmetic, and the PSP performs only a fixed, branch-free expansion into GE vertices (§5.4).

This divides responsibilities naturally:

- **Host:** application/game logic, simulation, AI, scripting, networking, filesystem, asset preparation, and authoritative resource management.
- **PSP:** graphics execution, local resource cache, display, audio, and controller input.
- **Transport:** initially USB 2.0; the protocol itself remains transport-independent.

The first development host should be a Mac or Linux computer because it permits rapid iteration and instrumentation. The same protocol and PSP runtime should subsequently operate unchanged with embedded hosts such as a Raspberry Pi, GameShell board, or Omega2+. No host platform has special architectural status.

The architectural principles are collected in §21 and the hardware facts they rest on in §3.2 and §22; the rest of this document expands on them. The v0.1 scope (§18) is frozen as of this revision.

---

## 2. Project Goals

### 2.1 Primary goals

1. Use the PSP as a remote hardware-accelerated presentation processor.
2. Execute applications on a host while rendering through the PSP GE.
3. Transfer rendering commands rather than complete framebuffers.
4. Keep application state and heavy processing off the PSP where appropriate.
5. Use PSP RAM primarily as a runtime and resource cache rather than as the memory limit of the complete application.
6. Provide bidirectional low-latency communication: rendering/audio toward the PSP and input/status toward the host.
7. Support multiple interchangeable host systems without changing the PSP application.
8. Provide a local SDL executor for development, testing, and reference rendering.
9. Keep the command protocol independent of USB and independent of any individual game engine.
10. Eventually make existing `SDL_Renderer`-based applications practical candidates for PSPExp integration (see §12 for the scope of this goal).

### 2.2 Non-goals for the first release

The initial release should not attempt to provide every possible coprocessor service. HTTPS offload, GPIO, Bluetooth, generic remote filesystems, transparent PSP networking replacement, and arbitrary Linux desktop streaming are explicitly secondary.

The first release exists to validate one proposition:

> A host application can interactively drive the PSP GE using a compact remote command stream with sufficiently low latency for games.

"Sufficiently low latency" is made measurable in §3.3: the initial target is **≤ 50 ms input-to-photon** and a sustained 60 Hz present rate for the sprite benchmark over USB.

---

## 3. System Model and Hardware Constraints

### 3.1 System model

```text
                     HOST APPLICATION
                            |
                         libPSPExp
                            |
                    PSPExp Command Stream
                            |
                 +----------+----------+
                 |                     |
           Local Executor         Transport
            (SDL / Null)      (USB first; TCP)
                                       |
                                       v
                                  PSP Runtime
                           +----------------------+
                           | Command processor    |
                           | Resource cache       |
                           | PSP GU / GE          |
                           | Audio                |
                           | Controller input     |
                           | 480 x 272 display    |
                           +----------------------+
```

The application does not need to know whether its command stream is being executed locally by SDL or remotely by a physical PSP.

### 3.2 PSP hardware constraints that shape the design

These figures are the fixed points the protocol must be designed around. Each row is marked **[doc]** where it is taken from PSPSDK headers/documentation or hardware references, **[conv]** where it is established PSPSDK practice rather than a documented hardware rule, or **[assume]** where it is an assumption to be replaced by a v0.1 measurement. Sources are listed in §22.

| Constraint | Value | Status | Consequence |
|---|---|---|---|
| Main RAM (PSP-1000) | 32 MiB; user partition 24 MiB, of which 4 MiB is "volatile" memory that must be explicitly locked; plan on ≈ 20 MiB, confirm with `sceKernelMaxFreeMemSize()` at startup | [doc] / [assume] for exact free figure | Resource cache, not application memory; the runtime reports the measured figure in `HELLO` |
| VRAM (all PSP models) | 2 MiB of eDRAM for the GE (a further 2 MiB belongs to the Media Engine and is not GE-addressable) | [doc] | Two 512×272×16-bit framebuffers plus a 16-bit depth buffer consume 816 KiB; VRAM texture budget ≈ 1.2 MiB, or ≈ 1.45 MiB without a depth buffer (2D does not need one) |
| CPU | Allegrex MIPS32 R4k-class, 222 MHz default, 333 MHz max; homebrew may run at 333 MHz | [doc] | Fixed per-element expansion is affordable (§5.4); anything with per-element branching or lookup chains is not |
| Media Engine | Second MIPS32 core, same clock, own 2 MiB eDRAM | [doc] | Used since 2026-10-03 through the firmware's audio codec, which runs on it: MP3 decoded on the PSP (§8, compressed audio). Our own code on it: still a candidate for command expansion or audio mixing (§18) |
| GE texture sizes | Power-of-two width and height; hardware corrupts textures wider or taller than 512 even though the register accepts larger values (PPSSPP renders them correctly, hardware does not) | [doc] | Host must atlas and pad to ≤ 512; **every** executor, including PPSSPP-hosted, enforces the 512 limit at upload |
| GE texture formats | `GU_PSM_5650`, `5551`, `4444`, `8888`, `T4`, `T8`, `T16`, `T32`, `DXT1/3/5` | [doc] | Advertised in the capability handshake; anything else is rejected at upload |
| GE vertex data | Every vertex member must be 16-bit aligned; vertex arrays are read **in place** by the GE from the address in the display list (not copied); data must be written back from the data cache and remain valid until the GE has consumed it | [doc] | Expansion writes go to a double-buffered ring the GE reads directly (§5.4) |
| Buffer alignment | 16-byte alignment of vertex, texture and CLUT buffers | [conv] | Adopted as a rule for all GE-consumed buffers; it costs nothing |
| Texture cache behaviour | Drawing wide 2D images is markedly faster in 32- or 64-pixel-wide vertical strips | [doc] | Host defines atlas frames no wider than 64 px and slices larger sprites into several records |
| `GU_SPRITES` primitive | Two vertices per axis-aligned filled rectangle; no rotation | [doc] | Rotation/skew is a later, triangle-based batch format |
| Texture source | VRAM (fast) or main RAM ("reduced rate") | [doc] | Two-tier resource cache, §6.3 |
| Display | 480×272, 60 Hz, framebuffer stride 512 | [doc] | Fixed logical resolution for all executors |
| Byte order | Little-endian (as are x86, ARM, and the Omega2+'s MT7688 MIPS) | [doc] | Wire format is little-endian, packed structs, no byte swapping |
| USB | USB 2.0 hi-speed device port; the PSP is the USB *device*, the host uses libusb | [doc] / [measured] | **Measured 2026-09-22** (PSP-1000, PSPExp kernel driver, macOS libusb host, see `docs/v01-usb-bringup.md`): 25–26 MB/s host→PSP and 23–24 MB/s PSP→host with ≥ 64 KiB transfers, 13 MB/s with 4 KiB transfers; round trip 0.25 ms for 16 B, 3.9 ms for 40 KB. The 8 MB/s planning assumption was three times too pessimistic |
| Wireless | 802.11b | [doc] | A second, higher-latency transport (§14) |
| Audio | 8 hardware channels; no per-channel sample rate; one SRC channel accepting 8–48 kHz; blocks of 64–65,472 samples in multiples of 64 | [doc] | Host resamples cached SFX to the output rate; streamed audio may use the SRC channel (§8) |
| Input | Digital buttons + one analog stick; `sceCtrlSetSamplingCycle` configurable | [doc] | Sample independently of rendering, §7 |

### 3.3 Budgets

The figures below were written against an **8 MB/s planning assumption** and are kept as the conservative case. The **measured** v0.1 link (§3.2) is about 25 MB/s for large transfers and per-transfer overhead of roughly 0.1–0.15 ms each way, so: a 500-sprite frame (≈ 4 KB, one transfer) costs ≈ 0.3 ms, a 5,000-sprite frame (≈ 40 KB) ≈ 2 ms, and a 512 KB texture ≈ 20 ms (1.2 frames) rather than 64 ms. The conclusions do not change, only the margins.

**Command budget.** With the compact batch record of §5.4 (8 bytes per sprite plus per-batch headers), 500 sprites is ≈ 4 KB per frame and 5,000 sprites ≈ 40 KB; at 60 Hz that is 0.25–2.4 MB/s, i.e. 0.5–5 ms of transfer time per frame. Transport, not PSP parsing, is the binding constraint on draw volume, which is why the wire format is compact rather than GE-identical.

**Resource budget.** A 512×512 RGB565 texture is 512 KB — ≈ 64 ms over USB at the planning rate, i.e. four frames. CLUT8 halves that, CLUT4 quarters it. Two conclusions follow: resource uploads must never sit on a frame's critical path, and indexed formats are the default for 2D content, not an optimisation.

**Latency budget.** The input-to-photon path is:

```text
PSP samples input          ~0–8 ms   (sampling interval / 2 on average)
  -> USB to host           ~1–2 ms
  -> host waits for tick   ~0–16 ms  (host frame interval / 2 on average)
  -> host builds frame     ~1–3 ms
  -> USB to PSP            ~1–5 ms  (frame size / measured throughput)
  -> PSP expands + builds  ~1–2 ms
  -> GE renders            ~1–5 ms
  -> wait for vblank       ~0–16 ms
  -> LCD scan-out          ~8 ms
```

A plausible total is 35–50 ms, roughly two to three frames, compared with about 30 ms for a well-paced native PSP application. The **≤ 50 ms** target in §2.2 is deliberately close to this estimate; the benchmark plan (§17) measures every stage so the estimate can be replaced by data.

**Measured so far (v0.1 link, 2026-09-22, no GE executor yet):** USB to host and USB to PSP are ≈ 0.15 ms each for input-sized packets and ≈ 2 ms for a 40 KB frame. With one frame in flight, present-to-`PRESENT_DONE` averaged 16.6 ms (max 22.6 ms) at a sustained 60.1 fps: the PSP executed each 500-sprite frame in 0.17 ms and the rest was the wait for the next vblank. The transport is therefore not the dominant term in the latency budget; display pacing is.

**Measured with the GE executor (sprite demo, 2026-09-22):** 60 Hz sustained at 500, 2,000 and 5,000 sprites per frame (4 KB, 16 KB and 40 KB per frame); PSP-side parse-plus-expand cost 0.25 / 0.71 / 1.85 ms (≈ 0.37 µs per sprite); input sample to the vblank that displayed the frame consuming it: one frame period, 16.7 ms (measured). Adding the LCD's scan-out (≈ 8 ms) and the sampling phase of a real button press (0–16.7 ms), both estimated rather than measured, gives an input-to-photon of ≈ 25–41 ms: within the ≤ 50 ms target by that estimate. No optical measurement (a camera on the button and the screen) has been made, so the target is not yet verified end to end. The proof of concept of §16, including the cable pull, is complete (`docs/v01-usb-bringup.md`).

---

## 4. Responsibility Split

### Host responsibilities

The host owns the authoritative application state:

- game simulation and world state;
- AI and pathfinding;
- scripting runtimes;
- networking;
- filesystem and large asset stores;
- archive handling and decompression;
- image/audio preprocessing, including atlasing into ≤ 64-px-wide frames, palette reduction, swizzling, and resampling audio to the PSP output rate;
- glyph layout for text (§5.5);
- visibility, resource-prefetch and priority decisions;
- generation of rendering commands in compact batch form (§5.4);
- optional music/audio decoding and mixing.

The host can range from a MacBook or desktop PC to a Raspberry Pi, GameShell board, or Omega2+.

### PSP responsibilities

The PSP acts as a presentation processor:

- parse PSPExp commands;
- maintain local texture/font/mesh/audio resources in a two-tier cache whose physical placement it controls (§6.3);
- expand compact batches into GE vertices with fixed, branch-free per-element work (§5.4);
- construct efficient GE display lists;
- render to the LCD and report vblank/present completion;
- play cached or streamed audio;
- sample buttons and analog input independently of rendering;
- report input, capabilities, resource state, and performance metrics to the host.

The PSP should not need to understand game-specific concepts. A PSP renderer does not know what a "Footman" or "Peon" is; it knows resource handles, source rectangles, coordinates, transforms, and render state.

---

## 5. Graphics Architecture

### 5.1 Rendering intent instead of pixels

A traditional remote display path would be:

```text
Host -> render framebuffer -> transfer pixels -> PSP display
```

PSPExp instead uses:

```text
Host -> generate draw commands -> USB -> PSP GE -> PSP display
```

Example frame (shown in a readable form; the actual encoding is described in §5.4):

```text
BEGIN_FRAME seq=18421
SET_CLIP 0 0 480 272
DRAW_TILE_BATCH atlas=4 count=120
DRAW_SPRITE_BATCH atlas=17 count=2
DRAW_RECT x=0 y=224 w=480 h=48
DRAW_SPRITE_BATCH atlas=3 count=10      ; "Gold: 1275", expanded by the host from a glyph atlas
PRESENT
```

Scrolling, scaling, rotation, alpha blending, clipping, and similar operations should be represented as compact state or draw parameters and executed by the PSP GE rather than pre-rendered by the host.

### 5.2 Command buffering

Individual graphics calls should not normally cause individual USB transfers.

The host builds a frame command buffer:

```text
Frame N
+----------------------+
| BEGIN_FRAME          |
| state changes        |
| tile batches         |
| sprite batches       |
| text (as batches)    |
| PRESENT              |
+----------------------+
```

The complete buffer is transferred asynchronously in relatively large blocks. This minimises USB transaction overhead.

### 5.3 Batching

High-frequency primitives should have batch forms:

- `DRAW_SPRITE_BATCH`
- `DRAW_TILE_BATCH`
- `DRAW_QUAD_BATCH`

Batching reduces both protocol overhead and PSP-side command-processing overhead and maps naturally to efficient GE display lists. Single-element forms (`DRAW_SPRITE`, `DRAW_RECT`) exist for convenience and are treated by libPSPExp as one-element batches.

### 5.4 Batch wire format: compact on the wire, trivial on the PSP

This is the most consequential encoding decision in the protocol, and it was chosen by weighing the two costs against each other rather than by assumption.

**The GE-identical option.** Encoding batches as the exact vertex arrays the GE consumes — `GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_2D`, two vertices per `GU_SPRITES` primitive — would let the PSP point the GE at the payload with no per-sprite work at all, since the GE reads vertex arrays in place (§3.2). But each vertex is 12 bytes and each sprite 24, so 5,000 sprites cost 120 KB per frame, 7.2 MB/s at 60 Hz: essentially the whole planning USB budget, and 15 ms of transfer per frame. The fast path would be transport-bound.

**The compact option.** A record of 8 bytes per sprite that refers to a per-atlas **frame table** uploaded once:

```c
/* Uploaded once per atlas as part of the texture resource: */
struct PspExpFrame {          /* 8 bytes */
    uint16_t u0, v0;          /* texel origin in the atlas */
    uint16_t w,  h;           /* frame size; w <= 64 by host construction */
};

/* Per sprite, in a DRAW_SPRITE_BATCH payload: */
struct PspExpSpriteRec {      /* 8 bytes */
    int16_t  x, y;            /* screen position */
    uint16_t frame;           /* index into the atlas frame table */
    uint16_t color;           /* 5650 tint, or 0xFFFF for none */
};
```

The PSP expands each record into two `GU_SPRITES` vertices:

```c
v[0] = { f.u0,        f.v0,        rec.color, rec.x,        rec.y,        0 };
v[1] = { f.u0 + f.w,  f.v0 + f.h,  rec.color, rec.x + f.w,  rec.y + f.h,  0 };
```

That is one table lookup and a dozen 16-bit stores per sprite, with no branches and no arithmetic beyond four additions: on the order of 50–80 cycles, so roughly 1 ms for 5,000 sprites at 333 MHz. The wire cost is 40 KB per frame — a third of the GE-identical option — and the CPU cost is an order of magnitude below the frame budget. **The compact record wins on the binding constraint and is the v0.1 format.**

The PSP runtime's path for a batch is:

```text
for each record: expand into the vertex ring (cached writes)
sceKernelDcacheWritebackRange(ring segment)
sceGuTexImage(atlas ...)                        ; only if the texture changed
sceGuDrawArray(GU_SPRITES, fmt, 2 * count, 0, ring segment)
```

The vertex ring is double-buffered and 16-byte aligned; the segment written for frame N is not reused until `PRESENT_DONE` for frame N has fired, because the GE reads it in place. Batches are split by libPSPExp at a conservative record count per draw so no single draw approaches the GE's primitive-count field limits.

**Consequences for the host.** The host bears every cost that is not fixed per element:

- atlas frames are defined ≤ 64 px wide, so the texture-cache-friendly strip drawing of §3.2 falls out of the frame table with no PSP-side slicing;
- large sprites are emitted as several records referencing adjacent frames;
- sorting by atlas to minimise texture changes is done on the host;
- any per-sprite arithmetic (animation frame selection, tint computation, screen-space culling) happens on the host.

**Other batch kinds.** `DRAW_TILE_BATCH` uses the same record with `x, y` implied by a grid origin and stride (4 bytes per tile: `frame`, `color`); `DRAW_QUAD_BATCH` carries explicit `u0, v0, u1, v1` for arbitrary source rectangles (12 bytes). Rotation and skew require triangles rather than `GU_SPRITES` and are deferred to a later triangle-based batch format. Additional vertex formats are advertised in the capability handshake and selected per batch.

**Static geometry.** Where a batch is static across frames (tile maps that do not change, HUD chrome), a later version may allow the host to register it as a **pre-expanded static vertex buffer resource** (§6), which the GE then reads by handle with neither transport nor expansion cost.

### 5.5 Text

`DRAW_TEXT` with a string payload would make the PSP responsible for glyph lookup, kerning, and UTF-8 decoding, in conflict with keeping the runtime generic. Instead:

- fonts are **host-baked glyph atlases** uploaded as ordinary textures;
- libPSPExp provides `DRAW_TEXT` as a host-side convenience that expands to a `DRAW_SPRITE_BATCH` against the glyph atlas;
- the PSP runtime has no text opcode.

The SDL executor benefits equally: it renders text with the same atlas and never needs a font library. Glyph frames are naturally ≤ 64 px wide, so text obeys the strip rule of §3.2 for free.

---

## 6. Resource Architecture

Resources are persistent objects identified by compact handles. **Handles are assigned by the host**, so creating a resource requires no round trip; the host simply chooses an unused id and uploads.

Possible resource classes include:

- textures and texture atlases;
- palettes/CLUTs;
- fonts (glyph atlases plus host-side metrics);
- sound samples;
- static vertex buffers;
- index buffers;
- meshes.

Example:

```text
RESOURCE_CREATE_TEXTURE id=0x0142
width=256
height=128
format=CLUT8
palette=0x0021
layout=PSP_SWIZZLED
priority=HIGH
frames=37            ; frame table (§5.4) follows the pixel data
<data>
<frame table>
```

Subsequent frames refer only to `0x0142` and frame indices into its table.

### 6.1 Host-side preprocessing

The host should prepare assets in a PSP-friendly representation whenever possible:

```text
PNG -> decode -> atlas/pad to power-of-two -> RGB565 or CLUT -> swizzle -> USB -> PSP cache
```

The PSP therefore avoids expensive or memory-hungry decoding and conversion. Uploads that do not satisfy the constraints in §3.2 (dimensions, format, alignment) are rejected with a `RESOURCE_ERROR`, and the SDL executor enforces the same rules so that constraint violations are caught on the desktop.

### 6.2 Resource residency and the authority model

Authority over resources is split explicitly, because the two sides know different things:

- **The host is authoritative for existence and content.** A handle exists from `RESOURCE_UPLOAD` until `RESOURCE_RELEASE`; the PSP never discards a resource on its own. If an upload cannot be accommodated in any tier, it fails with `RESOURCE_ERROR OUT_OF_MEMORY` and the host must release something — the host has the scene knowledge to choose what.
- **The PSP is authoritative for physical placement.** Which tier a resident resource occupies (§6.3) is the runtime's decision, guided by host-supplied priority hints. The host knows what will be needed soon; the PSP knows its actual memory pressure.

Messages:

```text
Host -> PSP                          PSP -> Host
RESOURCE_UPLOAD  id, priority, ...   RESOURCE_READY   id, tier
RESOURCE_RELEASE id                  RESOURCE_ERROR   id, reason
RESOURCE_HINT    id, priority        RESOURCE_TIER    id, tier   (placement changed)
MEMORY_QUERY                         MEMORY_STATUS    vram_free, ram_free, counts
```

`RESOURCE_MISS` is reserved for a frame that references an unknown handle; because the PSP never evicts, it indicates a host-side bug (drawing after release, or before upload in stream order) rather than cache pressure.

Because the host understands the game scene, it can make intelligent residency and prefetch decisions:

```text
camera approaches sector 12
    -> upload texture 391
    -> upload texture 402
    -> upload mesh 73

later:
    -> DRAW_MESH 73
```

Given the resource budget in §3.3, prefetch is a requirement rather than an optimisation: a texture uploaded in the same frame that first references it will arrive two or three frames late.

### 6.3 Two-tier cache

The runtime keeps a **VRAM tier** (≈ 1.2–1.45 MiB after framebuffers, §3.2; fastest sampling) and a **main-RAM tier** (several MiB; reduced sampling rate). `MEMORY_STATUS` reports both tiers separately.

**What is built** (through 2026-10): placement is decided once, at upload. A texture uploaded with `priority` low goes to main RAM; any other goes to VRAM when it fits there and to main RAM when it does not (the GE executor also falls back to RAM when VRAM is too fragmented for it). Nothing moves afterwards: `RESOURCE_HINT` records a new priority without acting on it, the runtime never sends `RESOURCE_TIER` (the host library only understands it), and a texture stays in its tier until it is evicted or deleted.

**The design, not yet built:** the runtime promoting high-priority textures into VRAM when space allows and demoting lower-priority ones under pressure, reporting each move with `RESOURCE_TIER`, the host influencing placement only through `priority` at upload and `RESOURCE_HINT`. Room for a render target by moving textures out to RAM is on §12a's Next list for the same reason.

This makes PSP RAM a working-set cache rather than requiring the complete application and asset set to fit simultaneously in the PSP-1000's 32 MB.

### 6.4 Ordering between resources and frames

Resource operations and frame commands share one ordered stream (§10.3). A frame that references a handle is guaranteed to observe every resource operation that preceded it in the stream. The host therefore does not need to wait for `RESOURCE_READY` before drawing — but a frame that draws with a handle uploaded immediately before it will stall behind the upload, which is exactly what prefetch exists to avoid.

---

## 7. Input Architecture

Controller sampling is independent of frame delivery.

```c
struct PspExpInput {
    uint32_t sequence;     /* monotonically increasing per sample */
    uint32_t timestamp_us; /* PSP clock, for latency measurement */
    uint32_t buttons;
    uint8_t  analog_x;
    uint8_t  analog_y;
    uint8_t  reserved[2];
};
```

The PSP samples and reports input at a fixed rate (initially 60 Hz via `sceCtrlSetSamplingCycle`; higher rates are a capability). The host consumes the newest available state. Input packets are never queued behind frame or resource traffic (§10.3).

The `timestamp_us` field, together with the timestamp in `PRESENT_DONE` (§10.2), lets the host measure the round-trip latency of §3.3 without external instrumentation.

Input should not wait for frame acknowledgements. This avoids coupling controller latency to rendering performance.

---

## 8. Audio Architecture

Two complementary modes are useful.

### Cached sound effects

The host uploads a sample once, **already resampled to the negotiated output rate** (the PSP's hardware channels have no per-channel sample rate, §3.2):

```text
RESOURCE_CREATE_SOUND id=72 channels=1
<data at output rate>
```

Later:

```text
PLAY_SOUND id=72 volume=220 pan=-48
```

The PSP plays cached sounds on its hardware audio channels, so frequent sound effects are extremely inexpensive on the transport. The number of simultaneous SFX voices is a capability (initially 7 hardware channels, leaving one for the stream); whether voices map one-to-one onto hardware channels or through a software mixer feeding one channel is an implementation choice for v0.3, not a protocol matter.

### Streamed audio

Music or arbitrary application audio can be decoded (and, if the host prefers, pre-mixed) on the host and transferred as a buffered PCM stream. At 44.1 kHz stereo 16-bit this is ≈ 176 KB/s, about 2% of the planning USB budget. The stream may be delivered at any rate the PSP's SRC channel accepts (8–48 kHz), which lets a constrained host send lower-rate music and have the hardware resample it.

The PSP maintains a **jitter buffer** for the stream with a configurable target depth (initially ≈ 100 ms). The host is informed of buffer fill via `AUDIO_STATUS` so it can pace delivery; underruns are reported and result in silence rather than a stall of the graphics pipeline.

### Compressed audio (2026-10-03)

A runtime with `PSPEXP_FEATURE_AUDIO_MP3` takes `AUDIO_CODED_STREAM` (0x0307): whole MP3 frames (MPEG-1, 2 or 2.5 layer III), any number a message. The PSP decodes each with the firmware's audio codec (`sceAudiocodec`, which runs on the Media Engine: the main CPU waits for each frame and does nothing else), resamples it linearly to the stream's rate when the file's differs, and writes it into the same stream as `AUDIO_STREAM`, so `AUDIO_STATUS`, `AUDIO_STREAM_RESET` and the jitter buffer apply unchanged. A frame that does not decode is skipped. The codec is set up again when the frames' version, rate or channel mode change (the next track): PPSSPP's decoder crashed on such a change. Music costs a few KB a second on the link instead of 176 KB (128 kbps MP3: 16 KB/s) and next to no board CPU, which matters on the Omega. The console's music player uses it (docs/Launcher_and_Boards.md §7.2); `protocol/pspexp_mp3.c` parses frame headers on both sides. Tested in PPSSPP (MPEG-1 stereo 44.1 kHz CBR, MPEG-2 mono 22.05 kHz VBR, with a change of format between tracks: no drops, no underruns), then on the PSP-1000 from the Pi 400 (2026-10-03): a real song, MP3 VBR ~197 kbps, played by hand ("cool!"), with pauses and seeks: no dropped frames, 7 underruns in about two minutes, ≈ 25 KB/s on the link. A review (2026-10-09) found two overflows, fixed: the runtime checked the declared length as header + length, which wraps 32 bits on the PSP (`bytes` 0xFFFFFFF0 + 16 = 0: the decoder would have read past the message), as did `AUDIO_STREAM`'s header + frames x 4 on every host; both now compare against what is left after the header. And the resampler's `(b - a) x fraction` overflowed an int for full-scale steps once the fraction passed one half (32 kHz into 44.1 kHz): now 64-bit. `tests/test_runtime.c` covers both (a raw message of each kind, full-scale swings at 32 kHz); the overflow showed under `-fsanitize=undefined`, and the wrap in a 32-bit ARM build of the tests (2 failures before, none after).

### Sound on the board's Bluetooth (2026-10-03)

When the console has routed sound to a Bluetooth device paired with the board (`pspexp-bt`, System > Sound output), libpspexp writes the stream to a `pw-play` on the board instead of `AUDIO_STREAM`, in every program, and the link carries no sound; the PSP's volume buttons (`PSPEXP_BUTTON_VOLUP` / `VOLDOWN`, read by the runtime in kernel mode) set the device's volume, and `OSD_VOLUME` (0x000C, `PSPEXP_FEATURE_OSD`) has the PSP draw a volume bar over its frames for 1.5 s. Details in docs/Launcher_and_Boards.md §11. The note button (`PSPEXP_BUTTON_NOTE`, 0x800000, runtimes from 2026-10-05) is read the same way and pauses or resumes the console's Music app (docs/Launcher_and_Boards.md §10.4).

Graphics, input, resources, and audio are separate logical protocol channels even when they share the same physical connection.

---

## 9. Protocol Architecture

The protocol is versioned and transport-independent. The wire format is little-endian with packed structures (all target processors are little-endian; see §3.2). Payloads destined for the GE are placed so that they are 16-byte aligned relative to the start of the message.

Message header:

```c
struct PspExpMessageHeader {
    uint16_t magic;
    uint8_t  version;
    uint8_t  channel;
    uint16_t opcode;
    uint16_t flags;
    uint32_t sequence;
    uint32_t length;      /* payload bytes following the header */
};
```

Logical channels:

```text
SYSTEM     handshake, reconnect, PRESENT_DONE, errors
GRAPHICS   frames
RESOURCE   uploads, releases, residency
AUDIO      cached SFX control, PCM stream
INPUT      controller samples (PSP -> host)
DEBUG      metrics, traces
FILE       the PSP's memory stick (host -> PSP requests, one reply each)
```

*SYSTEM also carries CLOCK_SET / CLOCK_STATUS (2026-09-28, runtimes with
`PSPEXP_FEATURE_CLOCK`). The host sets the PSP's clock and, with
`PSPEXP_CLOCK_ZONE`, its time zone and summer time. See
docs/Launcher_and_Boards.md §9. And POWER_QUERY / POWER_STATUS (2026-10-02,
runtimes with `PSPEXP_FEATURE_POWER`): the host asks, the PSP answers with
its battery (`PspExpPowerStatus`: charge, the power service's estimate of the
time left, the battery's current, temperature and voltage, and whether it is
charging, on the adapter or low), read from `scePower*` at that moment. The
host library asks without waiting (`pspexp_power_query`) and keeps the last
answer (`pspexp_power_status`); the launcher asks every 10 s. And
TEXT_REQUEST / TEXT_RESULT (2026-10-18, runtimes with `PSPEXP_FEATURE_TEXT`):
the host asks for typed text (`PspExpTextRequest`: a title, the text the field
starts with, a length; `pspexp_text_request`) and the PSP opens its own
keyboard (`sceUtilityOsk`) over the screen. The runtime draws it from its main
loop, one frame a pass between the host's frames
(`pspexp_ge_keyboard_frame`), the link read and written as ever (a keyboard
that blocked the loop would leave the link silent while someone types);
the PSP's buttons go to it, not to the host. Until the answer the host
library says not to draw (`pspexp_should_render`); a frame already on its
way is drawn and not shown. The answer (`PspExpTextResult`: typed,
cancelled, or not shown) comes once through `pspexp_text_result`. The SDL
layer opens it when a game turns text input on (a text field focused) after
the layer turned it off once the game was drawing (SDL2 starts with it on;
OpenMW then never turned it on for its first field), and gives the game the
text as SDL_TEXTINPUT and Enter. Only a turn from off to on opens it, never
within 1.5 s of an answer: X-Moto's first start, asking for a profile name,
called `SDL_StartTextInput` with text input already on and took Enter back to
its field, and the keyboard opened 1094 times in a test run (2026-10-18; with
the check, once). X-Moto turns text input on at every event (`GameInit.cpp`), so it
opened once, over its start-up note, and never for its name box; its box also takes
one character a `SDL_TEXTINPUT` and dropped the answer sent whole. The answer now
goes a character an event, and `PSPEXP_TEXT_BUTTON` opens the keyboard on a button:
its recipe has triangle, the nub as the pointer and square its click (New Profile
wants the mouse); a first start then makes its profile (2026-10-21). Tried: the PSP runtime under PPSSPP opened
the keyboard for OpenMW's name field from the Pi; the answer's path through
the stand-in (`PSPEXP_SERVER_TEXT`: a preset answer), the name taken and
character creation going on. On the PSP-1000 from the GameShell (a test
program turning text input off and on): the keyboard opened, the text
typed on the PSP arrived as SDL_TEXTINPUT followed by Enter, the link kept. OpenMW on the Pi from the launcher, with the PSP shooters'
layout (the nub looks, the face buttons move, L activates and presses a
dialog's default button, R attacks and clicks): the name typed on the PSP,
moving and looking right; its pointer at first invisible, two layer fixes
(the pointer left off-screen when its window shrank from 1920x1080 to
480x272; its cursor pictures empty, 32x32 with no visible pixel, the arrow
drawn instead). Its menus are laid out for at least 800x600 (the main menu's
items over the logo; the options window's MinSize 640x550). Rendered at
480x330 with its interface scale at 0.6 (550 virtual lines: every window
whole), shown by the layer at its own pixels (`PSPEXP_MAGNIFY=pointer`): in
its menus the 480x272 round the pointer, scrolled as the pointer reaches an
edge; in play (pointer hidden) the frame's bottom 272 lines, the HUD in view
and the top 58 lines of the scene cut. Fitting the whole 330 lines instead
(shrunk to 82%) made the text in play too small; the interface at 1.0 and
480x272 read best but cut the options window and overlapped the main menu.
Played on the PSP-1000 this way: playable. Packaged: `pspexp-openmw`
(`build-openmw.sh`: OpenMW 0.51.0 with Bullet 3.17, MyGUI 3.4.3 and its
RecastNavigation fetched at build, all in the source archive) with
`/usr/games/morrowind`, which finds the player's copy in ~/Morrowind (a game
folder or GOG's installer, unpacked by innoextract), writes openmw.cfg for it
and the PSP's settings once; recipe `morrowind`, a Pi only.*

RESTART (2026-10-24, runtimes with `PSPEXP_FEATURE_RESTART`): the host asks
PSPExp on the PSP to start itself again (`pspexp_restart_psp`). The runtime
leaves as it does for HOME (the runtime freed, the GE and the USB stopped) and
starts its own EBOOT again with `sceKernelLoadExec` (`argv[0]`, the path the
loader passed it), so the loader boots the active slot: a runtime the console
has just put in the spare one runs. The console does so right after its own
Update the PSP (only then: a new runtime that fell back never loops), and the
control port's `psp-restart` and `pspexp-ms restart` ask for it. Tried in
PPSSPP: an update into slot b, then a restart, booted slot b and confirmed it.
On the PSP-1000 (6.60 PRO-C) it never worked (found 2026-10-25): user mode's
`sceKernelLoadExec` failed at once each time ("starting again failed" in
`pspexp-rt.log`) and the PSP left for the XMB, PSPExp then started by hand.
Now the runtime asks PSPExp's USB module, in kernel mode, to start it as the
PSPExp plugin brings PSPExp back at a game's exit (`pspexpUsbRestart`:
PRO's `sctrlKernelLoadExecVSHWithApitype`, apitype 0x141, no ISO left set),
and only without PRO (PPSSPP) the plain call. On the PSP: `psp-restart`
back in 7 s, and a full Update the PSP (slot b, the plugin) restarted into
the new slot in 7 s, no fallback.

CAPTURE_SET and CAPTURE_DATA (DEBUG 0x0508 and 0x0509, 2026-10-24, runtimes
with `PSPEXP_FEATURE_CAPTURE`): Recording inside PSPExp (§12b). The host
sends CAPTURE_SET (`PspExpCaptureSet`, `on`: 0 off, 1 everything all the
time, 2 only while a ♪+L recording runs) when the PSPExp plugin host is
there, the receiver's hello saying which; the PSP then sends what it shows and plays as CAPTURE_DATA
(`PspExpCaptureData`: the count of bytes that follow, padded to 16), the
bytes being the receiver's own packets, cut anywhere. A new link (HELLO)
turns it off.

Future extensions could add networking, compute/RPC, or other services without changing the fundamental graphics architecture.

*The FILE channel (6), added 2026-09-28:*
- **Requests:** FS_LIST, FS_READ, FS_WRITE, FS_MKDIR, FS_REMOVE,
  FS_RENAME, FS_INFO and FS_STAT. Each carries a `PspExpFsRequest` (an id,
  flags, an offset, a length and two 256-byte paths); FS_WRITE's data
  follows the request.
- **Replies:** FS_REPLY is a `PspExpFsReply` (the same id, a status that
  is `PspExpFsError` when negative, and values). It is followed by up to 64
  `PspExpFsEntry`s (a listing, a stat) or up to 64 KB of data (a read).
- **Offering it:** the runtime announces the channel with the HELLO
  feature bit `PSPEXP_FEATURE_FILES`. A runtime without the bit would drop
  the link on the unknown channel, so hosts ask only when it is set.
- **Accepted paths:** `ms0:/` and `ef0:/`, with no `..` parts.
- **Where requests are answered:**
  - on the PSP: `psp/runtime/pspexp_fs_psp.c` (sceIo);
  - on desktop executors: `runtime/pspexp_fs_posix.c`, where a folder given
    by `PSPEXP_MS0` plays the stick.
- **Host API:** `pspexp_fs_*` in `libpspexp`. These calls block, pumping
  the link while they wait. Users are the console's file manager (a drive,
  docs/Launcher_and_Boards.md §7.3) and `tools/pspexp-ms`.

### 9.1 Capability negotiation

Connection startup negotiates capabilities rather than assuming a specific PSP model or runtime configuration.

```text
PSP -> HELLO
       protocol=1
       runtime_version=...
       display=480x272 refresh=60
       texture_formats=[RGB565, RGBA5551, RGBA4444, RGBA8888, CLUT4, CLUT8, DXT1, DXT3, DXT5]
       texture_max=512x512 pow2=true
       vertex_formats=[SPRITE_2D_5650, ...]
       vram_budget=...  ram_budget=...
       audio_channels=8 audio_rates=[...]
       input_rate_max=...
       features=[...]

Host -> HELLO_ACK
        protocol=1
        input_rate=60
        audio_stream=44100x2
```

When the two sides speak different protocol versions and the runtime has the file channel, the host doesn't refuse: it answers with a `HELLO_ACK` whose `flags` has `PSPEXP_ACK_MAINTENANCE`. Only SYSTEM and FILE are used then, no graphics or audio, and the host uses the link to install a matching runtime (§9.3). The framing, the first fields of `HELLO` and the FILE messages are frozen for this reason: new file operations are new opcodes.

Any `HELLO` invalidates all PSP-side state: the runtime releases every resource and empties its queues before replying. This is what makes reconnection simple (§9.2).

### 9.2 Reconnection and resynchronisation

The link will be lost: the cable is pulled, the PSP suspends, the host process restarts. Because the host is authoritative for all state, recovery does not require any PSP-side persistence:

1. The transport reports disconnection to libPSPExp.
2. libPSPExp marks every resource as non-resident and drops queued frames.
3. On reconnection the PSP sends `HELLO`; the host replies `HELLO_ACK`.
4. The host re-uploads its current working set (which it already tracks for prefetch purposes) and resumes frames.

An application using libPSPExp should experience this as a pause, not an error.

**Handing the PSP over** (2026-10-10). The console closes with `DISCONNECT` when it lets another program have the PSP. When the launcher starts a game, that `DISCONNECT` carries a `PspExpDisconnect` with `PSPEXP_DISCONNECT_HANDOVER`, a point and two colours: the launcher's last frame shows the game with "Starting" and a 24-pixel square beside it, and the runtime keeps that frame on screen and draws a spinner there (eight dots, the leading one brightest, every 80 ms, straight into the displayed framebuffer: `pspexp_ge_spinner`) until a program draws a frame (its frame count moves) or 30 s pass (then the status page). Before, the PSP showed its text status page while the game loaded. The console sends the launch only after that frame has gone (at most half a second's wait). A `DISCONNECT` with no payload is as before, and runtimes before this one ignore the payload. Checked in PPSSPP: the hand-over logged, the status page not shown, the next program connecting 6 s later and drawing.

### 9.3 Installing and updating the runtime

The EBOOT is a small boot loader that rarely changes. The runtime (`pspexp_runtime.prx` and `pspexp_usb.prx`) lives in two slots beside it, `slot-a/` and `slot-b/`. Each has a `VERSION` file with every file's size and SHA-256, and the loader checks the whole set before booting it. `active` names the slot and counts starts not yet confirmed by a connection. The host updates the slot not in use over the FILE channel (`pspexp_slots_update` in libPSPExp), and a complete `active.new` switches to it at the next start. Three unconfirmed starts, or a slot that doesn't check, make the loader boot the other slot. The full lifecycle and its failure cases are in [Package_Manager.md §6.1](Package_Manager.md).

---

## 10. Execution, Pacing, and Ordering

### 10.1 Asynchronous operation

The two processors operate asynchronously.

Avoid:

```text
host sends frame -> waits -> PSP renders -> ACK -> host continues
```

Prefer:

```text
Host                        PSP
build frame N
   +---- frame N ----------> queue
build frame N+1              render N
   +---- frame N+1 --------> queue
build frame N+2              render N+1
```

### 10.2 Frame pacing

A free-running host has no knowledge of the PSP's vblank. Without a signal back it either drops frames constantly (visible judder) or fills its queue to the cap (added latency). PSPExp therefore closes the loop:

**What is built** (through 2026-10): the host side of the loop.
- The PSP sends `PRESENT_DONE { frame_seq, timestamp_us, queue_depth, dropped, ... }` on the SYSTEM channel for each frame it presents (with its execute and GE times, bytes and sprite counts).
- The host library paces on it: it allows one frame in flight by default (`max_in_flight`), and the SDL layer waits (up to 40 ms) for the previous frame's `PRESENT_DONE` before presenting the next (`pspexp_wait_present`).
- The runtime has **no frame queue**: it executes each message as it is read from the link, in the main loop, and `queue_depth` and `dropped` are always 0. Nothing is discarded: a frame that arrives while another is executing waits in the transport. Reading the PSP's buttons and sending `INPUT_STATE` happens in the same loop after the commands read, so a long frame delays input by as much.

**The design, not yet built:** a runtime queue bounded at **one pending frame** (one displayed, one rendering, one queued), a newer complete frame replacing a queued one (counted in `dropped`), a frame whose execution has begun never discarded, and input read on its own schedule. Its policy for interactive rendering:

> Prefer the newest complete frame over preserving obsolete frames.

With the host waiting for each frame (above), at most one frame is ever ahead of the PSP, so a queue would rarely have anything to drop; it matters for a host that does not wait.

### 10.3 Ordering and drop policy

All transports used by PSPExp (USB bulk, TCP) are reliable, so the protocol does not distinguish messages by delivery guarantee. It distinguishes them by **ordering domain** and **drop policy**:

| Channel | Ordering | Drop policy |
|---|---|---|
| GRAPHICS + RESOURCE + SYSTEM (host → PSP) | One ordered stream | Frames may be discarded before execution begins; resource and system messages are always executed, in order |
| AUDIO stream | Ordered, own buffer | Never dropped by the runtime; jitter buffer may underrun |
| INPUT (PSP → host) | Ordered, own queue | Host keeps only the newest sample |
| DEBUG | Ordered | May be rate-limited |

Placing frames and resources in a single ordered stream gives the guarantee in §6.4 (a frame sees all preceding resource operations) at the cost of **head-of-line blocking**: a 512 KB upload occupies the stream for around four frame periods at the planning throughput (§3.3), and any frame queued behind it is delayed by that much regardless of prefetch. Prefetch reduces how often this happens; it does not remove the mechanism. The v0.1 design accepts this for the simplicity it buys, and benchmark workload (c) in §17 exists to quantify it. The expected eventual design, if the measurements demand it, is a separate bulk-resource stream with explicit fences (`RESOURCE_READY` acting as the fence a frame may wait on) so that latency-sensitive frames overtake bulk uploads; see §19. Input travels on its own path from the outset so that a stalled upload never delays controller data.

### 10.4 Backpressure

If the PSP cannot sustain the host's frame rate, `PRESENT_DONE` cadence tells the host so. The recommended application behaviour is to keep the simulation tick rate fixed and skip *render* frames, which libPSPExp exposes as a `should_render()` hint derived from the present cadence. The host must never let the simulation slow to the PSP's presentation rate.

---

## 11. Executors and Development Model

PSPExp begins with three command executors, plus one testing configuration.

### Null executor

Consumes and validates command streams without rendering.

Uses:

- unit tests;
- protocol validation, including the constraint checks of §6.1;
- benchmarks;
- resource lifetime checks;
- command statistics;
- continuous integration.

### SDL executor

Consumes the exact PSPExp command stream and renders into an SDL window. It is the reference implementation and development renderer, and shows *what the application intended*.

The SDL window intentionally models PSP constraints:

- 480 × 272 logical resolution;
- the texture formats, sizes, and alignment of §3.2, rejected on upload exactly as the PSP would;
- PSP-like VRAM/RAM budgets with the same two-tier accounting;
- equivalent clipping/blending semantics where practical;
- the same compact batch expansion as the PSP (§5.4), ideally sharing the expansion code from `protocol/`.

Keyboard or desktop gamepad input emulates PSP controls.

### PSP executor

Receives the same serialised command stream over the transport and translates it into PSP GU/GE operations. Implementation notes that are easy to get wrong the first time:

- data written by the CPU into upload or vertex buffers must be written back from the data cache (`sceKernelDcacheWritebackRange`) before the GE reads it, because the GE reads those buffers in place;
- the vertex ring segment for a frame must not be overwritten until that frame's `PRESENT_DONE`;
- all GE-consumed buffers are 16-byte aligned and, for VRAM, addressed through the uncached segment;
- textures uploaded to VRAM must not overlap the framebuffers or depth buffer.

### PSP executor under PPSSPP

The PSP executor can also run inside the PPSSPP emulator, connected to the host over the TCP transport (§14) via the emulator's network stack. This tests the *real* PSP executor — GE semantics, swizzling, CLUT handling, clipping — without hardware and with the emulator's GE debugger available. It complements rather than replaces the SDL executor: SDL shows intent, PPSSPP shows PSP behaviour, hardware shows timing.

PPSSPP is more permissive than hardware in places (it renders textures wider than 512 px that hardware corrupts, §3.2), so it never relaxes a constraint check and never stands in for the final hardware measurement.

This symmetry is critical:

```text
                     PSPExp command bytes
                    /        |         \
                   /         |          \
          SDL Executor   PSP Executor   PSP Executor
               |          (PPSSPP)      (hardware)
         desktop window   emulated GE    PSP GE/LCD
```

If SDL output is correct and PSP output differs, debugging is localised to the PSP executor or PSP-specific resource handling. If PPSSPP output is correct and hardware differs, the problem is timing, caching, or transport.

---

## 12. SDL Integration: Two Directions

Two separate SDL components are envisaged.

### PSPExp → SDL

`pspexp-sdl` executes PSPExp commands using SDL.

Purpose:

- reference renderer;
- virtual PSP for intent;
- development without physical hardware;
- CI and visual regression testing.

### SDL → PSPExp

A later `sdl-pspexp` compatibility/backend layer maps suitable SDL rendering/input/audio operations into PSPExp commands.

Conceptually:

```text
Existing SDL application
          |
    SDL_Renderer API
          |
      sdl-pspexp (custom render driver)
          |
     PSPExp commands
          |
       transport
          |
       PSP GE
```

**Scope.** `sdl-pspexp` can only capture applications that draw through `SDL_Renderer` (SDL2) or its SDL3 equivalent: textures, copies, fills, and lines. Applications that perform software blits into an `SDL_Surface` and present the result — a common pattern in older 2D engines, including Stratagus — hand SDL a completed framebuffer, which is precisely what PSPExp is designed not to transport. Such engines need an engine-specific renderer backend instead (§18, v0.2). This limits the reach of goal 10 in §2.1, but the `SDL_Renderer` population is large enough to make the layer worthwhile.

*Status (2026-09-22, v0.4 built).* The layer exists as `compatibility/sdl-pspexp`, an interposition library (`DYLD_INSERT_LIBRARIES` / `LD_PRELOAD`, `scripts/dev/pspexp-run`) rather than an SDL render driver: it needs no SDL rebuild and works with the SDL2 the program already links. It mirrors the `SDL_Render*` API into scaled quad batches (a 24-byte `PspExpQuadExRec` with `PSPEXP_BATCH_SCALED`, since `SDL_RenderCopy` scales and flips), rects and lines, shadows textures in RGBA8888 so `SDL_UpdateTexture` / `SDL_LockTexture` changes are re-uploaded, taps the audio device (callback or queue) into `AUDIO_STREAM`, and exposes the PSP's controls as a virtual game controller plus keyboard events. Against the paragraph above, the surface-blitting case turned out to be affordable after all at this resolution: a renderer-less program's window surface is shrunk to the PSP size and streamed as a texture every present, ≈270 KB per frame, which the measured USB link (§20a) carries at 30–60 fps. Not mirrored: rotation, render targets, `SDL_RenderGeometry`, OpenGL (OpenGL 1.x since 2026-09-30: §12a).

The two components must remain conceptually distinct:

```text
pspexp-sdl : PSPExp -> SDL
sdl-pspexp : SDL    -> PSPExp
```

---

## 12a. OpenGL 1.x

*Added 2026-09-30.* The GE is fixed-function hardware of OpenGL 1.x's kind: a projection, view and world matrix, four lights with materials and colour-material, linear fog, a depth buffer, alpha test, blending, texture functions (modulate, decal, blend, replace, add), and texture-coordinate generation from a texture matrix or an environment map. So an OpenGL 1.x program can be translated rather than captured: the board keeps GL's state, the PSP does the per-vertex and per-pixel work. Shaders (GL 2 and later) cannot; the only route for those would be rendering on a board with a GPU and streaming frames, left aside.

**Where.** `compatibility/sdl-pspexp/gl_pspexp.c`, an OpenGL implementation inside the SDL layer: programs that make their context through SDL2 get ours (`SDL_GL_*` hooked; on a headless board the window's `SDL_WINDOW_OPENGL` flag is dropped, SDL's dummy driver having no GL). The `gl*` functions are exported (Linux, first through `LD_PRELOAD`) or interposed (macOS), and `glXGetProcAddress`, `eglGetProcAddress` and `SDL_GL_GetProcAddress` return them, so GLEW, GLU and libraries such as QuesoGLC work unchanged.

**Messages** (GRAPHICS channel, only when HELLO has `PSPEXP_FEATURE_3D`; `HELLO_ACK` flag `PSPEXP_ACK_DEPTH` asks for a depth buffer, which takes the 272 KB after the two framebuffers; a repeated `HELLO_ACK` changes it while no texture is in VRAM):

| message | content |
|---|---|
| `3D_STATE` (96 B) | the render state in GE values: enables, depth/alpha/blend functions, winding kept by culling, texture handle, function, filters and wrap, fog, viewport and depth range, scissor, pixel mask, texture-coordinate mode |
| `3D_MATRICES` (192 B) | projection, modelview (the GE's world matrix; its view matrix stays the identity, so lighting and fog are in eye space), texture matrix |
| `3D_LIGHTS` (288 B) | scene ambient, material, colour-material, four lights |
| `3D_DRAW` | a primitive and its vertices in a GE vertex format: optional float texture coordinates, 8888 colour, float normal, float position; with `PSPEXP_FEATURE_3D_INDEXED`, 16-bit indices after the vertices (`index_count`, each index checked by the runtime): the board sends a `glDrawElements` triangle list so, each shared vertex once, when nothing in it needs cutting (else as triangles, cut); with `PSPEXP_FEATURE_3D16` also 16-bit coordinates (unsigned, 0..2) and positions (signed, -1..1), which the board uses for unlit draws of 16 vertices or more: positions over a power of two that fits them, the modelview sent times it (the GE does not renormalise normals, so lit draws stay floats); 24 bytes a vertex become 16 |
| `RESOURCE_UPLOAD_MESH` / `3D_DRAW_MESH` | vertices or 16-bit indices kept in the PSP's RAM (a resource like a texture); a draw names them (16 bytes) or brings its own indices |
| `3D_CLEAR` | colour and/or depth within the scissor |
| `3D_MORPH` (with `PSPEXP_FEATURE_3D_MORPH`, 2026-10-01) | the weights of the morph targets of the draws that follow (`sceGuMorphWeight`): vertices then carry 2 to 8 targets one after another (`PSPEXP_3D_VT_MORPH(n)`), 8-bit normals allowed (`PSPEXP_3D_VT_NORMAL8`); Quake's models |
| `RESOURCE_PATCH` (with `PSPEXP_FEATURE_PATCH`) | bytes of a resident texture replaced at an offset in its data as uploaded: in a swizzled texture the 8 × 8 blocks of an 8-row strip run left to right, so the changed rectangle of a `glTexSubImage2D` goes as one run per strip (Quake's lightmaps: a few KB instead of 128 KB) |

The board sends the state, matrices and lights only when they change (and once per frame), and translates GL's conventions: depth runs 65535 near to 0 far on the GE, so the comparisons turn round; `GL_ONE`/`GL_ZERO` blend factors become fixed colours; the framebuffer is 5650, so destination alpha counts as one; GL's viewport maps to the letterboxed window on the PSP. 2D draws may follow 3D ones in a frame: the executor puts its 2D state back first.

**Bytes.** Vertices sent with every draw cost 28–36 bytes each; a scene of Neverball's (60,000 vertices in 56 buffer-object draws) was 2.1 MB a frame, 3 fps. Buffer objects are therefore **meshes**: converted once, whole, into the GE format and kept on the PSP, keyed on everything the conversion read (arrays, buffer versions, vertex type); a vertex without a colour of its own takes the current colour from the material (the GE's rule for colourless vertices), so one mesh serves any `glColor`. Neverball: 85 KB a frame. Texture-coordinate generation stays on the GE too (object/eye-linear through the texture matrix with the vertex position as source; sphere maps as the environment map with two spare light slots as the x and y axes, exact for a view straight ahead), so it does not stop a draw being a mesh.

**Measured** 2026-09-30, PSP-1000 over USB from the Pi 400: the demo (`examples/gl-demo`) 59.9 fps, PSP CPU 1.9 ms and GE 0.12 ms a frame; Chromium B.S.U. (menu) 36 fps, 230 KB a frame of immediate-mode vertices; Neverball (title) 30 fps, GE 20 ms a frame. The GE's clipping of large polygons near the camera showed no holes in Neverball's level. The first real-PSP run also showed that PPSSPP's framebuffer dumps are stale (the emulator keeps drawn frames in its own GPU): the runtime now takes screenshots with a GE block transfer, which PPSSPP copies back faithfully.

**Shaders** (2026-09-30). The GE runs no programs, but most game shaders only say things it can do. `compatibility/sdl-pspexp/glsl_pspexp.c` parses GLSL and evaluates a program symbolically at each draw, with its uniforms as constants: uniform branches are taken, constant loops unrolled, functions inlined, and the vertex shader's outputs become the fragment shader's inputs, so the fragment colour ends as an expression of vertex attributes and texture samples. The common forms map exactly: `gl_Position` a matrix times one attribute (the GE's projection), a texture sample times a per-vertex factor (modulate, the factor worked out on the board), `discard` below a constant alpha (the alpha test). Other forms are drawn as near as the GE can and logged. No GPU or Mesa is used, so it works on every board, the Omega included. A survey of Debian's 46 SDL2 + OpenGL games (2026-09-30) found 23 with GLSL; the simple ones (openMrac, Numptyphysics, SuperTux's normal path, 6 of Widelands' 8 shader pairs) plan exactly; shadow maps, several textures blended per pixel and post-processing (Trackballs, Performous, Red Eclipse, Dolphin, MAME's filters) are beyond it. Framebuffer objects became **render targets** (`RESOURCE_CREATE_TARGET`, `3D_TARGET`): the GE draws into a VRAM texture, the same mechanism the SDL layer's render targets can use. On the PSP-1000 over USB from the Pi 400 (2026-09-30): openMrac (menu) 59.9 fps, Numptyphysics 59.9 fps (52 KB a frame), SuperTux 30 fps (263 KB a frame, its sprites streamed), Widelands 30 fps. Two more things came out of that run: textures now leave the PSP least-recently-drawn first when its memory is full (Widelands uploads hundreds while loading, all drawn into a render target in one long frame, so such a frame is sent early to make room), and a large draw whose vertices come again unchanged becomes a mesh by content (Numptyphysics rebuilds its pen strokes, 3 x 10,700 vertices, every frame: 517 to 52 KB a frame, 24.5 to 59.9 fps).

**Clipping** (2026-09-30, found playtesting Neverball on the PSP: parts of the level vanished as the view turned; PPSSPP had not shown it). The GE draws within a 4096 x 4096 space around the screen and drops a whole triangle that reaches outside it rather than cutting it. The board now works out where each vertex falls with the matrices it sends (for a mesh from a host copy of its positions, once per frame and matrix) and cuts the triangles that reach outside a band 10 % inside that space, or behind the near plane, in clip space with their attributes interpolated; the pieces go with the frame, the rest of a mesh is drawn by index. Neverball: nothing vanishes, 30 fps as before, 48 to about 67 KB a frame.

**Shader draws as meshes** (2026-09-30, from playtesting openMrac on the PSP: 3 fps in a race). A shader draw whose position is a matrix times one attribute, everything else it reads coming from buffers, is made a mesh: its vertices worked out once through the plan and kept on the PSP, keyed on the plan's per-vertex work (a hash of its expressions) and the arrays and buffers read; the board keeps them too, for clipping. Where the per-vertex work changes every frame (openMrac's lighting shader: colours from the normal matrix as the camera turns) the draw is seen making a new mesh frame after frame and is streamed instead for a while; small draws (under 192 vertices: text, 2D, often from buffers made each frame) are streamed from the start. With that, vertex arrays resolved once per draw instead of a buffer lookup per vertex, and texture lookups through a table: openMrac's race 3 to 41 fps on the PSP-1000 over USB from the Pi 400.

**Shader lighting on the GE** (2026-09-30). A fragment colour of the form `clamp(ambient + max(0, normal · light) × diffuse)`, per channel, with the normal an attribute times a matrix, is recognised by the plan (`lit`: the light's direction taken into the attribute's own space, the ambient and diffuse colours) and drawn with the GE's directional light, the attribute as the vertices' normals. The per-vertex work then no longer changes as the camera turns, so these draws are meshes too. openMrac's race: 41 to 59.9 fps on the PSP-1000 (71–113 KB a frame, PSP CPU 1.7–3.7 ms, GE 4.9–6.4 ms; 23 meshes, 10 draws streamed), looking as before.

**Cube maps: reflections** (2026-09-30). The GE has no cube maps and one texture. A fragment colour of the form `colour + K × textureCube(dir)`, or the cube map alone, is recognised by the plan: the rest is drawn as before, and the cube map in a pass of its own as the GE's environment map. That needs a sphere map, indexed by the eye-space normal's x and y. The board keeps the cube map's six faces and makes one from them through the shader's own expression for the direction: the normal is the vertex output that is linear in one attribute (a normal matrix times it), and the eye position is the point at the middle of the screen, so the view is straight ahead as with GL's sphere maps. The direction is worked out on a 17 × 17 grid, as a list of steps with each shared part evaluated once, and interpolated between (0.9 ms for a 64 × 64 map on the Pi 400; 245 ms as a tree per texel). A map is made again only when the direction at five probe normals has moved; a few are kept, one per car in openMrac. The added pass blends ONE, ONE with the depth test EQUAL, so it lands only where the colour's pass drew. A discard on a texture the colour does not use (openMrac's windows: the cube map where the body texture is clear) becomes a pass before, with that texture's alpha test and only the depth written; the colour's pass then draws where the depth is equal. A discard on the pixel's place (`gl_FragCoord`: a screen-door pattern for seeing through) is left out, drawn whole. The GE takes the environment map's u from the second light given to `sceGuTexMapMode` and v from the first, on the PSP-1000 as in PPSSPP: checked against a real OpenGL with a torus reflecting a cube map with a different colour on each face (`GLDEMO_CUBE=1`); the fixed-function sphere maps had them the wrong way round too. openMrac's windows went from black to tinted glass with the sky in it, the race still at 59.9 fps (93–134 KB a frame).

**Quake** (2026-09-30; Quakespasm from Debian, the shareware episode). Its fast path, which it takes with GLSL and three texture units, draws the world from one vertex buffer through a shader of the form `clamp(texture × lightmap × 2 + glow)`, with fog. The plan recognises a second 2D texture multiplied in and a third added: after the colour's pass, one pass per texture, each with its own texture coordinates and so its own mesh (the world: two meshes of 25,000 vertices, 1.2 MB), blended in where the colour's pass drew (depth EQUAL): DST_COLOR × ZERO or × SRC_COLOR (once or twice over), ONE + ONE. More than twice over (Quakespasm's 10-bit "wide" lightmaps want 8×) is the texture sent brighter, clamped: a texture keeps a factor it is stored with (10-bit uploads, `GL_UNSIGNED_INT_10_10_10_2`, keep their bits as 8-bit ×4) and one the draws want. The clamp around the colour is the framebuffer's own; `gl_Fog` is read from GL's fog. Its lightmaps change a few rows at a time as lights flicker: `RESOURCE_PATCH`. Found on the way: its window read as hidden (it stopped swapping), shaders deleted between attaching and linking (kept while attached, as GL has it). The layer reports one texture unit unless `PSPEXP_GL_UNITS` says more, since fixed-function multitexturing is not drawn. On the PSP-1000 from the Pi 400: 1,500 small immediate draws a frame (17–59 fps, 50–480 KB a frame, the PSP's CPU up to 29 ms) became about 100 (45–60 fps, mostly 50–130 KB; the dips when the demo loads its next level). Measured by message (`PSPEXP_TRACE_BYTES`) over its demos: streamed vertices 104 KB a frame, lightmap patches 13–27 KB, state 6–8 KB, sound 8 KB, matrices 3 KB, mesh draws 2–3 KB. 16-bit vertex parts took the streamed vertices to 74 KB; patches that carry only the changed rectangle's 8 × 8 blocks (Quake uploads whole rows of a lightmap for a few texels: the board compares, and sends nothing when nothing changed) took them to 3–15 KB. Its animated models, two poses blended in the shader (`mix(pose1, pose2, blend)`), are still streamed every frame (about 60 KB): the blend is made per vertex on the board, the matrix left to the GE. Before that was recognised, their vertices were projected and divided by w on the board, and the view model's, some behind the eye, turned into triangles across the whole screen while the shotgun recoiled (seen playing on the PSP, and in PPSSPP too; gone on both since). Its lighting helper returns from both sides of an `if`: both are read, each where its condition holds.

**OpenArena** (2026-10-01; Debian's, the Quake 3 engine, ioquake3). Its default renderer (`opengl2`) is all shaders and did not link; the classic one (`cl_renderer opengl1`) is OpenGL 1.x and draws right, once `glCompressedTexSubImage2D` exists (a stub). It draws everything from vertex arrays refilled each frame, as indexed triangle lists (`glDrawElements`), which the board used to expand into one vertex per index. With `PSPEXP_FEATURE_3D_INDEXED` a list goes as its vertices, each once, and 16-bit indices, the triangles that need cutting (near the camera: most frames have some) cut and sent apart. Its own settings for weak hardware (vertex lighting, one pass; coarser curves and models; no marks, shadows, flares, dynamic lights; a plain sky) took a frame from 420–660 KB to about 330 KB; indices then took a frame's vertices from 195 to 135 KB (the same view, no bots), the menu's from 147 to 85 KB. Recipe `openarena`: the nub aims (a mouse), cross fires.

On the PSP-1000 (2026-10-01; the PSP's CPU time per frame follows the bytes it takes, about 0.07 ms a KB): indexed and 16-bit vertices 75 KB a frame, 5 ms, 30 fps; floats 123 KB, 9 ms, 30 fps; without indices 230 KB, 17 ms, 16 fps. The Pi alone runs the same scene at 109 fps. The 30 is the PSP's own pipeline: a frame is received and its display list built, then the GE starts on it, then the next vertical blank; the CPU work and the GE's do not overlap, so a frame a little over 16.7 ms waits for the next blank. Tried and left: kicking the GE as the list grows, every 16 3D draws (the data cache written back, then the list's stall address moved on, as `sceGuFinish` does at the end), so it would draw a frame's start while the rest still arrived. It drew right on the PSP-1000, but slower: frames of 127–169 KB at 15–16 fps and 12–18 ms of CPU, where the runtime without it held 123 KB at 30 fps and 9 ms. The CPU's time per byte rose by about 40%: the GE working alongside takes memory bandwidth the CPU needs to take in the frame (and each kick writes the whole data cache back). The frame's CPU and GE work stay one after the other.

Tried and left: **islands**. Quake 3 appends its surfaces one after another into each indexed list; a run of triangles over vertices of its own that came back unchanged was made a mesh (vertices and indices) and drawn by handle. In bytes it paid in PPSSPP (a frame 105 to 74 KB with islands of 4 vertices and more), on the PSP it did not: hundreds of small draws a frame, and two resources an island against the runtime's table of 1,024 (textures share it), which filled (`TABLE_FULL`, then draws of meshes it never got): 20 fps without, 12–14 with, even with 256 resident. What stayed: the triangles an indexed list sends carry only the vertices they use.

**Multitexturing, fixed function** (2026-10-01). Texture unit 1 (its enable, environment, `glMultiTexCoord`, its own coordinate array) is drawn as a second pass with its texture and coordinates, blended as its environment says: modulate (`DST_COLOR`, or `GL_COMBINE` modulate with `GL_RGB_SCALE` 2: `DST_COLOR × SRC_COLOR`, Quake engines' overbright lightmaps), add, decal, replace; where the first pass drew (depth EQUAL). Checked against a real OpenGL: `GLDEMO_MULTI=1`, a checker times a light gradient, immediate mode and client arrays, both alike in PPSSPP. Reported only when asked (`PSPEXP_GL_UNITS`) at first: told of a second unit, Neverball keeps it on for every draw, and draws with unit 1 were streamed, not meshes: 110 to 790 KB a frame. Now (2026-10-01, later) unit 1's pass from buffer objects is a mesh of its own (unit 1's coordinate array in unit 0's place: another mesh by its key), and unit 1 keeps its own coordinate generation and texture matrix (they went to unit 0's, so Neverball's unit-0 coordinates were generated). Neverball with two units puts every surface's texture on unit 1 and its shadow on unit 0, off but where the ball's shadow falls, and draws blended (where a second pass cannot multiply): with unit 0 off, unit 1 is now the draw's only texture, one pass; unit 0 combined as the colour times one minus its alpha (the shadow) is left out, the GE having no such function. Neverball then draws as with one unit, 73 KB a frame, 30 fps on the PSP-1000 either way; OpenArena (its `opengl1` renderer collapses passes when it has two units) 98 to 71 KB, 30 fps, the CPU 7 to 5 ms; Chromium B.S.U., openMrac and the regression pictures unchanged. Two units are now reported by default.

**More games tried** (2026-10-01, from Debian, in PPSSPP from the Pi 400): X-Moto draws right; Blobby Volley right once letterbox bars are painted black; Trigger Rally right once a full-screen window counts as the PSP's (its menu is pointer-driven: the race not yet seen); Teeworlds right at 480x272 (`gfx_screen_width 480; gfx_screen_height 272`), 27–46 fps on the PSP-1000, once 3D textures exist: it keeps its map tiles one a slice (so tiles repeat and filter apart), drawn white before; the layer keeps a 3D texture as a 2D atlas, the slices in a grid, and maps each vertex's (s, t, r) into r's cell; HyperRogue 0.6 fps (285 KB a frame); Trackballs asks for a 3.x core context (refused); AssaultCube draws no frame yet (Debian's arm64 build reports its own protocol incompatible).

**Emilia Pinball** (2026-10-01; Debian's). It makes its window with `SDL_CreateWindowAndRenderer` and `SDL_WINDOW_OPENGL`, then draws with OpenGL: the layer now makes the window, a software renderer and its own context there. It is setgid (a shared high-score file), so it starts through the loader (pspexp-run does that). It looked frozen: it draws its menus only when an event comes and waits in `SDL_WaitEvent` between, and the test script's keys were only pushed at a frame's end; they now come while it waits too (on the PSP the buttons always did). It draws polygon by polygon in immediate mode, 7,800 draws of three vertices a frame, 666 KB. A draw of a triangle list (or lines, points) that follows another with nothing sent between (the same state, matrices and vertex format: the board sends those only when they change) now goes on as the same `3D_DRAW`: the library grows the frame's last message when it is that draw (`pspexp_3d_draw_more`; no protocol change, the PSP sees a longer draw). 465 KB a frame. The rest is the table's vertices every frame: its engine transforms them itself and its camera follows the ball, so they differ a little each frame and cannot be kept. They now go in 16 bits as well (positions and coordinates; unlit draws of fewer than 16 vertices went as floats): a frame's 16-bit draws share one power-of-two scale where their positions fit it losing at most two bits, so the matrices are not sent again and the draws still join; a small draw goes in 16 bits only when small beside where it lies (a floor quad near the horizon moved its edge a pixel), else as floats times the frame's scale. 376 KB. On the PSP-1000 (2026-10-01): 21.5 fps at 465 KB (CPU 25.6 ms), **30 fps** at 369 KB (CPU 19 ms); gl-demo pixel-identical. Recipe `recipes/pinball`, the Tux table to start.

**DreamChess** (2026-10-01; Debian's 0.3.0). It starts only with `GL_ARB_framebuffer_object` reported, which the layer does for a program told so (`PSPEXP_GL_EXTENSIONS`, per game: reported to all, others would take new paths). It draws each frame into a framebuffer with multisampled renderbuffers and copies that onto the window (`glBlitFramebuffer`), and each frame again, pieces in flat colours, into a 1920 × 1080 texture it reads one pixel of under the mouse (`glReadPixels`: colour picking). Renderbuffers were nothing, so the screen stayed black. Now a colour renderbuffer the window's size *is* the screen: nothing can texture from a renderbuffer, only copy it out, so the drawing goes straight to the screen and the copy is nothing (no render target: the PSP's VRAM has room for one 512 × 512 beside its framebuffers and the textures, and DreamChess's picking texture took it). Other renderbuffers are textures of their own and `glBlitFramebuffer` draws a textured quad. The picking target found no room on the PSP; the runtime kept drawing on the screen, the picking pass's clear and flat-coloured pieces over the frame. Now a 3D_TARGET naming a target that is not there drops draws and clears until the next target (runtime), the board drops draws into a target the PSP refused, and a target drawn into for 120 frames and never textured from is let go, its draws dropped (it could only be read back, which gives black). The pieces are display lists, each hundreds of short triangle strips: 9,400 draws and 4.4 MB a frame, 4 fps in PPSSPP. A run of `glBegin`/`glEnd` blocks in a list with nothing but vertices between them is now drawn as one: the strips as one strip, joined by repeating a vertex at each end (triangles of no area; one more where needed so each strip keeps its winding), the other primitives as one list of triangles, each a mesh by content (the same each frame; at any size, coming from a list). On the PSP-1000 (2026-10-01): as one list of triangles, 261 KB a frame and 20 fps, the CPU 30 ms: a large draw went in pieces of 1,500 vertices and the last, under 512, was not a mesh (pieces are now as even as they can be: 47 KB); then the GE took 34 ms, transforming and lighting three vertices a triangle; as strips, 107,000 vertices a frame instead of 239,000, 41 KB, **30 fps**, the CPU 2 ms and the GE 16.6 ms (the pieces drawn twice: their reflection in the board). Played by keys (the selector; mouse picking cannot work without reading back): a recipe, `recipes/dreamchess`, at 480 × 272.

**Regressions** (2026-10-01). `tests/psp/gl-ppsspp.sh` runs `examples/gl-demo` in each of its modes (immediate mode and arrays, display lists, fog and blending at three frames of its animation; the GLSL path; the lighting check; cube-map reflections; sphere maps; multitexturing) under a freshly booted PPSSPP and compares the PSP's frame with a picture in `tests/gl-ref/`: the demo's frames are the same on every run, so the layer's byte savings (16-bit vertices, joined draws, meshes) are checked to change no pixel. Games are not in it: their frames depend on time.

**Quake's models** (2026-10-01). Quakespasm draws each model with one shader: the position the mix of two poses by a uniform `Blend` (each pose its own vertex and normal attributes in one buffer), the colour `LightColor` times a shade of the normal against `ShadeVector` (`1 + d` facing it, `1 + 13/44 d` away). Through the plan every model vertex was worked out on the board and sent each frame. The GE morphs: a vertex can carry up to eight targets mixed by weights set per draw. So now (`PSPEXP_FEATURE_3D_MORPH`, `3D_MORPH`; a runtime update) a pose pair is a mesh on the PSP, each target 16-bit coordinates, an 8-bit normal and a 16-bit position (28 bytes a vertex), made once; a draw is its weights and a light. The shader is recognised by its names (`Pose1Vert`, `Pose2Vert`, `Pose1Normal`, `Pose2Normal`, `ShadeVector`: Quakespasm's and the engines from it), the plan's position checked against them. The light: the plan's colour measured at three normals (along `ShadeVector`, across, against), fitted by least squares to the GE's ambient + diffuse × max(0, n · light), which cannot fall more gently on the far side as Quake's does. `PSPEXP_GL_MORPH_CHECK=1` compares the GE's result with the shader's vertex by vertex: positions exact, colour 0.014–0.025 off on average (4–6 levels of 255), 0.17 at most (the far side). Found on the way: when `Blend` is 0 or 1 the plan sees one pose (most draws: the same mesh, weights 1 and 0); the plan's weights are against the larger one, its matrix carrying the scale (made to sum to 1, the matrix's columns times their sum); the plan's coordinate hash moves with unrelated uniforms (not in the mesh key: 4,859 meshes made over the demo before, 501 after); the weapon in view reaches outside the GE's range (its triangles that need it cut on the board from the poses blended there, the rest from the mesh). Brush entities (doors, buttons: a few faces each from the world's `GL_STATIC_DRAW` buffer) were streamed as small draws; small draws from static buffers are meshes now. Quake's `demo1` in PPSSPP at fixed time steps (`host_framerate 0.05`): 110.6 to 64.0 KB a frame over the whole demo. On the PSP-1000 (`timedemo demo1`, its first 170 s, runtime 010b547 against b86a491): 46.3 to 49.6 fps, 79.3 to 55.1 KB a frame, the PSP's CPU 8.0 to 7.1 ms (median); no errors, the models right. Seen there and in PPSSPP before the change too: speckled coloured strips along some ledges' edges.

**Quake's water** (2026-10-01). Those strips were water and slime, seen low. Quakespasm draws each liquid from a warp image: each frame it draws the liquid's texture warped into a corner of the framebuffer and copies it into that image (`glCopyTexSubImage2D`), which the layer cannot do (it does not read the PSP's framebuffer); the image keeps what it was made from, Quakespasm's memory as it happened to be (`hunk_base`): rainbow noise, black below. Found by bisecting over demo runs: textures off (`notex`), passes left out (`nomod`, `noadd`), lightmap patches off (`nopatch`), all textures in main RAM (`ramtex`), board-side cutting off (`PSPEXP_GL_NOCLIP`): all still striped; each draw a flat colour of its texture (`ids`): no strips; one texture left out (`PSPEXP_GL_HIDE_TEX`): gone; its uploads and binds traced (`PSPEXP_GL_TRACE_TEX`): one 256 × 256 image, bound right after the liquid's own texture each frame. Not large texture coordinates (`GLDEMO_UV`: a long floor with coordinates offset by up to ±2048 is clean). Quake's own `r_oldwater 1` warps the liquid's texture per polygon instead; it held only from a config file (on the command line the warp images were still drawn), so `quake-shareware` writes it into `id1/autoexec.cfg` (package 1.06-pspexp2). Tried on the way and left: mipmaps (levels in the texture upload, the GE choosing them): the strips were not aliasing, and a Neverball text texture drew as a white bar with them; the patch is kept aside. Copying the framebuffer into a texture on the GE (`sceGuCopyImage` into a linear render target) would keep Quakespasm's default look: in Next.

**Where a Quake frame's time went** (2026-10-01). Profiled on the PSP-1000 (`timedemo demo1`): the board's side (Quake and its GL here, `PSPEXP_PROFILE`'s new "program and its GL") took 6–17 ms a frame; the rest was waiting for the PSP. `PRESENT_DONE.execute_us` was measured from `BEGIN_FRAME` to `PRESENT`, the waits for the frame's bytes over USB with it, so the PSP looked idle in slow stretches; it is now what its definition says, the time spent handling the frame's messages. The runtime now times every message kind and writes the totals to `pspexp-rt.log` every 600 frames ("us/bytes a frame"): `3D_DRAW_MESH` took 2–9.5 ms for 4–8 KB, some 40 µs a mesh draw. Each draw looks up its vertex mesh, index mesh and texture, and a lookup scanned the resource table, 1,024 entries of some 60 bytes, much more than the PSP's 16 KB data cache. A lookup now tries the slot the id was last found in first (checked on use: in use, the same id), a 128 KB table beside the runtime. The timedemo: **52.6 to 58.7 fps**, 81 of its 84 two-second stretches at 50–60 fps (many were at 20–40); mesh draws 0.5–3 ms a frame. `PSPEXP_TRACE_BYTES=N` tallies every N frames (to line them up with the timings).

OpenArena on the same runtime, with two texture units reported (the default since the multitexturing work), ran at 15 fps, 160 KB a frame: with its lightmaps on (its saved config can turn them on whatever the command line says) it multitextures every surface, drawn twice here. Its recipe now asks for one unit (`PSPEXP_GL_UNITS=1`): 30 fps, 62–98 KB.

**Copies from the screen** (2026-10-01). `glCopyTexSubImage2D` is now done by the GE: the texture becomes a 16-bit render target (5650, as the screen) and `3D_COPY` (`PSPEXP_FEATURE_3D_COPY`, a runtime update) has the GE copy the rectangle in order with the draws (`sceGuCopyImage`), row by row with the rows turned (the screen's run down, GL's up). Only from the screen at the PSP's own scale (a block transfer does not scale). Quakespasm's default water (`r_oldwater 0`) draws right with it in PPSSPP: the warped liquid each frame; the Quake package keeps `r_oldwater 1` all the same (it needs no runtime update and saves the extra pass and copy).

**Neverball's shadow** (2026-10-01). With two texture units, Neverball's unit 0 multiplies the colour by one minus its shadow texture's alpha. That is now a pass of its own after each surface (the framebuffer times one minus the texture's alpha, depth equal), asked for with `PSPEXP_GL_SHADOW=1`: its coordinates are the vertex positions through a texture matrix that reads their third component, which the layer dropped (a stripe, not a disc): a matrix reading a third coordinate the vertices have is now applied here, and a coordinate array that is the position array becomes the GE's own projection from positions (`tex_source` position), so the pass stays on meshes. Still, every surface is drawn twice and the texture matrix and lights go again around each: on the PSP-1000 30 to 28 fps, 74 to 124 KB a frame, for a shadow mostly under the ball. Off by default.

**Quake's particles** (2026-10-01). In fights the streamed bytes are mostly particles: quads facing the camera, one draw of up to 1,440 (5,760 vertices), each two triangles, six vertices. A quad parallel to the screen and upright on it projects to an upright rectangle, which is what the GE's sprites draw from two vertices, its opposite corners, both transformed. A `GL_QUADS` draw (unlit, eight vertices or more) whose every quad is so, in eye space, with one colour and its texture upright on it, goes as sprites (`PSPEXP_FEATURE_3D_SPRITES`, primitive 6; a runtime update). Quake maps its particle texture turned a quarter, which a sprite cannot: accepted when the texture is the same mirrored across its diagonal (its round dot; checked once per contents). `demo1` in PPSSPP: 65.2 to 60.7 KB a frame over the demo, the heaviest stretch 115 to 97 KB. On the PSP-1000 (runtime c886cab, `timedemo demo1`'s first 170 s, back to back with `PSPEXP_GL_NOSPRITES`): 51.9 to 52.6 fps, 56.4 to 52.9 KB a frame; a small gain: the slowest stretches (15–40 fps) leave the PSP's CPU idle (3–6 ms a frame), held by something other than the bytes (loading, the board), and a run's average moves by a few fps from one run to the next (48.2 to 52.6 for the same build). `GLDEMO_SPRITES` draws the same quads both ways side by side, a regression picture (`tests/gl-ref/sprites.png`, with `uv.png`: large texture coordinates). Not exact pictures: demo frames do not repeat run to run here (the demo starts after a varying number of frames, and `pause` does not stop a playing demo in this Quakespasm), so correctness is the numeric check and the pictures by eye.

**Polygon offset.** The GE has none, and with its 16-bit depth buffer a decal on a surface flickers (openMrac's car shadow on the road). Draws use GL's depth range a little short of the GE's near end, and an offset draw is moved into that headroom by its factor and units.

**Played on the PSP** (PSP-1000 over USB from the Pi 400, Debian's packages unmodified; status on 2026-09-30):

| Game | GL | fps | Notes |
|---|---|---|---|
| gl-demo (ours) | 1.x, 2 | 59.9 | every path: immediate mode, arrays, display lists, fog, shaders, `GLDEMO_CUBE` reflections |
| Chromium B.S.U. | 1.x | 36–43 | `-w -v 0`, `PSPEXP_NUB=mouse`; its controls needed the input fixes (SDL layer README) |
| Neverball | 1.x | 30 | nothing vanishes since triangles are cut on the board; 20 ms of GE time (fill) is the limit |
| SuperTux | 2 | 52–58 | `--window --geometry 480x272` (else its text is tiny), `PSPEXP_JOYSTICK=0`; sound through OpenAL's pipe |
| Numptyphysics | 2 | 59.9 | its scene through a render target; pen strokes as meshes by content |
| Widelands | 2 | 30 | loads (hundreds of images into a render target), its image shader as near as the GE can |
| Quake (shareware) | 2 | 45–60 | Quakespasm's shader path (`PSPEXP_GL_UNITS=3`): the world as meshes, lightmaps in passes and patched; 30 in the biggest fights with its models streamed; morphed on the GE since 2026-10-01: `timedemo demo1` 46.3 to 49.6 fps, 79 to 55 KB a frame (see Quake's models) |
| OpenArena | 1.x | — | not yet on the PSP itself: in PPSSPP 22–23 fps (PPSSPP's own speed), 75–135 KB a frame; the Quake 3 engine's `opengl1` renderer with its low-end settings |
| openMrac | 2 | 59.9 | `--skip-settings`, keys; lighting on the GE, reflections as sphere maps, shadow without flicker; the first seconds of a race slower |
| DreamChess | 1.x | 30 | 2026-10-01, three moves a side played by scripted keys (1.e4 e6 2.d4 d6, 2.Nf3; not yet by hand), started from the launcher too; its colour-picking target kept (`PSPEXP_GL_KEEP_TARGETS`) found no VRAM and the runtime dropped its draws, the screen clean; `-W480 -H272`, `PSPEXP_GL_EXTENSIONS`; its pieces' display lists as stitched strips kept as meshes, GE-bound (16.6 ms) |
| Emilia Pinball | 1.x | 26–30 | 2026-10-01, a minute of flipping by scripted keys (not yet by hand), 30 at rest, 26 with the ball in play (the PSP's CPU 20 ms, the rest USB and the game on the Pi); started from the launcher (setgid, through the loader); the Tux table; 369 KB a frame of its own transformed vertices, joined draws, 16-bit |

The four from Debian are recipes (`recipes/supertux`, `chromium-bsu`, `openmrac`, `neverball`, with these options): installed with `pspexp-pkg install` and started from the launcher on the Pi 400 on 2026-09-30, their menus at the rates above (SuperTux with its sound). The fixed-function sphere maps are checked against a real OpenGL like the reflections: `GLDEMO_SPHERE=1`, the torus through `glTexGen(GL_SPHERE_MAP)` with a texture of four coloured quarters, the same in PPSSPP as on the Mac.

**On the GameShell** (2026-10-01; the PSP-1000 on the GameShell's USB, the console reached over Wi-Fi; pspexp-core 2ab3d0d armhf, Debian 13's packages, the same recipes as on the Pi). Each game run alone for 30–40 s with scripted keys (`PSPEXP_PROFILE=1`), not yet by hand:

| Game | fps | Notes |
|---|---|---|
| DreamChess | 59.4 | |
| Blobby Volley 2 | 56.7 | |
| OpenArena | 55.7 | one texture unit (its recipe) |
| SuperTux | 52.5 | |
| Teeworlds | 50.3 | |
| X-Moto | 50 | removed 2026-10-21: its text too small to read at 480x272 |
| openMrac | 49.5 | |
| Trigger Rally | 47.8 | |
| Quake (shareware) | 40.8 | the board's 22–25 ms a frame is the limit (the Pi's is a quarter of that) |
| Chromium B.S.U. | 22 | 43 ms a frame on the board, 23 ms of it CPU: its own frame pacing, not looked into |
| Neverball | 22 | the board 22 ms, then 15 ms waiting for the PSP; 112 KB a frame (74 KB from the Pi) |
| Emilia Pinball | 7.1 | the board's CPU (below) |

Emilia Pinball's 7,800 draws a frame took the board 226 ms (4.2 fps). Sampled with `perf` (Debian's `linux-perf`): one core busy, half of it the game's own engine (it transforms the table in software, `TransformVisitor`, about 65 ms a frame on the Cortex-A7), half the GL layer's work per draw. Three things in that were needless: the layer's environment settings looked up on every draw (`getenv`, then a list; now a table by the name's address: 165 ms); the toolchain's `memcmp` (zig's compiler_rt), which compares one byte at a time, comparing each draw's state, matrices and lights (some 550 bytes) with what went before (now compared a word at a time, `differs`), the projection times the modelview made for every draw's clipping test (kept while the matrices stay) and two allocations a draw (129 ms); `lroundf` calls into libm for the viewport and scissor (126 ms, 7.1 fps; gl-demo's pictures unchanged). Its own engine leaves it under 15 fps on this board whatever the layer does.

**OpenMW (Morrowind)** (2026-10-02, an experiment). Debian 13 has no OpenMW (Debian 12 and unstable do), so unstable's 0.51 source package was built on the Mac in a native arm64 Debian 13 container (OrbStack), with OpenMW's own pinned MyGUI 3.4.3 and Recast linked in statically (Debian 13's are too old), as `0.51.0-1~pspexp1`; the game data is the user's own (Steam's Game of the Year edition), in the Pi user's home, never in a package. Its settings for the PSP: 480x272, `lighting method = legacy`, no shaders forced, no shadows, no water shader, a 2048 view distance. Against PPSSPP from the Pi it reaches its main menu, drawn right, after four changes in the layer: S3TC (DXT1/3/5, `GL_EXT_texture_compression_s3tc`, offered only through `PSPEXP_GL_EXTENSIONS`) decoded on the board into ordinary textures (Morrowind's textures are all DXT; the GE reads DXT itself, which would take a quarter to an eighth of the bytes and of the PSP's memory: a later step); a stage given as several shader objects (OpenMW's libraries of functions beside the shader with `main()`) joined at link time, the libraries first; `gl_ClipVertex` taken (and ignored: the GE clips); `gl_TextureMatrix` an array as GLSL has it (a single matrix made `gl_TextureMatrix[0]` its first column, and OpenMW's menu drew in streaks). `PSPEXP_GL_DUMP` now writes each shader's source as the program gave it. On the PSP-1000 from the Pi its menu runs at 55-60 fps (1.7 ms of the PSP's CPU, 2.1 ms of the GE a frame), once two things were found. OpenSceneGraph, left to choose, draws on a thread of its own while the main thread reads SDL's events, and libpspexp is driven from one thread: two threads sent at once and the link went down every run or two (`OSG_THREADING=SingleThreaded` for it: three clean runs of three). And the USB transport's sends took libusb's `LIBUSB_ERROR_BUSY` for a dead link: a slot is marked free inside libusb's completion callback, and libusb counts the transfer in flight until that callback returns, so a frame sent right behind it could find it so; it is now submitted again for up to 100 ms. Every way the link can be given up now says why (`PSPEXP_USB_VERBOSE=1`: the transfer's error; the library: send or receive failed). The world's shaders then planned exactly once the reader took `gl_FrontMaterial`, `gl_BackMaterial` and `gl_LightModel` (asked of the layer, as `gl_Fog` was: the glMaterial and glLightModel state). A new game (`--skip-menu --new-game`, in PPSSPP from the Pi) starts and plays its music, and shows the opening's night sky, but the game scene is where it stops: with 256-pixel textures the PSP's memory ran out (`OUT_OF_MEMORY`; `PSPEXP_GL_TEXMAX=64` keeps it), and a frame of the first scene is 1,300-1,600 draws and 400,000-460,000 vertices (OpenMW's view distance 1024 and small-feature culling at 30 pixels barely change it: the geometry is near), where Quake's frames are a few thousand and the GE draws about a million a second; kept on the PSP as meshes it would be near all of its 12 MB, streamed it is megabytes a frame. Measured on the PSP-1000 from the Pi (`PSPEXP_PROFILE=1`, then `perf`): in the game scene a frame takes the board 18.2 s of CPU (the menu: 7-13 ms), the send 94-103 ms (2.4-2.7 MB), the PSP's CPU 21-29 ms and its GE 0.5 ms: 0.1 fps. The 18 s are the layer's: its GLSL reader (`nev` 38 %, `fold` 7 % of OpenMW's samples), the shaders planned again on nearly every draw because OpenMW changes uniforms per object (each object's lights, material, colour mode), with `store_pixels_changed` 8 % and FFmpeg (the music) about 10 %; OpenMW itself a few per cent. Planning a shader once with its uniforms as parameters, and only the values per draw, would take that away (a large change to the reader); the link and the PSP's CPU would then hold it near 5-8 fps in this scene, with the GE and the PSP's memory next. From the Mac (OpenMW 0.51 built natively with Homebrew's libraries, its own double-precision Bullet, and LuaJIT's headers put first: Homebrew's Lua 5.5 headers took sol's `<lua/lua.hpp>`), the same scene on the PSP-1000 over USB: 0.4 fps, the board 2.24 s of CPU a frame (the Pi's 18 s, eight times faster, still the reader's), the send 74-104 ms, the PSP's CPU 22-24 ms, its GE under 1 ms. Played by hand at 0.3 fps: character creation's windows draw (its name input reached), the ship's interior behind them black (its lights not yet looked at); left there ("uncontrollable"). So Morrowind's world is far more than the PSP's GE can be given this way: its menu is a demonstration, the game drawn by the GE is not.

**Host-rendered OpenGL** (2026-10-02). The other way round for programs too heavy for the GE: `libsdl-pspexp-hostgl` is the SDL layer built without its OpenGL (`PSPEXP_HOST_GL`), so the program's GL is the host's own GPU; at each `SDL_GL_SwapWindow` the finished frame is read back (`glReadPixels` of the window's back buffer, its rows turned) and sent as a window surface's is (`stream_frame`: shrunk to the PSP's screen when the drawable is larger, a Retina window's 960x544; only the pages that changed; CLUT8 where a page has 256 colours or fewer). The PSP shows it, plays the sound and gives the input as for any SDL program. OpenMW from the Mac (M5) on the PSP-1000 over USB, at its normal settings (shader lighting, view distance 7168) in a 480x272 window: **59.9 fps in play**, 14-119 KB a frame, the board 2 ms of CPU a frame, the PSP 0.08 ms of CPU and 0.7 ms of GE; played by hand ("playable!"). The host window must stay visible (macOS may not draw a hidden one), OpenSceneGraph single-threaded (`OSG_THREADING=SingleThreaded`: its draw thread would send while the main thread reads input), and on macOS `DYLD_INSERT_LIBRARIES` set by the last process before the program (a system binary between them, `perl` or `sh`, has it removed). **On the Pi 400 (2026-10-03):** its V3D gives OpenGL 3.1 core and OpenGL ES 3.1 with no display (Mesa through EGL; SDL's `offscreen` video driver makes the context), enough for the games the PSP's layer refused for asking OpenGL 3 (SuperTuxKart 3.1; OpenClonk asks 3.2 then 3.1; Trackballs 3.3, beyond V3D). `pspexp-run` takes `PSPEXP_HOSTGL=1` (the hostgl library, the offscreen driver); the library answers `SDL_GetWindowWMInfo` (offscreen has no window manager; Irrlicht stopped without it) and reads frames back through two pixel buffer objects. SuperTuxKart in a race (lighthouse, its default settings, ES 3.1 on V3D), into PPSSPP: at 854x480 the Pi's drawing 72 ms a frame (CPU 23 ms), at 640x360 47 ms, at **480x272 34 ms** (about 29 fps at most); 215 KB a frame (a moving 3D scene changes every page), read-back and conversion 1.3 ms; PPSSPP took 11-27 ms a frame to take the pictures, so 11-16 fps there (USB carries 215 KB in about 9 ms: on the PSP itself an estimated 22-25, not yet measured). The asynchronous read-back changed nothing measurable (the drawing itself is the cost). Recipe `supertuxkart` (Pi only): `graphics=board-gpu`. The other games refused for OpenGL 3, the same way: **Trackballs** draws with V3D when told it has 3.3 (`MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330`; what it uses runs), menu 26-31 fps in PPSSPP and readable. Both packaged for the Pi (`trackballs`, `opensurge` with `pspexp-opensurge`), `graphics=board-gpu`. **Open Surge** (Allegro 5 through SDL): Allegro had chosen OpenGL ES (GLU missing at its build) and Open Surge's GLSL 1.20 shaders failed; built with GLU and `GL_BUILD_TYPE=gl` it runs at about 30 fps at its own 426x240 (Allegro's SDL backend also patched to answer `$HOME` for the user's home folder: it gave the program's folder, and Open Surge's saves were refused under /usr/lib) (`--resolution 2` draws a quarter of the frame). Its frame enlarged to 480 showed a white column: each page's width was rounded on its own, leaving a pixel between two pages; page edges are now taken from the rounded positions of both sides. **OpenClonk**: GLEW built for GLX reported "no GLX display" (error 4) under EGL, now treated as success with `glXGetProcAddress` sent to SDL's; it then needs `MESA_GL_VERSION_OVERRIDE=3.2 MESA_GLSL_VERSION_OVERRIDE=150` and crashes in its start menu (`C4StartupMainDlg::UpdateParticipants`, a label's font null): parked. **The Nintendo 64** (2026-10-03), the clearest case: the PSP's own N64 emulator (DaedalusX64) runs GoldenEye at under 10 fps and Ocarina of Time at 10-20 with glitches. Mupen64Plus (its main branch: 2.6.0 refuses ROMs with the newer open-source boot code; Debian 13 builds none of it for arm64) with GLideN64, built in a container (`build-mupen64plus.sh`, package `pspexp-mupen64plus`, launch script `pspexp-n64`), its core's window and context SDL's, so the host-rendered layer takes them. GLideN64 asks for OpenGL 3.3 core; on V3D with Mesa told 3.3 it drew no 3D and its frame never reached the window (only fills and 2D text showed in its own screenshots): patched to ask for OpenGL ES 3.0, which V3D has, and everything draws. Super Mario 64's logo and intro right; GoldenEye 007 through its menus into the Dam mission (scripted Start presses), **about 20 fps reaching PPSSPP with the sound never short (the emulation at full speed)**, the dynamic recompiler for 64-bit ARM. Homebrew made with libdragon runs its own RSP microcode, which the high-level RSP plugin does not know: cxd4's low-level RSP takes it (`RspFallback`), but current libdragon games stop on their own RSP timeout (only accurate emulators run them). **Celeste 64, the showcase** (2026-10-04): the free 3D Celeste (Maddy Makes Games' own Linux arm64 build, .NET 8 and Foster) on the Pi's GPU. Its SDL2 is linked into `libFosterPlatform.so`, yet the layer takes it: the library exports its SDL functions and calls them through its PLT, so the preloaded ones win (worth trying before rebuilding anything that bundles SDL). The Pi draws it at its 60 fps cap with no PSP attached; into PPSSPP 19-24 fps at 133 KB a frame. `PSPEXP_WINDOW=WxH` (new) forces a window size and refuses full screen, which on a headless board meant the offscreen driver's 1024x768. **Fewer bytes for a moving 3D frame** (2026-10-03): from the board's GPU every page of a moving scene changes and has thousands of colours, so all went at 16 bits (SuperTuxKart's race: 230 KB a frame). Now such a page is reduced to its 256 best colours by median cut (counts of its 5650 colours; the box with the largest longest-side times pixels cut at its weighted median, by a counting sort since a channel has 32 or 64 values; each box its weighted mean; no dithering) and goes as CLUT8 with its palette, the path 256-colour games already take: **137 KB a frame (-41%), 2.3 ms of the board's CPU a frame against 1.9**, the picture alike even enlarged twice (a faint step in the road's gradient at most). The PSP needs nothing new. Default in the host-rendered layer, `PSPEXP_QUANT=0` off; the N64 gains little (its 16-bit output is mostly 256 colours a page already: GoldenEye 27 to 25 KB). What it does to the frame rate on the PSP itself is still to be measured. Five games compared, all into PPSSPP (the user's own ROMs, kept on the board; DaedalusX64 figures from its compatibility lists, not measured here): Super Mario 64 20-24 fps (DaedalusX64: near full speed), GoldenEye 007 about 20 (under 10), Ocarina of Time 16-23 (10-20 with text glitches), Banjo-Tooie 19-23 (1-5), Perfect Dark 17-19 (7-10, crashes with sound); every picture right, the sound at most a few gaps at loading moments (the emulation at full speed). Reached by scripted keys (`PSPEXP_SCRIPT`, Start and A given keyboard keys for the test): GoldenEye's Dam mission, Mario's castle grounds, Ocarina's opening, Banjo-Tooie's opening story, Perfect Dark's attract mode. The emulator tile is a system recipe with a program of its own (`exec=` in `[system]`, `%ROM%`), Pi only, the PSP's controller as the N64's. The GameShell's Mali-400 has OpenGL ES 2 only. Two of its shaders are also drawn as near as the GE can (a texture used other than as a factor; a `continue` under a condition).

**Indexed draws kept by content** (2026-10-04). Quake 3's engines rebuild each surface batch in client memory every frame and draw it with `glDrawElements`; the layer sent the vertices those triangles use and the indices every frame (only plain draws of 512 vertices or more were kept by content). ET: Legacy's Oasis is large: 195 draws, 130,000 indices, **1.3 MB a frame** with one pass a surface (1.6 MB with its lightmap pass), 4-12 fps into PPSSPP. Now an indexed draw's vertices (64 or more, packed as they would be sent) are hashed; the second time the same bytes come they are uploaded as a vertex mesh, and from then on only the indices of the triangles that need no cutting go (inline, 2 bytes each), the cut ones streamed as before; that index list too becomes an index mesh when it comes again unchanged (a view held still, or moving without a triangle starting or stopping needing a cut). Oasis still 167 KB a frame, in play walking 90-160 KB, about 20 fps into PPSSPP; OpenArena's oa_dm1 300-350 KB to 150 KB. Meshes kept this way filled the PSP's table of 1024 resources (shared with textures: `TABLE_FULL`, then every later upload refused and draws missing), so the layer now keeps at most 448 meshes, the least recently drawn going first. `PSPEXP_GL_NOMESH_IX=1` streams as before.

**OpenRA and threaded programs** (2026-10-04). OpenRA (release-20250330, Red Alert and Tiberian Dawn remade, online play) publishes no arm64 build: `build-openra.sh` runs its own packaging (`dotnet publish`, self-contained .NET 6, built with the .NET 8 SDK) in a container. Two things kept the layer out. .NET loads OpenRA's own `SDL2.so` and looks each function up in that file and what it needs, never in a preloaded library: the package's `SDL2.so` is an empty library needing the board-GPU layer and then Debian's SDL2, so the lookup finds the layer's functions first and SDL's for the rest (Celeste 64 differed: its native library calls SDL through its PLT, which the preload wins). And OpenRA draws on a render thread: its swap waited for the PSP's reply while the main thread's event poll read the link and took it, and both stopped; the layer now holds one recursive lock around the link's event side and frame side. Frames larger than the PSP's screen, shrunk before sending, were all sent every frame; now only the pages that changed, as at full size: OpenRA's battlefield 140 KB to 4-13 KB a frame. At 1024x576 (its lobby needs about 1024 wide; at 854x480 the Start button is cut off), OpenGL ES 3.1 on V3D (`Graphics.GLProfile=Embedded`), a skirmish about 10 fps into PPSSPP and menus 15-17, the Pi's GPU the limit (render thread 18% CPU). Content through OpenRA's own installer (Quick Install: its mirrors of the freeware content, music and videos aside). The launch scripts skip its first-run cards, the introduction (name Commander) and the request to send system information, which they leave unsent.

**The new games on the PSP-1000** (2026-10-04, over USB from the Pi 400; the layer's own figures, scripted input, the PSP's own screenshots): Celeste 64 (the Pi's GPU) **54-60 fps** in the level at 126 KB a frame; Sonic Robo Blast 2 **59.9 fps** running through Greenflower Zone (it interpolates its 35 tics to the display), GE 0.6-0.8 ms; SRB2Kart 56-57 at the start grid; Diablo 59.9 in Tristram; Unreal Tournament (the Pi's GPU) 30-40 in a Deck16 deathmatch, 60 on its scoreboard; Wolfenstein: ET up to 60 in play facing a wall, about 20 over Oasis's open ground (the GE the limit: 23 ms a frame), its team menu 15 (the PSP's CPU 27 ms for 280 KB a frame of 2D draws); OpenRA's skirmish 16-17 (10 in PPSSPP); GoldenEye's menus 17 (the game's own rate; the Dam not reached: its cursor wants the stick, which scripts cannot move). PPSSPP on the Mac had understated most of them (Celeste 64 and SRB2 about 20 there). Still unmeasured on the PSP in play: Lugaru, AssaultCube, Quake II, Luanti, the N64 (started once each, below).

**The N64's crackle** (2026-10-04, heard on the PSP-1000 in Super Mario 64). The PSP's sound buffer sat at its cap (32,768 frames, where it drops what comes) with no underruns: more sound arrived than it played. Mupen64Plus's audio plugin closes its SDL device and opens it again when a game starts; the layer had no close hook, SDL gave the new device the same number, and the old slot stayed paced, so the plugin's callback was called twice as often: twice the sound to the PSP and the plugin's own buffer run dry. Closing now lets the slot go (`SDL_CloseAudioDevice`, `SDL_CloseAudio`), under a lock the pacing thread shares. Then a second fault showed: the plugin pauses its device whenever its emulation is behind, and the pacer sent nothing while paused, so the PSP ran dry (an underrun about every two seconds in GoldenEye) and waited 100 ms to refill; a device that has played now sends silence while paused, as a sound card plays. On the PSP over a minute of each intro: Super Mario 64, Ocarina of Time and Banjo-Tooie no underruns and under 0.6 s of such silence; GoldenEye 10.8 s and Perfect Dark 3.5 s of it, the same with no PSP attached: the emulation itself falls behind there (the main thread in GLideN64's frame-buffer copies to and from RDRAM and waiting on the GPU; turning the colour copy off did not help, larger plugin buffers neither). Not sending a frame while the PSP still has the last (`PSPEXP_FRAME_DROP`) changed nothing there and is off. Also new for tests: `nub:XxY` in `PSPEXP_SCRIPT`.

**What those first seconds missed** (2026-10-14, the user filming): the N64 figures above are from intros and menus; in play Banjo-Tooie is far slower on the Pi 400 (too slow to show), Super Mario 64 stays smooth. And Unreal Tournament's pointer: in its menus it draws its own cursor without hiding SDL's and moves it by the motion, so the layer's arrow showed beside it and the layer's (invisible, clamped) pointer stopped the motion at the window's edge before the game's got there; `PSPEXP_POINTER=0` (the ut99 recipe) draws none and passes the whole motion, and the menus and Practice Session are right. Its Tournament's character creation stopped the cursor at 60% across and down: that screen is a game of its own (`UT-Logo-Map`, `LadderNewGame`) whose menus move their cursor by the motion times `MouseScale` (UWindow's `WindowConsole`, 0.6 by default), from the top-left corner, while the layer's pointer, which stops at the screen's edges, was wherever it was; going to an edge and back gained a little each time (the user found it, measured on the PSP with `PSPEXP_SHOT_TRIGGER` screenshots: the pointer at 479,271, the cursor at 287,163). Then `MouseScale=1.0` (the ut99 package's launch script sets it at every start, the game writing its settings back on quitting) and the layer's pointer put at the top-left on leaving relative mode: right on entering, wrong again after a match (the user). What the frames, timed (`PSPEXP_FRAME_DUMP` names carry the ms), then showed: the game takes the motion from the positions' differences, puts its cursor in the middle when its menus come (`Root.SetMousePos`, the system's pointer not moved) and stops it at its edges once a frame (`RenderUWindow`); a jump of the layer's pointer is a motion to it too (a reset to the middle moved its cursor away), and a pointer left unbounded let its cursor run off. So at the nub's first move after the game leaves relative mode the layer sends a jump far to the bottom-right and, a frame later, to the top-left corner: the game's cursor in its corner whatever its start, the layer's there too, then in step (the same rate, the same edges). Checked on the Pi without the PSP (scripted nub, timed frames): the first push reaches the bottom-right edge (a nudge back shows it just inside the corner), the top-left reached, the cursor one to one in between. Found with `PSPEXP_LOG_EVENTS` and `PSPEXP_FRAME_DUMP` on the Pi (the Mac's stand-in as the link), and the PSP's screenshots. Then (the same evening) the N64 stuttered and its sound broke up even in Super Mario 64, and OpenArena seemed to hang. Neither was the layer: Mario 64 into the stand-in gave the same frame rates second by second with the layer built that day and the one from before it (33-58 fps); the Pi had reported undervoltage 39 times since boot (`vcgencmd get_throttled` 0x50005 during the run: short of power and throttled at that moment), the PSP charging from its USB port. And OpenArena was waiting on ioquake3's "didn't exit properly, safe video settings?" question, put up with zenity on the Pi's own desktop because an earlier run had been killed and left its `ioq3.pid`: the recipe (version 5) now removes a pid file whose process is gone before starting the game. **The shooters' nub** (2026-10-16, the user on the PSP: AssaultCube's nub did not aim at all, Quake's and Quake II's far too slowly). AssaultCube, in a window, drops a mouse motion whose position is the window's centre, taking it for the echo of its own warp there; the layer kept its hidden pointer still in relative mode, at the centre where it starts (240,136 at the PSP's 480x272), so every motion was dropped. It now moves it with the motion, stopped at the edges, as SDL's does. A first test missed it: the test window was larger, its centre elsewhere; at 480x272 (`PSPEXP_WINDOW`) the same nub push turned the view 0 pixels before and 473 after. The slowness: the three ran at their desktop sensitivity (3); now OpenArena's 0.55 degrees a pixel (Quake and Quake II 25, AssaultCube 18, whose turn is sensitivity/33). Quake ignores every `+` command on its command line (`+set`, `+map`), so its recipe writes `sensitivity 25` into `autoexec.cfg` once (51 pixels to 406 for the same push); Quake II's demo binds an old layout (E/D/S/F, no arrows, Ctrl unbound), so its recipe writes the bindings and sensitivity into `pspexp.cfg` at each start. Measured on the Pi without the PSP: a scripted nub push (`PSPEXP_SCRIPT` `nub:`), the board's GPU drawing at 480x272, timed frames, the view's shift by phase correlation. AssaultCube was then removed from the catalogue (the user, 2026-10-16: still not playable on the PSP, and its text, drawn in a 1800-unit-high virtual screen and much of it at half that size, about 5 pixels high at 272 lines, unreadable). **Heroes of Might and Magic II and the magnifier** (2026-10-17): fheroes2 (Debian's, the demo from archive.org) draws at 640x480 at the least; shrunk to 0.567 its maps, pictures and buttons read well on the PSP, its small font does not (skill labels, the status line, dialogs: 4-5 pixels). The layer's `PSPEXP_MAGNIFY=BUTTON` switches such a game to its own pixels round the pointer (scale 1, the offset following the pointer each frame, stopped at the frame's edges; the textures sent again at the new size; the button the layer's alone), and back. Checked on the Pi through the Mac's stand-in, launched as the recipe does (first-run settings skip its language and resolution questions): into the Broken Alliance map with the PSP's buttons, R magnifying the hero screen (its labels sharp) and the map, the nub panning, each button's key as the recipe says (logged), circle held showing a creature's details, triangle's end of turn asking first; its frame rate unchanged (its 4.5 a second in menus, as it redraws only on a change). The installer package downloaded and checked the demo (20.8 MB) on the test card. Not yet on the PSP itself; on a Pi only until fheroes2 (Debian's contrib) is tried on the GameShell. **A sweep of Debian's games** (2026-10-17): each run 30 s through the layer at 480x272 (the Mac's stand-in for what the PSP gets, the Pi's GPU for OpenGL ones), then the promising ones played on the PSP-1000 itself from the launcher (`psp-try`: scripted PSP buttons, the PSP's own screenshots, the frame rate over USB). Shipped after that: Numpty Physics (OpenGL, the game filling a 480x272 window), LBreakout HD, Mirror Magic, Pushover. Two of them drew hundreds of small pictures a frame, each mirrored to the PSP as a draw: Ri-li 778 (6 fps), LBreakout HD 1500 (33 fps, the PSP's CPU 9 ms a frame on them); `PSPEXP_COMPOSE` lets SDL's software renderer compose the frame on the board and sends it as one picture, changed pages only: 58 and 55 fps. Ri-li is parked: composed it runs at 58 fps, but its levels stay on "press any key" whatever key the PSP sends (the events reach it; not yet understood; its settings file wants every key, a partial one aborted it). Failed the PSP's size: Project: Starfighter (800x600: its text about 4 pixels), Widelands (will not go below 800x600), LTris 2 (its interface scaled to 5-pixel text), Freeciv's SDL client (640x480, its interface text). The SDL 1.2 games (Pingus, Kobo Deluxe, TecnoballZ, Powermanga, LGeneral, Frozen Bubble, Kenta Cho's shooters): Debian runs them through sdl12-compat, which calls SDL2 through pointers it looks up itself, past the layer's hooks. Catching the lookups (overriding `dlsym`) was not enough: the layer preloaded into an SDL 1.2 program also took the program's own SDL 1.2 calls of the same names (`SDL_PollEvent` wrote SDL2's event into SDL 1.2's: a smashed stack), its real `SDL_Init` was SDL 1.2's (a loop), and its own calls to SDL went to SDL 1.2 (a worker thread handed its name for its data). For them `pspexp-run` now preloads the layer's build for SDL 1.2 programs, its hooks renamed `pspexp_SDL_*` and reached through its own `dlsym` (sdl12-compat's lookups), its OpenGL under its own names; the layer calls SDL2 by name and is linked `-Bsymbolic-functions` (the layer's README, "SDL 1.2 programs"). A first version preloaded only `dlsym` and loaded the layer privately: the 2D games ran, but rRootage's OpenGL went to the board's Mesa, no context, a crash. The build is not linked against SDL2: linked, SDL2 came before the SDL 1.2 libraries Frozen Bubble's Perl loads later, and SDL_image 1.2 called SDL2's functions of the same names ("Unsupported image format"). OpenGL SDL 1.2 games on the PSP-1000 (one screenshot each, scripted keys): rRootage, Parsec47, Tumiki Fighters (offered: `rrootage`, `parsec47`, `tumiki-fighters`, each played from the launcher, moving and firing) and GLtron 60 fps (offered as `gltron`: 4:3, its own sizes; its keys A/S to steer, mapped with the arrows), Neverputt about 58 (offered as `neverputt`, 480x272 by its settings, which it shares with Neverball, whose recipe now writes them too: both were 800x600, 4:3), BlockOut II about 60 with dips, Mu-cade 40-55, Titanion 25-38 and Torus Trooper 18-20 (too slow); BlockOut II plays (pieces fall, turn, move, about 60 fps) but its side panel (score, level, the next piece) is not drawn: not offered yet. 2D SDL 1.2 games on the PSP-1000: Noiz2sa (`noiz2sa`, 60 fps) and GNU Robbo (`gnurobbo`, 50 fps) offered; Abe's Amazing Adventure walks (30 fps) but its jump not yet confirmed; Toppler plays at 18 fps (to be checked against its own pace); Hex-a-hop stayed on its menu, Barrage showed a bare field, TuxPuck took no screenshots: not yet looked at; Gunroar, Briquolo, FooBillard++, Armagetron and Battle Tanks no picture yet. **The PSP's USB stuck** (the same day, after two console restarts 3 s apart in a test run): the PSP answered each of the board's syncs, the board never heard an answer, a thousand failed writes until HOME; the stuck state not reproduced in five double restarts since. The runtime now restarts its USB (stop, start: the board sees it leave and come back) after six syncs with no session; tried by syncing six times without a session (the console stopped, a test program): the PSP restarted its USB and the console, started after, connected in 1 s. Kobo Deluxe runs through it on the PSP (2026-10-17), offered as `kobodeluxe`: at 480x272 by its -width/-height (its playfield widened), 30 fps, the PSP's pace for its whole-screen frame (the board alone 95); Powermanga as `powermanga`, its 640x400 window at 68%, 60 fps in play, once SDL 1.2 programs were told the display's real size (at the PSP's, sdl12-compat refused its mode and it quit); TecnoballZ as `tecnoballz`, in its 320x200 mode (--320, shown at 1.13x: its 640x480 is the same pictures doubled, at 57% its menus' text about 4 pixels), the nub its mouse, about 51 fps; Frozen Bubble as `frozen-bubble` (a Perl program, so the bridge is asked for: PSPEXP_SDL12=1), its 640x480 at 57%, about 49 fps; Pingus parked: at 640x362 (-g, its menu whole; at 480x272 its buttons ran off the screen) it ran on the PSP, but quit (exit 0) at the click into Levelsets, which it did not without the PSP: not yet understood; LGeneral starts the same way (800x600), not yet tried on the PSP; SDL2 games unchanged (Quake 48-60 fps in play, C-Dogs' menus 30). Taisei (the same day): desktop OpenGL 3.3, which the Pi 4's V3D does not report (3.1, its context refused) and Debian's build has no ES renderer; with Mesa told 3.3 (`MESA_GL_VERSION_OVERRIDE=3.3`, `MESA_GLSL_VERSION_OVERRIDE=330`) it draws correctly on the board's GPU. Its picture first came in the bottom-left corner of the frame: it makes its window at the display's 480x272 and then sets a minimum of 640x480, which SDL's offscreen driver applies by growing the window, while Taisei draws at the size it made; on a headless board (the dummy or the offscreen driver) the layer now leaves a window's minimum size unapplied. With its defaults (FXAA, post-processing, full-resolution backgrounds, full screen at the driver's 1024x768) its stage ran at 17-19 fps on the PSP-1000; windowed at 480x272 with those off and the backgrounds and sprites at half resolution, 60. Offered as `taisei` (Pi 4, 400, 5). **On the GameShell** (the same day, the PSP-1000 on its USB, the current pspexp-core): Numpty Physics 57-60 fps (10-37 while many shapes fall), LBreakout HD 45-50 (dips near 30), Mirror Magic 46-49 with its magnifier, Pushover at its own 18; C-Dogs composed only 16-18 at a mission's start (its CPU composing) and drawn by the PSP 30 there, but in a fight, 1600-2000 sprites a frame (the PSP's CPU 10-15 ms on them, 150 KB a frame), 8-20: the user found it unplayable, which the first seconds measured had not shown; Pi only. A board's own section can now replace a repeated key such as `env=` as a whole (pspexp-pkg's rall). The other four are offered on it. Heroes II drew 4 frames a second and LinCity-NG 2 (0.7 uncomposed): Pi only. **Heroes III** (the same day) on Debian's VCMI 1.6.5, with the player's own copy: the package pspexp-heroes3 is only a start script that copies GOG's installer (innoextract) or an installed game's folder from ~/Heroes3 into VCMI's data folder at the first start (under 3 minutes from GOG's installer on a Pi 400, 75 s from a folder), and says what goes there when nothing does. For the PSP's 480x272 VCMI lays itself out at 1066x604 (800x600 at least, widened to the screen's shape), shown at 45%. That frame is a streaming texture over 512 a side, which the layer had kept at half size (533x302) for the PSP to shrink again: a dark line across where its texture's tiles met; with `PSPEXP_TEX_FIT` asked for, such textures are now fitted (averaged to 480x272, drawn 1:1) instead. Played on the PSP-1000 from the launcher with GOG's Complete edition: the menu, a scenario begun, the hero moved into a battle, 55-60 fps, R's magnifier reading its text. The original CDs are not read: a Restoration of Erathia CD image (the user's, a .7z of .bin/.cue) unpacked to its data (bchunk, bsdtar, then unshield on its InstallShield cabinets, which needed a fix: H3BITMAP.LOD goes on from data7.cab into data8.cab exactly at the cabinet's end, and unshield 1.5.1 and 1.6.2 read zero bytes there instead of opening the next; opened first, all 199 MB came out), but VCMI refuses Restoration of Erathia ("data files are not supported"): it needs Shadow of Death or Complete. So the start script takes only a copy whose Data has H3ab_bmp.lod (the expansion's) and otherwise says so (pspexp-heroes3 1.0-pspexp2); CD owners install from the CD on a computer and copy the game's folder (the manual). Today's other new games on the GameShell (the PSP-1000 on its USB, 60 s each from the launcher, a screenshot late in the run): Powermanga 56-59 fps, Neverputt about 57, GLtron 53-59, TecnoballZ about 50, GNU Robbo about 47, Frozen Bubble 37-43, offered there; Noiz2sa 27 (60 on a Pi), rRootage 35-47, Parsec47 29-39, Tumiki Fighters down to 15, which the user then played and found very fluid (rRootage slower, kept): offered there too, their START and SELECT now P (pause): Escape, on START before, quit them; Kobo Deluxe's menus about 23, a game not reached: Pi only for now; there it could not save a player ("Cannot save player profile"): Debian's games keep shared scores in /var/games, group games, and pspexp-run starts a setgid game as the user (so the layer loads), who on the GameShell was not in games (Raspberry Pi OS's is); pspexp-core's postinst now adds the board's user (uid 1000) to games, and the GameShell image makes its user with it. Tried there: Kobo Deluxe saved a new player (its file in /var/games, written by the user) and played, 22-31 fps; offered on the GameShell. On the GameShell the first start imported GOG's installer in about 4 minutes; the game about 14 fps there, left for the user to judge. **Freeciv** (the same day, a subagent in the stand-in, then the user on the GameShell with the PSP-1000): Debian's SDL2 client (freeciv-client-sdl) starts its own server for one player; its window stays 640x480 (it offers only the display's own modes), at 57%, its font raised to 14 (-F 14) so menus and the top bar read, R magnifying; the nub its mouse, the d-pad a unit, triangle a city (B), START the end of the turn (Shift+Return). It draws only when something changes: after loading (about 35 s on a Pi 400, longer on the GameShell) its menu shows once the pointer moves over it, and the magnifier switches at its next redraw; its frame counter near 0 between changes (which misled a test run here into "nothing on screen": the PSP showed it). In the stand-in 17-19 fps while units blink; the user played it on the GameShell, then a scripted game there on the PSP (New Game, Start, a city founded, a turn ended; about 9 fps on the map) and on the Pi 400 with the PSP (9-12 fps on the map: the stand-in's 17-19 was not the PSP's). Found on the way: on its six-sided tiles (the SDL client's default tileset sets the topology to WRAPX|ISO|HEX) the arrow key Right does nothing, so the d-pad's right is Keypad 9 (`kp9`, new key names `kp1`-`kp9`); its first unit is often an explorer, so square is W (wait: the next unit). And a pointer trail: while the game drew nothing, each pointer frame (`pspexp_replay_frame`) was kept whole, its pointer too, so the next one drew the old pointers again (a smear along the pointer's path, as the user had seen in Heroes II); a replay now marks the end of the replayed draws (`pspexp_frame_mark`'s place), the pointer after it. The PSP's screenshots on a trigger (`PSPEXP_SHOT_TRIGGER`) are taken between frames too, for games that draw only on change. Offered as `freeciv` on both boards. **Commander Keen** (2026-10-18, the first game chosen by the user's four rules, Package_Manager §4): Commander Genius 3.6.4 (GPL-2.0+, not in Debian) built for both boards (`build-commandergenius.sh`, package `pspexp-commandergenius`), the free episodes 1 and 4 fetched at install from where its own store gets them (installers `commander-keen1`, `commander-keen4`, into /usr/share/games/commandergenius/games, which it searches; `_installer.sh` now makes a DIR's parent folder). `pspexp-keen EPISODE` starts one directly (`dir=games/keenN`, no game picker) with its `[Launcher] gameWidth/gameHeight` set to 480x272 on every start (the game writes its settings back): the game then draws at the PSP's own pixels, its text crisp. Its keyboard defaults: Left Ctrl jumps, Left Alt pogo, Space fire, Enter status, F6/F9 quick save and load; the jump's height is how long it is held (the originals'), which the scripted taps never showed and the user confirmed on the PSP. Found on the way: a screenshot on a trigger stalls the PSP, and button presses made during it are lost (2 of 6 reached the game), so controls are checked with few shots; `level=N` starts a level directly (a test aid). On the PSP-1000 from the Pi 400: both intros, menus, Keen 1's map and first level, Keen 4's Shadowlands, 60 fps. On the GameShell with the PSP-1000: both episodes into play, 55-60 fps; offered on both boards. **The Dreamcast** (2026-10-17, a subagent, then the PSP-1000 from the Pi 400): Flycast (master `6bb0fc3e`, its own SDL2 front end, OpenGL ES 3, not the libretro core, which asks for hardware rendering pspexp-retro does not give) built in a Debian 13 container (`build-flycast.sh`, package `pspexp-flycast`, recipe `dreamcast`, a system like the N64's, Pi only); drawn by the Pi's GPU through the host-rendered layer (`PSPEXP_HOSTGL`), its internal resolution 480, the PSP the layer's virtual controller, which Flycast maps as a Dreamcast pad by itself; its own HLE BIOS (a player's dc_boot.bin and dc_flash.bin used when in `roms/dreamcast/bios`). Redream was tried first; Flycast was taken. On the PSP with the user's own discs (.chd, copied off a Steam Deck card read-only with e2fsprogs' debugfs on the Mac, the Pi's USB reader dropping out under steady reads): Power Stone 53-60 fps in menus and the start of a fight, 36-45 later in it; Rayman 2's cutscenes 48-57, its menus at its own 30; sound steady. A drop to 5-20 fps each few seconds with an underrun turned out to be the test's own PSP screenshots (each followed one); started from the launcher's Dreamcast tile without them, Power Stone 50-60 fps and no underrun. Flycast's output, the layer's lines included, goes to `~/.cache/pspexp-dreamcast.log`, not the launcher's log. **Its sound** (the user, the same evening: "sound is not good in any game so far", while the layer counted almost no underruns): each frame waited for the PSP, which takes about 45 a second, so Flycast ran below its 50 or 60 and made too little sound, its own buffer filling the gaps (the layer's buffer stayed full). `PSPEXP_FRAME_DROP=1` freed it (Hydro Thunder "much cleaner") but showed 22 a second: a game a little faster than the PSP found it busy every other frame. `PSPEXP_FRAME_DROP=2` lets two frames be on their way (as Media does for films): Power Stone 40-50 shown, 7 a second left out, the emulation at its full 49-50; Flycast's own frame skip (`pvr.AutoSkipFrame=2`) added nothing. The recipe sets it. Discs tried on the PSP (the user's, Europe): Power Stone 1 and 2, Rayman 2, Dynamite Cop, Hydro Thunder, Virtua Tennis; then the user played them with `FRAME_DROP=2`: Metropolis Street Racer smooth with clean sound (Flycast's busiest thread 60-70%); Sega Rally 2 "awful" (a Windows CE disc, Flycast's full MMU: its CPU thread at 100%, too heavy for a Pi 400); Sonic Adventure 2's menus clean but its play breaking up: two threads at 72-80%, 30 fps on the PSP, the emulation at 47 of its 50 frames a second with only 133 frames left out, so Flycast itself was behind, its rendering and the read-back of a 640x480 window at 480 lines. Version 2 of pspexp-dreamcast: a 363x272 window (the size the PSP shows, `window:fullscreen=no`), 240 lines, Flycast's frame skip when behind (`pvr.AutoSkipFrame=2`), a 128 ms sound buffer (`aica.BufferSize=5644`, from 2822): the busiest thread about 70%, 42-43 fps on the PSP, and the user: "plays and sounds much much better, music and sfx are clear". **The SDL 1.2 games screened again** (2026-10-17, the PSP-1000 from the Pi 400; before the SDL 1.2 build they showed nothing): Hex-a-hop (`hex-a-hop`: 46-60 fps; the d-pad and L/R its six directions, the nub's clicks on a tile, undo, its menu) and Barrage (`barrage`: 60 fps; grenades land, ammunition out and reloaded; its soldiers about 5 pixels high: the user played it and found them fine) offered on the Pi; on the GameShell with the PSP-1000 (2026-10-18) Hex-a-hop 15-31 fps (turn by turn), Barrage 56-60: offered there too. TuxPuck runs (60 fps) but its menus answer only keys (Up, Down, Return, Escape, not the mouse) and the test did not get past them: its paddle follows the relative mouse, which suits the nub; not offered yet. LGeneral (with Debian's free kukgen WWI data) shows its title at 640x480 (it gave up on 800x600) but no click on its icon bar did anything: it reads the mouse through an SDL 1.2 event filter (`SDL_SetEventFilter`) and `SDL_GetMouseState`, not yet looked into. The OpenGL ones crash: Gunroar (a segfault after its sounds load, with or without the virtual joystick), Briquolo (a segfault listing fullscreen modes), FooBillard++ (a segfault after GLU tessellation errors), Battle Tanks (quits at once), Armagetron ("OpenGL support is either not configured in SDL": its mode request is not the layer's); parked. The test script's `click` does nothing in an SDL 1.2 game; `pad:cross` with cross as the left button does. Naev (the same day) runs on the board's GPU (about 22 fps in its menus) but fails the PSP's size like Starfighter: its interface wants a large screen, and at 480x272 its welcome text is about 5 pixels high and its dialog overflows; a game of trading, missions and HUD text, not offered. **The console frozen after a game** (2026-10-17, the user quitting Pushover with START + SELECT): the game ended, the console never took the PSP back nor logged anything for over two minutes (its loop stuck, its 5 s status lines gone), while the PSP, black, wrote "link up (usb), HELLO queued" and "link write failed" to its `pspexp-rt.log` twice a second (the memory stick's light flashing) until switched off. Not reproduced in 11 start-and-quit cycles (12 s and 95 s of play). The console now pings systemd's watchdog from its main loop every 2 s (`WATCHDOG=1` to `$NOTIFY_SOCKET`); pspexp-core ships a drop-in (`WatchdogSec=60`, `NotifyAccess=main`; in effect from the next boot, so an older console running during the update is not stopped). Stuck a minute, it is stopped with SIGABRT, its main thread's stack goes to the journal (glibc's `backtrace`), and it is restarted: tried on the Pi with the PSP connected, the console frozen (SIGSTOP), restarted 61 s later and the PSP connected again 2 s after, without being touched. The next such freeze names where it was stuck. **It came again, and it was the PSP** (2026-10-17, LinCity-NG quit, the console restarted right after fetching the PSP's screenshots): black, its memory stick's light flashing, the console waiting for it for minutes though the PSP was on the USB bus; HOME held left it. Its log: the board's sync arrived (a new link), the PSP greeted it, and exactly 1.0 s later its write failed, then again every 0.5-1 s, 390 times. The driver answered a sync while a write of the PSP's was on the same endpoint (the screenshots it was sending, then each new HELLO): the reply never went out, gave up after its 1 s and cancelled the endpoint (the HELLO failing), the board heard no reply and synced again, and the runtime, its write failed on a link still marked connected, greeted again at once: a loop of its own. Now the driver announces the new link only once its reply is out, and after a failed write or read the runtime waits for the board's next sync (a new generation) instead of greeting the old link again, reading and dropping what the old link left so the sync has a buffer to land in; after 5 s with none it greets again (a board that never noticed). Failures are logged five times, then one in 100. Tried on the PSP-1000: the console frozen (SIGSTOP) 5 s while the PSP sent a 5.6 MB file, then restarted, five times: each time one "link write failed" and the PSP back 1 s later; before, a restart in a game's stream (six times) never even failed a write, which is why the freeze was rare. **LinCity-NG** (the same day): not OpenGL after all, its own drawing (`-s`) composed. At 480x272 its side panel, tool bar and buttons covered almost all the map and its main menu's buttons overlapped; at 720x408, composed and shrunk to the PSP's screen (averaged), the menu is whole, the map has room and its text, about two thirds, is small but readable, R magnifying round the pointer. Played on the PSP-1000: a game started, the map scrolled by the d-pad, a residence chosen and built. The game draws 10-13 frames a second, its own pace (as on the stand-in); the pointer is drawn at 60 between them. Offered as `lincity-ng`. Its waiting for events (`SDL_WaitEventTimeout`) was not in the event log; it is now. **The nub's rest** (the same day): LinCity-NG's pointer crept left and down by itself into the corner, where the map scrolled; the user's PSP-1000 nub rests at -44 / +31 of 128 untouched (worn for years, the user says), past the layer's 35. Now within 56 of the middle is the middle, for the nub as the mouse (its speed the square of its tilt less that at 56, a firm push as fast as before; Quake on the PSP: still for 5 s untouched, a second at full push turning it well round), as keys (pressed past 70, let go only back within 56: at 30 one resting at -44 kept left held once pushed) and as the joystick (was 8000 of 32767, now 56 x 258); the console's terminal scrolls past 48 and counts the nub as activity past 64. **Thousands of textures** (2026-10-17, C-Dogs): 1.5 fps even with no PSP, the board's CPU all in the SDL layer: each `SDL_RenderCopy` looked its texture up by walking the list of every texture, and C-Dogs keeps a texture a sprite and draws ~900 a frame. The textures are now hashed by their `SDL_Texture` (4096 buckets, the last found kept): 30 fps, its own cap, in both modes. C-Dogs then plays at the PSP's own size, more of its battlefield shown, by its settings (`WindowWidth` 480, `WindowHeight` 272, `ScaleFactor` 1 in `options.cnf`, the rest left to its defaults), composed: in a mission 58-60 fps on the PSP-1000 while moving and firing, its menus 30 (its own pace there), offered as `cdogs`. Measuring it showed that the PSP's own screenshots stall the game: with one every few seconds the mission fell under 1 fps (the scripted presses then landing late), with one only at the end it held 60.

**The readability audit's other findings** (2026-10-21; three agents looked at every game's screens at 1:1, the SDL games in the stand-in, the OpenGL ones in PPSSPP, the Pi-GPU ones from the board's frame dumps). Fixed: Quake opened the PSP's keyboard as it started (Quakespasm turns SDL's text input on for its console; the stand-in shows a keyboard request only with `PSPEXP_SERVER_TEXT`, so it went unseen): `PSPEXP_TEXT=0` in its recipe, and the other games swept for the same; Hurrican quit at once from the launcher (it writes `Game_Log.txt` where it stands, and the launcher starts it in its root-owned entry folder): its start script changes to its save folder (its menu then takes cross, after a 50 s load in PPSSPP); Trigger Rally's car choice has no buttons and no pointer by design (left / right, Return, Escape): cross sends Return too; OpenTTD's first start showed a red "no compatible GPU" box: `video_hw_accel = false` in its first-run config; Egoboo's own defaults have its developer mode on (a debug window over play): its start script turns it off; an itch.io download that came back as a small web page failed the install, and its message named no file (an itch.io package had no `FILE`): the installer asks itch.io for a fresh link up to three times, and Celeste 64 and Theia name their files. Not bugs: Solarus DX's name grid follows the d-pad (the agent's keys went another way); Simutrans's welcome window is taller than 272 lines on a first start (its New Game under the toolbar, which Simutrans 124 cannot move): closing it (triangle) leaves a world to play, now said on its controls sheet. Command & Conquer (fixed the same day): its title drew with a grid of gaps and its menu stayed black, the same in the stand-in, in PPSSPP and with whole frames composed on the board (`PSPEXP_COMPOSE=1`), so the game's own picture, and with a layer from 2026-10-12 too: never seen working (its first commit measured 25-28 fps only). Part of C&C95's files are inside the disc's `INSTALL/SETUP.Z`, an InstallShield 3 archive (neither 7z nor unar opens it): `UPDATE.MIX`, `UPDATEC.MIX` (the 640x400 art; without it the 320x200 pictures are doubled with gaps), `TRANSIT.MIX`, `SPEECH.MIX`, the three ICNH sets and the English `CCLOCAL.MIX` (the disc's folders, flattened, left a German one: "Neues Spiel"). `pspexp-vanillaconquer` now carries isextract (OmniBlade's, GPL-2.0+) and the disc installer unpacks `SETUP.Z` over the disc's files (its marker gains a "2", so a board that has the game fetches the disc again); the films then play clean, the menu is English, the d-pad and Return work in it, and a click with the nub's pointer opens Intro & Sneak Peek (the test script's absolute `@` pointer confused its relative-mouse pointer; the nub moves it relatively). The crash on Escape during the intro did not come back. Both Vanilla Conquer games then got the magnifier on R (`PSPEXP_MAGNIFY=r`): full screen they open a window of the display's size (480x272) and shrink their 640x400 into it themselves, so the layer's scale was 1.000 and it had nothing to magnify; with `[Video] Windowed=yes, WindowWidth=640, WindowHeight=400` (the start script adds it to `conquer.ini` / `redalert.ini` when they have no `[Video]`) the layer shrinks it (0.680) and R shows Red Alert's difficulty dialog ("Please set the difficulty level...") at its own pixels. Command & Conquer was then dropped from the packages (2026-10-22): its menus small even so, and its working install needing an InstallShield 3 extractor on the board; `cnc-freeware` and the cnc recipe are gone, isextract and the `SETUP.Z` step left out of `pspexp-vanillaconquer` and the disc installer. Red Alert keeps the magnifier and the 640x400 window (`redalert-freeware` 1.0-pspexp2, installed without a fresh download). Tiberian Dawn on OpenRA (`openra-cnc`, the same game) went the same day: its 1024x576 interface at 47% (an install dialog's text about 4 px), drawn by the Pi's GPU, where the layer's magnifier does not reach; `pspexp-openra` keeps its start script. archive.org's server for the discs answered 500 that night: the disc download now retries any error (`--retry-all-errors`). Open: Egoboo's module screen laid out wider than 480.

**1.2.0's GameShell image on the board** (2026-10-22, written from the release and started with the PSP-1000): the integration suite (`tests/psp/run.py`) 26 of 28 at first. The stick listing's date was 3.0 h off: a fresh card's board is in UTC and the PSP in its owner's zone (the same as on 2026-10-11, -2.0 h then; Settings > Clock > Time zone). The film check, never run on a GameShell before: a 1280x720 H.264 film at 13.1 pictures a second, its clock at 0.43x (the Pi: 29.5, 1.01x), now in the manual. Its games suite then failed to start OpenTyrian, but only at a first press: since the controls sheet (2026-10-18) the launcher's cross opens the sheet and a second cross starts the game, which the suite did not know (a run passed when the sheet was left open by the one before); `play()` now presses through it, and OpenTyrian passed twice from a first start (34 fps in its game screens, no link errors, no underruns).

**SDL 1.2 games and the PSP keyboard** (found 2026-10-21 by the readability audit): the keyboard's code (8529d25, a24a2c3) called `SDL_IsTextInputActive` directly, not through `REAL()`; the SDL 1.2 build of the layer has no such symbol, so a program that reached that code died with `undefined symbol: SDL_IsTextInputActive` (Frozen Bubble, Kobo Deluxe; GNU Robbo, Barrage and Hex-a-hop never reached it), in 1.1.0 too. Both calls now go through `REAL()`; both games start and play again (the Pi 400 into the stand-in). Not enough: the keyboard request also called `SDL_GetWindowTitle` directly and Frozen Bubble still died on it (found by the keyboard sweep); `nm -D --undefined-only` on the SDL 1.2 build now lists no `SDL_` symbol at all. The sweep (66 games and the Dreamcast, the stand-in answering with `PSPEXP_SERVER_TEXT`) found the keyboard opening where nothing is typed in Powermanga, TecnoballZ and Frozen Bubble, in their first frames: they stop and restart text input as they start, and a game's own stop counted before the layer had turned it off itself; now nothing counts before that (`g_text_armed`, 30 frames). LBreakoutHD turns text input on each time its main menu comes back: `PSPEXP_TEXT=0` in its recipe. Neverball's name field still opens it.

**The Nintendo DS** (2026-10-21, the PSP-1000 from a Pi 400 with the player's own games): melonDS DS 1.4.0 (`build-melonds.sh`, the libretro core with its ARM64 JIT, software 3D, threaded), played by pspexp-retro. Letting the core lay the two screens out (its hybrid layout, the main screen 3x) made a 1024x576 frame that the board upscaled in software and the layer shrank again: 261 KB a frame, a Mario Kart DS race at 9 fps on the PSP, the emulation dragged to 45-55 and the sound breaking. Now the core gives its plain 256x384 (the two screens stacked) and pspexp-retro lays them out on the PSP (`PSPEXP_RETRO_DUAL`): both 240x180 side by side, the main one at its own 256x192 with the other at 224x168 (the default), or the main one 360x270 with the other 120x90, `layout` and `swap` in `PSPEXP_RETRO_MAP`; the nub is the right analog stick (`PSPEXP_RETRO_NUB=right`, melonDS DS's joystick cursor), SELECT held the stylus (R2). A race then 54-60 fps on the PSP, the emulation at 60, the sound whole (`PSPEXP_FRAME_DROP=2`). Two traps on the way: the texture made at the core's largest frame (1024x576) was halved by the layer and the DS's 256x384 with it (128x192: unreadable; the texture is now the frame's own size), and the launcher keeps only 400 characters of a system's `env=` lines (the rest dropped silently, a test's settings with them), so melonDS DS's options go in `~/.config/pspexp-retro/melondsds.opt`. **Then on the Pi 400:** with the full-resolution picture the emulation fell behind (a race 50-58, New Super Mario Bros. 41 in a level, even with no PSP: the Mac, half a core, held 60 on the PSP), its thread at 100%. The recompiler was on (its log), a Cortex-A72 build with -O3 and LTO gained nothing; `perf` put 29% of the emulation thread in 2D drawing. pspexp-retro had never told a core a frame would not be shown: its environment switch drops `RETRO_ENVIRONMENT_EXPERIMENTAL` and the case for `GET_AUDIO_VIDEO_ENABLE` carried it, so it never matched (nor `GET_INPUT_BITMASKS`, `GET_FASTFORWARDING`, two more); with that fixed and melonDS patched to skip a frame's 2D drawing when it will not be shown and no display capture is on (`scripts/packages/melonds/`), Mario Kart DS's race held 59.8 emulated, 29.9 shown (`PSPEXP_RETRO_VIDEO_DIV=2`), the thread at 70%; New Super Mario Bros. 56-60. `PSPEXP_RETRO_CATCHUP=2` caps the frames run per picture (a core just too slow stayed at 4 for good: 11 fps shown) and drops a debt past a few frames. At 2.0 GHz (`arm_freq=2000`, `over_voltage=6` in config.txt, on the test Pi) New Super Mario Bros. 58-60 emulated, 21-30 shown, a short sound gap at a level's start. Packaged as the system `nds` (package `pspexp-melonds`: the core in Debian's libretro folder and its defaults in `/usr/share/pspexp-retro/melondsds.opt`, which pspexp-retro now reads before the player's own; melonDS pinned to its commit, melonDS DS naming a branch), Pi only.

**The Sega Saturn** (2026-10-21, a Pi 400, the player's own discs and BIOS): Yaba Sanshiro 3.5.0, its standalone SDL2 program (`retro_arena`) with its SH-2 recompiler for 64-bit ARM, built by `build-yabasanshiro.sh` from libretro's copy of its tree (the yabasanshiro branch of libretro/yabause: devmiyax/yabause is gone from GitHub). The accurate emulators are out of a Pi's reach (Beetle Saturn, Kronos), and the libretro core of Yaba Sanshiro asks for hardware rendering, which pspexp-retro does not give; so, as the Dreamcast, its own program drawn by the Pi's GPU through the host-rendered layer (`PSPEXP_HOSTGL`), started by `/usr/games/pspexp-saturn` (package `pspexp-yabasanshiro`, recipe `saturn`). Its build: its rp64 toolchain file names the processor `arm`, which picked the 32-bit recompiler's assembly (now `aarch64`); its `_POSIX_C_SOURCE` hid `ftello`/`strdup` from libchdr's FLAC, an error in GCC 14 (now `_GNU_SOURCE`). It ran at once, but: (1) its window was the display's whole 1024x768 (17-34 fps): `YABASANSHIRO_WINDOW=480x272` (patched) gives 51-60. (2) Its button map is `~/.yabasanshiro/keymapv2.json` (the README says keymap.json); without one every game saw no pad and Panzer Dragoon and Saturn Bomberman stopped on black after the Sega screen. (3) Sprites with palette colours (Saturn Bomberman's characters and logo, Panzer Dragoon's textures) came out as one colour on the V3D. Tried: its compute-shader renderer (the same, and Panzer Dragoon crashed), an OpenGL ES context asked for (Mesa had given a desktop GL 3.1 one; kept, the same picture); Mesa's software renderer (`LIBGL_ALWAYS_SOFTWARE=1`) drew everything right, so the fault was in what the V3D returns: its shaders turn a stored colour back into an integer with `int(c*255.0)`, which truncates a 127.99... the V3D gives for 128; rounded (`+0.5`, 30 places in `yglshaderes.c`) every game drew right. (4) Its menu's button was read only from an EmulationStation input file (else Escape alone opens it): now the map's `select`. (5) The Saturn pad has six face buttons: the layer's `PSPEXP_PAD_SHIFT=select` moves the four face buttons to the virtual controller's buttons 8-11 while SELECT is held (X and Z: SELECT + square, SELECT + circle) and reports SELECT alone as a tap when let go (the menu); the layer's keyboard keys are off (its default Escape for SELECT opened the menu). (6) An `analogx`/`analogy` entry in the map of a standard pad (`padmode 0`) crashed it now and then at start (written into a pad that has none): the map has none. Measured in the stand-in from the Pi 400 (the frames the Pi sent a second, three audit agents running on the same Pi): Saturn Bomberman 52-57, Panzer Dragoon in play 58, NiGHTS 57, Guardian Heroes 55, Sega Rally in a race 45; every picture right at 1:1 and its text readable (the Saturn's 320x224 at 4:3, 363x272 on the PSP). The Mac's stand-in dropped the connection when CoreAudio refused to start its sound (AudioQueueStart -66681, after a day of tests and seven stand-ins): `SDL_AUDIODRIVER=dummy` on the stand-in for picture tests. Not yet played on the PSP itself.

**The rest started once on the PSP-1000** (2026-10-04, each left on its menu, intro or demo for 45 s, no input; menus are light, so these say it shows and sounds, not how it plays): Lugaru HD, SuperTuxKart, Trackballs, Luanti (in its world) and Red Alert on Vanilla Conquer about 60 fps; Open Surge 51-60; Quake II's demo 44 median (58 at best; the PSP's CPU 7.9 ms for 165 KB a frame); OpenRA's Tiberian Dawn 19-20 at its menu; AssaultCube 20, with the PSP refusing a resource (`OUT_OF_MEMORY`) and one 2 MB frame at its start. Command & Conquer on Vanilla Conquer: its intro movies show (dotted: interlaced, shrunk from 400 lines), then the picture stays black at 60 fps with nothing uploaded, Escape pressed or not, and the same on the layer of the day it was added (bb69434), when its menus had worked: a fault of the game's setup to find. None of these yet played by hand.

**Red Alert silent** (2026-10-05, heard on the GameShell). Vanilla Conquer plays through OpenAL, which `pspexp-run` sends into a pipe the layer streams to the PSP. The layer made the pipe when it connected, at the program's first window; Red Alert opens its OpenAL device earlier, so OpenAL found no pipe, made a plain file of that name and wrote there, and the pipe the layer made afterwards was never written. `pspexp-run` now makes the pipe before the program starts, and the layer reads it from the moment it loads (OpenAL's opening waits for a reader), letting go of what comes before the link is up; it also now makes its sound ring before the ring's lock, which the early reader had raced (a crash). Red Alert on the GameShell into PPSSPP: the PSP's buffer 17-23 KB, no gaps after the first seconds. Its sound had been checked with a file: OpenAL itself was fine.

**The Omega2+ with the PSP-1000** (2026-10-05): updated to the redesigned launcher (`scripts/omega/build.sh`, `install.sh`, its catalogue republished: 12 packages); each game started once from its entry's own command, 45 s, no input: Freedoom Phase 1 and 2 at Doom's 35 fps, no sound gaps; Beneath a Steel Sky 17, Lure of the Temptress 26, Flight of the Amazon Queen 10, Drascula 8.5 (ScummVM: frames shown only as they change); OpenTyrian's title 6 (35 at best); Jazz Jackrabbit about 14 in its menus; Cave Story still making its sound effects after a minute (NXEngine synthesises them; the Omega has no FPU), as before too slow to start; Game Boy (Tetris) 60 fps; Mega Drive (Sonic) emulated at full speed, 28 shown. A game stopped by killing its shell left its program holding the PSP's USB (`LIBUSB_ERROR_BUSY` for the next): the launcher's own Stop does not.

**The nub at full tilt** (2026-10-05, Super Mario 64 played on the PSP: Mario "went in circles or stopped"). The virtual controller's stick took the nub (0..255) as `(v - 128) * 258`: at 0, fully up or left, that is -33024, which wrapped in 16 bits to +32512, the opposite direction at full strength; part-way it was right. Pushed up, Mario ran back at the camera, which turns to follow: circles. Now clamped to -32768..32767. Every program reading the PSP's nub as a stick was affected (the N64, SRB2's movement, SuperTuxKart's steering, DevilutionX); the Omega's games read the nub as keys.

**The virtual controller attached late** (2026-10-05, SRB2 on the GameShell: its intro would not skip, no PSP button did anything). The layer attached its virtual controller at the first input from the PSP, after a program had started SDL's joysticks and listed them; SRB2 lists once and does not watch for controllers added, so it had none. The controller is now attached in `SDL_Init` / `SDL_InitSubSystem` when joysticks are asked for, before the program lists them, and again after the joysticks are shut down and started anew (`SDL_QuitSubSystem`: SDL lets the virtual one go). That was not all: SRB2 still had no button. With the new event log (`PSPEXP_LOG_EVENTS=1`) it received only the controller's arrival; SDL itself dropped the button events, because no window of the program had keyboard focus (SDL's own focus: on a headless board none has it, and the layer's focus is an event it shows the program), unless `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is set. The layer now sets it as it loads; cross then skips SRB2's intro to its title. A test program had got the events because its window did get SDL's focus. (SRB2Kart's pad test on the Pi, its kart still on the grid, was probably the same.)

**Next.** Fewer bytes for lit vertices (16-bit positions and coordinates are done for unlit draws; lit ones need 8-bit normals, which the GE does not renormalise after a scaled matrix, and 16-bit colours); display lists as meshes of their own rather than by content (runs of `glBegin`/`glEnd` blocks are merged and kept by content since DreamChess; a list with state changes inside is still replayed in pieces); room in VRAM for a render target by moving textures out to RAM; Neverball's 20 ms of GE time (fill: large blended or filtered polygons); more games (Quakespasm and ioquake3 done: Quake, OpenArena; DreamChess and Emilia Pinball in PPSSPP; AssaultCube and others being tried); shaders that combine textures other than multiplied or added; a shadow combined as one minus a texture's alpha (Neverball's, left out: a texture made of the alpha and the GE's blend function would do it); a texture matrix of its own message (Neverball's shadow would then cost little); mipmaps (shimmering far surfaces; tried once: a Neverball text texture drew white); the water, sky and sprite polygons Quake draws in immediate mode, and its text; cube maps whose direction is not a reflection about a normal (a skybox drawn through one); a shorter warm-up before a churning shader draw is streamed (the first seconds of a race are slower); per-vertex branches (Widelands' image flavours) resolved per draw; the SDL layer's render targets onto `3D_TARGET`; the SDL executor has no 3D, so the Mac stand-in shows nothing of these programs (PPSSPP and the PSP do). Host-rendered on the Pi: SuperTuxKart, Trackballs, Open Surge and the N64 (GoldenEye, Mario 64; Ocarina of Time not tried) measured on the PSP itself (only PPSSPP so far); OpenClonk (2026-10-04, parked: too slow, not crashing): its "crash with a live link" was `timeout` killing it mid-frame (every SIGSEGV fell at the kill; quit through START+SELECT it ran minutes through the menus and the Options dialog), and it sounds through OpenAL, not SDL; with the gamepad on (`GamepadGuiControl=1`) and a 1024x576 window (`PSPEXP_WINDOW`; at 854x480 its start menu is cut off) Tutorial 1 plays, but at 5.4 fps into PPSSPP (640x360: 9), its main thread at 40% CPU: the Pi's V3D is the limit (its landscape and lighting shaders); Luanti under the PSP's layer (shaders off: crashes once in the world, framebuffer calls among the missing); fewer bytes for a moving 3D frame (done: median cut to CLUT8, 230 to 137 KB; next, if the PSP shows bytes still limit it: compression the PSP undoes). Wolfenstein: Enemy Territory (ET: Legacy, done 2026-10-04) and OpenArena with indexed draws kept by content, measured on the PSP itself; ET: Legacy's menu cursor is small (32 of its 640 units) and could be drawn larger. DevilutionX with Diablo's shareware data (done 2026-10-04: `devilutionx`, the project's own build; an armhf build for the GameShell later). Lugaru's 416 KB a frame (immediate-mode geometry each frame) on the real PSP: if too slow, its geometry kept as meshes as Quake's are, or the board's GPU on a Pi. Unreal Tournament on the PSP's GE: its OpenGLDrv needs ARB vertex and fragment programs (assembly shaders), which the GL layer would have to translate as it does GLSL; and SDL3 (OldUnreal's 469e) hooked as SDL2 is. OpenRA (done 2026-10-04: `openra-ra`, `openra-cnc`, below). SDL3 in the layer (backlog): UT2004 (freeware since 2026, OldUnreal's arm64 builds) and Unreal Gold (227k) ship only SDL3. Sonic Robo Blast 2 and SRB2Kart (done 2026-10-04): free Sonic fan games on a Doom-derived engine (software renderer, 35 fps cap, SDL2), the data downloaded from the SRB2 team's releases at install; SRB2 2.2.15 at the PSP's own 480x272 (SRB2Kart's older renderer needs 640x400), 22-23 fps into PPSSPP; on the PSP only an unfinished SRB2 2.0.6 (PSP-2000 and later, unstable) and a newer 2.2.13 port of unknown state. Built for armhf too (2026-10-05, with SRB2Kart, AssaultCube and Vanilla Conquer: their packages had been arm64 only, so the GameShell's catalogue offered games it could not install); on the GameShell (R16) with the PSP-1000: SRB2 at its full 35.7 fps in Greenflower Zone (about 40 s to load), Red Alert's intro movie at its own 15 fps, no sound gaps. Godot games (parked 2026-10-04): the notable ones are commercial (Brotato, Dome Keeper, Halls of Torment, Cassette Beasts, Buckshot Roulette) and none is owned for testing; the free catalogue has nothing comparable. If taken up: Godot 3 through an FRT-style runtime that draws with SDL2 (as PortMaster runs these games on ARM handhelds), which the SDL layer would catch as it is; Godot 4 only through a headless Wayland compositor and an EGL swap hook; the user's own game data in either case.

---

## 12b. The PSPExp plugin: recording native PSP games (stages 1–2, 2026-10-23)

*Renamed 2026-10-25: named PSPExp Capture until the PSP shelf's virtual disc (§12c) made it more
than a recorder; now the PSPExp plugin (`psp/plugin`, `pspexp_plugin.prx`, `PSP/GAME/PSPExp/plugin/`
on the stick) and its board side `pspexp-plugin-host` (`tools/pspexp-plugin-host`, which was
`pspexp-capture`). Below, "PSPExp Capture" in the history means the plugin; paths are the new ones.*

A Share button for the PSP: a native game's picture and sound sent over the USB cable to a
PSPExp receiver, which makes a finished clip. It lives beside PSPExp and is shipped as part of it.
Prior art, honestly: RemoteJoy / RemoteJoyLite (2008, Windows, picture on screen), psp-usb-capture
(pspcap, Windows, no file), xerpi's USB Video Class plugin (a webcam to the computer, tearing, no
sound), and two recent ones with the game's sound: remotejoy-audio (macOS, into RetroArch, 2026)
and psp-cast (Windows, October 2026: picture, sound and the PC's controls in a window, 30 or 60 fps
with half frames, 16-bit when a game runs fast; a design close to this one, and the model for its
handling of a game's exit). None makes the finished file itself (a window to record with OBS, or
RetroArch's recorder); what is PSPExp's own: the ready MP4, recording on the board without a
computer, the ♪ clip button (planned).

**The pieces.** `psp/plugin` builds `pspexp_plugin.prx`, a kernel plugin for custom firmware
(PRO / ARK: `ms0:/seplugins/game.txt`). PSPExp's USB driver is built into it
(`psp/transport-usb/main.c` with `PSPEXP_USB_EMBEDDED`, same IDs and sync as the runtime), so
nothing else of PSPExp is loaded while a game runs. `tools/pspexp-plugin-host` is the receiver
(`build/host/pspexp-plugin-host [-o clip.mp4] [-d folder] [-t seconds] [-c control-fifo] [-k]`): it
waits for the plugin, pipes the frames into ffmpeg (H.264), mixes the sound and joins it at the
end; with `-d` it also keeps `latest.ppm` (the screen now) and a picture every 5 s.

**On the link** (PSP to receiver), 32-byte headers:
- a frame: `"PXCF"`, u16 width (VRAM's stride, 512; the screen is its first 480), height, format
  (0 565, 1 5551, 2 4444, 3 8888), u16 reserved, u32 sequence, u32 the game's frame changes so
  far, u32 the time it was shown (µs), u32 payload bytes, two reserved; then the rows as the game
  drew them. The receiver places each frame at its time in a steady 30 a second (a late one held),
  so a clip plays at the game's own speed;
- a block of sound: `"PXCA"`, u8 kind (0 one of the eight channels, 1 output2, 2 SRC), u8 channel,
  u16 channels (1 or 2), u32 rate, u16 left and right volume (0x8000 full), u32 samples, u32 time,
  u32 payload bytes, u32 sequence; then the 16-bit samples as the game gave them;
- a reply to a command: `"PXCR"` (u32 command, i32 result) or `"PXCL"` (the log's end).

**The picture.** A watcher thread (0x18) looks at the shown frame once per refresh
(`sceDisplayGetFrameBuf`) and counts the new ones (the game's frame rate). When none is on its
way, a copier thread (0x19) has the DMA engine copy it out of VRAM in bands of 32 KB into a ring
of six in kernel memory (192 KB), while the sender (0x1A) writes each band to the link; the CPU
does nothing to the pixels. The copy must end before the game's next flip (it then draws into that
buffer): the ring holds it to the link's pace, ≈22 ms for a 32-bit frame; one flipped before its
copy ended is counted as late.

**The sound.** The PSP mixes up to eight channels in hardware, so there is no mixed stream to
tap: each block a game outputs is caught on its way in (`sceAudio`'s outputs, the channels'
reserve and size calls to learn their formats: twelve user syscalls redirected through the
plugin, from its start, before the game reserves its channels), copied with its channel, volume
and time into a 96 KB ring and sent between frames. The receiver places each channel's blocks end
to end (a channel quiet for over 0.25 s starts again at its block's time) and mixes them.

**Control from the receiver** (for testing without hands on the PSP; also `exp`, `copyprio`, `band`, `tx`, `idle`, `vram`, `gestat`, `ge`: the measurements below): `"PXCC"`, u32 command, u32
payload bytes. Buttons pressed for the game (its four `sceCtrl` buffer reads redirected; the
PSP's own save screen takes them too), a file written into `ms0:/seplugins/` (a new plugin, over
the cable: no USB mode), the log, pause/resume (the game's own frame rate, for comparing), and a
restart of the game from its ISO (PRO's `sctrlKernelLoadExecVSHWithApitype(0x120,
"disc0:/PSP_GAME/SYSDIR/EBOOT.BIN")`, as PRO's menu starts one), so a new plugin loads. The
receiver takes them from a FIFO (`-c`, one a line: `press cross 150`, `hold cross`, `release`,
`put file.prx`, `log`, `pause`, `resume`, `restart`); only one receiver runs at a time (a second,
idle one once read the FIFO's commands meant for the one holding the link). The plugin keeps the
last run's log as `pspexp_plugin.log.old`.

**What was tried, on a PSP-1000 (6.60 PRO-C) with Burnout Legends from the memory stick:**

| version | change | result |
|---|---|---|
| v1 | rows copied from the shown frame while sending | torn and half-drawn frames (a black band on a menu, half the car missing): sending took longer than a frame and the game drew into it meanwhile; 10–15 frames/s |
| v2 | whole frame snapshotted by DMA (544 KB) | clean pictures; 15.8 frames/s received of the game's 26 |
| v4 | sent as 565 | the game hung loading a saved profile: the snapshot sat in the game's memory (partition 2), where the PSP's save screen takes its own |
| v5 | snapshot in the volatile area (partition 5, 4 MB) | the game froze on its splash screen (it takes that area as it starts) |
| v6 | 16-bit frame in kernel memory, made band by band; the driver's receive buffers 192 KB → 12 KB | the profile loads |
| v8 | the game's exit caught | every version before switched the PSP off on HOME > Exit: a game's exit restarts the system without stopping plugins (`module_stop` never runs), with the USB link and threads still going. Now `sceKernelExitGame` / `...WithStatus` are redirected through the plugin (PRO's `sctrlHENFindFunction` + `sctrlHENPatchSyscall`), which stops everything first |
| v9 | the VFPU packs 8888 to 565 (`vt5650.q`) | races 14–23 frames/s of the game's 25–29 |
| v10 | the sound | 12 of 12 calls caught, two stereo channels in Burnout (353 KB/s), none dropped; a 4½-minute clip stays in sync |
| v11 | watcher and sender above the game's threads | no more frames, the game slower (20–25): back |
| v12 | copy and send overlapped | the per-frame times showed it: sending 10.5 ms (the link's full speed), copying 22–27 ms in a race; one after the other just over a frame, so every other frame was missed. Overlapped: every frame sent, but the game at 20 fps captured against 30 paused, in the same race: the copy (CPU packing at high priority) cost a third of its speed |
| v13 | the USB engine reading the frame from VRAM itself | the first frame failed and the link never recovered: it reads main memory only (other capture tools copy out of VRAM first too) |
| v14 | the DMA ring, 32-bit as drawn, no CPU on the pixels | the title and menus: every frame, 29.4–29.9 a second at 16.7 MB/s in full colour |

**2026-10-24: what costs a race its speed.** Measured in the same race, 20 s each (`exp`, `pause`, per-thread processor time from `sceKernelReferThreadRunStatus`):

| | the game | sent |
|---|---|---|
| paused | 30 fps | – |
| every frame copied out of VRAM, nothing sent (`exp 1`) | 29.9 | – (copier 1.4 ms of CPU a frame) |
| sending, nothing copied (`exp 2`) | 24.1–24.5 | all |
| both, 32 KB transfers | 22.7–23.1 | all |
| both, 64 KB transfers | 23.5–24.7 | all |
| both, 128 KB transfers (now the default: 2 bands of 128 KB, no short-ending splits) | 24.3–25.1 | all |

The copy is free; the PSP's USB driver spends processor time on every byte it sends (≈7 ms of a frame for 557 KB, only ≈3 ms of it in the plugin's threads: the rest in its interrupts), and Burnout uses all the processor (the idle meter, a thread below everything, got almost nothing). Fewer bytes without processor work would help; what was tried:
- **the GE redrawing the frame as 16-bit**: no free VRAM in a race (`vram`: two 32-bit frames, depth, textures, one 32 KB block unused); the depth buffer between frames (exactly a 16-bit frame's size) with the GE's transfer engine copying it out was built, but **any list of ours in the game's GE queue froze or switched off the PSP**, even an empty one without a context (`ge 6`). Along the way: Burnout flips from an interrupt (896 of 897 flips), where lists cannot be queued nor threads woken (the flip leaves a note the watcher takes); `sceGeListEnQueueHead` is refused while a list draws and each refusal loses one of the GE's 64 list slots for good (uOFW `ge.c`), until every enqueue fails with 0x80000022;
- **drawing commands instead of pixels** (`gestat`, the game's lists walked as it queues them): 4,300–5,900 draws, 81–127 KB of commands and 326–420 KB of vertices a frame: about as much as the picture, and walking them slowed the game;
- **the USB engine reading VRAM** (v13): it cannot;
- a USB driver of our own (in case the hardware can move data itself): not tried, weeks of reverse engineering.

**System screens.** The PSP's save, load, keyboard and message screens need memory the plugin otherwise holds: Castlevania: The Dracula X Chronicles stopped answering at its name entry (its save could not open). The plugin now gives back its rings (≈350 KB of the kernel's) when one opens (16 `sceUtility` calls caught) and takes them back 1.5 s after it closes: the name entry and its save then went through.

**A game's exit (2026-10-24): the memory stick at boot.** Castlevania: The Dracula X Chronicles switched the PSP off on every HOME > Exit (and on the `restart` command) with the plugin on, cleanly without it, eleven times. What it was not, each ruled out by a run: the USB (stopped before the exit, or never started), the plugin's interceptions (all off), its threads or memory, its shutdown (at once, gentle, or none). What it was: **the plugin touched the memory stick in `module_start`, while the game was starting** (renaming and opening its log, from v10 on). psp-cast had found the same: "an sceIoOpen on ms0 during the start (module_start, or right after) hung the PSP — always, after exiting a game". Now no memory-stick access before 2 s after the start, nor once HOME is pressed or the game exits: the log's lines wait in memory and go out together (`g_log_ready`, `g_log_hold`). With that, Castlevania exited cleanly with frames and sound streaming. Since then the plugin does not touch the memory stick at all: its log goes to the receiver over the cable ("PXCM" packets, kept in `psp.log`); only a developer build (`make DEV=1`) still writes it there, under the same rule, and takes `put` and `restart`. Kept as well: the module is never stopped or unloaded by the system (`PSP_MODULE_NO_STOP | SINGLE_LOAD | SINGLE_START`, attribute 0x1007, as psp-cast), and the USB stops when HOME is pressed (the button itself, or `sceImposeGetStatus`' top bit: 0x80050102 with the menu open, 0x00050102 in play, 1 at a game's start; not bit 0, which RemoteJoyLite toggles the menu by) and starts again once the menu has been closed for 0.5 s. The lesson: a plugin of the same kind that works is the first thing to compare against, line by line, notes included.

**The fallback and the DMA engine (2026-10-24, evening).** Jeanne d'Arc, Crisis Core and Phantasy Star Portable 2 leave too little of their own memory free for the buffers (497 KB, 15 KB, 1.4 MB, against the 4 MB margin), so theirs come from the kernel's memory: 32 KB bands, as many as the frame needs and fit with 192 KB of it left free (9 for Jeanne d'Arc's 16-bit frame, 8 of PSP2's 9, 9 of Crisis Core's 17 at 32 bits), and 16 KB for the sound. They are given back for every system screen and as soon as HOME is pressed (an exit may follow). Found on the way:
- **The DMA engine.** With two bands copied by `sceDmacMemcpy` as the sending freed them (nine copies spread over each frame), Jeanne d'Arc switched the PSP off the moment its frames were copied; its sound alone recorded fine for minutes. Copied by the processor (VRAM read uncached), it played on with the picture recorded, so the DMA engine was the cause, but that copy cost 40 % of the processor (≈27 ms a frame). The engine copies one thing at a time (≈225 MB/s, from PPSSPP's notes), and games use it too. The copy is now `sceDmacTryMemcpy`, which refuses while the engine is busy, tried again every 50 µs; after 3 ms the processor copies, reading VRAM through its cache, as psp-cast does. Jeanne d'Arc then played and recorded with the DMA at ≈3 % of the processor, and Crisis Core found the engine busy 39 times in a minute, waiting each time. The engine was never busy in Jeanne d'Arc, though, so which change made the difference there, the try or the copies grouped at the swap, is not separated.
- **Copied at the swap.** With five bands, the last four were copied 1.5–6 ms after the swap as the sending freed them, and in Jeanne d'Arc's illustrated scenes they came black or half drawn (a staircase in 64-pixel columns): the game draws into the frame it shows. Now the whole frame is copied at once where the bands allow, then checked against VRAM (1,024 words). If the game drew into it meanwhile, it is copied again 3 ms later (twice at most, never once the game has swapped). In Jeanne d'Arc 25 of ≈4,000 frames were drawn into and 11 of those copied again cleanly; 1 of 427 sampled frames still showed it, against 2 of 40 before.
- **Results:** Jeanne d'Arc 20–30 fps (the game's own rate), Crisis Core 27–30 of 30 at 32 bits (15–17 MB/s), Phantasy Star Portable 2 28–30 of 30, its system screens given the memory back twice. All three exited cleanly.
- **The volatile memory.** A 32-bit frame (557 KB) needs 17 bands, more than the kernel's memory spares. When the bands cannot hold the whole frame, the plugin now takes the PSP's volatile memory (4 MB at 0x08400000) if the game leaves it unlocked: the whole frame and the sound in it, one DMA copy, checked as above. It is given back the moment the game locks it (`sceKernelVolatileMemLock`/`TryLock` and `scePowerVolatileMemLock`/`TryLock` caught, the game's call then going through), at HOME and for system screens; with any of those four calls not caught it is never taken, as a game left waiting in its lock would wait forever. Crisis Core locks it itself (`TryLock`: 0x802B0200, in use), so it keeps 9 of its 17 bands: its frames still come whole, as it draws into the other buffer and the last bands are copied ≈10 ms after the swap (nothing late). Those last bands are now checked once copied and counted when drawn into: 3 times in a minute of Crisis Core. Giving it back ran later in Castlevania, at each HOME (see the note button below).
- **The cache at give-back.** The sound ring is written by the processor through its cache; lines not yet written out when the buffers were given back could be written later into memory that was the game's again. They are now written back and dropped first, for every kind of buffer.
- **Open:** the developer `restart` command switched the PSP off once, from Jeanne d'Arc with the fallback's bands held, before the new start (its log ends at "restarting"). Not used since.

**The note button (♪), 2026-10-24.** The PSP-1000's sound button makes clips without hands on the computer. **♪+R** saves the last 30 seconds as `clip-DATE.mp4`; **♪+L** starts a recording and, pressed again, ends it as `recording-DATE.mp4`; ♪ pressed alone still mutes and unmutes the sound, as it always did. Pressed with a pair:
- the plugin hides the game's buttons while ♪ is down (and those held with it until they are let go), so the game does not see R or L;
- the mute the system toggles on ♪ is put back within a frame: the HOME menu's driver sees the raw buttons before any controller mask (uOFW's `ctrl.c`), so the plugin keeps the mute setting as it was (`sceImposeGetParam`/`SetParam`, PRO resolving the NIDs) while ♪ is down and for half a second after; pressed alone, the system's toggle is applied when it is let go;
- a dot on the screen confirms it: green for a second for a clip, red while recording, in the frame's bottom-right corner (16 pixels, a dark ring). The plugin draws it into the frame the game shows once that frame has been copied (the screen is drawn top to bottom over ≈16 ms and the copy takes a few), so it shows from the frame's first showing and is never in the recording; the pixels under it are kept and put back when a buffer the game shows again still has it, or when it goes; the unchanged-frame and redraw checks skip its corner. In Castlevania the dots showed as meant, flickering slightly, and none of 390 frames of the clips had one. It replaced the first version's beeps (on a sound channel of the plugin's own) and its WLAN light (`sceSysconCtrlLED` answered 0 but the light stayed dark on the PSP-1000).
The plugin sends the press as an event ("PXCE": its type, its time on the PSP's clock) to the receiver, which keeps the picture as it comes in 1-second pieces (ffmpeg's segment muxer, a key frame each second; its first piece took 2 s until `segment_time_delta` was set to half a frame) and the mixed sound in memory, both only as far back as a clip or a recording under way needs. A clip is the pieces it spans joined without encoding again, with the sound of the same seconds, made in a thread of its own once its last piece is complete. Tried in Castlevania: The Dracula X Chronicles: clips of 30 s, recordings, the confirmation heard (the beeps then) and seen (the dot now), the sound left on by the pairs, ♪ alone still muting and unmuting. The volatile memory was also taken and given back there at each HOME, many times.

**When it starts, and ♪ before it (2026-10-24, night).** A ♪+R pressed 13 s into Bomberman showed the green dot and saved a black clip of 1 s: the plugin takes its buffers only once the game has set up (then a fixed 20 s; Jeanne d'Arc and Crisis Core froze or crashed with them taken 2 s in) and had nothing to save. Now ♪+R and ♪+L do nothing until the buffers are there (still a pair: no dot, the mute kept; the receiver also makes no clip of no picture), and the wait follows the game: the buffers are taken once the game's memory and the kernel's have not moved (within 64 KB) for 3 s while it shows pictures, never before 6 s, at most 20 s (`game_settled`, logged as "the game is set up after N s", which a board's quiet receiver keeps in its log with the ♪ lines). Tried on the PSP-1000 (the Pi 400 receiving, Clips on): Bomberman set up after 6.8 s, Jeanne d'Arc after 8.9 s (497 KB of its memory free, as once fully set up), Crisis Core after 6.7 s (40 KB free: the kernel's bands, as before); ♪+R pressed earlier did nothing ("not capturing yet", no dot); clips of 1–25 s with their picture and sound (the logos silent, as the games are there); all three played and exited cleanly.

**PSPExp itself (2026-10-24).** PRO loads `game.txt`'s plugins for homebrew too, so the plugin also started inside PSPExp's own runtime: its USB driver has the same name as PSPExp's, it took the USB first, and PSPExp could not reach its board (0 frames a second, a board's receiver connected to the plugin instead). Now, 2 s after the start, before starting anything, the plugin looks for PSPExp's modules (`PSPExpBoot`, `PSPExpRuntime`, `PSPExpUSB`, through `sceKernelFindModuleByName`, PRO resolving the NID) and keeps out entirely when it finds one; its catches stay idle, as they act only while it holds buffers or ♪ is down. PSPExp then reached the Pi 400's console with the plugin on.

**On a board (2026-10-24).** The plugin presents the link under a USB product of its own, 0x01CB with the driver name `PSPExpCapture` (PSPExp's runtime keeps 0x01CA and `PSPExpUSBDriver`), so a board's console, waiting for PSPExp, never takes a native game, and the receiver runs beside it (first tried as a second service; now the console starts it with the launcher over USB when it and ffmpeg are installed, `pspexp-plugin-host -k -q -s ~/Videos/PSPExp`, again 10 s after it ends, stopped with the console, its clips under way finished; `PSPEXP_CAPTURE=0` turns it off; `-q` keeps the console's log to the connections, the clips and the errors; on a 32-bit ARM board libx264 encodes at `ultrafast`); the boards' USB rule now allows both (`01c[ab]`). The receiver also builds on the Pi with gcc alone (its file, the USB transport, `pspexp_time.c`, libusb). On the Pi 400, with Castlevania: ♪+R clips and a ♪+L recording saved in `~/Videos/PSPExp` with nothing on a computer, the beeps heard; ffmpeg encoding at 36–48 % of one of its four cores, the system 77–87 % idle, 41 °C. PSPExp, started after, reached the console as before.

**Inside PSPExp (2026-10-24).** One feature for both worlds: PSPExp's own games record the same way, with the same receiver, clips and ♪ buttons. The board has a game's picture only in part (the PSP draws it: the 3D games' GE lists, the textures it keeps), so the PSP captures what it shows, as the plugin does:
- **The PSP** (`psp/runtime/pspexp_capture_psp.c`): at a frame's end the GE copies it into one of two buffers in RAM (`sceGuCopyImage`, 480x272 RGB 5650, before the volume bar and the dot), at most 30 a second and only when a buffer is free; the audio thread puts each block it is about to play into a ring (whole packets, the PSP's time on them). A thread below the main loop's priority sends the ring's packets, then a frame, in 32 KB CAPTURE_DATA messages (§9); a lock keeps each message whole against the main loop's own writes, which go in between. The packets are the plugin's ("PXCF", "PXCA", "PXCE"), so the receiver's clips, recordings and rolling buffer work unchanged. The buffers (522 KB) are taken from the runtime's heap the first time it is asked.
- **The board:** whichever program holds the link (the console, or a game through the SDL layer) asks for it once the receiver's socket (`/tmp/pspexp-plugin-host-<uid>.sock`) answers, at the connection and every 2 s after (the console starts the receiver only once the PSP is there), and passes the bytes on as they come; when the receiver's socket is full they are dropped, the receiver finding the next packet; when it goes, the PSP is told to stop. `PSPEXP_CAPTURE=0` keeps a program from asking.
- **The receiver** listens on that socket beside the USB cable; a session over the socket waits 8 s for the next holder (the console handing the PSP to a game and back), so its rolling buffer goes on across them. PSPExp draws only when something changes (the launcher: 0.2 frames a second), so the last frame is held up to the sound's time (the audio thread plays always, silence included), half a second behind for a frame on its way.
- **The note button** (`runtime/pspexp_note.c`, a plain function tested on the host: `tests/test_note.c`), while the receiver is there: ♪+R a clip, ♪+L a recording, sent as "PXCE" events in the stream. While ♪ is down the board sees none of the game's buttons (nor those held with it, until let go); ♪ alone reaches the board when let go, held 120 ms (the board keeps only the latest reading: a press of one reading could be missed), so the console's music toggle stays. The system's mute, which ♪ toggles, is put back for a pair as the plugin does, through `pspexpUsbMute` in PSPExp's kernel module (`sceImposeGetParam`/`SetParam` through PRO's `sctrlHENFindFunction`, imported weakly and called only once linked: its stub a jump, where PPSSPP leaves `jr ra`). A recording is forgotten 8 s after the receiver went (it waits that long for the next program holding the PSP).
- **The dot**, as the plugin's (green 1.2 s for a clip, red while recording): drawn by the GE into each frame after the capture's copy, and, as PSPExp may present nothing for seconds, into the frame on screen when it changes between frames; the GE keeps the 16x16 pixels under it in RAM and puts them back when it goes, or before that buffer is drawn into again (the processor never writes VRAM: in PPSSPP its writes would not reach the frames, which live in its GPU). Not over the status page (32-bit, both buffers).
- **The last 30 seconds, a setting (2026-10-24, evening).** Keeping them means recording all the time: the PSP sends 7.8 MB/s more (the GE copying each frame), and the board encodes all the while (36–48 % of a Pi 400 core), for a feature nobody may use. So it is a setting, off at first (Settings > Recording > Clips, beside Videos, which opens Files on them): the console runs the receiver with `-b 0`, recordings only, or `-b 30`. The receiver says which to each program connecting to its socket (a 32-byte hello, "PXCH" and its mode), which asks the PSP for that (CAPTURE_SET 2 or 1), and to the plugin with a command (CMD_KEEP, 14). Recordings only: the PSP reads ♪ as before, sends the events always, and the frames and the sound only between a ♪+L start and its stop (♪+R a pair doing nothing, no dot); the PSPExp runtime sends the frame on screen at the start (the launcher may draw nothing for seconds: the first try saved nothing), the receiver starts pieces of their own at the press and holds the last frame to the stop. The plugin likewise copies nothing and sends no sound outside a recording (its memory taken as before). Tried in PPSSPP with the self-pressing build: recordings only, no clip for ♪+R, a 10.0 s recording for 10 s pressed apart (the launcher at 1:1), 431 sound blocks sent in a minute against 2,217 keeping the last seconds, which still makes its clip and recording.
- **The board gone mid-game (2026-10-25, the GameShell).** In Burnout Legends from the XMB the red dot went out 30 s into a recording, without ♪+L, and ♪+L then did nothing until the game ended: the plugin's sends stopped going through ("receiver not reading"), the board kept 10 s, and nothing brought the link back, the plugin waiting for a receiver to sync and the board, its USB host having let go of the PSP, never looking again (not seen again in a minute's recording after; the GameShell's `usbfs: usb_submit_urb returned -121` lines are harmless: its musb host splits each 64 KB read into 16 KB URBs, and a transfer ending short refuses the rest, which the Pi's xHCI never splits). Now a receiver gone for 5 s, after one was there in this game, stops the plugin's USB, and `home_check` starts it again half a second later, as after the HOME menu, which the board sees as the PSP plugged in anew; then 15, 30, 60 s apart while it stays away (`link_check`). Tried on the PSP-1000 with the GameShell letting go of the PSP mid-recording on purpose (`echo 0 > /sys/bus/usb/devices/3-1/authorized`): "receiver not reading", "receiver gone", the USB stopped 5 s later (11 ms) and started again, every step 0, the board's receiver connected again 6 s after the cut and ♪+L recording at once; the 10 s before were saved. The GameShell's USB (sunxi musb, the processor moving every packet) also takes ≈40 ms for a 557 KB frame (the Pi: 23 ms), so its recordings get ≈17 frames a second in Bomberman (the game at 29.9), and 3–9 in Burnout's races, where the game locks the volatile memory and the frame goes in 32 KB bands (60–137 ms a frame; the Pi 30 ms); its encoder is not the limit (300 frames of 480x272 in 4.5 s at `ultrafast`).
- **A recording stopped by itself (2026-10-25, the Pi 400).** In OpenTyrian, recording with ♪+L, the PSP-1000's runtime logged "note+L: the recording stops" 32.8 s in, with neither ♪ nor L touched (L does nothing in OpenTyrian): ♪ and L read pressed together, in one reading. A person presses ♪ first, then R or L (the manual says so), so now a pair counts only with ♪ already held at the reading before; ♪ with R or L in one reading does nothing (no event, no tap for the board, the mute kept) and is logged with the buttons read, in PSPExp's runtime (`runtime/pspexp_note.c`, tested in `tests/test_note.c`) and in the plugin alike. Every pair now logs its buttons too. The next run, 47 s, ended only by its own ♪+L (`00800100`: ♪ and L) with no such reading; where the reading came from is not known yet (the log will show it). Burnout Legends' dot that went out on the GameShell, 32.8 s in as well, may have been the same.
- **Tried in PPSSPP** (the runtime over TCP, the receiver on the Mac): an SDL game at its 19 fps there, 30 s clips with 900 frames and their sound; the launcher, 0.2 frames a second, a 30 s clip of 900 frames and 30 s of sound, the picture right at 1:1. With a test build pressing ♪ itself (PPSSPP has no ♪ to press): ♪+R a clip, ♪+L a recording of 11 s for 10 s pressed apart, both saved; the red dot on the launcher's screenshot while recording, its corner back pixel for pixel after; no red in either video's corner.

**Next:**
- inside PSPExp, on the PSP (2026-10-24 evening, the Pi 400): the launcher's clip and recording, Daytona USA's and Crazy Taxi's clips saved with their picture (a film of them shown black in Media was Media's own bug: a film paused over a game's hand-over); still to try: the mute put back, ♪ alone the music, the setting's two modes, the plugin's recordings only; the cost while recording: the GE's copy, the link's 7.8 MB/s more beside a game's own (the board-GPU games send 137–230 KB a frame), the board's encoding. In the launcher the dot sits on the footer's last hint ("Search");
- the other games, with clips (Castlevania holds 30 of 30 and now exits cleanly);
- **memory, 2026-10-24 (afternoon):** the plugin's buffers moved out of the kernel's memory (≈450 KB free while a game runs, also what the system's screens, modules and a game's exit need: taking ≈520 KB of it left Bomberman's exit waiting forever) into the game's own spare memory, taken only with 4 MB left free and not in the game's first 20 s, and given back whenever the game or its system needs memory (a failed memory request, module load, pool, thread or message pipe is retried for the game once the plugin has given its buffers back; system screens). Jeanne d'Arc and Crisis Core froze or crashed when the buffers were taken 2 s in: once set up they leave 497 KB and 15–40 KB of their memory free. With that, six games in a row played and exited cleanly: Bomberman, Gran Turismo, Lunar recorded; Jeanne d'Arc, Crisis Core, Phantasy Star Portable 2 (1.4 MB free) recorded since through the fallback in the kernel's memory (above);
- more games: Ace Combat X holds 30 of 30 (16-bit, 8.4 MB/s); **Gran Turismo stays on black screens with the plugin on** (normal without it): not the frames (still black at one frame a second, the unchanged ones skipped), but twice it went on the moment the plugin's USB link was stopped (HOME, the restart command), and with the link not up at all it reached its first logo, slowly: the link itself, perhaps the Sony vendor and product ID it borrows from PSPExp's runtime (psp-cast, with its own, records GT); also, after HOME the link did not start again (80243001) when its steps were one call: now one by one. Bomberman, Crisis Core, Jeanne d'Arc, Lunar, Phantasy Star Portable 2 recorded since;
- a 32-bit frame in the fallback: Crisis Core's needs 17 bands and gets 9, its volatile memory locked by the game (the rest copied as the sending frees them, ≈10 ms after the swap; kept so, 2026-10-24, rather than a 16-bit conversion costing processor time); a game that leaves the volatile memory free, to see it taken and given back;
- the `restart` command from a game holding the fallback's bands (switched the PSP off once);
- 60 fps games (Gran Turismo) make 30 fps clips: the receiver's clip rate to follow the game's, the plugin's cap of 30 frames a second with it;
- **paced by the game (2026-10-25):** with Clips on, Gran Turismo fell to exactly 20 fps for 25 s in a heavier stretch of a race that held 60 with Clips off (from the stick and from the board's disc alike: §12c); the frames' copies cost it frames there. The plugin now copies fewer frames while the game is below 85% of its peak (30 a second stepping to 15, 10, 5, one step each 3 s; back a step at a time once at its peak, after a hold of 10–60 s; the peak kept while slowed, else the slowed rate passed for the peak and the 20 came back). The same race: 15 a second gave ≈36 there, the steps ≈51 (5 a second), a mean of 55.1 against 47.3, nothing under 43 (`pace_check`). Not a flat 60: a copy costs that stretch frames even at 5 a second; pausing the copies there (a frozen clip for those seconds) was not wanted. Then Burnout Legends from the XMB: its menus at 60, its races at 30, the race taken for a slowdown and copied at 5 a second (a choppy clip for up to a minute). Now the first step down is kept only if it helps (the game's rate over the next 3 s up by over 2 fps; if not, that rate is its own pace and the copies go back to 30), and slowed copies are probed back up a step when a hold ends (10 s, doubled to 60 s each time a probe costs the game over 3 fps). Tried on the PSP the same day: Gran Turismo as meant (its heavy stretch ≈50 at 5 a second, two probes up each costing it 8 fps and undone); Burnout worse than before: its race start went back to 30 at once (its own pace), but there a copy costs 23–25 ms (it leaves ≈440 KB: the kernel's bands), each probe cost it 4–12 fps and one set its "own pace" to 22, the race then at 20–22 with 15 a second (the earlier steps had kept 28–30 at 5 a second). Then, the same evening, the pace made to measure instead of guess (`psp/plugin/pace.c`, its own file so the host can test it: `tests/test_pace.c`): when the game's rate moves 10% from its rate without copies (either way: a new scene), the copies stop for one second and its frames are counted; faster without them, the copies go down a step (a step a second while it stays down); not faster, that is its own rate now; a rise while slowed is a new scene the copies do not hold back (that rate taken, a step faster tried at once); slowed copies are tried a step faster when a hold ends (10 s, doubled up to 60 s when that costs the game over 5%), and measured again at 5 a second only each time later. The test models the PSP's measurements (a scene's own rate, and the frames a copy a second costs it: Gran Turismo's heavy stretch ≈2, Burnout's races ≈0.6, most scenes 0; ±1 fps of noise): Gran Turismo's stretch ≈50 and 30 copies a second around it, Burnout's two races ≈27 (all that 5 a second allow there) with its menus back at 30, Bomberman and Ace Combat X untouched; 1–11 seconds without copies in a session. The model first found two holes the PSP would have: a load measured, the race after it taken at its rate while copying (18.6 for 30), and a menu after a race left at 5 a second. On the PSP the same evening: Burnout's race at 28–30 with 5–10 copies a second (two tries at 15 undone), its menus at 30; Gran Turismo's heavy stretch left at 43 instead of 50: the second measured fell on a hitch, 18 fps without copies against 53 with them, taken for its own rate. Now a measured second slower than copying is set aside (something else in it), and a rise while slowed is measured (whatever the hold) rather than taken as the game's rate; the test replays the PSP's sequence. Confirmed on the PSP (Gran Turismo from the shelf): a disturbed second (38 without copies, 51 with) set aside, the heavy stretch at 50.5–52 with 5 copies a second, the rest of the race 56–60 with the copies stepped back to 30;
- shipping the board's side: done in the repository (2026-10-24): the console runs the receiver, pspexp-core and the GameShell's build carry it, with its own USB rule (the Pi's build, `scripts/pi/install.sh build`, the release's route, left the receiver's target out until the evening of 2026-10-24, and the package took it silently when missing: a test upgrade removed the Pi's receiver; now built, and the package is not made without it); the GameShell to be tried; the clips play on the PSP in PSPExp's own Files and Media (tried on the Pi 400);
- a steady dot: drawn the moment a frame shows (the copy then cleaned of it with the kept pixels) and the frame looked for every 2 ms, not once a refresh (it flickers slightly now: drawn after the copy, and a swap in mid-refresh found only at the next);
- the receiver as the board's console in a capture mode (clips kept by PSPExp), plus the Mac/Linux tool for people without a board;
- the shutdown's last log line ("stopped") never appears though the exit works: find which step does not return in time;
- more games (one game tried so far); 60 fps games (a 32-bit copy at the link's pace, 22 ms, is longer than a 60 fps frame: 16-bit there, or every other frame);
- shipping: the plugin in PSPExp's zip, with THIRD_PARTY.txt and licenses/ beside it as the PSP's zip has (it carries the USB driver's PSPLINK parts and PSPSDK), a page in the manual, a section on the site;
- later: live streaming.

## 12c. The PSP games shelf: native PSP games from the board (started 2026-10-25)

The PSP's own games in PSPExp's launcher, kept on the board: a shelf of them with their covers, a
game started from it running natively on the PSP (at its own speed, recorded with PSPExp Capture
and ♪ as any native game), and, later, its disc read from the board over the cable (a virtual
disc), so the memory stick keeps only the saves. Proposed 2026-10-23 with capture as stage 1;
the earlier USB ISO loaders (Custom Firmware Extender's usbhost redirection, HostCore, 2008–2009:
Windows-only, no source found, called buggy in their own changelogs) had no library and no launcher.

**The board's library (2026-10-25).** `host/libpspexp/pspexp_disc.c` reads a PSP game from an
ISO, a CSO (version 1: deflate blocks and their index) or a zip holding an ISO (front to back,
inflating as it goes; a sector behind starts it again): sectors of 2048 bytes, a file found
through ISO 9660, PARAM.SFO's TITLE and DISC_ID, and the game's own icon, PSP_GAME/ICON0.PNG
(144x80), for a cover where there is no box art. zlib is optional (without it: ISOs only; the
GameShell's cross-build has none). `tests/test_disc.c` makes a small disc and reads it as all
four. On the Pi 400, over the collection on its disk (104 zips): 99 PSP games read, title, disc ID
and icon (the other 5 are PS1 games, BIN/CUE or a converted EBOOT, rightly refused); the
metadata sits 40–50 MB in (after the firmware update folder), at most 335 MB: 1–2 s a game, up to
45 s for three, 5 min 48 s for all (once, then kept).

**Booting a game, from PRO's source** (procfw at PRO-C's commit, ARK-4, uOFW; read, not yet tried):
- the XMB's own way (`Vshctrl/virtual_pbp.c`, `vpbp_loadexec`): `sctrlSESetUmdFile(iso)`,
  `sctrlSESetBootConfFileIndex(3)` (Inferno; 1 March33, 2 NP9660), `sctrlSESetDiscType(0x10)`,
  then `sctrlKernelLoadExecVSHWithApitype(0x120, "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN")` with the
  key "umdemu" (0x123 only on the PSP Go). All in SystemCtrlForUser too, so PSPExp's runtime or its
  kernel module can call them. The capture plugin's developer `restart` does the same but leaves
  the disc type out (it inherits it; a start from PSPExp must set it). The ISO mode stays for the
  next start until the XMB clears it: going back to PSPExp sets mode 0 and an empty ISO first;
- back to PSPExp when the game exits: as ARK-4 (`exitgame.c`): the plugin's catch of
  `sceKernelExitGame` (it has one) clears the ISO and the mode and starts
  `ms0:/PSP/GAME/PSPExp/EBOOT.PBP` (apitype 0x141, key "game") from a thread of its own,
  instead of the XMB;
- Inferno, PRO's ISO driver, opens the ISO in a thread that retries for ever (after the memory
  stick is ready), with `sceIoOpen` on whatever path was set: no check of its device. The
  plugins of game.txt load before the game's EBOOT (in SystemControl's start-module catch, when
  sceMediaSync loads), after Inferno has started. So a plugin can add a device of its own
  (`sceIoAddDrv`, "pxd0:") and Inferno reads the game from it once it is there: the virtual
  disc. Reads come synchronously, whole sectors, any length, no cache on a PSP-1000 (every read
  of the game's reaches the device). NP9660 opens in its own start without retrying: unusable.

**(A) in code, not yet tried on the PSP (2026-10-25).** SYSTEM 0x0010 BOOT_DISC
(`PspExpBootDisc`: the path; `PSPEXP_FEATURE_BOOT_DISC`, offered only when the kernel module
finds PRO's functions: not in PPSSPP, where the runtime starts as before and refuses it). The
runtime leaves as for RESTART (its USB stopped), writes `launch.txt` beside its EBOOT (the ISO,
then the EBOOT), and `pspexpUsbBootDisc` starts the game: `sctrlSESetUmdFile`,
`sctrlSESetBootConfFileIndex(3)`, `sctrlSESetDiscType(0x10)`, apitype 0x120 (found through
`sctrlHENFindFunction` in "SystemControl"; the NIDs are the names' SHA-1, as PRO-C exports them,
checked against the five the plugin already calls). The plugin reads the note once 6 s into the
game, removes it, and keeps PSPExp's EBOOT when the note names this game's ISO; its catch of the
game's exit then clears the ISO mode and starts that EBOOT (0x141, key "game", a thread at
priority 1), else the XMB as before. On the board: `pspexp_boot_disc`, the control port's
`psp-boot PATH`; the shelf still says "Not yet" (its games are on the board: (B)).

**(B), the board's side (2026-10-25).** The receiver serves the disc: a "PXDQ" packet from the
plugin (u32 seq, u64 offset, u32 length; 0: the size only) is answered with command 15,
CMD_DISC (seq, status, the disc's size, then at most 64 KB), from the image the console named
(an ISO or CSO through `pspexp_disc.c`; a zipped game unpacked first by the console, as the
shelf starts it). `tests/run_disc_serve.sh` drives it through a stand-in plugin over TCP
(`-T PORT`): sizes and reads aligned, not, across sectors, at and past the end, of an ISO and a
CSO, byte for byte (21 checks). The console's start, with its checks skipped (PPSSPP has no
PRO), unpacked Chameleon's zip into the cache identical to the zip's ISO and named it.

**Reviewed against PRO-C's and ARK-4's source (2026-10-25)** (NIDs, finding SystemControl's own
exports, the boot's order and arguments as vpbp_loadexec, a syscall's user thread allowed by
PRO-C's loadexec patches, `SetUmdFile("")` copied by PRO, 0x141 and "game" as the XMB's homebrew
start); fixed from it: the PSP takes only an ISO or CSO it has (`sceIoGetstat`, in the runtime
and again in the kernel module) or "pxd0:", as Inferno given anything else retries its open for
ever and the PSP hangs at the start; `launch.txt` at one fixed path (`ms0:/PSP/GAME/PSPExp/`),
removed as PSPExp starts and when a start fails (with the ISO mode cleared); the plugin keeps
an EBOOT to go back to only when it is there, and resets the display's hold mode first, as
ARK-4. Open: the plugin now opens a file 6 s into every game (the first test: a game from the
XMB, exited), multi-EBOOT games lose the note, PRO's EBOOT.OLD (Prometheus) not used.

**(B), the PSP's side, in code, not yet tried (2026-10-25).** The capture plugin, when the
game's ISO is "pxd0:...", adds that device at module_start (`sceIoAddDrv`, "pxd") and starts its
USB at once instead of 2 s in. Its open asks the board for the size (failing until the link is
up: Inferno tries again every 20 ms); each read becomes PXDQ asks of at most 64 KB, one at a
time, the command thread reading CMD_DISC's bytes straight into the reader's buffer (answers to
an ask given up on, after 3 s, dropped by their seq); while the link is down (the HOME menu
still stops the USB, for the exit's sake) a read waits and asks again: the game pauses, it is
not told of an error. A 64-bit seek (SET, CUR, END), a getstat with the size; writes refused.

**The first virtual disc, on the PSP (2026-10-25 morning): it hung.** Sega Mega Drive Collection
chosen on the shelf: unpacked, started (PSPExp left), the plugin's link connected to the
receiver twice, but no read ever came (the receiver never opened the disc) and the PSP stayed
black until its power switch. Why is not known: the plugin's log goes out with frames or sound,
and its loop skipped its 5 s report until the game shows a picture, so nothing of it came; and a
board's quiet receiver kept only the note button's lines. Now: in a virtual-disc start the
plugin logs the device's adding, Inferno's first open and the open's success, and every 5 s
until then how many opens and asks failed (and the last ask's result), the USB and the
receiver, sent without a picture; after 30 s without the disc it gives up and starts PSPExp
again (no PSP left stuck on black); the quiet receiver keeps those lines. Also: the games suite
(tests/psp) left the PSP shelf alone after it started a PSP game's unpacking while the next
game ran.

**(A) and (B) on the PSP (2026-10-25).** (A) works: Bomberman on the stick, started from the
board (`psp-boot`), played, HOME > Exit back to PSPExp (set up after 8 s, PSPExp connected again
12 s after the exit). (B), Bust-A-Move Ghost from the shelf: the disc opened (the device added,
Inferno's open succeeding once the link came up, 274 tries) and the game ran from the board's
disc, then the PSP switched itself off about 35 s in (Clips on: frames going up the cable while
the disc came down it; the plugin's lines before it lost: a board's quiet receiver now keeps all
of them, `~/.cache/pspexp/psp-plugin.log`). A second start (Clips off) found the receiver still
waiting its 8 s, and more, for PSPExp to come back on its socket while the plugin's link was up;
the plugin's 30 s safety net then fired just as the disc opened, PSPExp's restart in the middle
of the game's start left the PSP black, then off. Now the receiver takes the plugin's link at
once during that wait, and the safety net waits 90 s and only while the board is not there.
A third start (Clips off) died the same way, 0.4 s after the board's first disc answer, and
showed why: the plugin's 6–20 s before taking its memory counted from its load, but a game on the
virtual disc starts only once its disc opens (8 s, then 28 s later); past 20 s the memory was
taken the moment the link came up, before the game had even loaded (as Jeanne d'Arc and Crisis
Core crashed when taken 2 s in), and in the 8 s start the game was judged set up 5 s after its
disc opened, still loading (the switch-off at 35 s). Now, on the virtual disc, those seconds count
from the disc's opening, and nothing is judged set up before it.

**(B) works (2026-10-25 midday): the first sector came too late.** After the fixes above, every
start from the board still went black and switched the PSP off about 0.5 s after Inferno's first
reads, with no read of the game's after them. Ruled out one PSP start each: the USB and the board
(a developer build serving `pxd0:` from an ISO on the stick, `make LOCAL=1`, no USB, no log: the
same), Clips, the stick log, the open's timing, the caller's stack (infernoMount's is 0x800: every
driver call now runs on 16 KB of its own, `sceKernelExtendKernelStack`, the work on a disc thread),
the unpacking (the stick's own Bomberman through the device: the same). Bust-A-Move Ghost, the game
of the first tries, switches the PSP off at its save screen from the stick and the XMB too, plugin
off: a bad test game (and the 35 s of the first start). What showed it: a trace sending each step
over the cable before going on (`make TRACE=1`; the receiver keeps every line), then the system's
restart caught (`make CRASH=1`: `sceKernelExitVSHKernel` and `ExitVSHVSH` jumped to a screen of
the plugin's that shows the caller and the error, the PSP held there; an unhandled exception would
show there too, none came). The screen: `sceInit +0x3d8` (uOFW's ExitInit), error 0x80010013,
"device not found", from the ISO file system (isofs: `g_isofsMounted == 0`); asked as isofs asks,
Inferno had the sector count right but its buffer for the first sector (the PVD, devctl
0x01E28035) held zeros, and the order of the plugin's lines showed why: the system activated the
disc (drive status 0x32: `disc0:` assigned, isofs mounted, reading that buffer) before Inferno's
mount thread had read the sector into it. `inferno_mount` sleeps 20 ms after `iso_open` succeeds,
then reads it; `sceUmdActivate` waits only for the open. From the stick the open succeeds at the
very start, before the system looks; through the plugin's device it can only succeed once the
plugin has loaded (and the board answered), while the system polls every 10 ms. The plugin now
fetches the first sector while answering the open and writes it into Inferno's buffer (its address
from the same devctl) before the open succeeds; Inferno's own read of it later is answered from
the kept copy at once. Bomberman from the shelf then loaded and played from the board's disc
(set up after 6.1 s, as from the stick; ≈30 fps, Clips recording alongside), and HOME > Exit
brought PSPExp back (connected again 10 s after the game ended). Also kept from the
hunt: the driver's other entries filled ("not supported"), reads handed to the disc thread,
CPU-copied bytes written back from the cache, the drive status logged as it changes.

**From the board against from the stick (2026-10-25, Pi 400, PSP-1000).** The same four games
started from the XMB (their ISOs on the stick) and from the shelf (the board's disc), Clips off
(nothing copied: only the disc's way differs), the same route (the menu, a race or level or mission,
about a minute, HOME > Exit), the game's frame rate from the plugin every 5 s:

| Game (its cap) | Set up, stick / board | Mean fps, stick / board | Lowest 5 s, stick / board |
|---|---|---|---|
| Burnout Legends (30) | 8.7 / 6.3 s | 28.5 / 27.2 | 17 / 5 (loading) |
| Gran Turismo (60) | 11.6 / 10.0 s | 59.2 / 58.1 | 50 / 22 (the first 5 s) |
| Ace Combat X (30) | 6.6 / 6.3 s | 29.2 / 29.3 | 24 / 2 (the first 5 s) |
| Bomberman (30) | 6.7 / 6.1 s | 29.4 / 28.1 | 28 / 11 (the first 5 s) |

In play every game holds its cap from the board as from the stick (Gran Turismo 60, the others
30, the same dips in the same places); games set up as fast or faster (the board answers sooner
than the stick reads); the cost is in loading: the 5 s around a load runs lower from the board,
a load being many reads of one USB round trip each, one at a time. Earlier the same day, with
Clips on: Despicable Me (651 MB) 25–29 fps, PES 2014 (1.1 GB) menus 55–60, matches a steady 30,
recorded at 30 alongside. Not measured: loading times themselves (a stopwatch), a read-ahead.
With Clips on, Gran Turismo again from the stick and from the board, a clip saved with ♪+R at
the race's end each time (both saved, 4 s later): the two runs match sample for sample (mean 47.3 /
45.9, set up 11.7 / 9.8 s), the disc coming down the cable while the frames go up adding nothing.
Both, though, fall to exactly 20 fps for 25 s in a heavier part of the race (the clip shows the
car on the track) that held 60 with Clips off: a cost of Clips, not of the disc (each frame's
copy, 3–5 ms of DMA 20–28 times a second, likely pushing the game's frames past 16 ms there, to
the next vsync step). Next for Clips: fewer frames copied while the game's own rate falls.

**Plan.** (A) Boot from the shelf, the ISO on the memory stick (the board copying it there),
back to PSPExp after; (B) the virtual disc: the ISO's path `pxd0:/<id>.iso`; the plugin, seeing
it, starts its USB link at once (not 6–20 s later) and answers Inferno's reads with the board's
sectors (the receiver, already on the other end of the plugin's link, serving from the ISO, a
CSO, or a zip unpacked once into a cache). Its risks: the USB that early in a game's start, read
latency (one round trip a read, no cache), games using the USB port themselves, the cable pulled
mid-game (the disc gone). The smallest test on the PSP (about 6 starts): (1) a stick ISO booted
from a test command, its mode and disc type logged; (2) its exit back to PSPExp, no disc left
bound; (3) a plugin device serving the ISO from the stick (the path and the timing, no USB);
(4) the same with the USB up early, only connecting; (5) reads over the USB; (6) a long game
and HOME > Exit.

**The shelf (2026-10-25).** `psp.system` (pspexp-core): the games in `~/roms/psp` on a
"PSP" shelf, covers from their own icons (read in the background for the game selected; box
art only without one), "Runs on the PSP"; choosing one hands it to the console
(`launcher_take_native`), for now a "Not yet" notice (Launcher_and_Boards §6.2). Seen in
PPSSPP at 1:1. Open for the UX review: PSP games under Emulators, which they are not (done 2026-10-25, before 1.3: a **PSP** section of its own on the rail, first after Favourites and where the launcher opens, its icon the PSP's console picture its Emulators tile had; and the PSPExp plugin's switch with PSP clock in a Settings group **This PSP**, beside This board, not under Recording: the plugin serves the PSP section as much as recording; both settled so that 1.3 ships them where they stay); PS1
games in the same folder listed (their covers generated).

**Next:**
- **a clean round (2026-10-25 evening):** the Pi's 1.2.0 card image burned fresh and PSPExp removed from the PSP's stick, then upgraded as a user would (the new pspexp-core and pspexp-runtime, the emulators from Packages, Update the PSP, the plugin installed by hand from the PSP's zip as the manual's plugin page says): the games suite 32/32 (Clips off: Crazy Taxi 52 fps, Daytona USA 60), Bomberman from the shelf (unpacked, Inferno's buffer filled, set up 6.1 s, 28–30 fps, a ♪+L recording of 31 s, back to PSPExp) and Burnout Legends from the XMB (set up 8.6 s, a 56 s recording, 27.5 fps paced). Found: Update the PSP does not put the plugin on the stick (an upgrade needs the zip and copying by hand, and a Mac's Finder copy replaced the PSPExp folder whole, its slot b with it): the runtime package to carry the plugin and the update to install it, and a setting to switch it on (writing game.txt) — done the same evening: `pspexp-runtime` carries the plugin, Update the PSP puts it on the stick when the stick's differs (alone, without the slots or a restart, when only the plugin is new), Settings > Recording > PSPExp plugin switches its `game.txt` line (Launcher_and_Boards §3.8, Package_Manager §6.1); tried on the PSP-1000 through the Pi: off and on with `game.txt` byte for byte as before, the plugin missing put back by the switch, a differing one replaced by the update alone and with the slots; and the restart after an update, which had never worked on the PSP, fixed (§9's RESTART); recording inside PSPExp is not paced (Crazy Taxi 8 fps with Clips on against 52 off): pace.c in the runtime's capture too. Measured again the same evening, a minute of driving each, played on the PSP (the suite's 20 s never leave Crazy Taxi's title screen, 4 KB a frame): 35.4 fps on average with Clips on (28–42), 38.7 off (30–43), the game sending ≈111 KB a frame either way; the 8 against 52 was a loading moment against the title screen. A cost of 3–5 fps (8–14%), so not paced for now: pacing could win back at most that, the clips choppier for it. Then the GameShell's 1.2.0 image, upgraded the same way: the suite 27 of 29 (the two known: the stick's dates, the board in UTC; a 720p film at 12.8 pictures a second), OpenTyrian and PicoDrive's Castle of Illusion (59–60 fps) from a first start, Bomberman from the shelf (set up 6.3 s, a 27 s recording), Burnout from the XMB (the dot lost once: §12b, "The board gone mid-game"). Found too: the cartridge systems take no `.zip` or `.7z` (most ROM sets come so; the launcher lists only `md`, `gen`, `smd`, `bin` and the like): unpack them on the board, or have pspexp-retro open them; the console's test port splits `fs` paths at spaces (`sscanf %s`), so files with spaces in their names cannot be copied through it;
- (B): a long session, CSO, the cable pulled mid-game; loading times measured, and a read-ahead
  (the next 64 KB asked while the game uses the last) if they lag the stick's; the GameShell; the shelf's own games on the stick (A)
  listed too;
- PS1 games (the PSP's own player, POPS: an EBOOT.PBP made from BIN/CUE), later.

## 13. Host Independence

No protocol message may depend on the Onion Omega2+, Raspberry Pi, GameShell, or macOS.

Supported host classes therefore include:

```text
Mac / PC ---------+
Raspberry Pi -----+
GameShell --------+---- PSPExp protocol ---- PSP
Omega2+ ----------+
future host ------+
```

Recommended roles:

| Host | Role |
|---|---|
| MacBook / desktop | Primary development, debugging, tracing |
| Raspberry Pi 4 / 5 | Powerful Linux reference host |
| GameShell board | Portable mid-range Linux host |
| Omega2+ | Embedded/minimum-spec host and efficiency test |

The same PSP binary operates unchanged when moved between compatible hosts.

---

## 14. Transport Abstraction

USB 2.0 is the first physical transport, but it must not define the protocol architecture.

A transport interface supports implementations such as:

```text
USB        host <-> PSP (PSP is the USB device; host uses libusb)
TCP        host <-> PSP over the PSP's Wi-Fi, or host <-> PPSSPP
loopback   host-internal, for the SDL and Null executors
```

This permits configurations such as:

```text
Mac application -> USB -> PSP                          (primary)
Mac application -> TCP/Wi-Fi -> PSP                    (untethered; higher latency)
Mac application -> TCP -> PPSSPP                       (PSP executor without hardware)
Mac application -> TCP -> Linux relay -> USB -> PSP    (development)
```

The Wi-Fi transport is deliberately included even though its latency (tens of milliseconds, with jitter) is worse than USB: it exercises the pacing and backpressure design of §10 under adverse conditions, and it is the only transport that reaches PPSSPP.

Existing PSP USB projects such as PSPLink/USBHostFS are valuable implementation references for transport and host/device behaviour, but PSPExp's application protocol remains independent from them.

---

## 15. Proposed Software Structure

```text
PSPExp/
|
+-- protocol/                 shared by host and PSP builds: plain C99, no host dependencies
|   +-- pspexp_protocol.h
|   +-- pspexp_opcodes.h
|   +-- pspexp_batch.h        batch records, frame table, and the shared expansion routine
|   +-- serialization/
|
+-- host/
|   +-- libpspexp/            application API, frame builder, text expansion, resource tracking, pacing
|   +-- executor-null/
|   +-- executor-sdl/
|   +-- transport-usb/
|   +-- transport-tcp/
|
+-- psp/
|   +-- runtime/
|   +-- executor-ge/
|   +-- resources/
|   +-- audio/
|   +-- input/
|   +-- transport-usb/
|   +-- transport-tcp/
|
+-- compatibility/
|   +-- sdl-pspexp/
|
+-- examples/
|   +-- hello/
|   +-- sprite-demo/
|   +-- batching/
|   +-- audio/
|   +-- benchmark/
|
+-- tools/
|   +-- protocol-dump/
|   +-- metrics/
|   +-- atlas/                asset preparation: atlasing, palette reduction, swizzling
|
+-- docs/
```

`protocol/` is compiled by both the host toolchain and PSPSDK's GCC; it must remain plain C99 with no platform headers. Names are provisional; the architectural boundaries are more important than final naming.

---

## 16. Initial Proof of Concept

The first hardware demonstration is deliberately small.

### Sprite demo

1. Establish bidirectional USB communication.
2. Negotiate protocol/capabilities.
3. Upload one texture.
4. Host creates an application object and owns its coordinates.
5. PSP sends buttons/analog state to the host.
6. Host updates the object's coordinates.
7. Host sends `BEGIN_FRAME`, one `DRAW_SPRITE_BATCH`, and `PRESENT`.
8. PSP executes the commands using the GE and returns `PRESENT_DONE`.
9. Host paces on `PRESENT_DONE`; repeat interactively at 60 Hz.
10. Pull the cable and reconnect; the sprite resumes without restarting either side.

Important property:

> The moving object's state exists only on the host. The PSP is executing presentation and input duties.

The same demo must run through:

```text
sprite-demo -> Null executor
sprite-demo -> SDL executor
sprite-demo -> PSP executor (PPSSPP, TCP)
sprite-demo -> PSP executor (hardware, USB)
```

The SDL and PSP outputs should behave equivalently.

---

## 17. Benchmark Plan

After the sprite proof of concept, benchmark rather than speculate.

Suggested workloads:

```text
100 sprites
500 sprites
1,000 sprites
2,000 sprites
5,000 sprites
```

each run with (a) one texture, (b) many textures (state-change heavy), and (c) a concurrent 256 KB/s resource upload stream.

Record at least:

- application/frame generation time;
- command count and batch count;
- serialised command bytes/frame;
- resource bytes/frame;
- host serialisation time;
- transport transfer time;
- PSP parsing time;
- GE/display-list construction time;
- rendering/present time;
- achieved FPS and present cadence stability;
- input round-trip latency (from `PspExpInput.timestamp_us` to the `PRESENT_DONE` of the frame that consumed it);
- queue depth and dropped frames;
- VRAM/RAM tier occupancy.

Example diagnostics:

```text
FRAME #18421
Batches:            14
Sprites:           241
Tiles:             126
Command bytes:   8,442
Resource bytes:      0
Host build:          ...
Serialize:           ...
Transport:           ...
PSP parse:           ...
GE build+render:     ...
Input->photon:       ...
Queue depth / drops: 1 / 0
```

Actual performance targets are established from measurements on physical PSP hardware; the figures in §3.3 are estimates to be replaced.

---

## 18. Milestones

### v0.1 – Remote GE proof

*Status 2026-09-22: every item below is implemented and has run on a PSP-1000 over USB (`docs/v01-usb-bringup.md`); the sprite benchmark exists as the demo's 500 / 2,000 / 5,000-sprite field sizes rather than a separate tool, and benchmark workload (c) of §17 is still to be run.*

- protocol framing and little-endian packed encoding;
- USB transport (host and PSP);
- capability handshake and reconnect;
- texture upload with constraint validation;
- compact sprite batch record and per-atlas frame table (§5.4);
- USB throughput measurement (replaces the planning assumption in §3.3);
- frame batching, `PRESENT_DONE`, one-deep queue and pacing;
- PSP input return path with timestamps;
- Null executor;
- SDL executor;
- PSP GE executor;
- sprite benchmark.

### v0.2 – Real engine integration

*Status 2026-09-22: the Stratagus backend exists (`compatibility/stratagus`; removed from PSPExp 2026-10-10: it stays in the WarCraft project, Stratagus's `pspexp` branch, and the Warcraft packages run unmodified Stratagus through the SDL layer). Stratagus is a pure software renderer, so the backend hooks the leaf blits (`CGraphic::DrawSub*`, `DrawFrame*X`, the glyph blit, the `CVideo` primitives, the fog shroud and the minimap blit) and mirrors them into quad batches with per-batch palettes; the software path stays live for a side-by-side desktop view. Warcraft renders through the SDL executor at 60 fps with about 34 batches and 330 sprites per frame in 7.6 KB; the guichan menus are mirrored too, and the game's audio reaches the PSP as the streamed-PCM mode of §8 (SDL_mixer's post-mix output, 176 KB/s, 100 ms jitter buffer). On the PSP-1000 over USB the game runs at 60 fps with the PSP spending 0.5–0.8 ms of CPU and 0.3–0.5 ms of GE per frame, 4.5–9.5 KB per frame, audio streamed with no underruns; the native build spends about 15 ms per frame on the same hardware (`docs/v01-usb-bringup.md`).*

Use Stratagus as the first serious engine experiment, via an engine-specific renderer backend (Stratagus draws into SDL surfaces and is therefore out of scope for `sdl-pspexp`; see §12):

```text
Stratagus on host
       |
   PSPExp renderer backend
       |
    transport
       |
   PSP runtime
       |
      GE
       |
   Warcraft display
```

Stratagus is also a natural fit for the resource model: 8-bit palettised sprites map directly onto CLUT8 atlases, and its tile maps onto `DRAW_TILE_BATCH`.

A native PSP build of Stratagus/Warcraft already runs fluidly on the target PSP-1000 and provides a **control implementation** for rendering behaviour, performance, and native input-to-display latency. The PSPExp implementation can therefore be compared directly against execution of the same engine natively on the same hardware — the same scenes, the same assets, the same input — which makes this milestone unusually valuable: every latency, throughput and visual-fidelity figure from §17 has a native baseline to be judged against.

### v0.3 – Rich presentation

*Status 2026-09-22: cached SFX, the streamed PCM channel with jitter buffer and `AUDIO_STATUS`, host-side text, palettes/CLUT, the per-batch palette selection, host-side eviction (time-based, in the Stratagus backend) and the TCP transport with the PPSSPP configuration were pulled forward for v0.2 and are implemented; the PSP runtime, GE executor and audio thread have been exercised under PPSSPP (audio and CLUT not yet on hardware). Priority-guided placement, static vertex buffers and detailed profiling remain.*

- cached SFX with PSP-side mixing;
- streamed audio with jitter buffer and `AUDIO_STATUS`;
- host-side text expansion over glyph atlases;
- palettes/CLUT and priority-guided two-tier placement (`RESOURCE_HINT`, `RESOURCE_TIER`);
- static vertex buffer resources;
- resource prefetch and eviction policy in libPSPExp;
- TCP transport on the PSP (Wi-Fi) and PPSSPP configuration;
- detailed profiling.

### v0.4 – SDL compatibility

Prototype `sdl-pspexp` as an `SDL_Renderer` driver for suitable SDL applications. *Done 2026-09-22 as an interposition library instead of a render driver (§12); runs on the PSP-1000 over USB at 60 fps (§20a), headless test in `ctest`.*

### Later research

- triangle-based batches for rotation/skew (static meshes and 3D commands: done for OpenGL 1.x, §12a);
- offloading batch expansion or audio mixing to the Media Engine (our own code on it: first a small test on the real PSP, as PPSSPP does not run code there; does it coexist with the firmware's codec, which uses it for MP3 since 2026-10-03?);
- **a video player**: *done 2026-10-12 the simpler way, in Media (docs/Launcher_and_Boards.md §10.4): the board decodes any film with ffmpeg and sends finished 16-bit pictures, 272 KB each, about 8 MB/s at 30 a second (a third of the link); the Pi 400 shows 720p and 1080p at 29–30 on the PSP-1000 with two frames in flight.* Still open: H.264 up to 480x272 decoded on the PSP (`sceMpeg` / `sceVideocodec`, on the Media Engine like MP3), 100–200 KB/s over the link and 24-bit colour, for the weaker boards (the GameShell) and the Omega (no ffmpeg); a film already H.264 at that size would pass through as it is;
- sound to a Bluetooth device paired with the board: done 2026-10-03, with its volume bar (§8, docs/Launcher_and_Boards.md §11); open: headphones tried on the GameShell (PipeWire in its image since 2026-10-03);
- compute/RPC services;
- network/storage services (media servers: done 2026-10-12, UPnP / DLNA in Media, docs/Launcher_and_Boards.md §10.4);
- transparent or semi-transparent engine adapters;
- additional transports;
- additional presentation clients;
- **faster memory-stick copies over the link** (the FILE channel: file manager, `pspexp-ms`, runtime updates). Today 1.7–2.3 MB/s writing and 2.2–4.4 MB/s reading (4 MB, GameShell / Pi 400, 2026-09-29): every 64 KB piece is one request, and the PSP opens, writes and closes the file for each before the next is sent. Room: keep the file open for a copy, several requests in flight, larger pieces. Measure first how much of the time the stick itself is busy (its speed is the hard limit). Low priority: games live on the boards, so stick copies are occasional;
- **the USB link's ceiling**, ≈ 24–26 MB/s either way on every host (Pi 400 USB 2 and USB 3 ports, MacBook), set by the PSP's pace (≈ 3 packets per 125 µs microframe of the 13 USB 2.0 allows). Measured 2026-09-29; the host keeps two transfers queued each way since then (small transfers from the PSP ~25 % faster). Left to try, none needed by today's games (3–13 % of the link): two queued requests each way on the PSP side (a few percent expected, if Sony's driver takes a second request); 256 KB receive requests (tried: the PSP hung at start, likely the USB driver's three buffers in kernel memory; 128 KB, or buffers allocated rather than static, might work); a second bulk endpoint pair, in case the limit is per endpoint;
- **render targets in the SDL layer, fully:** today drawing into a target is recorded and replayed (enough for pixel-art upscaling); programs that compose whole frames or menus in targets draw partly or not at all. Waiting on it, from Debian: LTris, LBreakoutHD, Project: Starfighter, C-Dogs SDL (2026-09-30). One way: textures drawn into become PSP-side render targets (the GE can draw into a texture in VRAM), so the composition happens on the PSP;
- **two PSPs on one board: local multiplayer** (idea 2026-10-22, saved for later as it would make the project much bigger): a second PSP as player 2, each showing the game and sending its buttons; Saturn Bomberman, Marvel vs. Capcom, Power Stone, Street Fighter III, Sonic 2's two-player mode. Untried without a second PSP: PPSSPP on the Mac as player 2 over TCP, beside the real PSP over USB. Its parts: the board's link taking a second PSP (one over USB, one over Wi-Fi); the frames and the sound sent to both, each at its PSP's size; player 2's PSP as a second game controller (SDL's second joystick, pspexp-retro's port 2, Flycast's and Yaba Sanshiro's player 2); a "player 2 joined" notice, the menus player 1's. In stages: one emulator first (the Mega Drive, Sonic 2), then the rest;
- **fewer bytes for full-colour content**, should it come (whole 32-bit frames at 60 fps need ≈ 31 MB/s, over the ceiling): DXT textures, which the GE decodes itself (4x smaller, lossy), or LZ4 decoded on the PSP. Measured ratios on real traffic (2026-09-29): Doom 1.35x, OpenTyrian 2x, NES 7x, SNES 9x.

---

## 19. Open Questions

Items deliberately left undecided until measurements exist. None of them is in v0.1 scope.

1. **Head-of-line blocking from resource uploads** (§10.3). Measure with benchmark workload (c). If frame delay behind uploads is visible in practice despite prefetch, split into a bulk-resource stream plus a latency-sensitive graphics stream with explicit fences.
2. ~~**Real USB throughput** (§3.2).~~ Measured 2026-09-22: ≈ 25 MB/s for large transfers, 13 MB/s at 4 KiB transfers, 0.25 ms round trip. Question 1 becomes less pressing: a 512 KB upload blocks the stream for ≈ 20 ms, about one frame, not four.

~~1. **Head-of-line blocking**~~ Measured 2026-09-22 with workload (c): 512 KB uploads at 256 KB/s alongside 500-sprite frames delayed 1 frame in 600 past its vblank; 5,000-sprite frames with 512 KB/s of uploads, 1.8%, none by two vblanks. The single ordered stream stays. A new finding replaces it as the binding limit for sprite count: GE fill time, 9.2 ms per frame at 5,000 32 x 32 sprites.
3. **Placement policy** (§6.3). Whether priority-then-upload-order is good enough, or the runtime needs an LRU between tiers.
4. Whether a triangle-based batch for rotation/skew earns its place before v0.3.
5. The right jitter-buffer depth for streamed audio over Wi-Fi versus USB.
6. Whether the host should ever send raw GE display lists for static content (maximum performance, but it leaks GE semantics into the protocol and breaks the SDL executor's ability to interpret them).

---

## 20. Project Vision

PSPExp grew from the original PSPOmega coprocessor experiment into a broader host-independent architecture:

> **PSPExp turns the Sony PSP into a reusable hardware-accelerated graphics, audio, display, and input frontend for applications executing on another computer.**

The resulting system is a deliberately heterogeneous distributed console:

```text
       Host CPU / Memory / Linux
                 |
           PSPExp protocol
                 |
          USB 2.0 / Wi-Fi
                 |
       PSP CPU / GE / Audio / I/O
                 |
             PSP LCD
```

The first decisive result is not a complete game. It is a texture uploaded once, rendered by the PSP GE under commands from a host, moved interactively using PSP controls returned to that host, at 60 Hz, with a measured input-to-photon latency under 50 ms, and surviving a cable pull.

If that path is fast, stable, and measurable, the architecture is proven.

**Status 2026-09-22:** achieved on a PSP-1000 over USB (`docs/v01-usb-bringup.md`).

---

## 20a. Results (2026-09-22)

What the first day of implementation measured, against the estimates this document was written with. Details and raw numbers are in `docs/v01-usb-bringup.md`; commands in `docs/cheatsheet.md`.

| quantity | estimate in this document | measured on a PSP-1000 |
|---|---|---|
| USB throughput, large transfers | 8 MB/s (planning assumption) | 25–26 MB/s host→PSP, 23–24 MB/s PSP→host |
| USB throughput, 4 KiB transfers | — | 13 MB/s (per-transfer cost ≈ 0.1–0.15 ms each way) |
| round trip, 16 B / 40 KB | 1–2 ms / 1–5 ms one way | 0.25 ms / 3.9 ms |
| 512 KB texture on the stream | ≈ 64 ms, four frames | ≈ 20 ms; delays 1 frame in 600 at 256 KB/s of uploads |
| PSP cost per sprite (parse + expand + list) | 50–80 cycles | ≈ 120 cycles, 0.37 µs |
| 5,000 sprites per frame | transport-bound (40 KB) | GE-bound: 9.2 ms of GE fill, 2 ms of transfer |
| input to present | 35–50 ms input-to-photon | one frame period from sample to vblank (16.7 ms), ≈ 25–41 ms to photon |
| Warcraft (v0.2) | native build ≈ 15 ms/frame on the PSP | 60 fps, PSP CPU 0.5–0.8 ms + GE 0.3–0.5 ms per frame, 4.5–9.5 KB per frame, audio streamed with no underruns |
| an unmodified SDL2 program (v0.4, `sdl-pspexp`, test game: 95 scaled sprites + primitives + a streaming texture) | — | 60 fps, 3.7 KB per frame, PSP CPU 0.2–0.8 ms + GE 0.5–0.8 ms, 120 uploads/s (the streaming texture), audio streamed with no underruns |
| Chocolate Doom, unmodified, through `sdl-pspexp` (320x200 frame as a streaming texture) | the surface-streaming case this document ruled out (§12) | 35 fps (Doom's own rate), 160 KB per frame = 5.6 MB/s, GE 2.2 ms swizzled in VRAM (10.5 ms when linear in RAM), PSP CPU 0.06 ms, audio with no underruns |
| OpenTyrian, unmodified, through `sdl-pspexp` (320x200 frame, scaler off) | — | 35 fps in play, GE 2.3 ms, PSP CPU 0.06 ms, audio steady at ≈100 ms buffered with no underruns; needed input and audio serviced from the event poll (menus present ~1 fps) and host focus loss hidden (the game pauses when unfocused) |
| Raspberry Pi 400 as the host (Debian 13, headless over SSH), §13 | a small Linux box should do | USB 25 MB/s host→PSP / 22 PSP→host, bench frame loop 60 fps, Doom at 35 fps with GE 2.2 ms: indistinguishable from the Mac. The host's dummy audio driver ran slow and jittery (an underrun per second) until the SDL layer paced the game's callback on its own clock |
| Onion Omega2+ as the host (MT7688, 580 MHz MIPS, no FPU, 128 MB; static binaries cross-built with Zig), §13 | — | frame loop and sprite demo at 60 fps at 500 / 2,000 / 5,000 sprites, Omega CPU ≈ 9 % for the 5,000-sprite demo; USB 21 MB/s host→PSP and 16–18 PSP→host with 64–256 KB messages, but only 6 / 4 MB/s at 4 KB (per-transfer cost ≈ 0.6 ms vs 0.15 ms on the Mac); echo 0.39 ms (16 B), 6.9 ms (40 KB) |
| ClockworkPi GameShell as the host (Allwinner R16, 4x Cortex-A7 1.2 GHz, 1 GB; PSP on the OTG port; static tools and a glibc `LD_PRELOAD` shim cross-built with Zig), §13 | — | USB 24–26 MB/s host→PSP and 16 PSP→host with 64–256 KB messages, 12 / 8.6 MB/s at 4 KB; echo 0.22 ms (16 B), 5.6 ms (40 KB); bench frame loop 60 fps; sprite demo at 60 fps up to 15,000 sprites (PSP CPU 7.4 ms), 20,000 at 30 fps, all drawn, GameShell CPU under 10 %; the image's own chocolate-doom through `pspexp-run` at 35 fps with 0 underruns, ≈ 29 % of one core headless (20 fps when SDL also drives the X window, hence headless by default) |
| sprite ceiling at 60 fps (Mac host, 8x8 sprites, 4444 tint, PSP-side drawn/dropped counter) | GE fill was the ceiling at 5,000 (§20a above) | 20,000 sprites at 60 fps (PSP CPU 9.8 ms, 161 KB/frame); 24,000–40,000 at 30 fps, all drawn. From the Omega2+: 15,000 at 60 fps, 18,000 and up at 30 fps (slower USB transfers). PSP parse + expand ≈ 0.5 µs per sprite, plus ≈ 6.5 ms of transfer at 20,000: together they, not the GE, fill the 16.7 ms frame |

What changed against the plan:

- The transport is not the scarcest resource for 2D games at this resolution; the GE fill rate is, above a few thousand sprites. Principle 4 still holds for uploads, which is where the bytes are.
- CLUT textures, host-baked text, per-batch palette selection, alpha tints, translucent primitives, quad batches, streamed audio, a TCP transport and the PPSSPP configuration were all pulled forward from v0.3 into v0.2, because the Stratagus backend needed them. Frame-record sprites remain the fast path, but the quad record is what an engine with arbitrary blits actually uses.
- Two hardware-only facts were learned the hard way and are now in the executor: the LCD pixel format must be set explicitly by the GE executor (the debug status page leaves it 32-bit), and the host must never touch a transport after closing it (a cable pull turned that into a killed process).
- Reconnection works as designed (§9.2): the host replays palettes, then textures and sounds; the game resumes.
- A headless host works with SDL's dummy drivers; a Raspberry Pi 400 runs everything with the same numbers as the Mac (`scripts/pi/build.sh`).

- v0.4 came the same day: an unmodified SDL2 program runs on the PSP through an injected library (§12), including the surface-blitting programs this document had ruled out, because a 480x272 frame is only 270 KB on a 25 MB/s link.

Open, deliberately: Wi-Fi on real hardware (the TCP transport is proven only under PPSSPP), priority-guided placement between tiers, static vertex buffers, rotation in the SDL layer (needs the triangle batches of "later research").

## 21. Architectural Principles

The project should preserve the following principles as it evolves:

1. **Render intent, not completed frames.**
2. **The wire format is optimised for the PSP consumer, not the host producer: compact on the wire, trivial to expand.**
3. **The host owns application state and resource existence; the PSP owns presentation execution and resource placement.**
4. **The transport is the scarcest resource; spend host CPU to save bytes.**
5. **Cache resources; do not retransmit them per frame. Prefetch; never upload on the critical path.**
6. **Batch commands aggressively.**
7. **Keep input asynchronous from rendering.**
8. **Close the pacing loop: the PSP reports presents, the host paces on them.**
9. **One ordered stream for frames and resources; drop stale frames, never resources.**
10. **Do not make USB semantics part of the application protocol.**
11. **Do not make a particular host board part of the protocol.**
12. **SDL and PSP executors consume the same command representation and enforce the same constraints.**
13. **Measure real hardware before optimising around assumptions; emulators never relax a constraint.**
14. **Keep the PSP runtime generic and game-independent — no text layout, no game concepts.**
15. **Prefer bounded latency over preserving stale frames.**
16. **Reconnection is a pause, not an error: the host can always rebuild PSP state.**
17. **v0.1 is frozen. Make the sprite move before adding anything.**

---

## 22. References

Hardware and SDK facts in §3.2 were checked against the following; items marked [assume] in that table are not covered by any of them and are measured in v0.1.

- PSPSDK `pspgu.h` — vertex member alignment, `GU_SPRITES` definition, `GU_PSM_*` formats, `sceGuGetMemory` semantics: https://github.com/pspdev/pspsdk/blob/master/src/gu/pspgu.h
- PSPSDK pull request #95 — the 512-pixel texture limit is a hardware behaviour that PPSSPP does not reproduce: https://github.com/pspdev/pspsdk/pull/95
- PSPSDK `gu/speed` sample — 16-byte aligned buffers, `sceKernelDcacheWritebackAll`, strip-width experiments: https://github.com/pspdev/pspsdk/blob/master/src/samples/gu/speed/speed.c
- PPSSPP GE overview — GE reads textures from RAM at reduced rate; 32/64-px vertical strips are faster for 2D: https://www.ppsspp.org/docs/psp-hardware/gpu/ge-overview/
- PSPSDK `pspaudio.h` — 8 channels, sample block limits, SRC channel rates: https://github.com/pspdev/pspsdk/blob/master/src/audio/pspaudio.h
- PSP hardware overview — RAM per model, 2 MiB GE eDRAM, Media Engine, clocks, 802.11b: https://en.wikipedia.org/wiki/PlayStation_Portable_hardware
- PSPLink / usbhostfs — transport reference implementation (no throughput figures published): https://github.com/pspdev/psplinkusb
