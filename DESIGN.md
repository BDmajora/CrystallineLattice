# glacier — a purpose-built DRM/KMS display server for an open-source Windows shell

**Status:** design spec / phased roadmap
**Supersedes:** frostedglass (wlroots compositor) + the Wayland-driver hack layer
**Keeps:** Moonshine (Wine fork), PipeWire/WirePlumber audio, libreldr, the YetiOS staged build

> **Implementation status (this repo, 2026-06).** Phase 0 ✓ · Phase 1 partial
> (CPU compositor + software cursor; GL composition and the KMS hardware-cursor
> plane deferred) · Phase 2 ✓ (incl. absolute-pointer motion for VM/QEMU
> tablets) · **Phase 3 in progress** — the CrystallineLattice transport (v0 wire
> format, SEQPACKET socket, role registration, `SCM_RIGHTS` shm buffers with the
> render-fence fd already on the wire, crash-resilient reaping) and a trivial
> native test client (`glacier-client`) have landed · **Phase 4 landed** — the
> self-implemented Wayland frontend (Transport B: `wl_compositor`/`wl_surface`/
> `wl_shm`/`xdg_wm_base`/`wl_seat`/`wl_output` + xdg-decoration forcing SSD)
> reparents native toplevels into server-owned windows; Ctrl+Alt+T spawns a
> terminal. Remaining: input-to-Wine-client routing + dma-buf import (Phase 3),
> `winedrm.drv`, Xwayland bring-up + `wl_subsurface`/popups (Phase 4), and the
> Phase 1 GL/hardware-cursor back-fill. See `README.md` for the per-component
> breakdown.

## Naming (locked)

- **CrystallineLattice** — the project, and specifically its *native* client↔server protocol (Path β). The lattice that binds Wine clients to the server.
- **glacier** — the display-server component within CrystallineLattice (DRM/KMS engine + compositor + shell policy). The massive, foundational ice sheet that does the heavy lifting over the metal, replacing the thin frostedglass.
- **winedrm.drv** — the Wine graphics driver that implements the CrystallineLattice client side. It speaks CrystallineLattice and passes DRM/GEM buffers as dma-buf — hence `drm`. Replaces Moonshine's `winewayland.drv`.
- **Wayland compatibility frontend** — a *second* client transport inside glacier (not a separate codename; describe it functionally). A minimal, frozen Wayland server surface that hosts native Linux toolkits (GTK/Qt/Electron) and, via Xwayland, legacy X11 apps. See §3.5.

> **Scope note.** This is a from-scratch display server that speaks to DRM/KMS, GBM/EGL and libinput directly, with no wlroots and no upstream Wayland *compositor* underneath. It borrows ideas from X11, Wayland, and Arcan; it does not sit on any of them. The one deliberate exception is a thin, self-implemented Wayland *frontend* (libwayland-server + a frozen core-protocol subset) used solely to host foreign Linux apps — this is glacier's own code, not wlroots, not Weston. Arcan/Durden, KMSCON, Weston's DRM backend, and wlroots' backend layer are reference reading, not dependencies.

---

## 0. The thesis in six lines

1. The frostedglass hacks are not bugs — they are symptoms of an impedance mismatch. Wayland is deliberately client-sovereign (no global coordinates, compositor-as-mechanism-not-policy, client-owned cursors and decorations). A Windows desktop is server-authoritative (the shell owns the taskbar, z-order, global virtual-screen coordinates, the cursor, focus policy). Phases 1–4 of frostedglass were spent forcing one model to behave like the other.
2. If you write the server, you choose the model. Make it server-authoritative, like the X server + a window manager, or like win32k + DWM. The shell is the policy layer inside (or directly beside) the server.
3. Raw KMS gives you a hardware cursor plane. That one fact deletes the entire `yetios_cursor_manager_v1` / boot-cursor / no-op-`seat_request_cursor` saga. You own the cursor from frame zero, unconditionally — and because *the server* owns it, **every** client (Wine and native Linux alike) gets the one Windows cursor for free. The cursor is **not** Wine-provided anymore; that was the hack you're killing.
4. The cost is real: you re-derive what wlroots abstracts (atomic modeset, GBM/EGL buffer plumbing, libinput wiring, fencing, hotplug). Budget for it. The win is that you stop fighting and your dependency graph collapses.
5. Native Linux apps still need a home. glacier hosts them through a **minimal Wayland frontend it implements itself**, plus **Xwayland** for legacy X11 — both feeding the same protocol-agnostic engine as CrystallineLattice, both wearing server-drawn Windows decorations. This is how you get one consistent look and end the GTK-vs-Qt chrome mess (§3.5).
6. Audio is orthogonal — keep the PipeWire stack as-is. Don't reopen that during the rewrite.

---

## 1. Why frostedglass has to go — root cause, not symptoms

Every hack in the tree traces back to the same source: Wayland hands policy to clients, and Wine-as-shell is a client. The fix is not a better hack; it's a server that holds policy.

