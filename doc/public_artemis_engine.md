# Public Artemis engine source (integration branch)

This branch adds the public open-source [openartemis](https://github.com/luxiaoling-mc/openartemis) engine as a pinned Git submodule at `packages/OpenArtemis`.

Source SHA: `6465922e13d7cbb071809ad5e4a396601deac1d1`.

License: MIT OR GPL-3.0-or-later (the upstream project's stated dual license), subject to third-party dependencies.

**Not yet executable in Aether.** Aether's Artemis provider exists in the inaccessible private `AetherInternal` package. The public `openartemis` engine does not implement Aether's provider ABI, so this submodule is source provisioning only, not an engine implementation wired into the iOS app.

Required work before making a usable iOS IPA:
- Build the portable openartemis runtime core and dependencies for `arm64-apple-ios`, without the separate SDL window host.
- Implement the `engine_runtime_provider_v1_t` glue (game detection, PFS mounting, script lifecycle/ticks, input, audio/video, framebuffer, saves, teardown), linking it to Aether's existing engine dispatcher.
- Add CMake and iOS archive staging/link flags, test real Artemis games, then rebuild the unsigned IPA.

Upstream source explicitly includes regression configuration for `OA_TEST_TG3_PFS` (Amakano 3) in its CMake tests; that does not prove compatibility for every Amakano release or on iOS.
