# Third-party notices for the release asset set

This is a factual inventory for the 0.2.0 FNK0085 / ESP32-S3 firmware build
and accompanying host distributions. It is not a legal opinion or a
statement that this list answers every redistribution question. The release
workflow attaches this file and the project LICENSE as separate, checksummed
Release assets; it does not alter the firmware-bundle schema.

## Evidence used

- The project is MIT licensed: LICENSE.
- firmware/dependencies.lock locks ESP-IDF 5.5.4, espressif/esp_tinyusb
  2.2.1, and espressif/tinyusb 0.21.0~1.
- firmware/main/idf_component.yml directly selects esp_tinyusb; its component
  manifest publicly selects TinyUSB.
- The release firmware builder records the effective dependency lock and the
  build-tool versions in the verified bundle manifest. The release procedure
  does not change the locked dependency set.
- A representative clean ESP-IDF v5.5.4 ESP32-S3 link map for this profile
  contributes the ESP-IDF core, FreeRTOS, cJSON, ESP TinyUSB, TinyUSB, Mbed
  Crypto, the Espressif target PHY library, and target runtime libraries from
  the Xtensa ESP ELF toolchain. This is build evidence, not a claim about
  source availability.

The project source and separately attached LICENSE cover s3-hidbot itself.
The entries below identify material supplied by the locked component/build
inputs. They retain supplied copyright and license information where it is
available in those inputs.

## ESP-IDF and Espressif libraries

- ESP-IDF 5.5.4, including the ESP-IDF source components contributing to this
  profile, is supplied with an Apache License 2.0 LICENSE at the ESP-IDF
  repository root. Upstream: <https://github.com/espressif/esp-idf>.
- Espressif ESP TinyUSB 2.2.1 is identified by
  firmware/managed_components/espressif__esp_tinyusb/idf_component.yml; its
  supplied LICENSE is Apache License 2.0. Upstream source commit:
  8e779566ef71d43928cbf7e125e8eb54bab3f542 in
  <https://github.com/espressif/esp-usb>.
- Espressif target PHY library is an ESP-IDF component contributing to the
  profile. Its supplied component-library LICENSE is Apache License 2.0.

Apache License 2.0 text:
<https://www.apache.org/licenses/LICENSE-2.0.txt>

## TinyUSB

TinyUSB 0.21.0~1 is a public dependency of ESP TinyUSB in the locked component
metadata. The supplied component license reads:

> The MIT License (MIT)
>
> Copyright (c) 2012-2026, hathach (tinyusb.org)
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

Upstream: <https://github.com/espressif/tinyusb>, locked source commit
7049c58a0e895acc92c6407574b05b5536eddfc8.

## FreeRTOS and cJSON

The contributing ESP-IDF build components include FreeRTOS Kernel and cJSON.
Their supplied source trees identify the following MIT notices:

> Copyright (C) 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.
>
> Copyright (C) 2015-2019 Cadence Design Systems, Inc.
>
> Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

Upstream sources: <https://github.com/FreeRTOS/FreeRTOS-Kernel> and
<https://github.com/DaveGamble/cJSON>.

## Mbed TLS / Mbed Crypto

The contributing ESP-IDF profile links Mbed Crypto from the ESP-IDF Mbed TLS
component. Its supplied LICENSE describes the Mbed TLS files as dual
Apache-2.0 OR GPL-2.0-or-later and provides the full text of both licenses.
The bundled release workflow keeps the ESP-IDF version and source lock fixed;
this notice records the supplied dual-license identification without selecting
or interpreting an option. Upstream: <https://github.com/Mbed-TLS/mbedtls>.

## Xtensa ESP ELF runtime libraries

The linked firmware and bootloader use the target runtime archives libgcc,
libstdc++, libc, and libm from the Xtensa ESP ELF toolchain selected by the
pinned ESP-IDF build image. The installed toolchain supplies GNU GPL v3, GCC
Runtime Library Exception, Newlib, and Picolibc notice/license materials.
These are factual source-package references; the release workflow does not
claim signing, attestation, or an interpretation of their terms.

- GCC licenses and runtime exception:
  <https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html>
- Newlib notice materials: <https://sourceware.org/newlib/>
- Picolibc notice materials: <https://github.com/picolibc/picolibc>

Before any release changes the pinned ESP-IDF image, toolchain, dependency
lock, or firmware link inputs, this inventory must be rechecked against the
new component and build metadata.
