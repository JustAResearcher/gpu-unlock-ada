gpu_unlock now loads ahead of nvidia on every boot but does nothing until
you opt in on the kernel command line, for example:
  gpu_unlock.targets=0000:03:00.0     only these PCI addresses (comma-separated)
  gpu_unlock.targets=all              every accepted unbound card
Experimental Ada targets also need their own gpu_unlock.* flag.
Remove the option in your bootloader to boot without the unlock.
See /usr/share/doc/gpu-unlock/README.md.