| frostedglass hack | What it was working around | What replaces it under glacier |
|---|---|---|
| `fg_cursor_override.c`, `fg_boot_cursor.c`, no-op `seat_request_cursor`, `yetios_cursor_manager_v1` protocol, stripping `wp_cursor_shape_manager_v1` | Wayland: the cursor is whatever the focused client last set; before Wine sets one there is nothing | **KMS hardware cursor plane.** Server uploads one ARGB buffer and sets its position from raw pointer motion. Visible from the first frame, never client-controlled, identical across all client transports. |
| `fg_taskbar.c` geometry heuristics (`width ≥ 80% screen && height < 120`), `wlr_xdg_toplevel_set_tiled` to force size adherence, `screen_width` from `WM_SIZE` | Wayland clients negotiate their own size; `explorer.exe`'s taskbar is just another toplevel with no special status | **Explicit role registration** in the protocol. A client says "I am the shell taskbar"; the server places and sizes it. No geometry guessing, no tiled-hint trick. |
| `fg_desktop.c` — `desktop.exe` as a fullscreen window solely so the pointer is always over a Wine surface | Wayland: pointer focus drives cursor-setting, so you needed a Wine window under the pointer everywhere | Server draws the wallpaper natively on the primary plane; pointer focus is server policy, decoupled from who draws the cursor. The desktop window becomes optional. |
| `explorer.exe` `WM_CLOSE` no-op, split `fg_wine.c`/explorer deployment regressions, taskbar-loss rescans | Shell lifecycle at the mercy of Wine process-tree quirks | Shell chrome lifecycle is server-managed; a chrome client dying is a recoverable event, not a desktop-ending one. |
| `mmdevapi` driver-list surgery, "winepulse silently winning" | Orthogonal (audio) | Unchanged — keep PipeWire. |
| `desk.cpl` → `popen("wlr-randr")` for resolution changes | You had to shell out to a Wayland tool to modeset | You are the modesetting authority. The control panel asks the server over its control socket; the server does an atomic commit. `wlr-randr` and `wlr_output_management_v1` disappear. |

**Not in this table:** hosting native Linux apps. frostedglass did that *correctly* — as a wlroots compositor it spoke Wayland, so browsers and Flatpak apps just worked. That capability must be **preserved**, not killed (§3.5). It is the one thing the rewrite must not regress.

The pattern for the hacks is consistent: replace heuristic + protocol gymnastics with an explicit request to an authoritative server.

---

## 2. The options, honestly weighed

Recorded so the decision is on paper.

- **(A) Refactor frostedglass on wlroots.** Lowest effort, lowest risk, mainstream and maintained. But the mismatch in §1 is structural to Wayland; you'd be polishing the hacks, not removing them. **Rejected:** doesn't solve the root cause.
- **(B) Build on Arcan's engine, write a Lua appl shell.** Genuinely good technical fit (server-authoritative appl model, native shaders, crash resilience, raw KMS already). But it's a ~1.5-developer project whose author has signalled winding down after 0.8/0.9; SHMIF is documented as being in "aggressive flux"; you'd trade owning the stack for owning a Lua policy script on someone else's engine. **Rejected** per steer — and the bus-factor risk is real.
- **(C) From-scratch DRM/KMS display server, server-authoritative, purpose-built as a Windows shell.** Highest effort. Maximum control. The mismatch is gone because you define the model. Dependency graph collapses to a small, slow-moving set you control end to end. **This is the plan.**

A note on Weston, since it comes up: Weston **is** a Wayland compositor — it cannot exist without libwayland, so "use Weston but drop Wayland" is incoherent. More importantly, a nested Weston is a *desktop inside a window*, not a surface host; it cannot hand individual app windows to your taskbar with your titlebars. It is the wrong tool for foreign-app integration. The right tool is glacier implementing the Wayland *frontend* itself (§3.5).

---

## 3. Architecture

Four layers. The bottom two are the "engine"; above them sit **two parallel client transports**; the top is the Windows part. The engine is deliberately **protocol-agnostic** so the shell and the transports can evolve independently — which is exactly what lets a Wine transport and a Wayland transport share one server path.

```
┌──────────────────────────────────────────────────────────────┐
│  SHELL / WM POLICY  (server-authoritative)                     │
│  taskbar · Start · tray · SSD decorations · Alt-Tab · Aero     │
│  Snap · virtual-screen layout · focus & z-order · control sock │
└──────────────────────────────────────────────────────────────┘
┌───────────────────────────────┬──────────────────────────────┐
│  CrystallineLattice            │  Wayland compatibility       │
│  (Wine via winedrm.drv)        │  frontend (GTK/Qt/Electron)  │
│  native protocol — Path β      │   + Xwayland (legacy X11)    │
│  HWND handles · roles · dmabuf │  minimal frozen wl subset    │
└───────────────┬───────────────┴──────────────┬───────────────┘
                │   both feed the same          │
                ▼   protocol-agnostic engine     ▼
┌──────────────────────────────────────────────────────────────┐
│  COMPOSITOR  (scene → planes)                                  │
│  GLES/EGL composite · damage tracking · cursor plane ·         │
│  overlay-plane direct scanout · SSD renderer · pixman fallback │
└──────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────┐
│  PLATFORM  (the metal)                                         │
│  DRM atomic KMS · GBM · EGL(gbm) · libinput/evdev/udev ·       │
│  seatd (master + VT) · explicit fencing (syncobj) · hotplug    │
└──────────────────────────────────────────────────────────────┘
```

