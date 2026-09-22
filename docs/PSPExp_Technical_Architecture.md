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
| Media Engine | Second MIPS32 core, same clock, own 2 MiB eDRAM | [doc] | Not used in v0.1; candidate for command expansion or audio mixing later (§18) |
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

**Measured with the GE executor (sprite demo, 2026-09-22):** 60 Hz sustained at 500, 2,000 and 5,000 sprites per frame (4 KB, 16 KB and 40 KB per frame); PSP-side parse-plus-expand cost 0.25 / 0.71 / 1.85 ms (≈ 0.37 µs per sprite); input sample to the vblank that displayed the frame consuming it: one frame period, 16.7 ms. With LCD scan-out and the sampling phase of a real button press, input-to-photon is ≈ 25–41 ms, so the ≤ 50 ms target is met. The proof of concept of §16, including the cable pull, is complete (`docs/v01-usb-bringup.md`).

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

The runtime keeps a **VRAM tier** (≈ 1.2–1.45 MiB after framebuffers, §3.2; fastest sampling) and a **main-RAM tier** (several MiB; reduced sampling rate). Placement is the runtime's decision: it promotes high-priority textures into VRAM when space allows and demotes lower-priority ones under pressure, reporting each change with `RESOURCE_TIER`. The host influences placement only through the `priority` given at upload or changed with `RESOURCE_HINT`. `MEMORY_STATUS` reports both tiers separately.

For v0.1 the placement policy is deliberately naive — priority order, then upload order — and is one of the things the benchmark plan measures.

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
```

Future extensions could add networking, filesystem, compute/RPC, or other services without changing the fundamental graphics architecture.

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

Any `HELLO` invalidates all PSP-side state: the runtime releases every resource and empties its queues before replying. This is what makes reconnection simple (§9.2).

### 9.2 Reconnection and resynchronisation

The link will be lost: the cable is pulled, the PSP suspends, the host process restarts. Because the host is authoritative for all state, recovery does not require any PSP-side persistence:

1. The transport reports disconnection to libPSPExp.
2. libPSPExp marks every resource as non-resident and drops queued frames.
3. On reconnection the PSP sends `HELLO`; the host replies `HELLO_ACK`.
4. The host re-uploads its current working set (which it already tracks for prefetch purposes) and resumes frames.

An application using libPSPExp should experience this as a pause, not an error.

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

- After each vblank at which a new frame was displayed, the PSP sends `PRESENT_DONE { frame_seq, timestamp_us, queue_depth, dropped }` on the SYSTEM channel.
- The host paces frame generation on `PRESENT_DONE`, targeting exactly one frame in flight.
- The runtime's frame queue is bounded at **one pending frame** (triple-buffer semantics: one displayed, one rendering, one queued). If a newer complete frame arrives while one is still queued, the queued frame is discarded and `dropped` is incremented. A frame whose execution has begun is never discarded.

For interactive rendering the policy is:

> Prefer the newest complete frame over preserving obsolete frames.

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

*Status (2026-09-22, v0.4 built).* The layer exists as `compatibility/sdl-pspexp`, an interposition library (`DYLD_INSERT_LIBRARIES` / `LD_PRELOAD`, `scripts/pspexp-run`) rather than an SDL render driver: it needs no SDL rebuild and works with the SDL2 the program already links. It mirrors the `SDL_Render*` API into scaled quad batches (a 24-byte `PspExpQuadExRec` with `PSPEXP_BATCH_SCALED`, since `SDL_RenderCopy` scales and flips), rects and lines, shadows textures in RGBA8888 so `SDL_UpdateTexture` / `SDL_LockTexture` changes are re-uploaded, taps the audio device (callback or queue) into `AUDIO_STREAM`, and exposes the PSP's controls as a virtual game controller plus keyboard events. Against the paragraph above, the surface-blitting case turned out to be affordable after all at this resolution: a renderer-less program's window surface is shrunk to the PSP size and streamed as a texture every present, ≈270 KB per frame, which the measured USB link (§20a) carries at 30–60 fps. Not mirrored: rotation, render targets, `SDL_RenderGeometry`, OpenGL.

The two components must remain conceptually distinct:

```text
pspexp-sdl : PSPExp -> SDL
sdl-pspexp : SDL    -> PSPExp
```

---

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

*Status 2026-09-22: the Stratagus backend exists (`compatibility/stratagus`). Stratagus is a pure software renderer, so the backend hooks the leaf blits (`CGraphic::DrawSub*`, `DrawFrame*X`, the glyph blit, the `CVideo` primitives, the fog shroud and the minimap blit) and mirrors them into quad batches with per-batch palettes; the software path stays live for a side-by-side desktop view. Warcraft renders through the SDL executor at 60 fps with about 34 batches and 330 sprites per frame in 7.6 KB; the guichan menus are mirrored too, and the game's audio reaches the PSP as the streamed-PCM mode of §8 (SDL_mixer's post-mix output, 176 KB/s, 100 ms jitter buffer). On the PSP-1000 over USB the game runs at 60 fps with the PSP spending 0.5–0.8 ms of CPU and 0.3–0.5 ms of GE per frame, 4.5–9.5 KB per frame, audio streamed with no underruns; the native build spends about 15 ms per frame on the same hardware (`docs/v01-usb-bringup.md`).*

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

- triangle-based batches for rotation/skew, then static meshes and 3D commands (3D vertex formats, matrices, depth state);
- offloading batch expansion or audio mixing to the Media Engine;
- compute/RPC services;
- network/storage services;
- transparent or semi-transparent engine adapters;
- additional transports;
- additional presentation clients.

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

What changed against the plan:

- The transport is not the scarcest resource for 2D games at this resolution; the GE fill rate is, above a few thousand sprites. Principle 4 still holds for uploads, which is where the bytes are.
- CLUT textures, host-baked text, per-batch palette selection, alpha tints, translucent primitives, quad batches, streamed audio, a TCP transport and the PPSSPP configuration were all pulled forward from v0.3 into v0.2, because the Stratagus backend needed them. Frame-record sprites remain the fast path, but the quad record is what an engine with arbitrary blits actually uses.
- Two hardware-only facts were learned the hard way and are now in the executor: the LCD pixel format must be set explicitly by the GE executor (the debug status page leaves it 32-bit), and the host must never touch a transport after closing it (a cable pull turned that into a killed process).
- Reconnection works as designed (§9.2): the host replays palettes, then textures and sounds; the game resumes.
- A headless host works with SDL's dummy drivers; a Raspberry Pi build script exists (`scripts/build-pi.sh`), untested on a Pi.

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
