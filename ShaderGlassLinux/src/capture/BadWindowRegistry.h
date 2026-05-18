#pragma once

// Per-Display* BadWindow notification dispatch.
//
// Xlib's error handler is a single process-wide function pointer. To support
// multiple RealX11CaptureSession instances (each owning its own Display*),
// we keep a tiny registry keyed by Display* and dispatch incoming BadWindow
// errors to the right slot.
//
// Each session calls add() in its constructor and remove() in its destructor.
// The error handler calls note(ev->display) on every BadWindow it sees.
// The session's grab() loop calls consume(m_display) to atomically read and
// clear its own flag.
//
// All methods are safe to call from any thread.

struct _XDisplay;
typedef struct _XDisplay Display;

namespace BadWindowRegistry {

void add(Display* d);
void remove(Display* d);

// Mark `d` as having seen a BadWindow. No-op if `d` is not registered —
// other displays in the same process are not part of our session pool.
void note(Display* d) noexcept;

// Atomically reads and clears the BadWindow flag for `d`. Returns false if
// `d` is not registered or the flag was clear.
bool consume(Display* d) noexcept;

} // namespace BadWindowRegistry
