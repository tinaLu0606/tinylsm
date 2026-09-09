# SSTable v2 format

## Scope

SSTable v2 is the default output format after Goal 5. It prefix-compresses user
keys within each data block, records restart offsets, and persists table
properties. The Reader remains compatible with v1 tables; Compaction gradually
rewrites a v1 input set as v2 output without an in-place migration.

```text
v1: data blocks | index | footer(36 B)
v2: data blocks | index | properties | footer(52 B)
```

Both versions retain internal entries in this order:

```text
(user key ascending, sequence descending)
```

## v2 data block

Each entry stores these fixed-width fields followed by byte strings:

```text
shared_key_bytes u32 | unshared_key_bytes u32 | sequence u64 | type u8
value_bytes u32 | unshared_key | value
```

The first entry in a restart group has `shared_key_bytes = 0`. Every
`Options::sstable_restart_interval` entries (default 16) starts a new group.
The block trailer is:

```text
restart offsets[u32] | restart_count u32 | entry_count u32 | crc32c u32
```

The decoder validates checksum, restart ordering, every length, key ordering,
value/tombstone shape, and that restart offsets land exactly on entry boundaries.
It reconstructs complete keys before exposing entries. With the decoded-block
cache disabled, a v2 point Get binary-searches restart keys and reconstructs
only the selected restart group. With the cache enabled, it still prefers the
complete validated decoded block. This is a functional lookup path, not a
performance claim; its before/after cost remains deferred work.

## Properties and footer

v2 properties contain `entry_count`, smallest/largest user key, and min/max
sequence, protected by a separate CRC32C. The 52-byte v2 footer records the
index and properties ranges, magic, CRC32C, and a reserved zero field. The
Reader first attempts the fixed 36-byte v1 footer; if that fails, it validates
the fixed 52-byte v2 footer and requires contiguous referenced sections.

Properties provide cheap metadata loading, but they do not replace data-block
validation: startup continues to read every Manifest-referenced block and
checks that its computed properties match the persisted values.

## Compatibility and recovery

- v1 footer/data blocks have no properties section and remain readable.
- New flush and compaction output is v2.
- The Manifest format is unchanged because it already records table file size,
  key range, and sequence range independently.
- A malformed footer, section range, properties record, block, index, or
  cross-check is `Corruption`; no partial Scan is returned.

This format deliberately does not add Bloom filters or a compression-library
matrix. The Bloom decision specifically awaits a separate cold-data,
high-miss-ratio, multi-SSTable profile; no such performance evidence is claimed
by this implementation.
