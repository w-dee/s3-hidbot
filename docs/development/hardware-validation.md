# s3-hidbot hardware validation

## Scope and gate

Hardware work is a separate gate and begins only after explicit human approval.
This document records the currently established safety policy; it does not
define unverified baud rates, flash procedures, retry policies, USB-OTG
behavior, or benchmark procedures.

## Serial configuration

Provide the machine-local serial device through the environment when a future
hardware procedure requires it:

```bash
export S3_HIDBOT_SERIAL=/dev/serial/by-id/<s3-hidbot-uart>
```

The exact serial identifier belongs in local configuration, not in tracked
documentation. A device being absent from the normal sandbox is not evidence
that it is physically disconnected. Physical serial access may require
elevated execution.

## Local ESP-IDF environment

Keep the machine-specific ESP-IDF installation and activation paths in local
configuration. Do not add their absolute paths to tracked documentation.

Expand this document only after a hardware-validation procedure has been
established and reviewed.

## BOOT-button diagnostic

GPIO0 is the active-low BOOT button and a boot strapping pin. Do not hold it
low during reset, power-on, flashing, or bootloader entry unless download boot
is explicitly intended. The controlled Mouse-report bring-up diagnostic is
build-time opt-in; the default firmware does not configure GPIO0 or send HID
reports in response to a BOOT-button press.
