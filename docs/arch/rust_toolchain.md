# Rust toolchain for external modules

`CREATE MODULE` compiles Rust sources through the local toolchain, so every
machine that runs queries over external modules needs one. Two code paths in
`qdb/catalog/external_module.cpp` invoke it:

- native modules — `rustc <src> --emit llvm-bc`, and `qdb/utils/rust_std_loader.cpp`
  then `dlopen`s `libstd` from `rustc --print target-libdir`;
- wasm64 modules — `cargo rustc --target wasm64-unknown-unknown --release
  -Z build-std=std,panic_abort` with `RUSTC_BOOTSTRAP=1`, because Rust ships no
  prebuilt wasm64 std.

Two consequences follow. The `rust-src` component is mandatory (`build-std`
compiles std from source), and **the Rust release must match the LLVM the engine
links against**: LLVM 20 pairs with Rust 1.90.0, the version pinned in
`.github/workflows/c-cpp.yml`. A newer stable emits bitcode LLVM 20 refuses to
read.

## Packaged service

The service runs as the system user `qumirdb` with no home of its own, so a
per-user `rustup` install is not there and requests fail with:

```text
rustup could not choose a version of cargo to run, because one wasn't specified
explicitly, and no default is configured
```

Install the toolchain system-wide instead of into a home directory:

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/rustup-init.sh
sudo env RUSTUP_HOME=/opt/rust/rustup CARGO_HOME=/opt/rust/cargo \
     sh /tmp/rustup-init.sh -y --no-modify-path --profile minimal \
     --default-toolchain 1.90.0 --component rust-src
sudo chmod -R a+rX /opt/rust
```

Give the service a writable `CARGO_HOME` (cargo keeps `.package-cache` and the
registry index there) and point it at the toolchain through a systemd drop-in,
leaving the packaged unit untouched:

```sh
sudo install -d -o qumirdb -g qumirdb /var/lib/qumirdb-data/cargo
sudo systemctl edit qumirdb-service
```

```ini
[Service]
Environment=RUSTUP_HOME=/opt/rust/rustup
Environment=RUSTUP_TOOLCHAIN=1.90.0
Environment=CARGO_HOME=/var/lib/qumirdb-data/cargo
Environment=PATH=/opt/rust/cargo/bin:/usr/local/bin:/usr/bin:/bin
```

`RUSTUP_TOOLCHAIN` pins the version without depending on whose `rustup default`
was configured; `PATH` matters because the engine execs `cargo`/`rustc` directly,
and systemd's default `PATH` does not include `/opt/rust/cargo/bin`. Add
`Environment=CARGO_NET_OFFLINE=true` on hosts without network access — `build-std`
needs no crates beyond `rust-src`.

Then restart and verify as the service user:

```sh
sudo systemctl daemon-reload
sudo systemctl restart qumirdb-service

sudo -u qumirdb env HOME=/var/lib/qumirdb-data \
     RUSTUP_HOME=/opt/rust/rustup RUSTUP_TOOLCHAIN=1.90.0 \
     CARGO_HOME=/var/lib/qumirdb-data/cargo PATH=/opt/rust/cargo/bin:$PATH \
     sh -c 'rustc --version; cargo --version; rustc --print target-libdir'
```

It must print `rustc 1.90.0`, `cargo 1.90.0` and an existing `target-libdir`;
the last one is the directory the engine loads `libstd` from.

## Development machines

A normal per-user install is enough — the same pinned release and component:

```sh
rustup toolchain install 1.90.0 --profile minimal --component rust-src
export RUSTUP_TOOLCHAIN=1.90.0
```
