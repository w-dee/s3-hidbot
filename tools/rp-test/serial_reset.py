"""Single-attempt ESP32 application reset with explicit pre-open line state."""

from common import need


def application_hard_reset_once(endpoint, serial_module, hard_reset_factory):
    """Reset to application boot without relying on pyserial line defaults.

    ``endpoint`` remains private to the caller.  Dependencies are injected so
    host-only tests never enumerate or open a physical serial interface.
    """
    need(endpoint is not None and callable(hard_reset_factory)
         and hasattr(serial_module, 'Serial'), 'APPLICATION_RESET_INPUT_INVALID')
    port = serial_module.Serial(
        port=None,
        baudrate=115200,
        bytesize=serial_module.EIGHTBITS,
        parity=serial_module.PARITY_NONE,
        stopbits=serial_module.STOPBITS_ONE,
        timeout=0.2,
        write_timeout=1.0,
        xonxoff=False,
        rtscts=False,
        dsrdtr=False,
        exclusive=True,
    )
    try:
        # pyserial applies the cached DTR/RTS values during open().  Release
        # GPIO0 and EN before assigning/opening the real endpoint; esptool's
        # HardReset changes RTS only and therefore inherits the DTR state.
        port.dtr = False
        port.rts = False
        port.port = endpoint
        port.open()
        hard_reset_factory(port, uses_usb=False).reset()
    finally:
        # Preserve application-boot idle even when reset raises.  There is no
        # retry here; the mission's durable startup budget remains authoritative.
        try:
            port.dtr = False
            port.rts = False
        finally:
            port.close()