### 3.1 Platform layer (the DRM rabbit hole proper)

- **Device & master.** Open `/dev/dri/cardN`. Acquire DRM master and input fds via **seatd** (not logind — seatd is one tiny daemon and keeps the dependency graph lean). seatd also gives clean VT activation/deactivation so Ctrl-Alt-Fn still works.
- **Atomic KMS only.** Set `DRM_CLIENT_CAP_ATOMIC` (and `UNIVERSAL_PLANES`). Build a resource model from connectors → encoders → CRTCs → planes. All state changes are atomic commits with `DRM_MODE_ATOMIC_NONBLOCK` + `PAGE_FLIP_EVENT` for vsync'd presentation. Do **not** start with the legacy modeset API; you'll only have to rip it out.
- **Planes are the whole point.**
  - *Primary plane*: the composited desktop.
  - *Cursor plane*: a dedicated hardware cursor. Upload ARGB, set position from pointer motion. This is the §0.3 win.
  - *Overlay plane(s)*: direct scanout of a fullscreen client (Wine game, video) with **zero composition** — lower latency, lower power, no GPU copy. Probe capabilities and **fall back to GL composition** when a plane can't scan out the buffer's format/modifier.
- **Buffers.** GBM for scanout-capable allocation; EGL with `EGL_PLATFORM_GBM` + GLES2/3 for composition; import client buffers as EGL images from **dma-buf** with correct **format modifiers**. Keep a **pixman/CPU fallback** path for virtio-gpu-without-GL and bring-up.
- **Fencing.** Use in-fences/out-fences and DRM syncobj so composition waits on client render completion and scanout is tear-free. This is where stutter bugs live; design it in, don't bolt it on — and that means the **fence fd is a field in the very first version of the wire format**, not a later addition (see §3.3).
- **Hotplug & multi-GPU.** Listen for udev hotplug uevents on the DRM fd; re-enumerate. Single-GPU first, but keep the resource model multi-device-shaped.

> **Hardware reality check.** Plane counts, overlay formats and modifier support vary wildly. Desktop Intel/AMD give generous planes; **virtio-gpu under QEMU** is minimal; **ARM64 laptops (ThinkPad X13s / Snapdragon/Adreno)** and **TI AM62X-class SoCs** have constrained display controllers and vendor GLES/modifier quirks. Treat "primary + cursor only, GL-composite everything" as the absolute baseline and overlay scanout as an *optimization you probe for*, never assume.

### 3.2 Compositor layer

A scene of textured quads (windows) the server positions in a **single global virtual-screen coordinate space** spanning all outputs — the thing Wayland refuses to give you and Windows depends on (`GetCursorPos`, `SetWindowPos`, multi-monitor spanning). Damage-track so you only recomposite changed regions, and detect the "one fullscreen opaque window" case to hand it straight to an overlay plane. The compositor also renders **server-side decorations** (title bars, borders, min/max/close) so **every window is authentically Windows-styled regardless of which transport delivered it** — no CSD negotiation with Wine, and CSD *forcibly disabled* for Wayland clients (§3.5).

### 3.3 Transport A — CrystallineLattice (Wine, the deliberate-theft layer)

The native, server-authoritative transport for Wine. **Path β — locked.** No Wayland vestige on this path. `winedrm.drv` (new, client side of CrystallineLattice) replaces Moonshine's `winewayland.drv` and frostedglass together. Steal per source:

- **From Wayland:** dma-buf fd passing over a per-client unix socket (zero-copy GPU buffers) via `SCM_RIGHTS`, plus a `wl_shm`-style shared-memory fallback segment, and the clients-render/server-composites split. Mechanism only; none of its policy.
- **From X11/Windows:** **server authority.** The protocol exposes **HWND-like opaque window handles**, **first-class global virtual-screen coordinates**, and **explicit window roles** (`NORMAL | TASKBAR | DESKTOP | TRAY | MENU | TOOLTIP`). Clients *request* geometry / z-order / focus; the shell *decides*. This retires the `is_taskbar()` / `is_desktop()` heuristics.
- **From Arcan/SHMIF:** **one fixed, versioned control segment + event ring + buffer-handle table per client.** No extensible-protocol zoo, no `wl_registry` of optional globals. Add a field, bump the version. Crash-recovery is a protocol property: a client death is recoverable, with a reconnect handle offered, not a desktop-ending event.

