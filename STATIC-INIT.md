# Static initialization order

`DynOS_Init` is declared `DYNOS_AT_STARTUP` — a constructor attribute, so it
runs before `main`. It reaches `gfx_register_layout_graph_node`, which touches
`RT64Context`'s `std::unordered_map` members.

`RT64Context RT64;` was a plain global. The order in which globals across
different translation units are constructed is unspecified, so DynOS could —
and on Linux did — reach those maps before their constructor ran. Using an
unordered_map with a zero bucket count divides by zero: **SIGFPE, before a
single line of output.**

The fix is construct-on-first-use, which the standard does guarantee:

```cpp
RT64Context &gfx_rt64_context(void) {
    static RT64Context context;
    return context;
}
#define RT64 gfx_rt64_context()
```

The macro keeps every existing `RT64.field` site unchanged.

This was luck on Windows, not correctness — MSVC happened to construct the
global before running DynOS's constructor. Nothing guaranteed it.
