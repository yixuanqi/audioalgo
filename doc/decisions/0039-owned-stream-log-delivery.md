# 0039: Owned streaming logs and independent raw archives

2026-10-24, PC0402. Python stdout/stderr are user bytes, never control messages.
Each reader saves raw bytes under the shared 16MiB task budget. A streaming UTF-8
decoder retains incomplete code points and replaces malformed input; NUL is
displayed as U+2400. Raw byte totals and trimmed raw-tail counts remain distinct
from dropped displayed UTF-8 bytes. The worker drains bounded chunks at most
every20ms, with a final drain after pipe EOF. Observers run on the worker, never
on pipe reader threads. UI delivery owns values and checks the existing full
RunIdentity before display; it must have bounded pending bytes and callbacks.

PC-02 still requires exactly two process prints for513 frames/host512. Streaming
may place identity headers between chunks, so adjacent text from separate chunks
is not an API guarantee. The independent acceptance remains two literal print
records, current identity/source hash and exact audio. The earlier contiguous
two-line assertion is redundant with that exact count and is replaced by
per-record presence plus the unchanged exact count; no count/budget is relaxed.

Formal C ABI v1 layout and entry points are unchanged. New native log negotiation
is separate future work. Check-result terminal logs and live Run logs share text
presentation, with explicit Check/Run identities; desktop evidence stays pending.
