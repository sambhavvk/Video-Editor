<!-- SPDX-License-Identifier: MPL-2.0 -->

# Proxy service

The service transcodes the best decodable video stream and, when requested,
the best decodable audio stream. It scans the complete input stream table but
does not yet proxy alternate angles, multiple language tracks, subtitles,
attachments, alpha, or interlaced field cadence. Those streams remain
authoritative in the source and are never represented by the proxy.

Video presentation timestamps are rebased to the input media origin and stored
alongside their exact source timestamps in a versioned `.vepts` sidecar. The
sidecar is authoritative for mapping a proxy frame back to the original; the
proxy container's nominal frame-rate is not. Proxy audio is signed 16-bit PCM
at 48 kHz and preserves the original timestamp relationship to video, subject
to FFmpeg resampler rounding to the nearest output sample.

Official packages must dynamically link the project's pinned LGPL FFmpeg
bundle. The local developer build may link ABI-identical system FFmpeg only for
testing, and must not be treated as a redistributable build.

`discover_proxy` looks up a complete matching proxy in `CacheStore` (Proxy +
ProxyPtsMap kinds) then in an optional legacy `{assetId}.proxy.{mov|mkv}`
directory. Matching requires a valid `.vepts` whose source fingerprint still
content-matches. `proxy_parameter_hash` is the stable cache key for a profile.