Three things that must be right in **version 0** of the wire format, because retrofitting them means an immediate version bump:

- **The render-fence fd goes in the buffer-submit message from the first byte.** §3.1 says "design fencing in" — that applies to the protocol, not just the compositor. Submit carries an in-fence (syncobj/sync_file) so the server composites only after the client's render completes.
- **Decide buffer-allocation ownership before freezing the format.** Either Wine (wined3d/vkd3d/dxvk) allocates and you import whatever modifier the GPU driver picks — then your EGL import must accept the full tiled/compressed modifier set on Intel/AMD — or `winedrm.drv` allocates via GBM and hands Wine the target, trading control of format for a constraint on Wine's render path. Pick one now; deciding late means reworking the transport.
- **Version negotiation needs a min-supported-version handshake, not strict equality.** "We own both ends, bump in lockstep" holds only for atomic image swaps. The moment glacier and Moonshine update as separate packages, a non-atomic upgrade leaves vN server + vN+1 driver mid-update. For the immutable target you're fine; the handshake is cheap insurance regardless.

> **Repo note (Phase 3).** The decisions above are realized in `include/protocol.h` and `src/transport.c`: a SEQPACKET socket with a `magic` + version + **min-version** handshake; explicit roles mapped to `enum win_role`; `cl_buffer` carries `format`/`modifier` and a `has_fence` flag with the fence fd passed alongside the buffer fd via `SCM_RIGHTS`; buffer-allocation ownership is **client-allocates, server-imports** (shm/memfd today, dma-buf next). The fence fd is closed unwaited on the CPU path until the GL compositor lands.

Keyboard translation lives **here, in the driver**, not in the input layer. glacier keeps libxkbcommon and ships keysym + modifiers; `winedrm.drv` maps keysym → Win32 VK + `WM_CHAR`, including dead-key/IME composition. `winewayland.drv` carried a real chunk of code for exactly this — budget it into the driver.

### 3.4 Shell / WM policy layer

The Windows experience and your actual differentiator: taskbar, Start menu, system tray, Alt-Tab, Aero Snap, window decorations, wallpaper, multi-monitor layout. Because the server is authoritative, the shell is a privileged module *in* the server (X server + WM fused, or win32k + DWM).

**Hybrid shell chrome.** The shell draws from multiple sources (native GL rendering, ReactOS/Open-Shell behavioral references) to balance performance and authenticity.

**Aesthetic direction: Win7 with a modern coat of paint.** Incremental evolution from Windows 7 — the way macOS refines its UI over time rather than making jarring paradigm shifts. Modern skeuomorphism / neo-morphism layered over the classic Aero base: structural familiarity of Win7 (taskbar, Start menu, SSD framing) with visuals executed in modern GL shaders — updating the "glass" to contemporary soft-UI while keeping the muscle memory intact.

Expose all shell config/policy over **one control socket** ("everything is a path", which maps neatly onto the Windows registry / Group Policy mental model). `desk.cpl` and friends talk to that socket instead of `popen`-ing external tools.

### 3.5 Transport B — the Wayland compatibility frontend (native Linux apps)

This is the corrected answer to "how do Flatpak apps and browsers display under a non-Wayland server." The product goal is one consistent Windows look — native Linux apps wearing the **same Windows titlebar and the same server-owned Windows cursor** as Wine apps, ending the Qt-vs-GTK chrome mess that confronts newcomers to modern Linux.

**glacier implements a minimal Wayland *frontend* itself** — its own code, not wlroots, not Weston. It is a second transport into the same engine described in §3.2, so the dma-buf import, scene, input routing, and decoration renderer are all shared with Transport A. A Wayland toplevel is simply reparented as a native glacier window with role `NORMAL`.

What the frontend implements — a small, **frozen, stable** slice, not the churning extension constellation you're escaping:

- `wl_compositor`, `wl_surface`, `wl_subsurface`
- `xdg_wm_base` (`xdg_toplevel`, `xdg_popup`) — the window-shell core
- `linux-dmabuf-v1` — zero-copy GPU buffers (same import path as CrystallineLattice) with `wl_shm` fallback
- `wl_seat` (pointer, keyboard, touch) — input from the shared libinput layer
- `wl_output` — the global virtual-screen layout, exported read-only
- `xdg-decoration-unstable-v1` — used to **demand server-side decorations**; this is the mechanism that forbids GTK/Qt from drawing their own chrome and is what makes everything look like one OS. (Honest caveat: a handful of GNOME apps fight SSD; most honor the protocol.)

**Cursor:** the frontend simply **ignores `wl_pointer.set_cursor`** — the same policy you applied to `seat_request_cursor` in frostedglass — because glacier owns the KMS cursor plane globally. Native apps get the Windows arrow with zero per-toolkit work.

