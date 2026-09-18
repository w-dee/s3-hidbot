# v0.4.0 release authorization checklist

This is the prepared handoff at the boundary before tag creation, remote main
mutation, GitHub Release creation, or publication. Those actions require owner
authorization.

## Fixed release identity

| Item | Value |
| --- | --- |
| Intended tag | `v0.4.0` |
| Intended annotated tag target | `137d489d3d1219b203f84633cf8b570d9fe9f19a` |
| Intended title | `s3-hidbot v0.4.0` |
| Rendered release body SHA-256 | `62897e9a678681f46e80430deeae162428ed0db61f60413561c085453985b1fe` |
| Prepared tag annotation SHA-256 | `0fc26d192e801ed4f7a6e5b757a42481c9186ede64ec6f784099ce14829dee25` |
| Product version | `0.4.0` |
| Target/profile | `esp32s3` / `freenove-fnk0099` |
| Qualified firmware archive SHA-256 | `2e6b5a1a20a83e20cfeafa2d10835ddb605f4aff0319225b48e340c856b39a35` |

The intended tag target is the qualified product commit. The later
authority-v5 and release-preparation commits contain evidence tooling and
documentation only; they are deliberately separate from the qualified product
tree. The fixed public set is staged under
`~/Downloads/s3-hidbot-v0.4.0-release-candidate/assets/`. Its firmware archive
must retain the exact hash above through every upload and read-back.

Candidate Release build run `35349474513` passed both independent builds and
the Python 3.11/3.12 checkout-free consumers at the qualified source and epoch.
Its application, ELF, bootloader, partition table, flash plan, SDK lock/config,
and source/dependency authority are byte-identical to the qualified bundle.
Its outer archive SHA-256 is
`7715ef38d1c32a584e7facb63747d74049a74fdcf4237db77398eb57dc9c1ff1`
because its manifest truthfully records the pinned container and different
tool versions. That CI archive is not a release asset and must not replace the
qualified archive.

## Prepared local asset inventory

The repository release contract defines exactly ten assets. The firmware is
the byte-identical qualified archive; the host distributions were built from
an archive of the intended tag target and passed canonical metadata/content
validation. Repository policy makes no cross-build byte-identity claim for the
host distributions.

| Asset | SHA-256 |
| --- | --- |
| `LICENSE` | `68ae4d8a63072ae51282cb47ce3fce990c0f64c3e899163c55675df85efd4f8e` |
| `LICENSE.sha256` | `a915c5462f553540992d29e3c7924c39580ae9316c998ce8143b987ba73f153d` |
| `THIRD_PARTY_NOTICES.md` | `8c2d58f417519a44a2427138c443a7cf05d69c7be82e6b6f89d21b0ba3af15aa` |
| `THIRD_PARTY_NOTICES.md.sha256` | `60d3bbffc55a50e451fb2835bb51184fcb33c94dca7ce774239dbc296bbb9946` |
| `s3-hidbot-firmware-0.4.0-esp32s3-freenove-fnk0099.tar.gz` | `2e6b5a1a20a83e20cfeafa2d10835ddb605f4aff0319225b48e340c856b39a35` |
| `s3-hidbot-firmware-0.4.0-esp32s3-freenove-fnk0099.tar.gz.sha256` | `03fff80bd2c5cf89671a4eeb8df770a7f5a95ae2be92e3c1a36478deabb0666f` |
| `s3_hidbot_host-0.4.0-py3-none-any.whl` | `790915b5968b80ffbe0d19eaca6f2316434b5e7260a6b72daa9ed5c2dd743f8f` |
| `s3_hidbot_host-0.4.0-py3-none-any.whl.sha256` | `2b261e4e906d6af8d9a8d8402aef16428fd8fe470e9b338c9d32dcec5cfde734` |
| `s3_hidbot_host-0.4.0.tar.gz` | `3cc67c1ef21918e021c483167527a74f3d03e4bf2335d2bdf87cc4f1501587ba` |
| `s3_hidbot_host-0.4.0.tar.gz.sha256` | `3a46e61462b7c96634f58abea90b6b5425ed38d470ed90be27806ac6a4783611` |

## Completed gates

- [x] qualification preflight and zero-evidence early-failure packaging repair;
- [x] mechanical v0.4.0 version seal with no behavior change;
- [x] canonical software, package, firmware, privacy, and resource validation;
- [x] two byte-identical qualified firmware builds;
- [x] one official authority-v5 FNK0099 attempt with Q1–Q11 PASS;
- [x] independent read-only evidence review and safe final fixture state;
- [x] release notes, qualification summary, exact asset list, and hashes;
- [x] local canonical release-asset verification and qualified-archive byte comparison;
- [x] candidate Release build run `35349474513` and both checkout-free consumers;
- [ ] owner authorization for the irreversible/public release sequence.

## Owner-authorized release sequence

After reviewing the final dossier, the owner may separately authorize this
bounded sequence:

1. confirm the intended tag target and that no local or remote `v0.4.0` tag or
   Release exists;
2. create an annotated unsigned `v0.4.0` tag at the exact target above and push
   that tag normally;
3. dispatch the fixed recovery workflow from `release/v0.4.0` for the existing
   tag and exact target, require it to pass, and compare every firmware payload
   byte and authority field with the qualified archive;
4. retain the fixed local ten-asset set, verify all adjacent sidecars and the
   rendered release body, and reject the container-built outer firmware archive
   as a substitute;
5. create one stable GitHub Release draft without overwriting any existing
   object, then read it back and freshly download and compare all assets; and
6. only after a separate publication authorization, publish that same verified
   draft and repeat the read-back and fresh-download checks.

Do not retry a tag, draft, upload, or publication mutation blindly after an
unknown outcome. Recover state with read-only queries first.
