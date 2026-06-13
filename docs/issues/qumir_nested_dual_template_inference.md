# Qumir: nested dual-template inference loses the stored type

## Reproducer

A generic function has independent named templates `LookupKey` and
`StoredKey`. It casts byte storage to `<ptr StoredKey>` and calls another
generic function with that pointer and a concrete `LookupKey` value.

The nested call does not consistently specialize `StoredKey`. Fixed-width
keys report a named-to-integer conversion, while string keys fail to resolve
`rh_key_equal(StoredKey, LookupKey)`.

## QDB workaround

Pass an explicit concrete `StoredKey` witness to the outer generic function.
The value is unused, but it anchors both named template parameters before the
nested generic calls are resolved.