**Xwayland** rides on this frontend for legacy X11 apps (the classic apps in the target look — e.g. `xeyes`-class tools). It is the standard, well-maintained X11↔Wayland bridge and the only sanctioned reason any X server exists in the tree. Modern GTK4/Qt6/Electron/Chromium/Firefox all run Wayland-native, so Xwayland is for the legacy tail only; it can be omitted on images that don't need X11 apps.

**Dependency consequence:** this is **not** a return to the "constellation of shims." You keep `libwayland-server` plus the frozen core protocols above and Xwayland — a small, slow-moving set that GTK/Qt already speak. You still drop wlroots, `wlr-randr`, the `wlr_*` protocol zoo, and xdg-desktop-portal-as-display-path. See §4.

---

## 4. What this does to the Gentoo package count

| Drop | Add | Keep (minimal / frozen) |
|---|---|---|
| wlroots | libdrm | Wine / Moonshine |
| the `wlr_*` protocol zoo | mesa (libgbm, libEGL, libGLES) | PipeWire + WirePlumber |
| `wlr-randr` (was built from source!) | libinput | libreldr |
| `wlr_output_management_v1` usage | libudev / eudev | your control-panel applets |
| xdg-desktop-portal **as a display path** | seatd | **libwayland-server** (frontend only) |
| xcursor themes | pixman (fallback) | **wayland core protocols**: xdg-shell, linux-dmabuf-v1, wl_seat, wl_output, xdg-decoration |
| xorg cursor/randr cruft | | **Xwayland** (legacy X11 tail; optional per image) |
| | | libxkbcommon* |

\* The Arcan crowd statically translate XKB layouts to drop libxkbcommon at runtime. Worth doing later for an even tighter graph; not Phase-1 work, and the Wayland frontend wants xkb anyway.

> **Correction to the earlier draft.** The previous spec's "(no libwayland — Path β is native)" line was wrong once Flatpak apps are in scope. You keep a *minimal frozen* libwayland-server frontend and Xwayland. The net is still a large consolidation: the *display* side goes from "a constellation of compositor + protocol shims + a forked driver gluing them" to **one codebase over a small, slow-moving dependency set**, with two clean transports (native Path β for Wine, frozen Wayland subset for everything else) feeding one engine. The maintainability win comes from *consolidation and stability of the base*, not raw package subtraction. Wine stays heavy; audio stays as-is.

---

## 5. Phased plan (kill-gated)

Each phase has an explicit exit gate. If a gate slips badly, that's the "got a t-shirt, cut losses" checkpoint.

### Phase 0 — Platform bring-up: *does the metal cooperate?* — **done**

Each sub-phase is a standalone program (or a small `main()`-able module) you run and eyeball. Run **every** one across the target matrix — **QEMU virtio-gpu** (CI/dev loop), **one real Intel/AMD desktop** (real planes, modifiers, EDID), and **the ARM64 laptop (ThinkPad X13s)** / **embedded SoC** if in scope. Divergence between targets is the signal you're front-loading to catch. Strict dependency order: each needs the one before it.

**P0.1 — Resource enumeration (read-only, no master).**
Open `/dev/dri/cardN`; set `DRM_CLIENT_CAP_ATOMIC` + `DRM_CLIENT_CAP_UNIVERSAL_PLANES`; walk `drmModeGetResources`, `drmModeGetPlaneResources`, `drmModeObjectGetProperties` per object; read connector modes + EDID. Emit a topology dump.
*Retires:* "Can I see the display hardware at all, via atomic?" — and the dump reveals plane **types** (is there a `CURSOR` plane? how many `OVERLAY`?), deciding P1.3 and scanout strategy.
*Test:* diff your dump against `drm_info` and `modetest -c -p` on each target.
*Gate:* dump matches the reference tools everywhere.

**P0.2 — First light (dumb buffer, atomic, solid color).**
Become DRM master (bare VT, run as root for now — seatd is P0.5). Allocate a **dumb buffer** (`drmModeCreateDumbBuffer` + `mmap`), fill a flat color, `drmModeAddFB2`, build one atomic commit binding connector → CRTC → primary plane with a chosen mode, commit. **No GBM/GL yet** — isolate modeset from rendering.
*Retires:* "Does this driver do atomic modeset, and can I get *any* pixel up?"
*Test:* screen goes your color; `DRM_MODE_ATOMIC_TEST_ONLY` succeeds before the live commit.
*Gate:* full-screen solid color on all targets.

**P0.3 — Double-buffer + page-flip loop (vsync).**
Two dumb buffers; atomic commit with `DRM_MODE_PAGE_FLIP_EVENT`; `drmHandleEvent` on the DRM fd inside an `epoll` loop; flip between buffers; animate a CPU-drawn moving rectangle. **This is the frame loop everything else hangs off.**
*Retires:* "Tear-free vsync'd presentation + a correct flip-event loop."
*Test:* smooth motion, no tearing, stable inter-flip timing.
*Gate:* animated rectangle at refresh rate with no missed/duplicated flips.

