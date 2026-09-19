# Migrating from WebZero 1

1. Rebuild the native server and CLI together. New bundles have version 2. Old servers cannot read them.
2. Rebuild site directories with `node tools/wz.js build <directory>`. Routes previously dropped beyond the eighth child are now retained. Long names and colliding extensionless HTML routes produce explicit errors.
3. Use Node 18+ for the CLI, or run the C executable directly with no Node dependency. `serve` now actually prefers the native binary; use `--js` for the development server.
4. Check any deployment relying on dotfiles, symlinks or `node_modules` being published: these are now excluded. Percent signs, question marks, hashes, backslashes, control characters and wildcard names are rejected in source route segments.
5. Expect slightly larger on-disk bundles: text stores both Brotli and identity bytes. This removes request-time decompression and supports clients without Brotli in the native server. Binary media is not recompressed. Measure transfer bytes separately from disk bytes.
6. Cache policy changes to revalidation. Weak ETags identify representations, `If-None-Match` produces 304, and `Vary` covers encoding and WebP. Native and Node validators need not be identical; switching backends can cause one extra full response.
7. GET/HEAD are accepted for static files. POST requires a native bytecode route. Invalid framing returns a specific error and closes the connection. Chunked bodies and Expect requests are explicitly unsupported.

## Legacy bundles

Both readers accept structurally valid version-1 bundles using the compiler's **relative** asset offsets. The new native server serves their Brotli assets when the client accepts Brotli; otherwise it returns 406 because v1 has no identity representation. Rebuild as v2 for full negotiation. The Node server decompresses valid legacy assets once at startup, with a 128 MiB aggregate decoded-byte cap.

Legacy bundles with silently orphaned routes, invalid indices or unterminated metadata are now rejected. Rebuilding is the repair path. No existing `.web` file is rewritten by `serve` or `inspect`.

## Operational changes

The original documentation described hot reload, `sendfile`, `TransmitFile`, automatic native serving and a hard 4 MiB total footprint that the implementation did not provide. The README now describes tested behavior and distinguishes the arena from total resident memory. Bundle changes require a restart. Range support is single-range only, and inactivity limits do not constitute complete DoS protection.

The version is bumped to 2.0.0 because of the new bundle format and CLI runtime floor. Publishing tagged binaries or npm remains a separate release step.
