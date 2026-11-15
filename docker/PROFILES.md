<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Build profiles

Both profiles expose the same binary contract; they differ only in the libc
their embedded `libfractalsql-*.a` was compiled for. glibc 2.34 is the
supported floor. There is no glibc 2.28 ("legacy") profile.

| Profile | libc | Base | Artifact suffix | Use when |
|---|---|---|---|---|
| modern (default) | glibc 2.34 | `rockylinux:9` | *(none)* | RHEL 9+, Ubuntu 22.04+, Debian 12+, SLES 15 SP5+ |
| musl | musl 1.2 | `alpine:3.20` | `-musl` | Alpine 3.20+, distroless-musl, container-native deployments |

Both are built for amd64 and arm64. macOS (amd64 and arm64) is built
separately and has no profile.

## How `build.sh` reads the profile

`build.sh` reads `PROFILE` from env or the `--profile=` CLI flag,
defaulting to `modern`. The matching `PROFILE` Dockerfile build-arg
controls the build stage. When `PROFILE=musl`, the build:

1. Switches the base image to `alpine:3.20`
2. Links `include/<platform>/libfractalsql-community-minimal-c-musl.a`
   instead of the modern `.a`
3. Tags the resulting binary with the `-musl` suffix

## Invocation

```bash
./build.sh amd64                 # modern
PROFILE=musl ./build.sh amd64    # => dist/amd64/fractalsql-musl.so
```

## Consumer's perspective

```bash
# RHEL 9+:
sudo dnf install fractalsql-valkey

# Alpine:
sudo apk add fractalsql-valkey-musl
```