**P0.4 — GBM + EGL + GLES (the GPU path).**
`gbm_create_device(drm_fd)` → `gbm_surface_create` → `eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, …)` → GLES2 context. Render `glClear`, then a triangle, into the GBM surface; `gbm_surface_lock_front_buffer`; wrap the bo as a DRM FB (`drmModeAddFB2` with handle/stride/**modifier**); flip via the P0.3 loop. **Keep the dumb-buffer path as the pixman/CPU fallback — do not delete it.**
*Retires:* "Does EGL/GLES-on-GBM work here, and can I scan out GPU-rendered buffers?" — the single biggest virtio/SoC/ARM risk (virtio may need virgl or llvmpipe; Adreno/Imagination/Mali have EGL + modifier quirks).
*Test:* GL triangle on every target; record which need the CPU fallback.
*Gate:* GL frame scanned out on desktop + virtio + X13s at minimum; SoC works or is documented fallback-only.

**P0.5 — seatd + VT switching (drop root, coexist).**
Replace run-as-root/`drmSetMaster` with **libseat/seatd**: open the DRM device (and later input) through seatd; handle enable/disable seat events. On VT-away you lose master — stop committing, release; on VT-return — re-acquire, repaint. Done *now* (with a working display) so you can watch suspend/resume-on-switch behave.
*Retires:* "Run unprivileged; survive VT switches without corrupting the console or another session."
*Test:* Ctrl-Alt-F2 away/back; switch to a text VT and a second instance; no corruption.
*Gate:* clean bidirectional VT switching as a normal user.

**P0.6 — Hotplug stub.**
Watch udev for DRM uevents; on hotplug, re-run P0.1 enumeration and reconfigure. Minimal, but wire the path.
*Retires:* "Connector add/remove doesn't wedge the server."
*Test:* unplug/replug a monitor, or QEMU `device_del`/`device_add` a virtio display.
*Gate:* hotplug logged + re-enumerated without crash.

**Phase 0 exit gate (the kill-gate):** on every in-scope target you can come up *unprivileged via seatd*, set a mode, run a vsync'd GL frame loop with a CPU fallback, and survive VT switch + hotplug. If a target can't clear this, you've learned it cheaply — scope it out or mark it fallback-only — **before** writing any compositor or protocol code.

### Phase 1 — Compositor primitives: *turn the metal into a scene* — **partial** (CPU compositor + software cursor landed; GL composition and the KMS hardware-cursor plane deferred)

**P1.1 — The textured-quad primitive.** A GLES pipeline that draws one textured quad at an arbitrary `(x,y,w,h)` in a **global virtual-screen** ortho projection; upload a static PNG as a texture. The seed of the scene graph.
*Gate:* an image rendered at a server-chosen position and size.

**P1.2 — N quads as movable "windows."** A z-ordered list of quads (stand-in surfaces); the server moves/raises/lowers them on a timer or keypress. No clients yet — static textures standing in for windows.
*Gate:* several "windows" composited, reordered, and moved by server policy.

**P1.3 — Hardware cursor plane.** Take the `CURSOR` plane found in P0.1 (query `DRM_CAP_CURSOR_WIDTH/HEIGHT`); upload a 64×64 ARGB arrow; position it via the plane's `CRTC_X`/`CRTC_Y` in an atomic commit; drive from a scripted path first. If a target has no cursor plane, fall back to a top-most composited quad.
*Retires:* "Hardware cursor on this hardware" — and **this is the moment the entire frostedglass cursor-handoff subsystem becomes dead code.** Visible from frame zero, never client-owned, shared by all transports.
*Gate:* hardware cursor moving over the composited scene on every target (or documented fallback).

**P1.4 — Damage tracking, multi-output, atomic-state builder + fencing.** Recomposite only changed regions; drive multiple connectors (each its own CRTC + flip loop) in the shared global space; consolidate the ad-hoc commits into a proper **atomic state-builder** (gather all property changes, `TEST_ONLY` validate, one commit per frame); add explicit **in/out fences** (`IN_FENCE_FD`, `OUT_FENCE_PTR`, DRM syncobj) so composition waits on render and scanout is race-free.
*Gate:* efficient multi-monitor repaint, race-free presentation — the clean platform that Phases 2–4 hang off.

**Phase 1 exit gate:** composite static textures as windows, move them, hardware cursor from frame zero, multi-output, all VT-switch-safe — reached in small, individually-testable steps.

### Phase 2 — Input + server-authoritative WM — **done**
- libinput → focus + pointer routing in the **global virtual-screen** coordinate space; server-owned window model (create/destroy/move/resize/raise/lower/focus); server-side decorations; Alt-Tab. Both relative and **absolute** pointer motion are handled (VM/QEMU tablets emit the latter).
- **Exit:** drive windows with mouse + keyboard; the server, not any client, owns geometry and z-order.

### Phase 3 — Transport A: CrystallineLattice + `winedrm.drv` + first real Wine client — **in progress**
- Implement the CrystallineLattice server side in glacier: per-client socket, `SCM_RIGHTS` dma-buf import + shm fallback, the fixed control segment + event ring, HWND handles, **role registration**, **fence-fd in the submit message**, **min-version handshake**, buffer-allocation ownership decided (§3.3). *(Landed: socket, shm `SCM_RIGHTS` import, roles, fence-fd field, handshake, client-allocates ownership. Pending: dma-buf/EGLImage import, input routing to the focused client.)*
- Write a trivial **native test client** against the same wire format first (decouples protocol bugs from Wine bugs) — *landed as `glacier-client`* — then **`winedrm.drv`** — the new Wine graphics driver, including the keysym → Win32 VK / `WM_CHAR` translation — drawing one real window (notepad / solitaire) into the server, taking input, placed by the server.
- **Exit:** a Win32 app renders and is interactive through `winedrm.drv` — start of feature-parity with the PoC.

### Phase 4 — Transport B: Wayland compatibility frontend + Xwayland — **landed (frontend); Xwayland next**
- Implement glacier's own minimal Wayland server frontend (§3.5): `wl_compositor`/`wl_surface`/`wl_subsurface`, `xdg_wm_base`, `linux-dmabuf-v1` (+ `wl_shm` fallback), `wl_seat`, `wl_output`, `xdg-decoration` set to **enforce SSD**. Reparent each toplevel as a glacier `NORMAL` window through the same engine path as Transport A. Ignore `wl_pointer.set_cursor` (server owns the cursor plane). *(Landed in `src/wayland.c`: `wl_compositor`/`wl_surface`, `wl_shm` CPU buffers, `xdg_wm_base`/`xdg_surface`/`xdg_toplevel`, `wl_seat` pointer+keyboard, `wl_output`, and xdg-decoration always answering SERVER_SIDE; toplevels reparent into the shared window stack and wear the server title bar. `Ctrl+Alt+T` spawns `foot`. Pending: `linux-dmabuf-v1` import — shm only for now — and `wl_subsurface`/popups.)*
- Bring up **Xwayland** against this frontend for legacy X11 apps. *(Pending — `xorg-xwayland` is packaged; needs the rootless-Xwayland wl_client plumbing + X11 WM glue.)*
- **Exit:** a GTK app, a Qt app, and an X11 app (`xeyes`-class) each render inside glacier wearing Windows server-side decorations and the server cursor, composited as peers of the Wine window from Phase 3. This is the point the "Qt-vs-GTK chrome mess" is gone.

### Phase 5 — The Windows shell (parity gate)
- Taskbar via **explicit role registration** (no geometry heuristics), Start menu, tray, natively-drawn wallpaper, Aero Snap, multi-monitor layout. The taskbar manages **all** windows regardless of transport.
- **Resolution change through your control panel** → control socket → atomic modeset. (`wlr-randr` deleted.)
- **Exit — the one that matters:** match *every* capability the frostedglass PoC had — working cursor, taskbar that resizes correctly, resolution change from the control panel, native Linux apps displaying with consistent chrome, audio (PipeWire untouched) — but natively and with **none** of the §1 hacks. This is the "frostedglass is now deprecated" milestone.

### Phase 6 — Hardening & the payoffs raw-DRM unlocks
- Explicit sync/fencing throughout; hotplug; multi-GPU; **client-crash resilience** (a dead client — Wine *or* Wayland — doesn't kill the desktop; offer reconnect); **overlay-plane direct scanout** for fullscreen Wine apps/video; basic power management (idle blanking, DPMS).
- **Exit:** daily-driver stable on target hardware; fullscreen apps hit a direct-scanout fast path.

### Phase 7 — Distro integration
- Collapse the package set; write ebuilds from **your own source**; wire the boot path `libreldr → seatd → glacier` as the session; remove wlroots / the `wlr_*` zoo / `wlr-randr` from the world file. Keep the §4 minimal libwayland-server frontend protocols + Xwayland.
- **Exit:** YetiOS boots straight into the native shell; the display dependency set is the §4 "keep + add" columns and nothing more.

---

## 6. Ideas worth stealing or inventing (the sprinkles)

- **Hardware cursor plane is the headline.** Lead with it in Phase 1; it's the most satisfying "an entire subsystem just disappeared" moment, it de-risks the rest psychologically, and it's what gives native Linux apps the Windows cursor for free.
- **One server cursor + forced SSD = one consistent OS look.** Server-drawn decorations on *every* window plus `xdg-decoration` forbidding CSD is the precise mechanism that ends the GTK-vs-Qt chrome mess for newcomers. This is the differentiator your screenshot is reaching for.
- **Overlay-plane direct scanout for fullscreen Wine games** → near-native latency and power, genuinely better than a generic Wayland compositor. A real selling point for an "OS for running Windows apps."
- **One global virtual-screen coordinate space, baked in from Phase 2.** Windows assumes it; Wayland forbids it; you get to just *have* it. Don't retrofit it later.
- **Explicit window roles, not heuristics.** `register_role(TASKBAR|DESKTOP|TRAY|NORMAL)`. The single change that kills `is_taskbar()`/`is_desktop()` guessing.
- **One control socket, "everything is a path."** Maps onto the Windows registry / Group Policy mental model. Control-panel applets, theming, modeset, and scripting all speak it. No more `popen`.
- **Declarative theme (msstyles-flavored) for server-side decorations** → authentic Win7 Aero glass updated with modern neo-morphism / skeuomorphism via a GL blur shader on the title bar, themable without touching clients — and it applies identically to Wine and Wayland clients because the server draws it.
- **Crash resilience borrowed from Arcan, without Arcan.** Design *both* transports so client death is recoverable. Your frostedglass SIGCHLD comments show you already think this way; bake it into the protocols.
- **seatd over logind, and consider the static-XKB trick** to keep the dependency graph as tight as the project's premise demands.
- **Resist A12-style network transparency / thin-client temptation** during the rewrite. Fascinating rabbit hole, pure scope creep for v1. Note as a post-1.0 maybe.

---

## 7. Reference reading (steal patterns, not code)

- **David Herrmann's `drm-howto` (`modeset-atomic`)** — the canonical raw atomic-KMS tutorial. He also wrote **KMSCON** (raw-KMS, GL-rendered console, no X/Wayland) — almost exactly your Phase 0/1 in miniature.
- **kmscube** — minimal GBM+EGL+KMS; the hello-world of this domain.
- **wlroots `backend/drm` and `backend/libinput`** — production-grade DRM/input handling. Read the backend; ignore the Wayland-policy opinions.
- **Weston `compositor-drm.c` / `libweston` DRM backend** — older but instructive plane-assignment and direct-scanout logic; **libliftoff** for plane-offload heuristics.
- **Xwayland internals (glamor backend)** — for understanding how the X11 tail bridges onto your Wayland frontend in Phase 4; you consume Xwayland as-is, you don't fork it.
- **A minimal Wayland-compositor tutorial (e.g. `tinywl` from wlroots, or a from-scratch libwayland-server example)** — read for the *frontend protocol surface* (xdg-shell handshake, dmabuf import, seat), not for the wlroots scaffolding you're not using.
- **Arcan's egl-dri platform** — another "engine on raw DRM" reference point.
- **The TI AM62X display doc** — the "embedded reality" stress test: constrained planes, vendor DRM driver, Imagination GLES. Where the assume-nothing-about-planes discipline pays off.

---

## 8. The licensing footnote (because you'll want Win7 look-and-feel)

Use **ReactOS and Open-Shell as behavioral and visual references, not as code to copy** — ReactOS is GPLv2 and *aggressively* clean-room for historical legal reasons, so lifting source carries real risk; Open-Shell's licensing should be checked per-file before vendoring. Shipping pixel-identical Windows icons/sounds/cursors is trademark/copyright exposure regardless of source. You already sidestep this on audio by *generating* event sounds (`gen-media.py`) — apply the same instinct to icons, cursors, and decorations: original assets that *evoke* Win7, not copies of it. (Bonus: the server cursor you upload to the KMS plane is your own original ARGB arrow, so it's clean by construction.)

---

## 9. Resolved design questions

1. ~~Transport: Path α or β?~~ **Resolved: Path β — CrystallineLattice native protocol + `winedrm.drv`** for the Wine path. See §3.3.
2. ~~Shell chrome: native-drawn, Wine-drawn, or hybrid?~~ **Resolved: Hybrid.** Aesthetic target is Windows 7 with a modern coat of paint — incremental refinement (like macOS) over the classic Aero base, leaning into modern skeuomorphism/neo-morphism.
3. ~~Target hardware breadth?~~ **Resolved: Keep the ARM64 path open.** Desktop GPUs primary; the ThinkPad X13s / Snapdragon path stays viable. Locks the conservative "primary + cursor only, GL-composite everything" baseline from Phase 1.
4. ~~How do native Linux / Flatpak apps display under a non-Wayland server, and how do they get a consistent Windows look?~~ **Resolved: a second transport.** glacier implements its *own* minimal, frozen Wayland frontend (not Weston, not wlroots) plus **Xwayland** for legacy X11, both feeding the protocol-agnostic engine. `xdg-decoration` forces server-side decorations and the server-owned KMS cursor plane is shared across all clients, so native apps wear the same Windows titlebar and cursor as Wine apps. This keeps a small `libwayland-server` + frozen-core-protocols + Xwayland set (§4); it does **not** reinstate wlroots, the `wlr_*` zoo, or `wlr-randr`. See §3.5, Phase 4. (Corrects the earlier "drop libwayland entirely" line.)
