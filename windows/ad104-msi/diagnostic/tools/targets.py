"""Probe build targets. Values must match src/target.h."""
import os

TARGETS = {
    'ad104-msi': dict(build='build', binary='AD104-PROBE.EFI', device='0x2782',
                      config_ids=('de108227', '62143251'), subsystem=('0x1462', '0x5132'),
                      cached_rom_exact=150016, cached_rom_limit=150016),
    'ad102-fe': dict(build='build-ad102-fe', binary='AD102-FE-PROBE.EFI', device='0x2684',
                     config_ids=('de108426', 'de10f416'), subsystem=('0x10de', '0x16f4'),
                     cached_rom_exact=0, cached_rom_limit=0x80000),
}


def selected():
    """Build target named by PROBE_TARGET; the AD104 profile when unset."""
    return TARGETS[os.environ.get('PROBE_TARGET', 'ad104-msi')]


def for_capture(data):
    """AD104 captures predate the probe_target field."""
    name = data.get('probe_target', 'ad104-msi')
    if name not in TARGETS:
        raise ValueError('Unknown probe target')
    return TARGETS[name]
